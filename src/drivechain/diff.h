// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_DIFF_H
#define BITCOIN_DRIVECHAIN_DIFF_H

#include <drivechain/messages.h>
#include <drivechain/state.h>
#include <primitives/transaction_identifier.h>
#include <serialize.h>

#include <cstdint>
#include <ios>
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

    SERIALIZE_METHODS(NewSidechainProposal, obj) { READWRITE(obj.sidechain); }
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

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << id << static_cast<uint8_t>(effect) << replaced;
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t raw_effect;
        s >> id >> raw_effect >> replaced;
        if (raw_effect > static_cast<uint8_t>(Effect::REPLACE_ACTIVE)) {
            throw std::ios_base::failure("unknown sidechain ack effect");
        }
        effect = static_cast<Effect>(raw_effect);
    }
};

/** An M3 proposed a withdrawal bundle. */
struct ProposeBundle {
    SlotNum slot{0};
    Txid m6id;

    SERIALIZE_METHODS(ProposeBundle, obj) { READWRITE(obj.slot, obj.m6id); }
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

        template <typename Stream>
        void Serialize(Stream& s) const
        {
            s << static_cast<uint8_t>(kind) << upvoted << downvoted;
        }

        template <typename Stream>
        void Unserialize(Stream& s)
        {
            uint8_t raw_kind;
            s >> raw_kind >> upvoted >> downvoted;
            if (raw_kind > static_cast<uint8_t>(Kind::UPVOTE)) {
                throw std::ios_base::failure("unknown bundle ack kind");
            }
            kind = static_cast<Kind>(raw_kind);
        }
    };

    std::map<SlotNum, Action> actions;

    SERIALIZE_METHODS(AckBundles, obj) { READWRITE(obj.actions); }
};

/** Proposals discarded this block for having run out of time. */
struct FailedProposals {
    std::vector<Sidechain> removed;

    SERIALIZE_METHODS(FailedProposals, obj) { READWRITE(obj.removed); }
};

/** Bundles discarded this block for having aged out.
 *
 *  Keyed by the position each held in its slot's list, so undo can put them
 *  back where they were rather than at the end.
 */
struct FailedBundles {
    std::map<SlotNum, std::map<uint32_t, PendingWithdrawal>> removed;

    SERIALIZE_METHODS(FailedBundles, obj) { READWRITE(obj.removed); }
};

using CoinbaseMsgDiff = std::variant<NewSidechainProposal, AckSidechainProposal, ProposeBundle, AckBundles>;

//! Serialize a variant as a one-byte discriminator followed by the alternative
//! it holds. Found by argument-dependent lookup, so the container serializers
//! in serialize.h pick these up for a vector of them.
//!
//! The discriminator is std::variant's alternative index, which means the order
//! of the alternatives above is part of the on-disk format. Adding one at the
//! end is compatible; reordering is not.
template <typename Stream, typename... Alternatives>
void Serialize(Stream& s, const std::variant<Alternatives...>& value)
{
    s << static_cast<uint8_t>(value.index());
    std::visit([&s](const auto& alternative) { s << alternative; }, value);
}

template <typename Stream, typename... Alternatives>
void Unserialize(Stream& s, std::variant<Alternatives...>& value)
{
    uint8_t index;
    s >> index;
    if (index >= sizeof...(Alternatives)) {
        throw std::ios_base::failure("unknown drivechain diff discriminator");
    }
    // Walk the alternatives until the index matches, and read into that one.
    size_t current{0};
    ([&] {
        if (current++ == index) {
            Alternatives alternative;
            s >> alternative;
            value = std::move(alternative);
        }
    }(), ...);
}

/** Everything the coinbase transaction did, plus the ageing that happens with
 *  it. */
struct CoinbaseDiff {
    //! In the order the messages appear in the coinbase's outputs.
    std::vector<CoinbaseMsgDiff> msgs;
    FailedProposals failed_proposals;
    FailedBundles failed_bundles;

    SERIALIZE_METHODS(CoinbaseDiff, obj) { READWRITE(obj.msgs, obj.failed_proposals, obj.failed_bundles); }
};

//! A treasury pointer moving, with what it replaced so undo can restore it.
struct TreasuryChange {
    Ctip new_ctip;
    //! False for the first ever deposit into a slot, where undo removes the
    //! pointer instead of restoring one.
    bool had_previous{false};
    Ctip previous;

    SERIALIZE_METHODS(TreasuryChange, obj) { READWRITE(obj.new_ctip, obj.had_previous, obj.previous); }
};

/** An M5 deposit. One transaction may deposit into several slots. */
struct M5Diff {
    std::map<SlotNum, TreasuryChange> ctips;

    SERIALIZE_METHODS(M5Diff, obj) { READWRITE(obj.ctips); }
};

/** An M6 withdrawal, which pays out one bundle and moves one treasury. */
struct M6Diff {
    SlotNum slot{0};
    TreasuryChange ctip;
    //! Position the paid-out bundle held in the slot's list. An M4 votes by
    //! position, so undo has to restore it at the same index.
    uint32_t removed_index{0};
    PendingWithdrawal removed;

    SERIALIZE_METHODS(M6Diff, obj) { READWRITE(obj.slot, obj.ctip, obj.removed_index, obj.removed); }
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

    SERIALIZE_METHODS(BlockDiff, obj) { READWRITE(obj.coinbase, obj.txs); }
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_DIFF_H
