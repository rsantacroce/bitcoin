// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_MESSAGES_H
#define BITCOIN_DRIVECHAIN_MESSAGES_H

#include <primitives/transaction_identifier.h>
#include <script/script.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

//! Wire formats for the BIP-300/301 messages.
//!
//! Everything here is a pure function over bytes: no chain state is consulted
//! and nothing can reject a block. Recognising a message and validating it are
//! deliberately separate, because the validation rules need the UTXO set and
//! accrued BIP-300 state that only ConnectBlock has.
//!
//! BIP-300: https://github.com/LayerTwo-Labs/bip300_bip301_specifications/blob/master/bip300.md
//! BIP-301: https://github.com/LayerTwo-Labs/bip300_bip301_specifications/blob/master/bip301.md
class CTransaction;

namespace drivechain {

//! A sidechain slot number. BIP-300 defines 256 slots, addressed by a single
//! unsigned byte, and a slot holds at most one sidechain.
using SlotNum = uint8_t;

//! OP_DRIVECHAIN, the opcode marking a sidechain treasury output.
//!
//! BIP-300 redefines OP_NOP5 for this purpose. Under current consensus OP_NOP5
//! is a no-op, so a treasury output is anyone-can-spend to a node that does not
//! enforce BIP-300 — which is what makes the proposal a soft fork, and why the
//! rules rejecting treasury spends are the peg itself.
//!
//! Which NOP to redefine is an open question with the specification authors;
//! nothing else in BIP-300 depends on the choice. Keeping the alias here rather
//! than in script.h means changing it touches one line.
static constexpr opcodetype OP_DRIVECHAIN{OP_NOP5};

//! Length of a treasury scriptPubKey, in bytes.
static constexpr size_t TREASURY_SCRIPT_SIZE{4};

/**
 * Build the scriptPubKey of the treasury output ("CTIP") for a sidechain slot.
 *
 * The script is exactly `OP_DRIVECHAIN OP_PUSHBYTES_1 <slot> OP_TRUE`. The slot
 * is a raw unsigned byte, not a script number: it is never encoded as
 * OP_1..OP_16, never sign-padded, and written as-is even above 127. The
 * trailing OP_TRUE is what keeps the redefinition a soft fork — without it,
 * slots 0 and 128 would leave the legacy interpreter with a false result.
 *
 * BIP-300, "OP_DRIVECHAIN".
 */
CScript TreasuryScript(SlotNum slot);

/**
 * If `script` is a treasury scriptPubKey, return the slot it names.
 *
 * The match is byte-exact and the script must end after OP_TRUE: a longer
 * script that merely begins this way is an ordinary script, not a treasury
 * output. Mirrors `parse_op_drivechain` in the reference implementation
 * (lib/messages.rs).
 *
 * BIP-300, "OP_DRIVECHAIN".
 */
std::optional<SlotNum> ParseTreasuryScript(const CScript& script);

/**
 * If `script` is exactly `OP_RETURN <push>`, return the pushed bytes.
 *
 * Two things in BIP-300/301 have this shape, and both are strict about it:
 *
 * - the address output an M5 deposit must place immediately after its treasury
 *   output, whose payload is an opaque sidechain address that enforcing nodes
 *   MUST treat as a meaningless byte array (BIP-300, "M5 — Deposit"); and
 * - the outer layer of every coinbase message, whose payload begins with a
 *   4-byte message tag (BIP-300 "M1".."M4", BIP-301 "M7").
 *
 * A script carrying anything after the push, or whose second instruction is an
 * opcode rather than a data push, is not either of those. Note that M8 is
 * *not* parsed this way; see ParseM8Request.
 */
std::optional<std::vector<unsigned char>> ParseOpReturnPayload(const CScript& script);

//! Length of a message tag, in bytes. M8 is the exception; see ParseM8Request.
static constexpr size_t MESSAGE_TAG_SIZE{4};

/** M1 — propose that the sidechain described by `description` take a slot.
 *
 *  BIP-300, "M1 — Propose Sidechain".
 */
struct M1ProposeSidechain {
    static constexpr std::array<unsigned char, MESSAGE_TAG_SIZE> TAG{0xD5, 0xE0, 0xC4, 0xAF};

    SlotNum slot;
    //! The sidechain description `D`, an opaque byte array. BIP-300 assigns it
    //! no structure: whatever a sidechain's authors agree it means is between
    //! them and their users, and consensus code parses none of it. Its length
    //! is bounded only by Bitcoin's own rules.
    std::vector<unsigned char> description;

    //! sha256d(D), the identifier an M2 votes for.
    uint256 ProposalId() const;
};

/** M2 — acknowledge a sidechain proposal.
 *
 *  BIP-300, "M2 — ACK Sidechain Proposal".
 */
struct M2AckSidechain {
    //! Spec divergence: both specifications give this tag as D6 E1 C5 BF; the
    //! reference implementation uses D6 E1 C5 DF (lib/messages.rs). One of the
    //! three sources has a wire-format bug and the question is open with the
    //! specification authors. Following the implementation, since that is what
    //! interoperating software has to match today.
    static constexpr std::array<unsigned char, MESSAGE_TAG_SIZE> TAG{0xD6, 0xE1, 0xC5, 0xDF};

    SlotNum slot;
    uint256 proposal_id;
};

/** M3 — propose a withdrawal bundle.
 *
 *  BIP-300, "M3 — Propose Bundle".
 */
struct M3ProposeBundle {
    static constexpr std::array<unsigned char, MESSAGE_TAG_SIZE> TAG{0xD4, 0x5A, 0xA9, 0x43};

