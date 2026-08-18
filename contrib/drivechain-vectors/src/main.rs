// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

//! Records what the reference implementation of BIP-300/301 decides about a set
//! of inputs, so that Bitcoin Core's own tests can check themselves against it.
//!
//! Inputs are chosen for the places the two implementations could plausibly
//! drift: the message tags, the encodings that are byte-exact where their
//! neighbours are not, the boundaries of every length rule, and the arithmetic
//! behind `M6ID`. A vector that both implementations get right tells you
//! nothing; these are the ones where getting it wrong is easy.

use bip300301_enforcer_lib::messages::{
    compute_m6id, parse_op_drivechain, try_parse_op_return_address, CoinbaseMessage, M1ProposeSidechain,
    M2AckSidechain, M3ProposeBundle, M4AckBundles, M7BmmAccept, M8BmmRequest,
};
use bitcoin::hashes::Hash as _;
use bitcoin::opcodes::all::{OP_PUSHBYTES_1, OP_PUSHDATA1, OP_RETURN};
use bitcoin::opcodes::OP_TRUE;
use bitcoin::script::PushBytesBuf;
use bitcoin::{Amount, ScriptBuf, Transaction, TxIn, TxOut};
use serde_json::{json, Value};

/// `OP_RETURN <push>` carrying `payload`, which is how every coinbase message
/// is encoded.
fn op_return(payload: &[u8]) -> ScriptBuf {
    ScriptBuf::new_op_return(PushBytesBuf::try_from(payload.to_vec()).unwrap())
}

fn tagged(tag: &[u8], body: &[u8]) -> ScriptBuf {
    op_return(&[tag, body].concat())
}

/// The same payload behind an explicit `OP_PUSHDATA1` rather than a minimal
/// push. Coinbase messages are parsed as instructions and accept it; an M8 is
/// matched byte for byte and does not.
fn op_return_pushdata1(payload: &[u8]) -> ScriptBuf {
    let mut bytes = vec![OP_RETURN.to_u8(), OP_PUSHDATA1.to_u8(), payload.len() as u8];
    bytes.extend_from_slice(payload);
    ScriptBuf::from_bytes(bytes)
}

fn treasury_script(slot: u8) -> ScriptBuf {
    ScriptBuf::from_bytes(vec![0xB4, OP_PUSHBYTES_1.to_u8(), slot, OP_TRUE.to_u8()])
}

/// What the reference implementation makes of a coinbase output.
fn coinbase_result(script: &ScriptBuf) -> Value {
    match CoinbaseMessage::parse(script) {
        Ok((_, message)) => match message {
            CoinbaseMessage::M1ProposeSidechain(M1ProposeSidechain {
                sidechain_number,
                description,
            }) => json!({"type": "m1", "slot": sidechain_number.0, "description": hex::encode(&description.0)}),
            CoinbaseMessage::M2AckSidechain(M2AckSidechain {
                sidechain_number,
                description_hash,
            }) => json!({"type": "m2", "slot": sidechain_number.0, "proposal_id": hex::encode(description_hash.to_byte_array())}),
            CoinbaseMessage::M3ProposeBundle(M3ProposeBundle {
                sidechain_number,
                bundle_txid,
            }) => json!({"type": "m3", "slot": sidechain_number.0, "m6id": hex::encode(bundle_txid)}),
            CoinbaseMessage::M4AckBundles(m4) => match m4 {
                M4AckBundles::RepeatPrevious => json!({"type": "m4", "version": 0, "votes": []}),
                M4AckBundles::OneByte { upvotes } => {
                    json!({"type": "m4", "version": 1, "votes": upvotes.iter().map(|v| *v as u64).collect::<Vec<_>>()})
                }
                M4AckBundles::TwoBytes { upvotes } => {
                    json!({"type": "m4", "version": 2, "votes": upvotes.iter().map(|v| *v as u64).collect::<Vec<_>>()})
                }
                M4AckBundles::LeadingBy50 => json!({"type": "m4", "version": 3, "votes": []}),
            },
            CoinbaseMessage::M7BmmAccept(M7BmmAccept {
                sidechain_number,
                sidechain_block_hash,
            }) => json!({"type": "m7", "slot": sidechain_number.0, "hash": hex::encode(sidechain_block_hash.0)}),
        },
        Err(_) => Value::Null,
    }
}

fn coinbase_vector(name: &str, script: ScriptBuf) -> Value {
    json!({
        "kind": "coinbase",
        "name": name,
        "script": hex::encode(script.as_bytes()),
        "result": coinbase_result(&script),
    })
}

fn treasury_vector(name: &str, script: ScriptBuf) -> Value {
    let result = match parse_op_drivechain(script.as_bytes()) {
        Ok((_, slot)) => json!({"slot": slot.0}),
        Err(_) => Value::Null,
    };
    json!({"kind": "treasury", "name": name, "script": hex::encode(script.as_bytes()), "result": result})
}

