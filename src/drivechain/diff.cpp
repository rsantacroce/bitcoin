// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/diff.h>

#include <drivechain/messages.h>
#include <drivechain/state.h>

#include <algorithm>
#include <cstdint>
#include <variant>
#include <vector>

namespace drivechain {
namespace {
PendingWithdrawal* FindBundle(PendingWithdrawals& pending, const Txid& m6id)
{
    const auto it{std::find_if(pending.begin(), pending.end(),
                               [&m6id](const PendingWithdrawal& bundle) { return bundle.m6id == m6id; })};
    return it == pending.end() ? nullptr : &*it;
}

//! Give back a vote to each of `downvoted` that is still pending.
//!
//! A bundle named here may have since been paid out or aged away, in which case
//! there is nothing to restore; that is not an inconsistency.
void RestoreDownvotes(PendingWithdrawals& pending, const std::vector<Txid>& downvoted)
{
    for (const Txid& m6id : downvoted) {
        if (PendingWithdrawal* bundle{FindBundle(pending, m6id)}) {
            ++bundle->vote_count;
        }
    }
}

bool ApplyMsg(const NewSidechainProposal& diff, DrivechainState& state, int32_t)
{
    state.PutProposal(diff.sidechain);
    return true;
}

bool UndoMsg(const NewSidechainProposal& diff, DrivechainState& state, UndoError& error)
{
    if (!state.EraseProposal(diff.sidechain.Id())) {
        error = UndoError::STATE_MISMATCH;
        return false;
    }
    return true;
}

bool ApplyMsg(const AckSidechainProposal& diff, DrivechainState& state, int32_t height)
{
    const Sidechain* proposal{state.FindProposal(diff.id)};
    if (proposal == nullptr) return false;

    Sidechain acked{*proposal};
    ++acked.vote_count;

    if (diff.effect == AckSidechainProposal::Effect::NO_ACTIVATION) {
        state.PutProposal(acked);
        return true;
    }

    // Activation moves the entry out of the proposal list and into the slot.
    acked.activation_height = height;
    state.ActivateSidechain(acked);
    state.EraseProposal(diff.id);
    return true;
}

bool UndoMsg(const AckSidechainProposal& diff, DrivechainState& state, UndoError& error)
{
    Sidechain acked;
    switch (diff.effect) {
    case AckSidechainProposal::Effect::NO_ACTIVATION: {
        const Sidechain* proposal{state.FindProposal(diff.id)};
        if (proposal == nullptr) {
            error = UndoError::STATE_MISMATCH;
            return false;
        }
        acked = *proposal;
        break;
    }
    case AckSidechainProposal::Effect::REPLACE_ACTIVE: {
        const Sidechain* active{state.FindActiveSidechain(diff.id.slot)};
        if (active == nullptr) {
            error = UndoError::STATE_MISMATCH;
            return false;
        }
        acked = *active;
        // Put the displaced sidechain back. Its withdrawal list was never
        // taken away, so activating it again restores the slot entirely.
        state.ActivateSidechain(diff.replaced);
        acked.activation_height = NO_HEIGHT;
        break;
    }
    case AckSidechainProposal::Effect::SLOT_ACTIVATION: {
        const Sidechain* active{state.FindActiveSidechain(diff.id.slot)};
        if (active == nullptr) {
            error = UndoError::STATE_MISMATCH;
            return false;
        }
        acked = *active;
        state.DeactivateSidechain(diff.id.slot);
        acked.activation_height = NO_HEIGHT;
        break;
    }
    }

    if (acked.vote_count == 0) {
        error = UndoError::SIDECHAIN_VOTE_COUNT_UNDERFLOW;
        return false;
    }
    --acked.vote_count;
    state.PutProposal(acked);
    return true;
}

bool ApplyMsg(const ProposeBundle& diff, DrivechainState& state, int32_t height)
{
    PendingWithdrawals* pending{state.ModifyPendingWithdrawals(diff.slot)};
    if (pending == nullptr) return false;

    // BIP-300 M3: being proposed counts as the bundle's first upvote.
    pending->push_back(PendingWithdrawal{.m6id = diff.m6id, .vote_count = 1, .proposal_height = height});
    return true;
}

bool UndoMsg(const ProposeBundle& diff, DrivechainState& state, UndoError& error)
{
    PendingWithdrawals* pending{state.ModifyPendingWithdrawals(diff.slot)};
    if (pending == nullptr) {
        error = UndoError::STATE_MISMATCH;
        return false;
    }
    const auto it{std::find_if(pending->begin(), pending->end(),
                               [&diff](const PendingWithdrawal& bundle) { return bundle.m6id == diff.m6id; })};
    if (it == pending->end()) {
        error = UndoError::STATE_MISMATCH;
        return false;
    }
    pending->erase(it);
    return true;
}

bool ApplyMsg(const AckBundles& diff, DrivechainState& state, int32_t)
{
    for (const auto& [slot, action] : diff.actions) {
        PendingWithdrawals* pending{state.ModifyPendingWithdrawals(slot)};
        if (pending == nullptr) return false;

        if (action.kind == AckBundles::Action::Kind::ALARM) {
            // Downvote everything. Saturating, so a bundle at zero stays there
            // -- which is why undo works from the recorded list instead.
            for (PendingWithdrawal& bundle : *pending) {
                if (bundle.vote_count > 0) --bundle.vote_count;
            }
            continue;
        }

        PendingWithdrawal* upvoted{FindBundle(*pending, action.upvoted)};
        if (upvoted == nullptr) return false;
        ++upvoted->vote_count;
        for (const Txid& m6id : action.downvoted) {
            if (PendingWithdrawal* bundle{FindBundle(*pending, m6id)}; bundle != nullptr && bundle->vote_count > 0) {
                --bundle->vote_count;
            }
        }
    }
    return true;
}

bool UndoMsg(const AckBundles& diff, DrivechainState& state, UndoError& error)
{
    for (const auto& [slot, action] : diff.actions) {
        PendingWithdrawals* pending{state.ModifyPendingWithdrawals(slot)};
        if (pending == nullptr) {
            error = UndoError::STATE_MISMATCH;
            return false;
        }

        if (action.kind == AckBundles::Action::Kind::ALARM) {
            RestoreDownvotes(*pending, action.downvoted);
            continue;
        }

        PendingWithdrawal* upvoted{FindBundle(*pending, action.upvoted)};
        if (upvoted == nullptr) {
            error = UndoError::STATE_MISMATCH;
            return false;
        }
        if (upvoted->vote_count == 0) {
            error = UndoError::BUNDLE_VOTE_COUNT_UNDERFLOW;
            return false;
        }
        --upvoted->vote_count;
        RestoreDownvotes(*pending, action.downvoted);
    }
    return true;
}

bool ApplyFailedProposals(const FailedProposals& diff, DrivechainState& state)
{
    for (const Sidechain& proposal : diff.removed) {
        state.EraseProposal(proposal.Id());
    }
    return true;
}

bool UndoFailedProposals(const FailedProposals& diff, DrivechainState& state)
{
    for (const Sidechain& proposal : diff.removed) {
        state.PutProposal(proposal);
    }
    return true;
}

bool ApplyFailedBundles(const FailedBundles& diff, DrivechainState& state)
{
    for (const auto& [slot, failed] : diff.removed) {
        PendingWithdrawals* pending{state.ModifyPendingWithdrawals(slot)};
        if (pending == nullptr) return false;

        // Indices name positions in the list as it stands now, so drop by
        // position rather than by identity and let the rest close up.
        PendingWithdrawals kept;
        kept.reserve(pending->size());
        for (uint32_t index{0}; index < pending->size(); ++index) {
            if (failed.count(index) == 0) kept.push_back((*pending)[index]);
        }
        *pending = std::move(kept);
    }
    return true;
}

bool UndoFailedBundles(const FailedBundles& diff, DrivechainState& state, UndoError& error)
{
    for (const auto& [slot, failed] : diff.removed) {
        PendingWithdrawals* pending{state.ModifyPendingWithdrawals(slot)};
        if (pending == nullptr) {
            error = UndoError::STATE_MISMATCH;
            return false;
        }

        // Rebuild the list by walking the positions it had before: at each
        // index either a bundle that failed goes back, or the next surviving
        // bundle follows. Appending the failed ones instead would reorder the
        // list, and an M4 votes by position.
        PendingWithdrawals restored;
        restored.reserve(pending->size() + failed.size());
        auto survivor{pending->begin()};
        for (uint32_t index{0}; index < pending->size() + failed.size(); ++index) {
            const auto it{failed.find(index)};
            if (it != failed.end()) {
                restored.push_back(it->second);
            } else if (survivor != pending->end()) {
                restored.push_back(*survivor++);
            } else {
                error = UndoError::STATE_MISMATCH;
                return false;
            }
        }
        *pending = std::move(restored);
    }
    return true;
}

void ApplyTreasuryChange(const TreasuryChange& change, SlotNum slot, DrivechainState& state)
{
    state.PutCtip(slot, change.new_ctip);
}

void UndoTreasuryChange(const TreasuryChange& change, SlotNum slot, DrivechainState& state)
{
    if (change.had_previous) {
        state.PutCtip(slot, change.previous);
    } else {
        state.EraseCtip(slot);
    }
}

bool ApplyTx(const M5Diff& diff, DrivechainState& state)
{
    for (const auto& [slot, change] : diff.ctips) {
        ApplyTreasuryChange(change, slot, state);
    }
    return true;
}

bool UndoTx(const M5Diff& diff, DrivechainState& state, UndoError&)
{
    for (const auto& [slot, change] : diff.ctips) {
        UndoTreasuryChange(change, slot, state);
    }
    return true;
}

bool ApplyTx(const M6Diff& diff, DrivechainState& state)
{
    PendingWithdrawals* pending{state.ModifyPendingWithdrawals(diff.slot)};
    if (pending == nullptr) return false;
    if (diff.removed_index >= pending->size()) return false;
    if ((*pending)[diff.removed_index].m6id != diff.removed.m6id) return false;

    ApplyTreasuryChange(diff.ctip, diff.slot, state);
    pending->erase(pending->begin() + diff.removed_index);
    return true;
}

bool UndoTx(const M6Diff& diff, DrivechainState& state, UndoError& error)
{
    PendingWithdrawals* pending{state.ModifyPendingWithdrawals(diff.slot)};
    if (pending == nullptr) {
        error = UndoError::STATE_MISMATCH;
        return false;
    }
    if (diff.removed_index > pending->size()) {
        error = UndoError::STATE_MISMATCH;
        return false;
    }
    UndoTreasuryChange(diff.ctip, diff.slot, state);
    pending->insert(pending->begin() + diff.removed_index, diff.removed);
    return true;
}
} // namespace

bool BlockDiff::Apply(DrivechainState& state, int32_t height) const
{
    for (const CoinbaseMsgDiff& msg : coinbase.msgs) {
        if (!std::visit([&](const auto& diff) { return ApplyMsg(diff, state, height); }, msg)) return false;
    }
    if (!ApplyFailedProposals(coinbase.failed_proposals, state)) return false;
    if (!ApplyFailedBundles(coinbase.failed_bundles, state)) return false;

    for (const TxDiff& tx : txs) {
        if (!std::visit([&](const auto& diff) { return ApplyTx(diff, state); }, tx)) return false;
    }
    return true;
}

bool BlockDiff::Undo(DrivechainState& state, UndoError& error) const
{
    // Exactly the reverse of Apply, in every nested step. Undoing the coinbase
    // messages in order rather than in reverse would, for instance, try to
    // remove a bundle from a slot whose activation had already been undone.
    for (auto tx{txs.rbegin()}; tx != txs.rend(); ++tx) {
        if (!std::visit([&](const auto& diff) { return UndoTx(diff, state, error); }, *tx)) return false;
    }

    if (!UndoFailedBundles(coinbase.failed_bundles, state, error)) return false;
    if (!UndoFailedProposals(coinbase.failed_proposals, state)) return false;

    for (auto msg{coinbase.msgs.rbegin()}; msg != coinbase.msgs.rend(); ++msg) {
        if (!std::visit([&](const auto& diff) { return UndoMsg(diff, state, error); }, *msg)) return false;
    }
    return true;
}

} // namespace drivechain
