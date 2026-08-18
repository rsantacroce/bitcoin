// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <drivechain/diff.h>
#include <drivechain/messages.h>
#include <drivechain/params.h>
#include <drivechain/state.h>
#include <drivechain/validation.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

using namespace drivechain;

namespace {
constexpr SlotNum MAX_SLOT{3};

//! A vote count drawn from the values that decide something, not uniformly.
//!
//! Uniform draws over 16 bits essentially never land on zero, on the maximum,
//! or on either side of a threshold, which are the only values any rule here
//! treats differently -- so a uniform generator exercises the arithmetic and
//! none of the decisions.
uint16_t InterestingVoteCount(FuzzedDataProvider& provider)
{
    static constexpr uint16_t INCLUSION{SHORT_THRESHOLDS.withdrawal_bundle_inclusion_threshold};
    switch (provider.ConsumeIntegralInRange<int>(0, 6)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return INCLUSION;
    case 3: return INCLUSION + 1;
    case 4: return std::numeric_limits<uint16_t>::max() - 1;
    case 5: return std::numeric_limits<uint16_t>::max();
    default: return provider.ConsumeIntegralInRange<uint16_t>(0, 16);
    }
}

//! A starting state with a few slots occupied, so the rules that need one have
//! something to work against. Blocks are then thrown at it.
DrivechainState RandomState(FuzzedDataProvider& provider)
{
    DrivechainState state;
    for (SlotNum slot{0}; slot <= MAX_SLOT; ++slot) {
        switch (provider.ConsumeIntegralInRange<int>(0, 2)) {
        case 0:
            break; // empty slot
        case 1: {
            Sidechain proposal;
            proposal.slot = slot;
            proposal.description = provider.ConsumeBytes<unsigned char>(4);
            proposal.vote_count = InterestingVoteCount(provider);
            proposal.proposal_height = provider.ConsumeIntegralInRange<int32_t>(0, 1000);
            state.PutProposal(proposal);
            break;
        }
        default: {
            Sidechain active;
            active.slot = slot;
            active.description = provider.ConsumeBytes<unsigned char>(4);
            active.activation_height = provider.ConsumeIntegralInRange<int32_t>(0, 1000);
            state.ActivateSidechain(active);
            const int bundles{provider.ConsumeIntegralInRange<int>(0, 3)};
            for (int i{0}; i < bundles; ++i) {
                state.ModifyPendingWithdrawals(slot)->push_back(PendingWithdrawal{
                    .m6id = Txid::FromUint256(uint256{provider.ConsumeIntegral<uint8_t>()}),
                    .vote_count = InterestingVoteCount(provider),
                    .proposal_height = provider.ConsumeIntegralInRange<int32_t>(0, 1000),
                });
            }
            if (provider.ConsumeBool()) {
                state.PutCtip(slot, Ctip{
                                        .outpoint = COutPoint{Txid::FromUint256(uint256{provider.ConsumeIntegral<uint8_t>()}),
                                                              provider.ConsumeIntegral<uint8_t>()},
                                        .value = provider.ConsumeIntegralInRange<CAmount>(0, 1000000),
                                    });
            }
            break;
        }
        }
    }
    return state;
}

//! A well-formed BIP-300/301 coinbase message, with fuzz-chosen contents.
//!
//! Random bytes behind an OP_RETURN would have to hit an exact four-byte tag to
//! parse as a message at all, which is a one in four billion event: without
//! this the target would exercise only the parsers' rejection paths and none of
//! the rules behind them. Fields are drawn from what the state actually
//! contains where that is what makes a path reachable -- an ack for a proposal
//! that exists, a bundle proposal naming one that is already pending.
CScript RandomMessageScript(FuzzedDataProvider& provider, const DrivechainState& state)
{
    std::vector<unsigned char> payload;
    const auto tag = [&payload](std::span<const unsigned char> bytes) {
        payload.insert(payload.end(), bytes.begin(), bytes.end());
    };
    const auto hash = [&payload](const uint256& value) {
        payload.insert(payload.end(), value.begin(), value.end());
    };
    const SlotNum slot{provider.ConsumeIntegralInRange<SlotNum>(0, MAX_SLOT)};

    switch (provider.ConsumeIntegralInRange<int>(0, 4)) {
    case 0: {
        tag(M1ProposeSidechain::TAG);
        payload.push_back(slot);
        // A short description, so repeats of an existing proposal are common.
        const std::vector<unsigned char> description{provider.ConsumeBytes<unsigned char>(2)};
        payload.insert(payload.end(), description.begin(), description.end());
        break;
    }
    case 1: {
        tag(M2AckSidechain::TAG);
        payload.push_back(slot);
        // Half the time, ack a proposal that is really there.
        if (provider.ConsumeBool() && !state.Proposals().empty()) {
            auto it{state.Proposals().begin()};
            std::advance(it, provider.ConsumeIntegralInRange<size_t>(0, state.Proposals().size() - 1));
            hash(it->first.description_hash);
        } else {
            hash(uint256{provider.ConsumeIntegral<uint8_t>()});
        }
        break;
    }
    case 2: {
        tag(M3ProposeBundle::TAG);
        payload.push_back(slot);
        // Half the time, name a bundle that is already pending, which is the
        // rejection this message has that nothing else does.
        const PendingWithdrawals* pending{state.GetPendingWithdrawals(slot)};
        if (provider.ConsumeBool() && pending != nullptr && !pending->empty()) {
            hash(pending->at(provider.ConsumeIntegralInRange<size_t>(0, pending->size() - 1)).m6id.ToUint256());
        } else {
            hash(uint256{provider.ConsumeIntegral<uint8_t>()});
        }
        break;
    }
    case 3: {
        tag(M4AckBundles::TAG);
        const int version{provider.ConsumeIntegralInRange<int>(0, 3)};
        payload.push_back(static_cast<unsigned char>(version));
        if (version == 1 || version == 2) {
            // Lengths around the active-slot count, and values around the
            // bundle indices and the sentinels: the array is rejected outright
            // unless its length matches, so a uniform length never gets past
            // the first rule.
            const size_t active{state.ActiveSlots().size()};
            const size_t length{provider.ConsumeIntegralInRange<size_t>(0, active + 1)};
            for (size_t i{0}; i < length; ++i) {
                const uint16_t vote{[&] {
                    switch (provider.ConsumeIntegralInRange<int>(0, 4)) {
                    case 0: return uint16_t{M4AckBundles::ABSTAIN_TWO_BYTES};
                    case 1: return uint16_t{M4AckBundles::ALARM_TWO_BYTES};
                    case 2: return uint16_t{300}; // needs two bytes
                    default: return provider.ConsumeIntegralInRange<uint16_t>(0, 4);
                    }
                }()};
                if (version == 1) {
                    payload.push_back(static_cast<unsigned char>(vote & 0xFF));
                } else {
                    payload.push_back(static_cast<unsigned char>(vote & 0xFF));
                    payload.push_back(static_cast<unsigned char>(vote >> 8));
                }
            }
        }
        break;
    }
    default:
        tag(M7BmmAccept::TAG);
        payload.push_back(slot);
        hash(uint256{provider.ConsumeIntegral<uint8_t>()});
        break;
    }

    return CScript() << OP_RETURN << payload;
}

//! A transaction whose scripts come from the fuzz input, so treasury outputs,
//! address outputs, BMM requests and nonsense all turn up.
CMutableTransaction RandomTx(FuzzedDataProvider& provider, const DrivechainState& state)
{
    CMutableTransaction tx;
    const int inputs{provider.ConsumeIntegralInRange<int>(0, 3)};
    for (int i{0}; i < inputs; ++i) {
        // Half the time, spend a treasury that actually exists: a purely
        // random outpoint would almost never hit one, and the rules that
        // matter most are the ones about spending them.
        if (provider.ConsumeBool() && !state.Ctips().empty()) {
            auto it{state.Ctips().begin()};
            std::advance(it, provider.ConsumeIntegralInRange<size_t>(0, state.Ctips().size() - 1));
            tx.vin.emplace_back(it->second.outpoint);
        } else {
            tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{provider.ConsumeIntegral<uint8_t>()}),
                                          provider.ConsumeIntegral<uint8_t>()});
        }
    }

    const int outputs{provider.ConsumeIntegralInRange<int>(0, 4)};
    for (int i{0}; i < outputs; ++i) {
        CScript script;
        switch (provider.ConsumeIntegralInRange<int>(0, 3)) {
        case 0:
            script = TreasuryScript(provider.ConsumeIntegralInRange<SlotNum>(0, MAX_SLOT));
            break;
        case 1:
            script = CScript() << OP_RETURN << ConsumeRandomLengthByteVector<unsigned char>(provider, 32);
            break;
        case 2:
            script = RandomMessageScript(provider, state);
            break;
        default:
            script = ConsumeScript(provider);
            break;
        }
        tx.vout.emplace_back(provider.ConsumeIntegralInRange<CAmount>(0, 1000000), script);
    }
    return tx;
}
} // namespace

