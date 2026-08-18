// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/validation.h>

#include <drivechain/m6id.h>
#include <drivechain/messages.h>
#include <drivechain/params.h>
#include <drivechain/state.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>
#include <set>
#include <string>
#include <variant>

namespace drivechain {
namespace {
//! Bundles in a slot that would actually lose a vote, meaning those above zero.
//!
//! Recording only these is what keeps apply and undo exact inverses: a bundle
//! already at zero stays at zero when downvoted, so undo must not hand it a
//! vote back.
std::vector<Txid> Downvoted(const PendingWithdrawals& pending, const Txid* except)
{
    std::vector<Txid> downvoted;
    for (const PendingWithdrawal& bundle : pending) {
        if (bundle.vote_count == 0) continue;
        if (except != nullptr && bundle.m6id == *except) continue;
        downvoted.push_back(bundle.m6id);
    }
    return downvoted;
}

//! Build the upvote action for a bundle, or nothing if it cannot take a vote.
std::optional<AckBundles::Action> Upvote(const PendingWithdrawals& pending, const PendingWithdrawal& target)
{
    // A saturated count silently casts no vote rather than overflowing.
    if (target.vote_count == std::numeric_limits<uint16_t>::max()) return std::nullopt;
    return AckBundles::Action{
        .kind = AckBundles::Action::Kind::UPVOTE,
        .upvoted = target.m6id,
        .downvoted = Downvoted(pending, &target.m6id),
    };
}

//! Resolve a vote array: one element per active slot, in ascending slot order.
bool ResolveVotes(const std::vector<uint16_t>& votes, const DrivechainState& state, AckBundles& out, BlockError& error)
{
    const std::vector<SlotNum> active{state.ActiveSlots()};
    // Spec divergence: BIP-300 invalidates a block whose vote array is longer
    // than the active-slot vector. The reference implementation requires the
    // two to be equal, so a short array is rejected too.
    if (votes.size() != active.size()) {
        error = BlockError::M4_VOTE_COUNT_MISMATCH;
        return false;
    }

    for (size_t i{0}; i < votes.size(); ++i) {
        const SlotNum slot{active[i]};
        const uint16_t vote{votes[i]};
        if (vote == M4AckBundles::ABSTAIN_TWO_BYTES) continue;

        const PendingWithdrawals* pending{state.GetPendingWithdrawals(slot)};
        if (pending == nullptr) {
            error = BlockError::STATE_MISMATCH;
            return false;
        }

        if (vote == M4AckBundles::ALARM_TWO_BYTES) {
            std::vector<Txid> downvoted{Downvoted(*pending, nullptr)};
            // An alarm over a slot where nothing can lose a vote does nothing.
            if (downvoted.empty()) continue;
            out.actions[slot] = AckBundles::Action{
                .kind = AckBundles::Action::Kind::ALARM,
                .downvoted = std::move(downvoted),
            };
            continue;
        }

        if (vote >= pending->size()) {
            error = BlockError::M4_BUNDLE_INDEX_OUT_OF_RANGE;
            return false;
        }
        if (const auto action{Upvote(*pending, (*pending)[vote])}) {
            out.actions[slot] = *action;
        }
    }
    return true;
}

//! Per slot, upvote a bundle leading every rival by at least the margin.
void ResolveLeadingBy50(const DrivechainState& state, AckBundles& out)
{
    for (const SlotNum slot : state.ActiveSlots()) {
        const PendingWithdrawals* pending{state.GetPendingWithdrawals(slot)};
        if (pending == nullptr || pending->empty()) continue;

        const PendingWithdrawal* leader{&pending->front()};
        uint16_t runner_up{0};
        for (const PendingWithdrawal& bundle : *pending) {
            if (bundle.vote_count > leader->vote_count) {
                runner_up = leader->vote_count;
                leader = &bundle;
            } else if (&bundle != leader && bundle.vote_count > runner_up) {
                runner_up = bundle.vote_count;
            }
        }
        // A sole bundle leads an implicit zero-vote rival. A tie for the lead
        // leaves a margin of zero, so ties never upvote and the result does not
        // depend on which of the tied bundles is called the leader.
        if (leader->vote_count - runner_up < LEADING_BY_50_MARGIN) continue;
        if (const auto action{Upvote(*pending, *leader)}) {
            out.actions[slot] = *action;
        }
    }
}

//! Replay the previous block's resolved votes against the current state.
void ResolveRepeatPrevious(const AckBundles& previous, const DrivechainState& state, AckBundles& out)
{
    for (const auto& [slot, action] : previous.actions) {
        const PendingWithdrawals* pending{state.GetPendingWithdrawals(slot)};
        if (pending == nullptr) continue;

        if (action.kind == AckBundles::Action::Kind::ALARM) {
            // An alarm applies to whatever is pending now, so the set of
            // bundles that lose a vote is recomputed rather than replayed.
            std::vector<Txid> downvoted{Downvoted(*pending, nullptr)};
            if (downvoted.empty()) continue;
            out.actions[slot] = AckBundles::Action{
                .kind = AckBundles::Action::Kind::ALARM,
                .downvoted = std::move(downvoted),
            };
            continue;
        }

        // A repeated upvote naming a bundle that is no longer pending casts no
        // vote in that slot, and must not invalidate the block.
        const auto target{std::find_if(pending->begin(), pending->end(),
                                       [&action](const PendingWithdrawal& bundle) {
                                           return bundle.m6id == action.upvoted;
                                       })};
        if (target == pending->end()) continue;
        if (const auto repeated{Upvote(*pending, *target)}) {
            out.actions[slot] = *repeated;
        }
    }
}
} // namespace

std::string BlockErrorString(BlockError error)
{
    switch (error) {
    case BlockError::DUPLICATE_M1: return "bad-drivechain-duplicate-m1";
    case BlockError::DUPLICATE_M2: return "bad-drivechain-duplicate-m2";
    case BlockError::DUPLICATE_M4: return "bad-drivechain-duplicate-m4";
    case BlockError::DUPLICATE_M7: return "bad-drivechain-duplicate-m7";
    case BlockError::M3_INACTIVE_SIDECHAIN: return "bad-drivechain-m3-inactive-sidechain";
    case BlockError::M3_BUNDLE_ALREADY_PENDING: return "bad-drivechain-m3-bundle-already-pending";
    case BlockError::M4_TWO_BYTES_WITHIN_BYTE_RANGE: return "bad-drivechain-m4-two-bytes-unnecessary";
    case BlockError::M4_VOTE_COUNT_MISMATCH: return "bad-drivechain-m4-vote-count";
    case BlockError::M4_BUNDLE_INDEX_OUT_OF_RANGE: return "bad-drivechain-m4-bundle-index";
    case BlockError::AMBIGUOUS_TREASURY_TX: return "bad-drivechain-ambiguous-treasury-tx";
    case BlockError::MISSING_DEPOSIT_ADDRESS: return "bad-drivechain-missing-deposit-address";
    case BlockError::MULTIPLE_TREASURY_OUTPUTS: return "bad-drivechain-multiple-treasury-outputs";
    case BlockError::OLD_CTIP_UNSPENT: return "bad-drivechain-old-treasury-unspent";
    case BlockError::TREASURY_SPENT_WITHOUT_NEW_CTIP: return "bad-drivechain-treasury-spent-without-replacement";
    case BlockError::ZERO_VALUE_CHANGE: return "bad-drivechain-zero-value-change";
    case BlockError::M6_INPUT_COUNT: return "bad-drivechain-m6-input-count";
    case BlockError::M6_TREASURY_OUTPUT_INDEX: return "bad-drivechain-m6-treasury-output-index";
    case BlockError::M6_TREASURY_OUTPUT_COUNT: return "bad-drivechain-m6-treasury-output-count";
    case BlockError::M6_UNKNOWN_BUNDLE: return "bad-drivechain-m6-unknown-bundle";
    case BlockError::M6_INSUFFICIENT_VOTES: return "bad-drivechain-m6-insufficient-votes";
    case BlockError::BMM_REQUEST_NOT_ACCEPTED: return "bad-drivechain-bmm-request-not-accepted";
    case BlockError::BMM_REQUEST_EXPIRED: return "bad-drivechain-bmm-request-expired";
    case BlockError::MULTIPLE_BMM_REQUESTS: return "bad-drivechain-multiple-bmm-requests";
    case BlockError::STATE_MISMATCH: return "drivechain-state-mismatch";
    }
    return "bad-drivechain-unknown";
}

bool CollectCoinbaseMessages(const CTransaction& coinbase, CoinbaseMessages& out, BlockError& error)
{
    out = CoinbaseMessages{};

    // The duplicate rules are not all the same shape, and the differences are
    // deliberate. An M1 is a duplicate only of an identical proposal, since a
    // coinbase may legitimately propose two different sidechains for one slot.
    // An M2 is a duplicate of any other M2 for the same slot, whatever it acks,
    // because a slot gets one vote per block. An M4 is a duplicate of any other
    // M4 at all. An M7 is a duplicate of any other M7 for the same slot.
    std::set<SidechainProposalId> proposed;
    std::set<SlotNum> acked_slots;

    for (uint32_t vout{0}; vout < coinbase.vout.size(); ++vout) {
        const std::optional<CoinbaseMessage> message{ParseCoinbaseMessage(coinbase.vout[vout].scriptPubKey)};
        // Not a message: an ordinary output, and the block stays valid.
        if (!message) continue;

        if (const auto* m1{std::get_if<M1ProposeSidechain>(&*message)}) {
            const SidechainProposalId id{.slot = m1->slot, .description_hash = m1->ProposalId()};
            if (!proposed.insert(id).second) {
                error = BlockError::DUPLICATE_M1;
                return false;
            }
        } else if (const auto* m2{std::get_if<M2AckSidechain>(&*message)}) {
            if (!acked_slots.insert(m2->slot).second) {
                error = BlockError::DUPLICATE_M2;
                return false;
            }
        } else if (std::holds_alternative<M4AckBundles>(*message)) {
            if (out.has_m4) {
                error = BlockError::DUPLICATE_M4;
                return false;
            }
            out.has_m4 = true;
        } else if (const auto* m7{std::get_if<M7BmmAccept>(&*message)}) {
            if (!out.bmm_accepts.emplace(m7->slot, m7->sidechain_block_hash).second) {
                error = BlockError::DUPLICATE_M7;
                return false;
            }
        }
        // M3 has no duplicate rule of its own here. Two M3s naming the same
        // bundle for the same slot are still rejected, by the rule that a
        // bundle already pending cannot be proposed again -- the first one
        // makes it pending, and the second one then breaks that rule.

        out.messages.emplace_back(*message, vout);
    }

    return true;
}

std::optional<NewSidechainProposal> HandleM1(const M1ProposeSidechain& m1, const DrivechainState& state, int32_t height)
{
    Sidechain sidechain;
    sidechain.slot = m1.slot;
    sidechain.description = m1.description;
    sidechain.vote_count = 0;
    sidechain.proposal_height = height;
    sidechain.activation_height = NO_HEIGHT;

    // Already proposed: ignore rather than overwrite. The description hash is
    // part of the identity, so an entry with this id has the same description,
    // and the only thing overwriting could change is the vote count -- which is
    // exactly what must not be resettable.
    if (state.FindProposal(sidechain.Id()) != nullptr) return std::nullopt;

    return NewSidechainProposal{.sidechain = sidechain};
}

std::optional<AckSidechainProposal> HandleM2(const M2AckSidechain& m2,
                                             const DrivechainState& state,
                                             const Thresholds& thresholds,
                                             int32_t height)
{
    const SidechainProposalId id{.slot = m2.slot, .description_hash = m2.proposal_id};
    const Sidechain* proposal{state.FindProposal(id)};
    // No matching proposal, or one whose slot does not match: ignored. Note
    // that the slot is part of the id, so a mismatch simply fails to find it.
    if (proposal == nullptr) return std::nullopt;

    // BIP-300 counts an ack only once the proposal exists in an ancestor
    // block. Within one coinbase the M1 is applied before any later M2, so a
    // proposal acked in the block that proposed it still has this height.
    if (proposal->proposal_height == height) return std::nullopt;

    const uint16_t vote_count{static_cast<uint16_t>(proposal->vote_count + 1)};
    // Saturating: a proposal held over from a previous sync may sit above the
    // current height, and an age that wrapped would silently never activate.
    const int32_t age{std::max(0, height - proposal->proposal_height)};

    const Sidechain* incumbent{state.FindActiveSidechain(m2.slot)};
    const bool slot_is_used{incumbent != nullptr};

    AckSidechainProposal ack;
    ack.id = id;
    if (!thresholds.ProposalActivates(vote_count, age, slot_is_used)) {
        ack.effect = AckSidechainProposal::Effect::NO_ACTIVATION;
    } else if (slot_is_used) {
        // The displaced sidechain has to travel with the diff: undo has no
        // other way to put it back.
        ack.effect = AckSidechainProposal::Effect::REPLACE_ACTIVE;
        ack.replaced = *incumbent;
    } else {
        ack.effect = AckSidechainProposal::Effect::SLOT_ACTIVATION;
    }
    return ack;
}

FailedProposals CollectFailedProposals(const DrivechainState& state, const Thresholds& thresholds, int32_t height)
{
    FailedProposals failed;
    for (const auto& [id, proposal] : state.Proposals()) {
        const int32_t age{std::max(0, height - proposal.proposal_height)};
        if (thresholds.ProposalFailed(proposal.vote_count, age, state.IsActive(proposal.slot))) {
            failed.removed.push_back(proposal);
        }
    }
    return failed;
}

FailedBundles CollectFailedBundles(const DrivechainState& state, const Thresholds& thresholds, int32_t height)
{
    FailedBundles failed;
    for (const SlotNum slot : state.ActiveSlots()) {
        const PendingWithdrawals* pending{state.GetPendingWithdrawals(slot)};
        // Every active slot has a withdrawal list; ActivateSidechain creates
        // one and nothing else removes it.
        if (pending == nullptr) continue;

        for (uint32_t index{0}; index < pending->size(); ++index) {
            const int32_t age{std::max(0, height - (*pending)[index].proposal_height)};
            if (thresholds.BundleExpired(age)) {
                failed.removed[slot][index] = (*pending)[index];
            }
        }
    }
    return failed;
}

bool HandleM3(const M3ProposeBundle& m3, const DrivechainState& state, ProposeBundle& out, BlockError& error)
{
    const PendingWithdrawals* pending{state.GetPendingWithdrawals(m3.slot)};
    if (pending == nullptr) {
        error = BlockError::M3_INACTIVE_SIDECHAIN;
        return false;
    }

    for (const PendingWithdrawal& bundle : *pending) {
        if (bundle.m6id == m3.m6id) {
            error = BlockError::M3_BUNDLE_ALREADY_PENDING;
            return false;
        }
    }

    out = ProposeBundle{.slot = m3.slot, .m6id = m3.m6id};
    return true;
}

bool HandleM4(const M4AckBundles& m4,
              const DrivechainState& state,
              const AckBundles& previous,
              AckBundles& out,
              BlockError& error)
{
    out = AckBundles{};

    switch (m4.version) {
    case M4AckBundles::Version::REPEAT_PREVIOUS:
        // Replays the votes the previous block resolved to, which chains
        // transitively. Repeating an UPVOTE_LEADING_BY_50 therefore replays
        // what it decided rather than re-deciding it against current counts.
        ResolveRepeatPrevious(previous, state, out);
        return true;
    case M4AckBundles::Version::UPVOTE_LEADING_BY_50:
        ResolveLeadingBy50(state, out);
        return true;
    case M4AckBundles::Version::VOTES_ONE_BYTE:
        return ResolveVotes(m4.NormalizedUpvotes(), state, out, error);
    case M4AckBundles::Version::VOTES_TWO_BYTE:
        // BIP-300 rejects the two-byte encoding where one byte would have
        // sufficed. Decidable only from the raw values, which is why the
        // message keeps them unnormalized.
        if (std::ranges::all_of(m4.upvotes, [](uint16_t vote) { return vote <= 253; })) {
            error = BlockError::M4_TWO_BYTES_WITHIN_BYTE_RANGE;
            return false;
        }
        return ResolveVotes(m4.NormalizedUpvotes(), state, out, error);
    }

    error = BlockError::STATE_MISMATCH;
    return false;
}

bool HandleTreasuryTx(const CTransaction& tx,
                      const DrivechainState& state,
                      const Thresholds& thresholds,
                      std::optional<TxDiff>& out,
                      BlockError& error)
{
    out.reset();

    // Which treasuries this transaction spends. A slot keeps its treasury even
    // if its sidechain stops being active, so every pointer is considered, not
    // only those of active slots.
    std::map<SlotNum, CAmount> spent;
    for (const CTxIn& input : tx.vin) {
        for (const auto& [slot, ctip] : state.Ctips()) {
            if (ctip.outpoint == input.prevout) spent[slot] = ctip.value;
        }
    }

    // Which treasuries it creates. Iteration is ordered by slot, so when a
    // transaction breaks several rules at once the one reported is the same on
    // every node -- not that validity depends on it, but a reject reason that
    // varied by node would be miserable to debug.
    std::map<SlotNum, std::pair<uint32_t, Ctip>> created;
    for (uint32_t vout{0}; vout < tx.vout.size(); ++vout) {
        const std::optional<SlotNum> slot{ParseTreasuryScript(tx.vout[vout].scriptPubKey)};
        if (!slot) continue;

        // An OP_DRIVECHAIN output designates a treasury only for a slot that
        // holds an active sidechain. For any other slot it is an ordinary
        // anyone-can-spend output and the block is valid: without this guard a
        // zero-value output naming a nonexistent sidechain would be read as a
        // treasury moving to zero and reject a perfectly good block.
        if (!state.IsActive(*slot)) continue;

        const Ctip ctip{.outpoint = COutPoint{tx.GetHash(), vout}, .value = tx.vout[vout].nValue};
        if (!created.emplace(*slot, std::make_pair(vout, ctip)).second) {
            error = BlockError::MULTIPLE_TREASURY_OUTPUTS;
            return false;
        }
    }

    // Spending a treasury without putting one back would drain the slot, and
    // the script interpreter will not stop it.
    for (const auto& [slot, value] : spent) {
        if (created.count(slot) == 0) {
            error = BlockError::TREASURY_SPENT_WITHOUT_NEW_CTIP;
            return false;
        }
    }

    M5Diff deposits;
    std::optional<M6Diff> withdrawal;

    for (const auto& [slot, entry] : created) {
        const auto& [vout, new_ctip] = entry;

        CAmount previous_value{0};
        TreasuryChange change{.new_ctip = new_ctip};
        if (const Ctip* existing{state.GetCtip(slot)}) {
            // A slot has at most one treasury, so creating a second without
            // spending the first would be two at once -- the invariant the
            // whole design rests on.
            const auto it{spent.find(slot)};
            if (it == spent.end()) {
                error = BlockError::OLD_CTIP_UNSPENT;
                return false;
            }
            previous_value = it->second;
            change.had_previous = true;
            change.previous = *existing;
        }

        if (new_ctip.value == previous_value) {
            // Neither a deposit nor a withdrawal.
            error = BlockError::ZERO_VALUE_CHANGE;
            return false;
        }

        if (new_ctip.value > previous_value) {
            // A deposit. BIP-300 requires the opaque sidechain address in an
            // OP_RETURN immediately after the treasury output; without it the
            // deposit cannot be attributed to any account on the sidechain.
            if (vout + 1 >= tx.vout.size() || !ParseOpReturnPayload(tx.vout[vout + 1].scriptPubKey)) {
                error = BlockError::MISSING_DEPOSIT_ADDRESS;
                return false;
            }
            if (withdrawal) {
                error = BlockError::AMBIGUOUS_TREASURY_TX;
                return false;
            }
            deposits.ctips[slot] = change;
            continue;
        }

        // A withdrawal. The structural checks come before the ambiguity check,
        // matching the order the reference implementation uses: which rule a
        // transaction breaking several at once is rejected under does not
        // affect validity, but keeping the order identical keeps a
        // differential harness from reporting disagreements that are not.
        if (tx.vin.size() != 1) {
            error = BlockError::M6_INPUT_COUNT;
            return false;
        }
        if (vout != 0) {
            error = BlockError::M6_TREASURY_OUTPUT_INDEX;
            return false;
        }

        M6Error m6_error{};
        const std::optional<BlindedM6> blinded{BlindM6(tx, previous_value, m6_error)};
        if (!blinded) {
            // The structural checks above cover every way BlindM6 can fail
            // except arithmetic, which means the outputs claim more than the
            // treasury held.
            error = BlockError::M6_UNKNOWN_BUNDLE;
            return false;
        }

        const PendingWithdrawals* pending{state.GetPendingWithdrawals(slot)};
        if (pending == nullptr) {
            error = BlockError::STATE_MISMATCH;
            return false;
        }
        uint32_t index{0};
        const PendingWithdrawal* bundle{nullptr};
        for (uint32_t i{0}; i < pending->size(); ++i) {
            if ((*pending)[i].m6id == blinded->m6id) {
                index = i;
                bundle = &(*pending)[i];
                break;
            }
        }
        if (bundle == nullptr) {
            error = BlockError::M6_UNKNOWN_BUNDLE;
            return false;
        }
        if (!thresholds.BundleIsPayable(bundle->vote_count)) {
            error = BlockError::M6_INSUFFICIENT_VOTES;
            return false;
        }

        if (!deposits.ctips.empty()) {
            error = BlockError::AMBIGUOUS_TREASURY_TX;
            return false;
        }
        if (withdrawal) {
            error = BlockError::M6_TREASURY_OUTPUT_COUNT;
            return false;
        }

        withdrawal = M6Diff{
            .slot = slot,
            .ctip = change,
            .removed_index = index,
            .removed = *bundle,
        };
    }

    if (withdrawal) {
        out = *withdrawal;
    } else if (!deposits.ctips.empty()) {
        out = deposits;
    }
    return true;
}

bool HandleM8(const CTransaction& tx,
              const std::map<SlotNum, uint256>* accepted,
              const uint256& parent_hash,
              std::optional<SlotNum>& slot,
              BlockError& error)
{
    slot.reset();

    const std::optional<M8BmmRequest> request{ParseM8Request(tx)};
    if (!request) return true;

    if (accepted != nullptr) {
        // A miner can only collect on a request she accepted. Without this
        // rule she could take the payment and mine someone else's side:block,
        // or none at all.
        const auto it{accepted->find(request->slot)};
        if (it == accepted->end() || it->second != request->sidechain_block_hash) {
            error = BlockError::BMM_REQUEST_NOT_ACCEPTED;
            return false;
        }
    }

    // A request is written for one parent and expires with it. Without this a
    // miner could hoard old requests and mine them later, collecting payment
    // for side:blocks that can no longer be connected.
    if (request->prev_main_block_hash != parent_hash) {
        error = BlockError::BMM_REQUEST_EXPIRED;
        return false;
    }

    slot = request->slot;
    return true;
}

} // namespace drivechain