    SlotNum slot;
    //! The blinded withdrawal's txid; see BlindM6 in drivechain/m6id.h.
    Txid m6id;
};

/** M4 — acknowledge withdrawal bundles, one vote per active sidechain slot.
 *
 *  BIP-300, "M4 — ACK Bundle(s)".
 */
struct M4AckBundles {
    static constexpr std::array<unsigned char, MESSAGE_TAG_SIZE> TAG{0xD7, 0x7D, 0x17, 0x76};

    enum class Version : uint8_t {
        //! Cast the votes the previous block's M4 resolved to. Carries no
        //! vote array.
        REPEAT_PREVIOUS = 0x00,
        VOTES_ONE_BYTE = 0x01,
        VOTES_TWO_BYTE = 0x02,
        //! Per slot, upvote a bundle leading every other by at least 50.
        //! Carries no vote array.
        UPVOTE_LEADING_BY_50 = 0x03,
    };

    static constexpr uint8_t ALARM_ONE_BYTE{0xFE};
    static constexpr uint8_t ABSTAIN_ONE_BYTE{0xFF};
    static constexpr uint16_t ALARM_TWO_BYTES{0xFFFE};
    static constexpr uint16_t ABSTAIN_TWO_BYTES{0xFFFF};

    Version version;
    //! Votes exactly as encoded, widened to 16 bits without translation. A
    //! one-byte 0xFF is stored as 0x00FF, *not* as ABSTAIN_TWO_BYTES, because
    //! the encoding has to survive parsing intact: BIP-300 rejects a block
    //! whose M4 uses VOTES_TWO_BYTE where one byte would have sufficed, which
    //! is only decidable from the raw values. Use NormalizedUpvotes() to read
    //! the votes; do not compare these against the two-byte sentinels.
    std::vector<uint16_t> upvotes;

    //! The votes with the sentinels translated into their two-byte form, so a
    //! caller can interpret both encodings the same way. Empty for the two
    //! versions that carry no vote array.
    std::vector<uint16_t> NormalizedUpvotes() const;
};

/** M7 — accept a blind merged mined sidechain block ("BMM Accept").
 *
 *  A miner may endorse at most one sidechain block hash per slot per block, and
 *  her endorsement is what decides which side:block is found.
 *
 *  BIP-301, "M7 — BMM Accept".
 */
struct M7BmmAccept {
    static constexpr std::array<unsigned char, MESSAGE_TAG_SIZE> TAG{0xD1, 0x61, 0x73, 0x68};

    SlotNum slot;
    //! The side:block hash, `h*` in the specification.
    uint256 sidechain_block_hash;
};

//! A message carried in an output of the coinbase transaction.
using CoinbaseMessage = std::variant<M1ProposeSidechain, M2AckSidechain, M3ProposeBundle, M4AckBundles, M7BmmAccept>;

/**
 * Parse a coinbase message from an output's scriptPubKey.
 *
 * A script that is not a well-formed message returns nullopt, which is not an
 * error: BIP-300 says such an output MUST be treated as an ordinary script and
 * the block stays valid. Malformed and absent are the same thing here.
 *
 * The caller must only apply this to outputs of the coinbase transaction. An
 * M1/M2/M3/M4-shaped output anywhere else MUST be ignored, and this function
 * has no way to tell the difference.
 *
 * BIP-300, "Transaction validation".
 */
std::optional<CoinbaseMessage> ParseCoinbaseMessage(const CScript& script);

/** M8 — offer payment for having a sidechain block blind merge mined ("BMM
 *  Request").
 *
 *  Carried by an ordinary transaction, not the coinbase. How the miner is paid
 *  is outside consensus: enforcing nodes interpret nothing in the transaction
 *  beyond the output at index 0.
 *
 *  BIP-301, "M8 — BMM Request".
 */
struct M8BmmRequest {
    //! Three bytes, where every other tag in the BIP-300/301 family is four.
    //! This is a fossil of a design in which a BMM request was a distinct
    //! transaction type carrying a critical-data field rather than an ordinary
    //! transaction with an OP_RETURN output, and not a constraint on any
    //! replacement encoding.
    static constexpr std::array<unsigned char, 3> TAG{0x00, 0xBF, 0x00};

    SlotNum slot;
    //! The side:block hash, `h*`, which must match an M7 in the same block.
    uint256 sidechain_block_hash;
    //! The hash of this block's parent, `P`, in internal byte order — directly
    //! comparable to CBlockHeader::hashPrevBlock. It confines a request to the
    //! single block it was written for, so a miner cannot hoard old requests
    //! and mine them later for side:blocks that can no longer connect.
    uint256 prev_main_block_hash;
};

//! Length of an M8 scriptPubKey, in bytes: OP_RETURN, OP_PUSHBYTES_68, and 68
//! bytes of payload.
static constexpr size_t M8_SCRIPT_SIZE{70};

/**
 * If `script` is an M8 request, parse it.
 *
 * Unlike every other message here, this is a fixed byte-prefix match rather
 * than a parse of script instructions: the script MUST begin literally
 * `6A 44` and end after `P`. An OP_PUSHDATA1 encoding of the same 68 payload
 * bytes is not an M8 at all. The asymmetry is inherited from the deployed
 * format rather than required by BIP-301, but it is consensus-visible, so it
 * is reproduced exactly.
 *
 * BIP-301, "M8 — BMM Request", and Appendix A item 5.
 */
std::optional<M8BmmRequest> ParseM8Request(const CScript& script);

/**
 * If `tx` carries an M8 request, parse it.
 *
 * Only the output at index 0 is examined; an M8-shaped output anywhere else in
 * the transaction is not a BMM request. Enforcing nodes MUST NOT interpret any
 * other part of the transaction.
 *
 * BIP-301, "M8 — BMM Request".
 */
std::optional<M8BmmRequest> ParseM8Request(const CTransaction& tx);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_MESSAGES_H
