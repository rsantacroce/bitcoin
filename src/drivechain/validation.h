// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_VALIDATION_H
#define BITCOIN_DRIVECHAIN_VALIDATION_H

#include <drivechain/diff.h>
#include <drivechain/messages.h>
#include <drivechain/params.h>
#include <drivechain/state.h>
#include <primitives/transaction.h>

#include <cstdint>
#include <optional>
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

/**
 * Handle an M1: propose that a sidechain occupy a slot.
 *
 * Returns nothing when the proposal is ignored, which happens when the same
 * (slot, description) already has an entry. That rule is what stops a miner
 * resetting an accumulated vote count by re-proposing: without it, any
 * proposal's progress could be wiped at will.
 *
 * An M1 cannot make a block invalid.
 *
 * BIP-300, "M1 — Propose Sidechain".
 */
std::optional<NewSidechainProposal> HandleM1(const M1ProposeSidechain& m1, const DrivechainState& state, int32_t height);

/**
 * Handle an M2: acknowledge a sidechain proposal, possibly activating it.
 *
 * Returns nothing when the M2 is ignored, which happens when no proposal
 * matches the (slot, description hash) it names, or when the proposal it names
 * was made in this very block. The second case matters: without it a miner
 * could seed a fresh proposal with a vote in the block that proposes it, and
 * BIP-300 requires the proposal to sit in an ancestor block.
 *
 * `state` must already carry the effect of the earlier messages in this
 * coinbase, which is how the same-block case is detected at all.
 *
 * An M2 cannot make a block invalid.
 *
 * BIP-300, "M2 — ACK Sidechain Proposal".
 */
std::optional<AckSidechainProposal> HandleM2(const M2AckSidechain& m2,
                                             const DrivechainState& state,
                                             const Thresholds& thresholds,
                                             int32_t height);

/**
 * Proposals that run out at this height.
 *
 * A proposal fails when its age exceeds the window it had, and -- following the
 * reference implementation rather than the specification -- also once it has
 * missed enough blocks that it can no longer reach the threshold inside the
 * window remaining. See Thresholds::ProposalFailed.
 *
 * BIP-300, "M2 — ACK Sidechain Proposal", Activation.
 */
FailedProposals CollectFailedProposals(const DrivechainState& state, const Thresholds& thresholds, int32_t height);

/**
 * Withdrawal bundles that age out at this height.
 *
 * An expired bundle is removed and pays nothing. Each is recorded with the
 * position it held, because an M4 votes for a bundle by position and undo has
 * to put it back where it was.
 *
 * BIP-300, "D2 — The Withdrawal List".
 */
FailedBundles CollectFailedBundles(const DrivechainState& state, const Thresholds& thresholds, int32_t height);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_VALIDATION_H
