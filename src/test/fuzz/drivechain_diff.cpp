// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/diff.h>
#include <drivechain/messages.h>
#include <drivechain/state.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <uint256.h>

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

using namespace drivechain;

namespace {
//! Slots are drawn from a small range so that collisions -- two proposals for
//! one slot, an overwrite of an occupied slot -- happen often rather than
//! almost never.
constexpr SlotNum MAX_SLOT{4};
//! Enough blocks to build a deep reorg, few enough to stay fast.
constexpr int MAX_BLOCKS{32};

SlotNum RandomSlot(FuzzedDataProvider& provider)
{
    return provider.ConsumeIntegralInRange<SlotNum>(0, MAX_SLOT);
}

Txid RandomBundleId(FuzzedDataProvider& provider)
{
    return Txid::FromUint256(uint256{provider.ConsumeIntegral<uint8_t>()});
}

Ctip RandomCtip(FuzzedDataProvider& provider)
{
    return Ctip{
        .outpoint = COutPoint{Txid::FromUint256(uint256{provider.ConsumeIntegral<uint8_t>()}),
                              provider.ConsumeIntegral<uint8_t>()},
        .value = provider.ConsumeIntegralInRange<CAmount>(0, 21000000),
    };
}

TreasuryChange RandomTreasuryChange(const DrivechainState& state, SlotNum slot, FuzzedDataProvider& provider)
{
    TreasuryChange change;
    change.new_ctip = RandomCtip(provider);
    if (const Ctip* previous{state.GetCtip(slot)}) {
        change.had_previous = true;
        change.previous = *previous;
    }
    return change;
}

//! Apply one piece of a block on its own, so the generator below can keep a
//! scratch state in step with what it has built so far.
[[nodiscard]] bool ApplyMsg(const CoinbaseMsgDiff& msg, DrivechainState& state, int32_t height)
{
    BlockDiff single;
    single.coinbase.msgs.push_back(msg);
    return single.Apply(state, height);
}

[[nodiscard]] bool ApplyTx(const TxDiff& tx, DrivechainState& state, int32_t height)
{
    BlockDiff single;
    single.txs.push_back(tx);
    return single.Apply(state, height);
}

//! Build a diff that is valid against `state` by construction.
//!
//! Pieces are generated in the order BlockDiff::Apply runs them, each against a
//! scratch state carrying the effect of the ones before it. Generating them
//! against the starting state instead would produce blocks that contradict
//! themselves -- activating a proposal and then ageing that same proposal out,
//! for instance -- and the fuzzer would spend its time rejecting its own input
//! rather than exercising undo.
BlockDiff GenerateDiff(const DrivechainState& state, FuzzedDataProvider& provider, int32_t height)
{
    DrivechainState scratch{state};
    BlockDiff diff;

    const int msg_count{provider.ConsumeIntegralInRange<int>(0, 4)};
    for (int i{0}; i < msg_count; ++i) {
        const std::vector<SlotNum> active{scratch.ActiveSlots()};

        switch (provider.ConsumeIntegralInRange<int>(0, 4)) {
        case 0: {
            Sidechain sidechain;
            sidechain.slot = RandomSlot(provider);
            sidechain.description = provider.ConsumeBytes<unsigned char>(provider.ConsumeIntegralInRange<size_t>(0, 8));
            sidechain.proposal_height = height;
            // A repeat proposal is ignored by BIP-300 rather than recorded, so
            // it would not appear in a diff.
            if (scratch.FindProposal(sidechain.Id()) != nullptr) break;
            const CoinbaseMsgDiff msg{NewSidechainProposal{.sidechain = sidechain}};
            if (!ApplyMsg(msg, scratch, height)) break;
            diff.coinbase.msgs.push_back(msg);
            break;
        }
        case 1: {
            if (scratch.Proposals().empty()) break;
            auto it{scratch.Proposals().begin()};
            std::advance(it, provider.ConsumeIntegralInRange<size_t>(0, scratch.Proposals().size() - 1));
            const SidechainProposalId id{it->first};

            AckSidechainProposal ack;
            ack.id = id;
            const bool activate{provider.ConsumeBool()};
            if (!activate) {
                ack.effect = AckSidechainProposal::Effect::NO_ACTIVATION;
            } else if (const Sidechain* incumbent{scratch.FindActiveSidechain(id.slot)}) {
                ack.effect = AckSidechainProposal::Effect::REPLACE_ACTIVE;
                ack.replaced = *incumbent;
            } else {
                ack.effect = AckSidechainProposal::Effect::SLOT_ACTIVATION;
            }
            const CoinbaseMsgDiff msg{ack};
            if (!ApplyMsg(msg, scratch, height)) break;
            diff.coinbase.msgs.push_back(msg);
            break;
        }
        case 2: {
            if (active.empty()) break;
            const SlotNum slot{active[provider.ConsumeIntegralInRange<size_t>(0, active.size() - 1)]};
            const Txid m6id{RandomBundleId(provider)};
            // A bundle already pending is a block-invalidating duplicate, not
            // a state change.
            const PendingWithdrawals* pending{scratch.GetPendingWithdrawals(slot)};
            assert(pending != nullptr);
            bool already_pending{false};
            for (const PendingWithdrawal& bundle : *pending) {
                if (bundle.m6id == m6id) already_pending = true;
            }
            if (already_pending) break;

            const CoinbaseMsgDiff msg{ProposeBundle{.slot = slot, .m6id = m6id}};
            if (!ApplyMsg(msg, scratch, height)) break;
            diff.coinbase.msgs.push_back(msg);
            break;
        }
        case 3: {
            // One vote per slot at most, which is what an M4 can express.
            AckBundles ack;
            for (const SlotNum slot : active) {
                const PendingWithdrawals* pending{scratch.GetPendingWithdrawals(slot)};
                assert(pending != nullptr);
                if (pending->empty()) continue;
                if (provider.ConsumeBool()) continue; // abstain

                AckBundles::Action action;
                if (provider.ConsumeBool()) {
                    action.kind = AckBundles::Action::Kind::ALARM;
                    for (const PendingWithdrawal& bundle : *pending) {
                        if (bundle.vote_count > 0) action.downvoted.push_back(bundle.m6id);
                    }
                } else {
                    const size_t index{provider.ConsumeIntegralInRange<size_t>(0, pending->size() - 1)};
                    if ((*pending)[index].vote_count == std::numeric_limits<uint16_t>::max()) continue;
                    action.kind = AckBundles::Action::Kind::UPVOTE;
                    action.upvoted = (*pending)[index].m6id;
                    for (size_t i2{0}; i2 < pending->size(); ++i2) {
                        if (i2 != index && (*pending)[i2].vote_count > 0) {
                            action.downvoted.push_back((*pending)[i2].m6id);
                        }
                    }
                }
                ack.actions[slot] = action;
            }
            if (ack.actions.empty()) break;
            const CoinbaseMsgDiff msg{ack};
            if (!ApplyMsg(msg, scratch, height)) break;
            diff.coinbase.msgs.push_back(msg);
            break;
        }
        default:
            break;
        }
    }

    // Proposals that ran out of time this block.
    for (const auto& [id, sidechain] : scratch.Proposals()) {
        if (provider.ConsumeBool()) diff.coinbase.failed_proposals.removed.push_back(sidechain);
    }
    {
        BlockDiff single;
        single.coinbase.failed_proposals = diff.coinbase.failed_proposals;
        if (!single.Apply(scratch, height)) diff.coinbase.failed_proposals.removed.clear();
    }

    // Bundles that aged out this block, recorded by the position each held.
    for (const SlotNum slot : scratch.ActiveSlots()) {
        const PendingWithdrawals* pending{scratch.GetPendingWithdrawals(slot)};
        assert(pending != nullptr);
        for (uint32_t index{0}; index < pending->size(); ++index) {
            if (provider.ConsumeBool()) {
                diff.coinbase.failed_bundles.removed[slot][index] = (*pending)[index];
            }
        }
    }
    {
        BlockDiff single;
        single.coinbase.failed_bundles = diff.coinbase.failed_bundles;
        if (!single.Apply(scratch, height)) diff.coinbase.failed_bundles.removed.clear();
    }

    const int tx_count{provider.ConsumeIntegralInRange<int>(0, 3)};
    for (int i{0}; i < tx_count; ++i) {
        const std::vector<SlotNum> active{scratch.ActiveSlots()};
        if (active.empty()) break;
        const SlotNum slot{active[provider.ConsumeIntegralInRange<size_t>(0, active.size() - 1)]};

        const PendingWithdrawals* pending{scratch.GetPendingWithdrawals(slot)};
        assert(pending != nullptr);

        if (!pending->empty() && provider.ConsumeBool()) {
            M6Diff m6;
            m6.slot = slot;
            m6.ctip = RandomTreasuryChange(scratch, slot, provider);
            m6.removed_index = provider.ConsumeIntegralInRange<uint32_t>(0, pending->size() - 1);
            m6.removed = (*pending)[m6.removed_index];
            const TxDiff tx{m6};
            if (!ApplyTx(tx, scratch, height)) break;
            diff.txs.push_back(tx);
        } else {
            M5Diff m5;
            m5.ctips[slot] = RandomTreasuryChange(scratch, slot, provider);
            const TxDiff tx{m5};
            if (!ApplyTx(tx, scratch, height)) break;
            diff.txs.push_back(tx);
        }
    }

    return diff;
}

//! Undo `diff` and require the state to come back exactly.
void CheckUndoRestores(const BlockDiff& diff, DrivechainState& state, const DrivechainState& expected)
{
    // Undo through a serialized copy, so the round trip through disk is on the
    // same path as the invariant it has to preserve.
    DataStream stream;
    stream << diff;
    BlockDiff restored;
    stream >> restored;

    UndoError error{};
    const bool undone{restored.Undo(state, error)};
    assert(undone);
    assert(state == expected);
}
} // namespace

FUZZ_TARGET(drivechain_block_diff)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};

    DrivechainState state;
    // Each entry is the state as it was before the block above it connected.
    std::vector<std::pair<DrivechainState, BlockDiff>> connected;
    int32_t height{0};

    while (provider.remaining_bytes() > 0 && connected.size() < MAX_BLOCKS) {
        if (!connected.empty() && provider.ConsumeBool()) {
            // Disconnect the tip.
            const auto [before, diff]{connected.back()};
            connected.pop_back();
            --height;
            CheckUndoRestores(diff, state, before);
            continue;
        }

        const DrivechainState before{state};
        const BlockDiff diff{GenerateDiff(state, provider, height)};
        const bool applied{diff.Apply(state, height)};
        // GenerateDiff builds only pieces it has already applied to a scratch
        // state, so a diff it returns must apply.
        assert(applied);
        connected.emplace_back(before, diff);
        ++height;
    }

    // Unwind the whole chain. Undoing every block in turn has to land back on
    // the empty state, which is the reorg invariant stated end to end.
    while (!connected.empty()) {
        const auto [before, diff]{connected.back()};
        connected.pop_back();
        CheckUndoRestores(diff, state, before);
    }
    assert(state == DrivechainState{});
}
