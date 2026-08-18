// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/state.h>

#include <drivechain/messages.h>
#include <hash.h>

#include <vector>

namespace drivechain {

SidechainProposalId Sidechain::Id() const
{
    return SidechainProposalId{.slot = slot, .description_hash = Hash(description)};
}

const Sidechain* DrivechainState::FindProposal(const SidechainProposalId& id) const
{
    const auto it{m_proposals.find(id)};
    return it == m_proposals.end() ? nullptr : &it->second;
}

const Sidechain* DrivechainState::FindActiveSidechain(SlotNum slot) const
{
    const auto it{m_active.find(slot)};
    return it == m_active.end() ? nullptr : &it->second;
}

std::vector<SlotNum> DrivechainState::ActiveSlots() const
{
    std::vector<SlotNum> slots;
    slots.reserve(m_active.size());
    for (const auto& [slot, _] : m_active) {
        slots.push_back(slot);
    }
    return slots;
}

const PendingWithdrawals* DrivechainState::GetPendingWithdrawals(SlotNum slot) const
{
    const auto it{m_pending.find(slot)};
    return it == m_pending.end() ? nullptr : &it->second;
}

PendingWithdrawals* DrivechainState::ModifyPendingWithdrawals(SlotNum slot)
{
    const auto it{m_pending.find(slot)};
    return it == m_pending.end() ? nullptr : &it->second;
}

const Ctip* DrivechainState::GetCtip(SlotNum slot) const
{
    const auto it{m_ctip.find(slot)};
    return it == m_ctip.end() ? nullptr : &it->second;
}

void DrivechainState::PutProposal(const Sidechain& sidechain)
{
    m_proposals[sidechain.Id()] = sidechain;
}

bool DrivechainState::EraseProposal(const SidechainProposalId& id)
{
    return m_proposals.erase(id) > 0;
}

void DrivechainState::ActivateSidechain(const Sidechain& sidechain)
{
    m_active[sidechain.slot] = sidechain;
    // An overwrite leaves the outgoing sidechain's pending bundles in place.
    // Whether that is the right answer is an open question in BIP-300 -- see
    // doc/drivechain.md -- but it is what the reference implementation does,
    // and inventing a different one here would be a silent divergence.
    m_pending.try_emplace(sidechain.slot);
}

bool DrivechainState::DeactivateSidechain(SlotNum slot)
{
    m_pending.erase(slot);
    return m_active.erase(slot) > 0;
}

void DrivechainState::PutCtip(SlotNum slot, const Ctip& ctip)
{
    m_ctip[slot] = ctip;
}

bool DrivechainState::EraseCtip(SlotNum slot)
{
    return m_ctip.erase(slot) > 0;
}

} // namespace drivechain
