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
    state.SetBestBlock(uint256{42});
    return state;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_db_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(fresh_database_has_no_state)
{
    const auto db{MakeDB()};
    DrivechainState state;
    BOOST_CHECK(!db->ReadState(state));
}

BOOST_AUTO_TEST_CASE(state_round_trips)
{
    const auto db{MakeDB()};
    const DrivechainState written{PopulatedState()};
    db->Flush(written);

    DrivechainState read;
    BOOST_REQUIRE(db->ReadState(read));
    BOOST_CHECK(read == written);
    // The state names the block it describes, so reading it back tells a node
    // where it is without a second key that could disagree.
    BOOST_CHECK(read.GetBestBlock() == uint256{42});
}

BOOST_AUTO_TEST_CASE(flush_replaces_the_previous_state)
{
    const auto db{MakeDB()};
    db->Flush(PopulatedState());

    // Writing the state whole means a later flush cannot leave stale entries
    // from an earlier one behind.
    DrivechainState empty;
    empty.SetBestBlock(uint256{2});
    db->Flush(empty);

    DrivechainState read;
    BOOST_REQUIRE(db->ReadState(read));
    BOOST_CHECK(read == empty);
    BOOST_CHECK(read.GetBestBlock() == uint256{2});
}

BOOST_AUTO_TEST_CASE(block_diffs_round_trip_and_can_be_dropped)
{
    const auto db{MakeDB()};
    const uint256 block_hash{uint256{9}};

    BlockDiff diff;
    diff.coinbase.msgs.push_back(NewSidechainProposal{.sidechain = MakeProposal(2, "beta")});

    std::map<uint256, BlockDiff> store;
    store[block_hash] = diff;
    db->Flush(PopulatedState(), store);

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
    db->Flush(PopulatedState(), {}, {block_hash});
    BOOST_CHECK(!db->ReadBlockDiff(block_hash, read));
}

BOOST_AUTO_TEST_CASE(the_state_names_its_own_block)
{
    // The block a state describes travels inside it, so a reader can never
    // find the two disagreeing: there is only one thing to read.
    const auto db{MakeDB()};
    DrivechainState written{PopulatedState()};

    written.SetBestBlock(uint256{1});
    db->Flush(written);
    DrivechainState read;
    BOOST_REQUIRE(db->ReadState(read));
    BOOST_CHECK(read.GetBestBlock() == uint256{1});

    written.SetBestBlock(uint256{2});
    db->Flush(written);
    BOOST_REQUIRE(db->ReadState(read));
    BOOST_CHECK(read.GetBestBlock() == uint256{2});

    // And two states that differ only in the block they describe are not equal,
    // so the reorg invariant covers it like everything else.
    DrivechainState other{read};
    other.SetBestBlock(uint256{3});
    BOOST_CHECK(!(other == read));
}

BOOST_AUTO_TEST_SUITE_END()
