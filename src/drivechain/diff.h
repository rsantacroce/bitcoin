// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_DIFF_H
#define BITCOIN_DRIVECHAIN_DIFF_H

#include <drivechain/messages.h>
#include <drivechain/state.h>
#include <primitives/transaction_identifier.h>

#include <cstdint>
#include <map>
#include <variant>
#include <vector>

//! What one block does to the BIP-300 state, recorded so it can be undone.
//!
//! Connecting a block builds one of these and applies it; disconnecting reads
//! it back and undoes it. The invariant the whole design rests on is that undo
//! restores the state exactly, so that a reorg through a block leaves nothing
//! behind.
//!
//! Several of the diffs carry more than the change itself -- which bundles lost
//! a vote, where in the list a bundle used to sit. That is not redundancy. Vote
//! counts saturate at zero and bundle positions are what an M4 votes by, so
//! undo cannot recompute either from the post-state; it has to be told.
//!
//! Ported from lib/validator/dbs/diff.rs of the reference implementation, whose
//! structure this follows closely and deliberately.
namespace drivechain {

//! Why a diff could not be undone.
enum class UndoError {
    //! The state does not contain what the diff says it changed. This means the
    //! stored diff and the state disagree, which is a bug or corruption rather
    //! than a bad block.
    STATE_MISMATCH,
    //! Undoing would drive a proposal's vote count below zero.
    SIDECHAIN_VOTE_COUNT_UNDERFLOW,
    //! Undoing would drive a bundle's vote count below zero.
    BUNDLE_VOTE_COUNT_UNDERFLOW,
};

/** An M1 created a new proposal. */
struct NewSidechainProposal {
    Sidechain sidechain;
};

/** An M2 acked a proposal, possibly activating it. */
struct AckSidechainProposal {
    enum class Effect : uint8_t {
        //! The ack counted but did not reach the threshold.
        NO_ACTIVATION = 0,
        //! The proposal activated into a slot that was empty.
        SLOT_ACTIVATION = 1,
        //! The proposal activated into a slot that was occupied.
        REPLACE_ACTIVE = 2,
    };

    SidechainProposalId id;
    Effect effect{Effect::NO_ACTIVATION};
    //! The sidechain this ack displaced. Only meaningful for REPLACE_ACTIVE,
    //! and the only way undo can put it back.
    Sidechain replaced;
};

/** An M3 proposed a withdrawal bundle. */
struct ProposeBundle {
    SlotNum slot{0};
    Txid m6id;
};

/** An M4 moved vote counts, in at most one way per slot. */
struct AckBundles {
    struct Action {
        enum class Kind : uint8_t {
            //! Every pending bundle in the slot loses a vote.
            ALARM = 0,
            //! One bundle gains a vote and every other loses one.
            UPVOTE = 1,
        };

        Kind kind{Kind::ALARM};
        //! The upvoted bundle. UPVOTE only.
        Txid upvoted;
        //! Bundles that actually lost a vote, meaning those whose count was
        //! above zero when the diff was built.
        //!
        //! BIP-300 downvotes *every* other bundle, but a bundle already at zero
        //! stays at zero, so undo must not increment it. Listing only the
        //! bundles that moved is what keeps apply and undo exact inverses.
        std::vector<Txid> downvoted;
    };

    std::map<SlotNum, Action> actions;
};

/** Proposals discarded this block for having run out of time. */
struct FailedProposals {
    std::vector<Sidechain> removed;
};

/** Bundles discarded this block for having aged out.
 *
 *  Keyed by the position each held in its slot's list, so undo can put them
 *  back where they were rather than at the end.
 */
struct FailedBundles {
    std::map<SlotNum, std::map<uint32_t, PendingWithdrawal>> removed;
};

using CoinbaseMsgDiff = std::variant<NewSidechainProposal, AckSidechainProposal, ProposeBundle, AckBundles>;

/** Everything the coinbase transaction did, plus the ageing that happens with
 *  it. */
struct CoinbaseDiff {
    //! In the order the messages appear in the coinbase's outputs.
    std::vector<CoinbaseMsgDiff> msgs;
    FailedProposals failed_proposals;
    FailedBundles failed_bundles;
};

//! A treasury pointer moving, with what it replaced so undo can restore it.
struct TreasuryChange {
    Ctip new_ctip;
    //! False for the first ever deposit into a slot, where undo removes the
    //! pointer instead of restoring one.
    bool had_previous{false};
    Ctip previous;
};

/** An M5 deposit. One transaction may deposit into several slots. */
struct M5Diff {
    std::map<SlotNum, TreasuryChange> ctips;
};

/** An M6 withdrawal, which pays out one bundle and moves one treasury. */
struct M6Diff {
    SlotNum slot{0};
    TreasuryChange ctip;
    //! Position the paid-out bundle held in the slot's list. An M4 votes by
    //! position, so undo has to restore it at the same index.
    uint32_t removed_index{0};
    PendingWithdrawal removed;
};

using TxDiff = std::variant<M5Diff, M6Diff>;

/** Everything one block did to the BIP-300 state. */
struct BlockDiff {
    CoinbaseDiff coinbase;
    //! In the order the transactions appear in the block.
    std::vector<TxDiff> txs;

    //! Apply this diff to `state`. `height` is the height of the block that
    //! produced it, which is what a new proposal or bundle records.
    //!
    //! Returns false if `state` does not contain what the diff expects, which
    //! means the two disagree: a bug, not a bad block.
    [[nodiscard]] bool Apply(DrivechainState& state, int32_t height) const;

    //! Undo this diff, restoring `state` to exactly what it was before Apply.
    [[nodiscard]] bool Undo(DrivechainState& state, UndoError& error) const;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_DIFF_H
