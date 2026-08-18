// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_VALIDATION_H
#define BITCOIN_DRIVECHAIN_VALIDATION_H

#include <drivechain/diff.h>
#include <drivechain/messages.h>
#include <drivechain/state.h>
#include <primitives/transaction.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

//! The BIP-300/301 rules a block must satisfy.
//!
//! Every rule here needs either accrued BIP-300 state or the value of a
//! treasury output being spent, so none of them can be hoisted into
//! CheckBlock: this is a connect-time rule set, which is also how the
//! reference implementation is arranged.
namespace drivechain {

//! Why a block is invalid under BIP-300/301.
//!
//! Each of these corresponds to a rejection the reference implementation makes.
//! A message that merely fails to parse is not here: BIP-300 says such an
//! output is an ordinary script and leaves the block valid.
enum class BlockError {
    //! Two M1s proposing the same sidechain for the same slot.
    DUPLICATE_M1,
    //! Two M2s acking for the same slot, whatever they ack.
    DUPLICATE_M2,
    //! More than one M4, of any content.
    DUPLICATE_M4,
    //! Two M7s accepting a block for the same slot.
    DUPLICATE_M7,
    //! The state does not contain what a diff built against it expects. A bug
    //! or corruption, not a bad block.
    STATE_MISMATCH,
};

//! A human-readable tag for a rejection, used as Core's block-invalid reason.
std::string BlockErrorString(BlockError error);

/**
 * The BIP-300/301 messages in a coinbase transaction, in output order.
 *
 * Collecting them is a step of its own because the duplicate rules are about
 * the coinbase as a whole: a second M4 invalidates the block whatever it says,
 * and a second M2 for a slot invalidates it whatever it acks. Those are decided
 * while reading the outputs, before any of the messages are acted on.
 *
 * BIP-300, "M1".."M4"; BIP-301, "M7 — BMM Accept".
 */
struct CoinbaseMessages {
    //! Each message with the index of the output that carried it. The index is
    //! what breaks ties in the canonical order of pending bundles.
    std::vector<std::pair<CoinbaseMessage, uint32_t>> messages;

    //! Whether an M4 was present. Its absence is not the same as an M4 that
    //! abstains everywhere: a block with no M4 abstains for every slot, which
    //! still has to be applied.
    bool has_m4{false};
};

/**
 * Read the BIP-300/301 messages out of a coinbase transaction.
 *
 * Outputs that are not well-formed messages are skipped rather than rejected.
 * Returns false, setting `error`, if the coinbase breaks one of the duplicate
 * rules.
 */
[[nodiscard]] bool CollectCoinbaseMessages(const CTransaction& coinbase, CoinbaseMessages& out, BlockError& error);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_VALIDATION_H
