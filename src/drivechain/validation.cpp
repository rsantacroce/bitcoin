// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/validation.h>

#include <drivechain/messages.h>
#include <drivechain/params.h>
#include <drivechain/state.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>

namespace drivechain {

std::string BlockErrorString(BlockError error)
{
    switch (error) {
    case BlockError::DUPLICATE_M1: return "bad-drivechain-duplicate-m1";
    case BlockError::DUPLICATE_M2: return "bad-drivechain-duplicate-m2";
    case BlockError::DUPLICATE_M4: return "bad-drivechain-duplicate-m4";
    case BlockError::DUPLICATE_M7: return "bad-drivechain-duplicate-m7";
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
    std::set<SlotNum> accepted_slots;

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
            if (!accepted_slots.insert(m7->slot).second) {
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

} // namespace drivechain