fn address_vector(name: &str, script: ScriptBuf) -> Value {
    let result = match try_parse_op_return_address(&script) {
        Some(address) => json!({"payload": hex::encode(address)}),
        None => Value::Null,
    };
    json!({"kind": "address", "name": name, "script": hex::encode(script.as_bytes()), "result": result})
}

fn m8_vector(name: &str, script: ScriptBuf) -> Value {
    let result = match M8BmmRequest::parse(script.as_bytes()) {
        Ok((_, request)) => json!({
            "slot": request.sidechain_number.0,
            "hash": hex::encode(request.sidechain_block_hash.0),
            "prev": hex::encode(request.prev_mainchain_block_hash.to_byte_array()),
        }),
        Err(_) => Value::Null,
    };
    json!({"kind": "m8", "name": name, "script": hex::encode(script.as_bytes()), "result": result})
}

/// A withdrawal: one input spending the treasury, the treasury change at
/// vout[0], and the payouts after it.
fn withdrawal(slot: u8, change: u64, payouts: &[u64]) -> Transaction {
    let mut output = vec![TxOut {
        value: Amount::from_sat(change),
        script_pubkey: treasury_script(slot),
    }];
    for payout in payouts {
        output.push(TxOut {
            value: Amount::from_sat(*payout),
            script_pubkey: ScriptBuf::from_bytes(vec![OP_TRUE.to_u8()]),
        });
    }
    Transaction {
        version: bitcoin::transaction::Version::TWO,
        lock_time: bitcoin::absolute::LockTime::ZERO,
        input: vec![TxIn::default()],
        output,
    }
}

fn m6id_vector(name: &str, tx: Transaction, treasury_spent: u64) -> Value {
    let result = match compute_m6id(tx.clone(), Amount::from_sat(treasury_spent)) {
        Ok((m6id, slot)) => json!({"slot": slot.0, "m6id": hex::encode(m6id.0.to_byte_array())}),
        Err(_) => Value::Null,
    };
    json!({
        "kind": "m6id",
        "name": name,
        "tx": hex::encode(bitcoin::consensus::serialize(&tx)),
        "treasury_spent": treasury_spent,
        "result": result,
    })
}