FUZZ_TARGET(drivechain_connect_block)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};

    const DrivechainState state{RandomState(provider)};

    CBlock block;
    CMutableTransaction coinbase{RandomTx(provider, state)};
    // A coinbase spends nothing, and the messages live in its outputs.
    coinbase.vin.clear();
    coinbase.vin.emplace_back(COutPoint{});
    block.vtx.push_back(MakeTransactionRef(coinbase));

    const int extra{provider.ConsumeIntegralInRange<int>(0, 3)};
    for (int i{0}; i < extra; ++i) {
        block.vtx.push_back(MakeTransactionRef(RandomTx(provider, state)));
    }

    BlockContext context;
    context.height = provider.ConsumeIntegralInRange<int32_t>(0, 100000);
    context.parent_hash = uint256{provider.ConsumeIntegral<uint8_t>()};

    BlockDiff diff;
    BlockError error{BlockError::STATE_MISMATCH};
    if (!ConnectBlock(block, context, state, SHORT_THRESHOLDS, /*activation_height=*/0, diff, error)) {
        // A rejection must name a reason, and never the internal one: reaching
        // STATE_MISMATCH would mean a diff this code built did not fit the
        // state it was built against.
        assert(error != BlockError::STATE_MISMATCH);
        assert(!BlockErrorString(error).empty());
        return;
    }

    // An accepted block's diff must apply to the state it was built against,
    // and undo back to exactly where it started. Everything else in the port
    // rests on that holding for every block, not only well-formed ones.
    DrivechainState applied{state};
    const bool ok{diff.Apply(applied, context.height)};
    assert(ok);

    UndoError undo_error{};
    const bool undone{diff.Undo(applied, undo_error)};
    assert(undone);
    assert(applied == state);
}
