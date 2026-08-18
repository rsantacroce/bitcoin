// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/diff.h>
#include <drivechain/messages.h>
#include <drivechain/state.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace drivechain;

namespace {
constexpr SlotNum SLOT{1};
constexpr int32_t HEIGHT{500};

Sidechain MakeProposal(SlotNum slot, const char* description, int32_t height = 100)
{
    Sidechain sidechain;
    sidechain.slot = slot;
    sidechain.description = std::vector<unsigned char>{description, description + strlen(description)};
    sidechain.proposal_height = height;
    return sidechain;
}

Txid BundleId(uint8_t seed) { return Txid::FromUint256(uint256{seed}); }

Ctip MakeCtip(uint8_t seed, CAmount value)
{
    return Ctip{.outpoint = COutPoint{Txid::FromUint256(uint256{seed}), 0}, .value = value};
}

//! A state with one active sidechain in SLOT and the given bundle vote counts.
DrivechainState ActiveWithBundles(const std::vector<uint16_t>& votes)
{
    DrivechainState state;
    Sidechain sidechain{MakeProposal(SLOT, "alpha")};
    sidechain.activation_height = 10;
    state.ActivateSidechain(sidechain);
    for (size_t i{0}; i < votes.size(); ++i) {
        state.ModifyPendingWithdrawals(SLOT)->push_back(
            PendingWithdrawal{.m6id = BundleId(static_cast<uint8_t>(i + 1)), .vote_count = votes[i], .proposal_height = 20});
    }
    return state;
}

//! Apply the diff, then undo it, and require the state to come back exactly.
void CheckRoundTrip(const BlockDiff& diff, DrivechainState state)
{
    const DrivechainState before{state};
    BOOST_REQUIRE(diff.Apply(state, HEIGHT));
    BOOST_CHECK(!(state == before));

    UndoError error{};
    BOOST_REQUIRE(diff.Undo(state, error));
    BOOST_CHECK(state == before);
}

std::vector<uint16_t> VoteCounts(const DrivechainState& state)
{
    std::vector<uint16_t> votes;
    for (const PendingWithdrawal& bundle : *state.GetPendingWithdrawals(SLOT)) {
        votes.push_back(bundle.vote_count);
    }
    return votes;
}

//! Bundle identities in list order, as strings so a failure prints something
//! readable rather than a static assertion about operator<<.
std::vector<std::string> BundleOrder(const DrivechainState& state)
{
    std::vector<std::string> order;
    for (const PendingWithdrawal& bundle : *state.GetPendingWithdrawals(SLOT)) {
        order.push_back(bundle.m6id.ToString());
    }
    return order;
}

std::vector<std::string> BundleOrder(const std::vector<Txid>& ids)
{
    std::vector<std::string> order;
    for (const Txid& id : ids) {
        order.push_back(id.ToString());
    }
    return order;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_diff_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(new_proposal_round_trip)
{
    BlockDiff diff;
    diff.coinbase.msgs.push_back(NewSidechainProposal{.sidechain = MakeProposal(SLOT, "alpha")});
    CheckRoundTrip(diff, DrivechainState{});
}

BOOST_AUTO_TEST_CASE(ack_without_activation_round_trip)
{
    DrivechainState state;
    const Sidechain proposal{MakeProposal(SLOT, "alpha")};
    state.PutProposal(proposal);

    BlockDiff diff;
    diff.coinbase.msgs.push_back(AckSidechainProposal{
        .id = proposal.Id(),
        .effect = AckSidechainProposal::Effect::NO_ACTIVATION,
    });

    DrivechainState applied{state};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    BOOST_CHECK_EQUAL(applied.FindProposal(proposal.Id())->vote_count, 1);
    BOOST_CHECK(!applied.IsActive(SLOT));

    CheckRoundTrip(diff, state);
}

BOOST_AUTO_TEST_CASE(activation_round_trip)
{
    DrivechainState state;
    const Sidechain proposal{MakeProposal(SLOT, "alpha")};
    state.PutProposal(proposal);

    BlockDiff diff;
    diff.coinbase.msgs.push_back(AckSidechainProposal{
        .id = proposal.Id(),
        .effect = AckSidechainProposal::Effect::SLOT_ACTIVATION,
    });

    DrivechainState applied{state};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    BOOST_CHECK(applied.IsActive(SLOT));
    BOOST_CHECK_EQUAL(applied.FindActiveSidechain(SLOT)->activation_height, HEIGHT);
    // Activation moves the entry out of the proposal list.
    BOOST_CHECK(applied.FindProposal(proposal.Id()) == nullptr);
    // ... and gives the slot an empty withdrawal list.
    BOOST_REQUIRE(applied.GetPendingWithdrawals(SLOT) != nullptr);

    CheckRoundTrip(diff, state);
}

BOOST_AUTO_TEST_CASE(overwrite_round_trip_restores_the_displaced_sidechain)
{
    DrivechainState state;
    Sidechain incumbent{MakeProposal(SLOT, "alpha")};
    incumbent.activation_height = 10;
    state.ActivateSidechain(incumbent);

    const Sidechain challenger{MakeProposal(SLOT, "beta")};
    state.PutProposal(challenger);

    BlockDiff diff;
    diff.coinbase.msgs.push_back(AckSidechainProposal{
        .id = challenger.Id(),
        .effect = AckSidechainProposal::Effect::REPLACE_ACTIVE,
        .replaced = incumbent,
    });

    DrivechainState applied{state};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    BOOST_CHECK(applied.FindActiveSidechain(SLOT)->description == challenger.description);

    // Undo has to put the incumbent back, which it can only do because the
    // diff carried it.
    CheckRoundTrip(diff, state);
}

BOOST_AUTO_TEST_CASE(propose_bundle_round_trip)
{
    BlockDiff diff;
    diff.coinbase.msgs.push_back(ProposeBundle{.slot = SLOT, .m6id = BundleId(9)});

    DrivechainState applied{ActiveWithBundles({3, 4})};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    // BIP-300 M3: a proposed bundle starts with one upvote, appended last.
    const std::vector<uint16_t> expected{3, 4, 1};
    const std::vector<uint16_t> votes{VoteCounts(applied)};
    BOOST_CHECK_EQUAL_COLLECTIONS(votes.begin(), votes.end(), expected.begin(), expected.end());

    CheckRoundTrip(diff, ActiveWithBundles({3, 4}));
}

BOOST_AUTO_TEST_CASE(upvote_round_trip)
{
    AckBundles ack;
    ack.actions[SLOT] = AckBundles::Action{
        .kind = AckBundles::Action::Kind::UPVOTE,
        .upvoted = BundleId(1),
        .downvoted = {BundleId(2)},
    };
    BlockDiff diff;
    diff.coinbase.msgs.push_back(ack);

    DrivechainState applied{ActiveWithBundles({3, 4})};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    const std::vector<uint16_t> expected{4, 3};
    const std::vector<uint16_t> votes{VoteCounts(applied)};
    BOOST_CHECK_EQUAL_COLLECTIONS(votes.begin(), votes.end(), expected.begin(), expected.end());

    CheckRoundTrip(diff, ActiveWithBundles({3, 4}));
}

BOOST_AUTO_TEST_CASE(downvotes_saturate_and_undo_stays_exact)
{
    // The bundle at zero must not go negative on apply, and must not be handed
    // a vote it never lost on undo. It is therefore absent from `downvoted`,
    // which is the whole reason that list is recorded rather than recomputed.
    AckBundles ack;
    ack.actions[SLOT] = AckBundles::Action{
        .kind = AckBundles::Action::Kind::UPVOTE,
        .upvoted = BundleId(1),
        .downvoted = {BundleId(2)},
    };
    BlockDiff diff;
    diff.coinbase.msgs.push_back(ack);

    DrivechainState applied{ActiveWithBundles({3, 1, 0})};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    const std::vector<uint16_t> expected{4, 0, 0};
    const std::vector<uint16_t> votes{VoteCounts(applied)};
    BOOST_CHECK_EQUAL_COLLECTIONS(votes.begin(), votes.end(), expected.begin(), expected.end());

    CheckRoundTrip(diff, ActiveWithBundles({3, 1, 0}));
}

BOOST_AUTO_TEST_CASE(alarm_round_trip)
{
    AckBundles ack;
    ack.actions[SLOT] = AckBundles::Action{
        .kind = AckBundles::Action::Kind::ALARM,
        .downvoted = {BundleId(1), BundleId(2)},
    };
    BlockDiff diff;
    diff.coinbase.msgs.push_back(ack);

    // The third bundle is already at zero, so it is not in `downvoted`.
    DrivechainState applied{ActiveWithBundles({3, 1, 0})};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    const std::vector<uint16_t> expected{2, 0, 0};
    const std::vector<uint16_t> votes{VoteCounts(applied)};
    BOOST_CHECK_EQUAL_COLLECTIONS(votes.begin(), votes.end(), expected.begin(), expected.end());

    CheckRoundTrip(diff, ActiveWithBundles({3, 1, 0}));
}

BOOST_AUTO_TEST_CASE(upvote_undo_refuses_to_underflow)
{
    AckBundles ack;
    ack.actions[SLOT] = AckBundles::Action{
        .kind = AckBundles::Action::Kind::UPVOTE,
        .upvoted = BundleId(1),
    };
    BlockDiff diff;
    diff.coinbase.msgs.push_back(ack);

    // Undoing an upvote against a bundle that has no vote to give back means
    // the diff and the state disagree. Refuse rather than wrap around.
    DrivechainState state{ActiveWithBundles({0})};
    UndoError error{};
    BOOST_CHECK(!diff.Undo(state, error));
    BOOST_CHECK(error == UndoError::BUNDLE_VOTE_COUNT_UNDERFLOW);
}

BOOST_AUTO_TEST_CASE(failed_proposals_round_trip)
{
    DrivechainState state;
    const Sidechain doomed{MakeProposal(SLOT, "alpha")};
    const Sidechain surviving{MakeProposal(SLOT, "beta")};
    state.PutProposal(doomed);
    state.PutProposal(surviving);

    BlockDiff diff;
    diff.coinbase.failed_proposals.removed.push_back(doomed);

    DrivechainState applied{state};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    BOOST_CHECK(applied.FindProposal(doomed.Id()) == nullptr);
    BOOST_CHECK(applied.FindProposal(surviving.Id()) != nullptr);

    CheckRoundTrip(diff, state);
}

BOOST_AUTO_TEST_CASE(failed_bundles_restore_their_positions)
{
    // An M4 votes for a bundle by position, so removing the middle two and
    // putting them back has to leave the list in exactly its old order.
    BlockDiff diff;
    auto& failed{diff.coinbase.failed_bundles.removed[SLOT]};
    failed[1] = PendingWithdrawal{.m6id = BundleId(2), .vote_count = 4, .proposal_height = 20};
    failed[2] = PendingWithdrawal{.m6id = BundleId(3), .vote_count = 5, .proposal_height = 20};

    DrivechainState applied{ActiveWithBundles({3, 4, 5, 6})};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    const std::vector<std::string> expected{BundleOrder({BundleId(1), BundleId(4)})};
    const std::vector<std::string> order{BundleOrder(applied)};
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());

    CheckRoundTrip(diff, ActiveWithBundles({3, 4, 5, 6}));
}

BOOST_AUTO_TEST_CASE(first_deposit_round_trip)
{
    DrivechainState state{ActiveWithBundles({})};

    M5Diff m5;
    m5.ctips[SLOT] = TreasuryChange{.new_ctip = MakeCtip(1, 1000), .had_previous = false};
    BlockDiff diff;
    diff.txs.push_back(m5);

    DrivechainState applied{state};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    BOOST_REQUIRE(applied.GetCtip(SLOT) != nullptr);
    BOOST_CHECK_EQUAL(applied.GetCtip(SLOT)->value, 1000);

    // Undoing the first ever deposit removes the pointer rather than
    // restoring one.
    CheckRoundTrip(diff, state);
}

BOOST_AUTO_TEST_CASE(later_deposit_round_trip)
{
    DrivechainState state{ActiveWithBundles({})};
    state.PutCtip(SLOT, MakeCtip(1, 1000));

    M5Diff m5;
    m5.ctips[SLOT] = TreasuryChange{
        .new_ctip = MakeCtip(2, 2500),
        .had_previous = true,
        .previous = MakeCtip(1, 1000),
    };
    BlockDiff diff;
    diff.txs.push_back(m5);

    CheckRoundTrip(diff, state);
}

BOOST_AUTO_TEST_CASE(withdrawal_restores_the_bundle_at_its_index)
{
    DrivechainState state{ActiveWithBundles({3, 4, 5})};
    state.PutCtip(SLOT, MakeCtip(1, 1000));

    M6Diff m6;
    m6.slot = SLOT;
    m6.ctip = TreasuryChange{
        .new_ctip = MakeCtip(2, 400),
        .had_previous = true,
        .previous = MakeCtip(1, 1000),
    };
    m6.removed_index = 1;
    m6.removed = PendingWithdrawal{.m6id = BundleId(2), .vote_count = 4, .proposal_height = 20};

    BlockDiff diff;
    diff.txs.push_back(m6);

    DrivechainState applied{state};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    const std::vector<std::string> expected{BundleOrder({BundleId(1), BundleId(3)})};
    const std::vector<std::string> order{BundleOrder(applied)};
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
    BOOST_CHECK_EQUAL(applied.GetCtip(SLOT)->value, 400);

    // Re-inserting at the end instead of at index 1 would silently renumber
    // every bundle after it, and with them every pending M4 vote.
    CheckRoundTrip(diff, state);
}

BOOST_AUTO_TEST_CASE(whole_block_round_trip)
{
    // A block that does several things at once, to pin the ordering: undo runs
    // every step in reverse, or the withdrawal below would be undone into a
    // slot whose activation had already been rolled back.
    DrivechainState state;
    const Sidechain proposal{MakeProposal(SLOT, "alpha")};
    state.PutProposal(proposal);

    BlockDiff diff;
    diff.coinbase.msgs.push_back(AckSidechainProposal{
        .id = proposal.Id(),
        .effect = AckSidechainProposal::Effect::SLOT_ACTIVATION,
    });
    diff.coinbase.msgs.push_back(ProposeBundle{.slot = SLOT, .m6id = BundleId(1)});
    diff.coinbase.msgs.push_back(NewSidechainProposal{.sidechain = MakeProposal(2, "beta")});

    M5Diff m5;
    m5.ctips[SLOT] = TreasuryChange{.new_ctip = MakeCtip(1, 1000), .had_previous = false};
    diff.txs.push_back(m5);

    DrivechainState applied{state};
    BOOST_REQUIRE(diff.Apply(applied, HEIGHT));
    BOOST_CHECK(applied.IsActive(SLOT));
    BOOST_CHECK_EQUAL(applied.GetPendingWithdrawals(SLOT)->size(), 1U);
    BOOST_CHECK_EQUAL(applied.Proposals().size(), 1U);

    CheckRoundTrip(diff, state);
}

BOOST_AUTO_TEST_SUITE_END()
