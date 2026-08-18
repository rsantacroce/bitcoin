// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/messages.h>
#include <drivechain/state.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <vector>

using namespace drivechain;

namespace {
Sidechain MakeProposal(SlotNum slot, const char* description, int32_t height = 100)
{
    Sidechain sidechain;
    sidechain.slot = slot;
    sidechain.description = std::vector<unsigned char>{description, description + strlen(description)};
    sidechain.vote_count = 0;
    sidechain.proposal_height = height;
    return sidechain;
}

PendingWithdrawal MakeBundle(uint8_t seed, uint16_t votes = 1, int32_t height = 100)
{
    return PendingWithdrawal{
        .m6id = Txid::FromUint256(uint256{seed}),
        .vote_count = votes,
        .proposal_height = height,
    };
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_state_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(proposal_id_distinguishes_slot_and_description)
{
    const Sidechain a{MakeProposal(1, "alpha")};
    const Sidechain b{MakeProposal(1, "beta")};
    const Sidechain c{MakeProposal(2, "alpha")};

    // The description hash is sha256d(D), exactly what an M2 votes for.
    BOOST_CHECK(a.Id().description_hash == Hash(a.description));

    // Same slot, different proposals; and same description in another slot is
    // a different proposal again. Both must have independent vote counts.
    BOOST_CHECK(a.Id() != b.Id());
    BOOST_CHECK(a.Id() != c.Id());
}

BOOST_AUTO_TEST_CASE(proposals_are_independent_per_slot_and_description)
{
    DrivechainState state;
    state.PutProposal(MakeProposal(1, "alpha"));
    state.PutProposal(MakeProposal(1, "beta"));
    BOOST_CHECK_EQUAL(state.Proposals().size(), 2U);

    Sidechain voted{MakeProposal(1, "alpha")};
    voted.vote_count = 5;
    state.PutProposal(voted);
    BOOST_CHECK_EQUAL(state.Proposals().size(), 2U);
    BOOST_REQUIRE(state.FindProposal(voted.Id()) != nullptr);
    BOOST_CHECK_EQUAL(state.FindProposal(voted.Id())->vote_count, 5);

    BOOST_CHECK(state.EraseProposal(voted.Id()));
    BOOST_CHECK(!state.EraseProposal(voted.Id()));
    BOOST_CHECK_EQUAL(state.Proposals().size(), 1U);
}

BOOST_AUTO_TEST_CASE(activation_creates_and_removes_the_withdrawal_list)
{
    DrivechainState state;
    BOOST_CHECK(state.GetPendingWithdrawals(1) == nullptr);
    BOOST_CHECK(!state.IsActive(1));

    Sidechain sidechain{MakeProposal(1, "alpha")};
    sidechain.activation_height = 200;
    state.ActivateSidechain(sidechain);
    BOOST_CHECK(state.IsActive(1));
    BOOST_REQUIRE(state.GetPendingWithdrawals(1) != nullptr);
    BOOST_CHECK(state.GetPendingWithdrawals(1)->empty());

    // An overwrite keeps the outgoing sidechain's bundles, per the reference
    // implementation.
    state.ModifyPendingWithdrawals(1)->push_back(MakeBundle(1));
    Sidechain replacement{MakeProposal(1, "beta")};
    replacement.activation_height = 300;
    state.ActivateSidechain(replacement);
    BOOST_REQUIRE(state.FindActiveSidechain(1) != nullptr);
    BOOST_CHECK(*state.FindActiveSidechain(1) == replacement);
    BOOST_CHECK_EQUAL(state.GetPendingWithdrawals(1)->size(), 1U);

    BOOST_CHECK(state.DeactivateSidechain(1));
    BOOST_CHECK(!state.IsActive(1));
    BOOST_CHECK(state.GetPendingWithdrawals(1) == nullptr);
    BOOST_CHECK(!state.DeactivateSidechain(1));
}

BOOST_AUTO_TEST_CASE(active_slots_are_ascending)
{
    // An M4's votes index into this vector, so the order is consensus
    // critical and sparse slot numbers must not shift it.
    DrivechainState state;
    for (const SlotNum slot : {200, 3, 17}) {
        Sidechain sidechain{MakeProposal(slot, "s")};
        sidechain.activation_height = 1;
        state.ActivateSidechain(sidechain);
    }
    const std::vector<SlotNum> expected{3, 17, 200};
    const std::vector<SlotNum> slots{state.ActiveSlots()};
    BOOST_CHECK_EQUAL_COLLECTIONS(slots.begin(), slots.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(one_treasury_per_slot)
{
    DrivechainState state;
    BOOST_CHECK(state.GetCtip(1) == nullptr);

    const Ctip first{.outpoint = COutPoint{Txid::FromUint256(uint256{1}), 0}, .value = 1000};
    state.PutCtip(1, first);
    BOOST_REQUIRE(state.GetCtip(1) != nullptr);
    BOOST_CHECK(*state.GetCtip(1) == first);

    // Replacing is how a deposit or withdrawal moves the treasury forward.
    // There is never a second entry for the same slot.
    const Ctip second{.outpoint = COutPoint{Txid::FromUint256(uint256{2}), 0}, .value = 1500};
    state.PutCtip(1, second);
    BOOST_CHECK(*state.GetCtip(1) == second);

    BOOST_CHECK(state.EraseCtip(1));
    BOOST_CHECK(state.GetCtip(1) == nullptr);
    BOOST_CHECK(!state.EraseCtip(1));
}

BOOST_AUTO_TEST_CASE(serialization_round_trips)
{
    Sidechain sidechain{MakeProposal(9, "alpha", 4242)};
    sidechain.vote_count = 77;
    sidechain.activation_height = 5000;

    DataStream stream;
    stream << sidechain;
    Sidechain restored;
    stream >> restored;
    BOOST_CHECK(restored == sidechain);

    // A pending proposal carries NO_HEIGHT, which has to survive the round
    // trip as a negative rather than wrapping.
    const Sidechain pending{MakeProposal(0, "")};
    BOOST_CHECK_EQUAL(pending.activation_height, NO_HEIGHT);
    DataStream pending_stream;
    pending_stream << pending;
    Sidechain restored_pending;
    pending_stream >> restored_pending;
    BOOST_CHECK(restored_pending == pending);

    const PendingWithdrawal bundle{MakeBundle(3, 13150, 900)};
    DataStream bundle_stream;
    bundle_stream << bundle;
    PendingWithdrawal restored_bundle;
    bundle_stream >> restored_bundle;
    BOOST_CHECK(restored_bundle == bundle);

    const Ctip ctip{.outpoint = COutPoint{Txid::FromUint256(uint256{7}), 3}, .value = 21000000};
    DataStream ctip_stream;
    ctip_stream << ctip;
    Ctip restored_ctip;
    ctip_stream >> restored_ctip;
    BOOST_CHECK(restored_ctip == ctip);
}

BOOST_AUTO_TEST_CASE(state_equality_covers_every_field)
{
    // The reorg invariant is stated as state equality, so equality has to see
    // everything a block can change.
    DrivechainState empty;
    DrivechainState state;
    BOOST_CHECK(state == empty);

    state.PutProposal(MakeProposal(1, "alpha"));
    BOOST_CHECK(!(state == empty));

    DrivechainState other;
    other.PutProposal(MakeProposal(1, "alpha"));
    BOOST_CHECK(state == other);

    Sidechain sidechain{MakeProposal(2, "beta")};
    sidechain.activation_height = 10;
    state.ActivateSidechain(sidechain);
    other.ActivateSidechain(sidechain);
    BOOST_CHECK(state == other);

    // A pending bundle is part of the state.
    state.ModifyPendingWithdrawals(2)->push_back(MakeBundle(1));
    BOOST_CHECK(!(state == other));
    other.ModifyPendingWithdrawals(2)->push_back(MakeBundle(1));
    BOOST_CHECK(state == other);

    // So is a vote on one.
    state.ModifyPendingWithdrawals(2)->at(0).vote_count = 2;
    BOOST_CHECK(!(state == other));
    other.ModifyPendingWithdrawals(2)->at(0).vote_count = 2;
    BOOST_CHECK(state == other);

    // And so is the treasury.
    state.PutCtip(2, Ctip{.outpoint = COutPoint{Txid::FromUint256(uint256{1}), 0}, .value = 5});
    BOOST_CHECK(!(state == other));
    other.PutCtip(2, Ctip{.outpoint = COutPoint{Txid::FromUint256(uint256{1}), 0}, .value = 5});
    BOOST_CHECK(state == other);
}

BOOST_AUTO_TEST_SUITE_END()