fn main() {
    let hash32 = [0x11u8; 32];
    let other32 = [0x22u8; 32];
    let mut vectors: Vec<Value> = Vec::new();

    // --- treasury scripts -------------------------------------------------
    // Every slot, because a script-number encoding gets three quarters of the
    // range wrong: 0, 1..16 and everything above 127.
    for slot in [0u8, 1, 16, 127, 128, 255] {
        vectors.push(treasury_vector(&format!("treasury slot {slot}"), treasury_script(slot)));
    }
    vectors.push(treasury_vector(
        "treasury with a trailing byte",
        ScriptBuf::from_bytes(vec![0xB4, 0x01, 0x01, OP_TRUE.to_u8(), OP_TRUE.to_u8()]),
    ));
    vectors.push(treasury_vector(
        "treasury missing its OP_TRUE",
        ScriptBuf::from_bytes(vec![0xB4, 0x01, 0x01]),
    ));
    vectors.push(treasury_vector(
        "treasury on a different NOP",
        ScriptBuf::from_bytes(vec![0xB3, 0x01, 0x01, OP_TRUE.to_u8()]),
    ));
    vectors.push(treasury_vector(
        "slot pushed as a script number",
        ScriptBuf::from_bytes(vec![0xB4, 0x51, OP_TRUE.to_u8()]),
    ));

    // --- deposit address --------------------------------------------------
    vectors.push(address_vector("address output", op_return(&[0xAB, 0xCD])));
    vectors.push(address_vector("empty address", op_return(&[])));
    vectors.push(address_vector(
        "two pushes is not an address",
        ScriptBuf::builder()
            .push_opcode(OP_RETURN)
            .push_slice([0xABu8, 0xCD])
            .push_slice([0xABu8, 0xCD])
            .into_script(),
    ));

    // --- coinbase messages ------------------------------------------------
    vectors.push(coinbase_vector(
        "m1 with a description",
        tagged(&M1ProposeSidechain::TAG, &[&[1u8][..], b"sidechain"].concat()),
    ));
    vectors.push(coinbase_vector(
        "m1 with an empty description",
        tagged(&M1ProposeSidechain::TAG, &[1u8]),
    ));
    vectors.push(coinbase_vector("m1 with no slot byte", tagged(&M1ProposeSidechain::TAG, &[])));

    // The tag both specifications disagree with the implementation about.
    vectors.push(coinbase_vector(
        "m2",
        tagged(&M2AckSidechain::TAG, &[&[3u8][..], &hash32].concat()),
    ));
    vectors.push(coinbase_vector(
        "m2 with the tag the specifications give",
        tagged(&[0xD6, 0xE1, 0xC5, 0xBF], &[&[3u8][..], &hash32].concat()),
    ));
    vectors.push(coinbase_vector(
        "m2 with a trailing byte",
        tagged(&M2AckSidechain::TAG, &[&[3u8][..], &hash32, &[0u8][..]].concat()),
    ));
    vectors.push(coinbase_vector(
        "m2 one byte short",
        tagged(&M2AckSidechain::TAG, &[&[3u8][..], &hash32[..31]].concat()),
    ));
    // Parsed as instructions, so any valid push encoding of the same payload
    // is the same message. An M8 is not.
    vectors.push(coinbase_vector(
        "m2 behind OP_PUSHDATA1",
        op_return_pushdata1(&[&M2AckSidechain::TAG[..], &[3u8], &hash32].concat()),
    ));

    vectors.push(coinbase_vector(
        "m3",
        tagged(&M3ProposeBundle::TAG, &[&[1u8][..], &hash32].concat()),
    ));

    for (name, body) in [
        ("m4 repeat previous", vec![0x00]),
        ("m4 repeat previous with a trailing byte", vec![0x00, 0x00]),
        ("m4 leading by fifty", vec![0x03]),
        ("m4 one byte votes", vec![0x01, 0x00, 0x07, 0xFE, 0xFF]),
        ("m4 one byte votes, empty", vec![0x01]),
        ("m4 two byte votes", vec![0x02, 0x01, 0x01, 0xFE, 0xFF]),
        ("m4 two byte votes, odd length", vec![0x02, 0x01, 0x01, 0xFE]),
        ("m4 unknown version", vec![0x04]),
        ("m4 with no version byte", vec![]),
    ] {
        vectors.push(coinbase_vector(name, tagged(&M4AckBundles::TAG, &body)));
    }

    vectors.push(coinbase_vector(
        "m7",
        tagged(&M7BmmAccept::TAG, &[&[2u8][..], &hash32].concat()),
    ));
    vectors.push(coinbase_vector("not a message", op_return(&[0xFF, 0xFF, 0xFF, 0xFF, 0x00])));
    vectors.push(coinbase_vector(
        "tag one byte short",
        op_return(&[0xD6, 0xE1, 0xC5]),
    ));

    // --- m8 ---------------------------------------------------------------
    let m8_payload = [&M8BmmRequest::TAG[..], &[1u8], &hash32, &other32].concat();
    vectors.push(m8_vector("m8", op_return(&m8_payload)));
    // The asymmetry that matters: the same payload behind OP_PUSHDATA1 is not
    // an M8, where the same trick on a coinbase message changes nothing.
    vectors.push(m8_vector("m8 behind OP_PUSHDATA1", op_return_pushdata1(&m8_payload)));
    vectors.push(m8_vector(
        "m8 with a trailing byte",
        ScriptBuf::from_bytes([op_return(&m8_payload).as_bytes(), &[0u8]].concat()),
    ));
    vectors.push(m8_vector(
        "m8 with the wrong tag",
        op_return(&[&[0x00u8, 0xBE, 0x00][..], &[1u8], &hash32, &other32].concat()),
    ));
    // An M7 is not an M8, whatever else is true of it.
    vectors.push(m8_vector(
        "m7 read as an m8",
        tagged(&M7BmmAccept::TAG, &[&[2u8][..], &hash32].concat()),
    ));

    // --- m6id -------------------------------------------------------------
    // The property the whole design rests on: the same payouts and the same
    // fee give the same M6ID however the treasury is spent, and changing the
    // fee changes it.
    vectors.push(m6id_vector("m6id, fee 1000", withdrawal(1, 6000, &[3000]), 10000));
    vectors.push(m6id_vector("m6id, fee 2000", withdrawal(1, 6000, &[3000]), 11000));
    vectors.push(m6id_vector("m6id, zero fee", withdrawal(1, 6000, &[4000]), 10000));
    vectors.push(m6id_vector("m6id, several payouts", withdrawal(1, 6000, &[3000, 400]), 10000));
    vectors.push(m6id_vector("m6id, no payouts", withdrawal(1, 6000, &[]), 10000));
    vectors.push(m6id_vector("m6id, another slot", withdrawal(200, 6000, &[3000]), 10000));
    vectors.push(m6id_vector(
        "m6id, outputs exceed the treasury",
        withdrawal(1, 6000, &[3000]),
        8999,
    ));

    let output = json!({
        "comment": [
            "Generated by contrib/drivechain-vectors from the reference implementation",
            "of BIP-300/301. Hashes are hex in internal byte order, as they appear on",
            "the wire, not the reversed order Bitcoin displays them in.",
            "A null result means the input is not that kind of message, which is not",
            "an error: BIP-300 says such an output is an ordinary script."
        ],
        "vectors": vectors,
    });
    println!("{}", serde_json::to_string_pretty(&output).unwrap());
}
