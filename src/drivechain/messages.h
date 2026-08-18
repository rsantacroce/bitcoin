// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_MESSAGES_H
#define BITCOIN_DRIVECHAIN_MESSAGES_H

#include <script/script.h>

#include <cstdint>
#include <optional>
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

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_MESSAGES_H
