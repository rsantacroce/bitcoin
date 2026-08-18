// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <drivechain/db.h>
#include <drivechain/diff.h>
#include <drivechain/messages.h>
#include <drivechain/state.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <map>
#include <memory>
#include <vector>

using namespace drivechain;

namespace {
constexpr SlotNum SLOT{1};

std::unique_ptr<DrivechainDB> MakeDB()
{
    return std::make_unique<DrivechainDB>(DBParams{
        .path = "",
        .cache_bytes = 1 << 20,
        .memory_only = true,
        .wipe_data = false,
        .obfuscate = false,
    });
}

Sidechain MakeProposal(SlotNum slot, const char* description)
{
    Sidechain sidechain;
    sidechain.slot = slot;
    sidechain.description = std::vector<unsigned char>{description, description + strlen(description)};
    sidechain.proposal_height = 100;
    return sidechain;
}

//! A state with something in every one of its four containers, so a field
//! missing from serialization cannot pass unnoticed.
DrivechainState PopulatedState()
{
    DrivechainState state;
    state.PutProposal(MakeProposal(7, "pending"));

    Sidechain active{MakeProposal(SLOT, "alpha")};
    active.activation_height = 200;
    active.vote_count = 1815;
    state.ActivateSidechain(active);
    state.ModifyPendingWithdrawals(SLOT)->push_back(PendingWithdrawal{
        .m6id = Txid::FromUint256(uint256{3}),
        .vote_count = 13151,
        .proposal_height = 250,
    });
    state.PutCtip(SLOT, Ctip{.outpoint = COutPoint{Txid::FromUint256(uint256{4}), 1}, .value = 21000});
    return state;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_db_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(fresh_database_has_no_state)
{
    const auto db{MakeDB()};
    DrivechainState state;
    uint256 tip;
    BOOST_CHECK(!db->ReadState(state, tip));
}

BOOST_AUTO_TEST_CASE(state_round_trips)
{
    const auto db{MakeDB()};
    const DrivechainState written{PopulatedState()};
    const uint256 tip{uint256{42}};
    db->Flush(written, tip);

    DrivechainState read;
    uint256 read_tip;
    BOOST_REQUIRE(db->ReadState(read, read_tip));
    BOOST_CHECK(read == written);
    BOOST_CHECK(read_tip == tip);
}

BOOST_AUTO_TEST_CASE(flush_replaces_the_previous_state)
{
    const auto db{MakeDB()};
    db->Flush(PopulatedState(), uint256{1});

    // Writing the state whole means a later flush cannot leave stale entries
    // from an earlier one behind.
    const DrivechainState empty;
    db->Flush(empty, uint256{2});

    DrivechainState read;
    uint256 read_tip;
    BOOST_REQUIRE(db->ReadState(read, read_tip));
    BOOST_CHECK(read == empty);
    BOOST_CHECK(read_tip == uint256{2});
}

BOOST_AUTO_TEST_CASE(block_diffs_round_trip_and_can_be_dropped)
{
    const auto db{MakeDB()};
    const uint256 block_hash{uint256{9}};

    BlockDiff diff;
    diff.coinbase.msgs.push_back(NewSidechainProposal{.sidechain = MakeProposal(2, "beta")});

    std::map<uint256, BlockDiff> store;
    store[block_hash] = diff;
    db->Flush(PopulatedState(), block_hash, store);

    BlockDiff read;
    BOOST_REQUIRE(db->ReadBlockDiff(block_hash, read));
    // Compare by what it does, as elsewhere: the diff types have no equality.
    DrivechainState from_original;
    DrivechainState from_read;
    BOOST_REQUIRE(diff.Apply(from_original, 300));
    BOOST_REQUIRE(read.Apply(from_read, 300));
    BOOST_CHECK(from_original == from_read);

    BOOST_CHECK(!db->ReadBlockDiff(uint256{10}, read));

    // Pruning a diff the chain can no longer reorg through.
    db->Flush(PopulatedState(), block_hash, {}, {block_hash});
    BOOST_CHECK(!db->ReadBlockDiff(block_hash, read));
}

BOOST_AUTO_TEST_CASE(state_and_tip_are_written_together)
{
    // The state is only meaningful alongside the block it reflects: without
    // the tip there is no way to tell what still needs connecting. One batch
    // carries both, so a reader can never see one without the other.
    const auto db{MakeDB()};
    const DrivechainState written{PopulatedState()};

    db->Flush(written, uint256{1});
    DrivechainState read;
    uint256 read_tip;
    BOOST_REQUIRE(db->ReadState(read, read_tip));
    BOOST_CHECK(read_tip == uint256{1});

    db->Flush(written, uint256{2});
    BOOST_REQUIRE(db->ReadState(read, read_tip));
    BOOST_CHECK(read_tip == uint256{2});
}

BOOST_AUTO_TEST_SUITE_END()
