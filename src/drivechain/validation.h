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
#include <uint256.h>

#include <cstdint>
#include <map>
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
    //! An M3 proposing a bundle for a slot that holds no sidechain.
    M3_INACTIVE_SIDECHAIN,
    //! An M3 proposing a bundle that is already pending. Allowing it would
    //! reset the bundle's ack count and its clock.
    M3_BUNDLE_ALREADY_PENDING,
    //! An M4 using the two-byte encoding where one byte would have done.
    M4_TWO_BYTES_WITHIN_BYTE_RANGE,
    //! An M4 whose vote array does not have one element per active slot.
    M4_VOTE_COUNT_MISMATCH,
    //! An M4 selecting a bundle position that does not exist in that slot.
    M4_BUNDLE_INDEX_OUT_OF_RANGE,
    //! A transaction that satisfies both the deposit and the withdrawal rules.
    AMBIGUOUS_TREASURY_TX,
    //! A deposit without the address output that must follow the treasury.
    MISSING_DEPOSIT_ADDRESS,
    //! More than one treasury output for a slot in one transaction.
    MULTIPLE_TREASURY_OUTPUTS,
    //! A treasury output created for a slot whose treasury is still unspent.
    OLD_CTIP_UNSPENT,
    //! A treasury spent without creating a replacement for that slot.
    TREASURY_SPENT_WITHOUT_NEW_CTIP,
    //! A treasury moved to an equal value: neither deposit nor withdrawal.
    ZERO_VALUE_CHANGE,
    //! A withdrawal with more or fewer than one input.
    M6_INPUT_COUNT,
    //! A withdrawal whose treasury output is not at vout[0].
    M6_TREASURY_OUTPUT_INDEX,
    //! More than one withdrawal in a single transaction.
    M6_TREASURY_OUTPUT_COUNT,
    //! A withdrawal whose M6ID matches no pending bundle for the slot.
    M6_UNKNOWN_BUNDLE,
    //! A withdrawal whose bundle has not been approved by enough votes.
    M6_INSUFFICIENT_VOTES,
    //! An M8 with no matching M7 in the same block's coinbase.
    BMM_REQUEST_NOT_ACCEPTED,
    //! An M8 written for a different parent block.
    BMM_REQUEST_EXPIRED,
    //! More than one valid M8 for the same slot in one block.
    MULTIPLE_BMM_REQUESTS,
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

    //! The side:block hash each slot's M7 endorsed, at most one per slot.
    std::map<SlotNum, uint256> bmm_accepts;

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

/**
 * Handle an M3: propose a withdrawal bundle.
 *
 * Unlike M1 and M2, an M3 can make the block invalid: for a slot that holds no
 * sidechain, and for a bundle that is already pending. The second is what stops
 * a re-proposal resetting an accumulated ack count and expiry clock.
 *
 * A bundle that is no longer pending -- paid out or expired -- may be proposed
 * again, starting a fresh count. BIP-300 deliberately does not blacklist it: a
 * bundle that expired through miner apathy would otherwise strand its
 * withdrawals forever, and one that was paid out cannot be paid twice, since
 * the treasury output it spent no longer exists.
 *
 * BIP-300, "M3 — Propose Bundle".
 */
[[nodiscard]] bool HandleM3(const M3ProposeBundle& m3, const DrivechainState& state, ProposeBundle& out, BlockError& error);

//! The margin a bundle must lead every rival by for UPVOTE_LEADING_BY_50.
static constexpr uint16_t LEADING_BY_50_MARGIN{50};

/**
 * Handle an M4: cast this block's withdrawal votes.
 *
 * Votes are positional twice over. `A[i]` is the vote for the i'th active slot
 * in ascending slot order -- active slots may be sparse, so array positions are
 * not slot numbers -- and within a slot the vote selects a bundle by its
 * position in that slot's list.
 *
 * Three ways to make the block invalid: an array that does not have exactly one
 * element per active slot, a vote selecting a bundle position that does not
 * exist, and the two-byte encoding where no element needed it. All three are
 * raised as open questions by the LayerTwo-Labs draft, and all three are kept
 * as the reference implementation enforces them.
 *
 * `previous` is the previous block's resolved votes, which REPEAT_PREVIOUS
 * replays. Pass an empty value when the previous block had no M4, or none that
 * resolved to anything; repeats then cast no votes.
 *
 * BIP-300, "M4 — ACK Bundle(s)".
 */
[[nodiscard]] bool HandleM4(const M4AckBundles& m4,
                            const DrivechainState& state,
                            const AckBundles& previous,
                            AckBundles& out,
                            BlockError& error);

/**
 * Handle a transaction that may move a sidechain treasury: M5 or M6.
 *
 * The two are told apart by arithmetic rather than by a tag. A transaction that
 * creates a treasury output worth more than the one it spent is a deposit; one
 * worth less is a withdrawal; one worth the same is neither, and invalid.
 *
 * `out` is left empty for a transaction that touches no treasury, which is
 * almost all of them.
 *
 * These are the rules that hold the peg. `OP_DRIVECHAIN` evaluates true with an
 * empty scriptSig, so to the script interpreter a treasury output is
 * anyone-can-spend; nothing but these block-level rules stops it being taken.
 *
 * BIP-300, "M5 — Deposit", "M6 — Withdrawal", "Transaction validation".
 */
[[nodiscard]] bool HandleTreasuryTx(const CTransaction& tx,
                                    const DrivechainState& state,
                                    const Thresholds& thresholds,
                                    std::optional<TxDiff>& out,
                                    BlockError& error);

/**
 * Handle a transaction that may be an M8 blind-merged-mining request.
 *
 * `slot` is set when the transaction is a valid request, and left empty when it
 * is not a request at all -- which is almost every transaction. A request that
 * is a request but not a valid one makes the block invalid.
 *
 * `accepted` is the set of side:block hashes this block's coinbase endorsed.
 * Pass nullptr when there is no coinbase to check against, as when judging a
 * transaction for the mempool: only the expiry rule can be decided then, since
 * the M7 that would accept the request does not exist yet.
 *
 * BIP-301, "M8 — BMM Request", Validation.
 */
[[nodiscard]] bool HandleM8(const CTransaction& tx,
                            const std::map<SlotNum, uint256>* accepted,
                            const uint256& parent_hash,
                            std::optional<SlotNum>& slot,
                            BlockError& error);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_VALIDATION_H
