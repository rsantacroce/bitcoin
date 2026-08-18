// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_STATE_H
#define BITCOIN_DRIVECHAIN_STATE_H

#include <consensus/amount.h>
#include <drivechain/messages.h>
#include <primitives/transaction.h>
#include <primitives/transaction_identifier.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <vector>

//! The two lists BIP-300 asks an enforcing node to maintain, and the treasury
//! pointers that go with them.
//!
//! Nothing here validates anything. This is the state a block is applied to;
//! deciding what a block does to it is ConnectBlock's job.
namespace drivechain {

//! Sentinel for a height that has not happened, used where the enforcer's
//! reference implementation carries an optional.
static constexpr int32_t NO_HEIGHT{-1};

/** Identifies a sidechain proposal: which slot it wants, and what it says.
 *
 *  Two proposals for the same slot with different descriptions are different
 *  proposals with independent vote counts, which is why the slot alone is not
 *  the key.
 */
struct SidechainProposalId {
    SlotNum slot{0};
    //! sha256d(D), the identifier an M2 votes for.
    uint256 description_hash;

    SERIALIZE_METHODS(SidechainProposalId, obj) { READWRITE(obj.slot, obj.description_hash); }

    friend bool operator==(const SidechainProposalId& a, const SidechainProposalId& b) = default;
    //! uint256 orders lexicographically but offers no operator<=>, so this
    //! cannot be defaulted. Ordering exists only to key a std::map; nothing in
    //! BIP-300 depends on which proposal sorts first.
    friend bool operator<(const SidechainProposalId& a, const SidechainProposalId& b)
    {
        if (a.slot != b.slot) return a.slot < b.slot;
        return a.description_hash < b.description_hash;
    }
};

/** An entry in D1, the sidechain list.
 *
 *  The same type covers a proposal still gathering votes and a sidechain that
 *  has activated; activation_height is what separates them.
 *
 *  BIP-300, "D1 — The Sidechain List".
 */
struct Sidechain {
    SlotNum slot{0};
    //! The sidechain description `D`, opaque to consensus.
    std::vector<unsigned char> description;
    //! ACKs accumulated by the proposal.
    uint16_t vote_count{0};
    //! Height of the block carrying the M1 that created this proposal.
    int32_t proposal_height{0};
    //! Height at which the proposal activated, or NO_HEIGHT while pending.
    int32_t activation_height{NO_HEIGHT};

    SidechainProposalId Id() const;

    SERIALIZE_METHODS(Sidechain, obj)
    {
        READWRITE(obj.slot, obj.description, obj.vote_count, obj.proposal_height, obj.activation_height);
    }

    friend bool operator==(const Sidechain& a, const Sidechain& b) = default;
};

/** An entry in D2, the withdrawal list.
 *
 *  BIP-300, "D2 — The Withdrawal List".
 */
struct PendingWithdrawal {
    //! The blinded withdrawal's txid; see BlindM6 in drivechain/m6id.h.
    Txid m6id;
    //! Miner upvotes. Starts at 1, changes by at most 1 per block, saturates
    //! at zero.
    uint16_t vote_count{0};
    //! Height of the block carrying the M3 that proposed this bundle.
    int32_t proposal_height{0};

    SERIALIZE_METHODS(PendingWithdrawal, obj) { READWRITE(obj.m6id, obj.vote_count, obj.proposal_height); }

    friend bool operator==(const PendingWithdrawal& a, const PendingWithdrawal& b) = default;
};

/** The treasury UTXO of a sidechain slot, historically "CTIP".
 *
 *  BIP-300, "OP_DRIVECHAIN". There MUST never be two of these for one slot;
 *  that is the central fund-safety invariant, and it is why this is a map from
 *  slot to a single outpoint rather than a set.
 */
struct Ctip {
    COutPoint outpoint;
    CAmount value{0};

    SERIALIZE_METHODS(Ctip, obj) { READWRITE(obj.outpoint, obj.value); }

    friend bool operator==(const Ctip& a, const Ctip& b) = default;
};

//! Pending bundles for one slot, in the canonical order BIP-300 defines:
//! ascending by proposal height, then by coinbase output index within a block.
//! Blocks are connected in order and their coinbase outputs read in order, so
//! insertion order *is* that order — which is what lets an M4 select a bundle
//! by position. Removals therefore have to preserve it.
using PendingWithdrawals = std::vector<PendingWithdrawal>;

/**
 * The BIP-300 databases, D1 and D2, plus the treasury pointers.
 *
 * Mutation goes through the named methods rather than through the containers,
 * because two of the invariants live in them: activating a sidechain gives the
 * slot an empty withdrawal list, and deactivating it takes that list away.
 *
 * Deliberately absent: the deposit sequence index. BIP-300 says enforcing nodes
 * SHOULD keep one so deposits can be served without rescanning, but no
 * validation rule reads it, and the reference implementation uses it only to
 * label the events it emits. Keeping it out of the consensus state means it can
 * be optional later without the peg depending on it.
 */
class DrivechainState
{
public:
    //! D1 entries that have not activated. Keyed by proposal, not by slot: a
    //! slot may have several proposals competing for it at once.
    const std::map<SidechainProposalId, Sidechain>& Proposals() const { return m_proposals; }
    //! D1 entries that have activated, one per occupied slot.
    const std::map<SlotNum, Sidechain>& ActiveSidechains() const { return m_active; }

    const Sidechain* FindProposal(const SidechainProposalId& id) const;
    const Sidechain* FindActiveSidechain(SlotNum slot) const;
    bool IsActive(SlotNum slot) const { return m_active.count(slot) > 0; }
    //! Active slot numbers, ascending. This is the vector an M4's votes index
    //! into, so the order is consensus-critical.
    std::vector<SlotNum> ActiveSlots() const;

    //! D2 for a slot, or nullptr if the slot holds no active sidechain.
    const PendingWithdrawals* GetPendingWithdrawals(SlotNum slot) const;
    PendingWithdrawals* ModifyPendingWithdrawals(SlotNum slot);

    const Ctip* GetCtip(SlotNum slot) const;
    //! Every treasury pointer. A slot keeps its treasury even if its sidechain
    //! stops being active, so this is not the same as iterating active slots.
    const std::map<SlotNum, Ctip>& Ctips() const { return m_ctip; }

    void PutProposal(const Sidechain& sidechain);
    bool EraseProposal(const SidechainProposalId& id);

    //! Activate a sidechain into a slot, giving it an empty withdrawal list if
    //! it does not already have one. An overwrite keeps the outgoing
    //! sidechain's list, matching the reference implementation.
    void ActivateSidechain(const Sidechain& sidechain);
    //! Remove an active sidechain and its withdrawal list.
    bool DeactivateSidechain(SlotNum slot);

    void PutCtip(SlotNum slot, const Ctip& ctip);
    bool EraseCtip(SlotNum slot);

    friend bool operator==(const DrivechainState& a, const DrivechainState& b) = default;

    //! Written whole rather than entry by entry; see DrivechainDB.
    SERIALIZE_METHODS(DrivechainState, obj)
    {
        READWRITE(obj.m_proposals, obj.m_active, obj.m_pending, obj.m_ctip);
    }

private:
    std::map<SidechainProposalId, Sidechain> m_proposals;
    std::map<SlotNum, Sidechain> m_active;
    std::map<SlotNum, PendingWithdrawals> m_pending;
    std::map<SlotNum, Ctip> m_ctip;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_STATE_H
