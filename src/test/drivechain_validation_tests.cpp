// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/diff.h>
#include <drivechain/messages.h>
#include <drivechain/params.h>
#include <drivechain/state.h>
#include <drivechain/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

using namespace drivechain;

namespace {
CScript MessageScript(std::span<const unsigned char> tag, const std::vector<unsigned char>& body)
{
    std::vector<unsigned char> payload{tag.begin(), tag.end()};
    payload.insert(payload.end(), body.begin(), body.end());
    return CScript() << OP_RETURN << payload;
}

CScript M1Script(SlotNum slot, const char* description)
{
    std::vector<unsigned char> body{slot};
    body.insert(body.end(), description, description + strlen(description));
    return MessageScript(M1ProposeSidechain::TAG, body);
}

CScript SlotAndHashScript(std::span<const unsigned char> tag, SlotNum slot, uint8_t seed)
{
    const uint256 hash{seed};
    std::vector<unsigned char> body{slot};
    body.insert(body.end(), hash.begin(), hash.end());
    return MessageScript(tag, body);
}

CScript M4Script(const std::vector<unsigned char>& votes)
{
    std::vector<unsigned char> body{0x01};
    body.insert(body.end(), votes.begin(), votes.end());
    return MessageScript(M4AckBundles::TAG, body);
}

CMutableTransaction Coinbase(const std::vector<CScript>& outputs)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{});
    // A real coinbase pays the subsidy somewhere; the payout output is here so
    // the messages are never at index 0, as they are not in practice.
    tx.vout.emplace_back(5000000000, CScript() << OP_TRUE);
    for (const CScript& script : outputs) {
        tx.vout.emplace_back(0, script);
    }
    return tx;
}

Sidechain MakeProposal(SlotNum slot, const char* description, int32_t height)
{
    Sidechain sidechain;
    sidechain.slot = slot;
    sidechain.description = std::vector<unsigned char>{description, description + strlen(description)};
    sidechain.proposal_height = height;
    return sidechain;
}

M1ProposeSidechain M1(SlotNum slot, const char* description)
{
    return M1ProposeSidechain{
        .slot = slot,
        .description = std::vector<unsigned char>{description, description + strlen(description)},
    };
}

M2AckSidechain M2(const Sidechain& proposal)
{
    return M2AckSidechain{.slot = proposal.slot, .proposal_id = proposal.Id().description_hash};
}

BlockError CollectError(const std::vector<CScript>& outputs)
{
    const CMutableTransaction tx{Coinbase(outputs)};
    CoinbaseMessages messages;
    BlockError error{BlockError::STATE_MISMATCH};
    BOOST_REQUIRE(!CollectCoinbaseMessages(CTransaction{tx}, messages, error));
    return error;
}

CoinbaseMessages CollectOk(const std::vector<CScript>& outputs)
{
    const CMutableTransaction tx{Coinbase(outputs)};
    CoinbaseMessages messages;
    BlockError error{BlockError::STATE_MISMATCH};
    BOOST_REQUIRE(CollectCoinbaseMessages(CTransaction{tx}, messages, error));
    return messages;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_validation_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(non_messages_are_ignored)
{
    // An output that is not a well-formed message is an ordinary script, and
    // the block stays valid.
    const CoinbaseMessages messages{CollectOk({
        CScript() << OP_TRUE,
        CScript() << OP_RETURN << std::vector<unsigned char>{0x01, 0x02},
        TreasuryScript(1),
    })};
    BOOST_CHECK(messages.messages.empty());
    BOOST_CHECK(!messages.has_m4);
}

BOOST_AUTO_TEST_CASE(messages_keep_their_output_index)
{
    // The index breaks ties in the canonical order of pending bundles, so it
    // has to survive collection.
    const CoinbaseMessages messages{CollectOk({
        M1Script(1, "alpha"),
        CScript() << OP_TRUE,
        SlotAndHashScript(M3ProposeBundle::TAG, 1, 7),
    })};
    BOOST_REQUIRE_EQUAL(messages.messages.size(), 2U);
    // Output 0 is the subsidy payout, so the messages are at 1 and 3.
    BOOST_CHECK_EQUAL(messages.messages[0].second, 1U);
    BOOST_CHECK_EQUAL(messages.messages[1].second, 3U);
    BOOST_CHECK(std::holds_alternative<M1ProposeSidechain>(messages.messages[0].first));
    BOOST_CHECK(std::holds_alternative<M3ProposeBundle>(messages.messages[1].first));
}

BOOST_AUTO_TEST_CASE(duplicate_m1_is_only_an_identical_proposal)
{
    // A coinbase may propose two different sidechains for one slot, and one
    // sidechain for two slots. Only an exact repeat is a duplicate: without
    // that rule a miner could reset a proposal's vote count at will.
    BOOST_CHECK_EQUAL(CollectOk({M1Script(1, "alpha"), M1Script(1, "beta")}).messages.size(), 2U);
    BOOST_CHECK_EQUAL(CollectOk({M1Script(1, "alpha"), M1Script(2, "alpha")}).messages.size(), 2U);

    BOOST_CHECK(CollectError({M1Script(1, "alpha"), M1Script(1, "alpha")}) == BlockError::DUPLICATE_M1);
}

BOOST_AUTO_TEST_CASE(duplicate_m2_is_any_second_ack_for_a_slot)
{
    // A slot gets one vote per block, so a second M2 for it invalidates the
    // block whatever it acks -- unlike M1, the content does not matter.
    BOOST_CHECK_EQUAL(CollectOk({SlotAndHashScript(M2AckSidechain::TAG, 1, 1),
                                 SlotAndHashScript(M2AckSidechain::TAG, 2, 1)})
                          .messages.size(),
                      2U);

    BOOST_CHECK(CollectError({SlotAndHashScript(M2AckSidechain::TAG, 1, 1),
                              SlotAndHashScript(M2AckSidechain::TAG, 1, 2)}) == BlockError::DUPLICATE_M2);
}

BOOST_AUTO_TEST_CASE(duplicate_m4_is_any_second_m4)
{
    const CoinbaseMessages messages{CollectOk({M4Script({0x00})})};
    BOOST_CHECK(messages.has_m4);

    // Identical or not, a second M4 invalidates the block: the votes it would
    // cast have no defined order against the first.
    BOOST_CHECK(CollectError({M4Script({0x00}), M4Script({0x00})}) == BlockError::DUPLICATE_M4);
    BOOST_CHECK(CollectError({M4Script({0x00}), M4Script({0x01})}) == BlockError::DUPLICATE_M4);
}

BOOST_AUTO_TEST_CASE(duplicate_m7_is_any_second_accept_for_a_slot)
{
    // BIP-301: at most one block may be blind merge mined per slot per block.
    BOOST_CHECK_EQUAL(CollectOk({SlotAndHashScript(M7BmmAccept::TAG, 1, 1),
                                 SlotAndHashScript(M7BmmAccept::TAG, 2, 1)})
                          .messages.size(),
                      2U);

    BOOST_CHECK(CollectError({SlotAndHashScript(M7BmmAccept::TAG, 1, 1),
                              SlotAndHashScript(M7BmmAccept::TAG, 1, 2)}) == BlockError::DUPLICATE_M7);
}

BOOST_AUTO_TEST_CASE(repeated_m3_is_allowed_through_collection)
{
    // The reference implementation has no duplicate rule for M3 at this stage.
    // Two M3s naming the same bundle are still rejected, but by the rule that
    // a pending bundle cannot be proposed again -- which is checked when the
    // messages are applied, not when they are read.
    BOOST_CHECK_EQUAL(CollectOk({SlotAndHashScript(M3ProposeBundle::TAG, 1, 7),
                                 SlotAndHashScript(M3ProposeBundle::TAG, 1, 7)})
                          .messages.size(),
                      2U);
}

BOOST_AUTO_TEST_CASE(error_strings_are_distinct)
{
    // These reach the user as Core's block-rejection reason, so a shared or
    // missing string would make two different failures indistinguishable.
    const std::vector<BlockError> errors{
        BlockError::DUPLICATE_M1, BlockError::DUPLICATE_M2, BlockError::DUPLICATE_M4,
        BlockError::DUPLICATE_M7, BlockError::STATE_MISMATCH,
    };
    std::set<std::string> seen;
    for (const BlockError error : errors) {
        const std::string reason{BlockErrorString(error)};
        BOOST_CHECK(!reason.empty());
        BOOST_CHECK(reason != "bad-drivechain-unknown");
        BOOST_CHECK(seen.insert(reason).second);
    }
}

BOOST_AUTO_TEST_CASE(m1_creates_a_proposal_with_no_votes)
{
    DrivechainState state;
    const auto diff{HandleM1(M1(1, "alpha"), state, 500)};
    BOOST_REQUIRE(diff.has_value());
    BOOST_CHECK_EQUAL(int{diff->sidechain.slot}, 1);
    BOOST_CHECK_EQUAL(diff->sidechain.vote_count, 0);
    BOOST_CHECK_EQUAL(diff->sidechain.proposal_height, 500);
    BOOST_CHECK_EQUAL(diff->sidechain.activation_height, NO_HEIGHT);
}

BOOST_AUTO_TEST_CASE(m1_repeating_an_existing_proposal_is_ignored)
{
    // Without this an M1 would reset the proposal's accumulated votes, and any
    // miner could wipe any proposal's progress at will.
    DrivechainState state;
    Sidechain existing{MakeProposal(1, "alpha", 100)};
    existing.vote_count = 900;
    state.PutProposal(existing);

    BOOST_CHECK(!HandleM1(M1(1, "alpha"), state, 500).has_value());

    // A different description for the same slot is a different proposal.
    BOOST_CHECK(HandleM1(M1(1, "beta"), state, 500).has_value());
    // As is the same description in a different slot.
    BOOST_CHECK(HandleM1(M1(2, "alpha"), state, 500).has_value());
}

BOOST_AUTO_TEST_CASE(m2_for_an_unknown_proposal_is_ignored)
{
    DrivechainState state;
    const Sidechain proposal{MakeProposal(1, "alpha", 100)};
    BOOST_CHECK(!HandleM2(M2(proposal), state, MAINNET_THRESHOLDS, 500).has_value());

    // The slot is part of the identity, so an ack naming the right hash under
    // the wrong slot finds nothing.
    state.PutProposal(proposal);
    M2AckSidechain wrong_slot{M2(proposal)};
    wrong_slot.slot = 2;
    BOOST_CHECK(!HandleM2(wrong_slot, state, MAINNET_THRESHOLDS, 500).has_value());
    BOOST_CHECK(HandleM2(M2(proposal), state, MAINNET_THRESHOLDS, 500).has_value());
}

BOOST_AUTO_TEST_CASE(m2_cannot_ack_a_proposal_made_in_the_same_block)
{
    // BIP-300 counts an ack only once the proposal sits in an ancestor block.
    // Otherwise a miner could seed a fresh proposal with a vote in the very
    // block that proposed it.
    DrivechainState state;
    const Sidechain proposal{MakeProposal(1, "alpha", 500)};
    state.PutProposal(proposal);

    BOOST_CHECK(!HandleM2(M2(proposal), state, MAINNET_THRESHOLDS, 500).has_value());
    BOOST_CHECK(HandleM2(M2(proposal), state, MAINNET_THRESHOLDS, 501).has_value());
}

BOOST_AUTO_TEST_CASE(m2_activates_an_empty_slot_at_the_bar)
{
    DrivechainState state;
    Sidechain proposal{MakeProposal(1, "alpha", 0)};
    // One ack short of the 90% bar for claiming an empty slot.
    proposal.vote_count = 1814;
    state.PutProposal(proposal);

    // This ack makes 1815, which is not more than 1815.
    const auto no_activation{HandleM2(M2(proposal), state, MAINNET_THRESHOLDS, 2016)};
    BOOST_REQUIRE(no_activation.has_value());
    BOOST_CHECK(no_activation->effect == AckSidechainProposal::Effect::NO_ACTIVATION);

    proposal.vote_count = 1815;
    state.PutProposal(proposal);
    const auto activation{HandleM2(M2(proposal), state, MAINNET_THRESHOLDS, 2016)};
    BOOST_REQUIRE(activation.has_value());
    BOOST_CHECK(activation->effect == AckSidechainProposal::Effect::SLOT_ACTIVATION);

    // One block past the window and the same ack no longer activates.
    BOOST_CHECK(HandleM2(M2(proposal), state, MAINNET_THRESHOLDS, 2017)->effect ==
                AckSidechainProposal::Effect::NO_ACTIVATION);
}

BOOST_AUTO_TEST_CASE(m2_overwriting_a_slot_carries_the_incumbent)
{
    // Overwriting an occupied slot needs only the used-slot bar, which is a
    // bare majority sustained over 26300 blocks rather than the 90% required
    // to claim an empty one.
    DrivechainState state;
    Sidechain incumbent{MakeProposal(1, "alpha", 0)};
    incumbent.activation_height = 10;
    state.ActivateSidechain(incumbent);

    Sidechain challenger{MakeProposal(1, "beta", 0)};
    challenger.vote_count = 13150;
    state.PutProposal(challenger);

    const auto ack{HandleM2(M2(challenger), state, MAINNET_THRESHOLDS, 26300)};
    BOOST_REQUIRE(ack.has_value());
    BOOST_CHECK(ack->effect == AckSidechainProposal::Effect::REPLACE_ACTIVE);
    // Undo has no other way to put the displaced sidechain back.
    BOOST_CHECK(ack->replaced == incumbent);
}

BOOST_AUTO_TEST_CASE(m2_ack_applied_to_state_matches_what_it_says)
{
    // The diff is what actually moves the state, so check the two agree rather
    // than only checking the diff.
    DrivechainState state;
    Sidechain proposal{MakeProposal(1, "alpha", 0)};
    proposal.vote_count = 1815;
    state.PutProposal(proposal);

    const auto ack{HandleM2(M2(proposal), state, MAINNET_THRESHOLDS, 2016)};
    BOOST_REQUIRE(ack.has_value());

    BlockDiff block;
    block.coinbase.msgs.push_back(*ack);
    const DrivechainState before{state};
    BOOST_REQUIRE(block.Apply(state, 2016));

    BOOST_REQUIRE(state.FindActiveSidechain(1) != nullptr);
    BOOST_CHECK_EQUAL(state.FindActiveSidechain(1)->vote_count, 1816);
    BOOST_CHECK_EQUAL(state.FindActiveSidechain(1)->activation_height, 2016);
    BOOST_CHECK(state.FindProposal(proposal.Id()) == nullptr);

    UndoError undo_error{};
    BOOST_REQUIRE(block.Undo(state, undo_error));
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(proposals_fail_when_their_window_closes)
{
    DrivechainState state;
    Sidechain acked_every_block{MakeProposal(1, "alpha", 0)};
    acked_every_block.vote_count = 2016;
    state.PutProposal(acked_every_block);

    // Alive at the last block of its window, gone one block later.
    BOOST_CHECK(CollectFailedProposals(state, MAINNET_THRESHOLDS, 2016).removed.empty());
    BOOST_CHECK_EQUAL(CollectFailedProposals(state, MAINNET_THRESHOLDS, 2017).removed.size(), 1U);
}

BOOST_AUTO_TEST_CASE(proposals_fail_once_they_cannot_win)
{
    // The rule that is in the reference implementation and in neither
    // specification: a proposal that has missed more blocks than its window
    // can spare is done, without waiting for the window to close.
    DrivechainState state;
    Sidechain neglected{MakeProposal(1, "alpha", 0)};
    neglected.vote_count = 100;
    state.PutProposal(neglected);

    // 201 missed blocks is one too many for a 2016 window with an 1815 bar.
    BOOST_CHECK(CollectFailedProposals(state, MAINNET_THRESHOLDS, 300).removed.empty());
    BOOST_CHECK_EQUAL(CollectFailedProposals(state, MAINNET_THRESHOLDS, 301).removed.size(), 1U);
}

BOOST_AUTO_TEST_CASE(a_proposal_for_an_occupied_slot_gets_the_longer_window)
{
    // Overwriting an occupied slot is measured against 26300 blocks, not the
    // 2016 an empty slot allows, so the same proposal survives far longer.
    DrivechainState state;
    Sidechain incumbent{MakeProposal(1, "alpha", 0)};
    incumbent.activation_height = 10;
    state.ActivateSidechain(incumbent);

    Sidechain challenger{MakeProposal(1, "beta", 0)};
    challenger.vote_count = 5000;
    state.PutProposal(challenger);

    BOOST_CHECK(CollectFailedProposals(state, MAINNET_THRESHOLDS, 5000).removed.empty());

    // The same proposal in an empty slot would be long gone by then.
    DrivechainState empty_slot;
    empty_slot.PutProposal(challenger);
    BOOST_CHECK_EQUAL(CollectFailedProposals(empty_slot, MAINNET_THRESHOLDS, 5000).removed.size(), 1U);
}

BOOST_AUTO_TEST_CASE(bundles_age_out_with_their_positions_recorded)
{
    DrivechainState state;
    Sidechain sidechain{MakeProposal(1, "alpha", 0)};
    sidechain.activation_height = 0;
    state.ActivateSidechain(sidechain);

    // Three bundles proposed at different heights, so they expire in turn.
    for (const int32_t proposed_at : {0, 100, 200}) {
        state.ModifyPendingWithdrawals(1)->push_back(PendingWithdrawal{
            .m6id = Txid::FromUint256(uint256{static_cast<uint8_t>(proposed_at / 100 + 1)}),
            .vote_count = 1,
            .proposal_height = proposed_at,
        });
    }

    // Nothing has aged out at exactly the maximum age of the oldest.
    BOOST_CHECK(CollectFailedBundles(state, MAINNET_THRESHOLDS, 26300).removed.empty());

    // One block later the oldest is gone, and it is recorded at index 0.
    const FailedBundles first{CollectFailedBundles(state, MAINNET_THRESHOLDS, 26301)};
    BOOST_REQUIRE_EQUAL(first.removed.count(1), 1U);
    BOOST_REQUIRE_EQUAL(first.removed.at(1).size(), 1U);
    BOOST_CHECK_EQUAL(first.removed.at(1).begin()->first, 0U);

    // Later still, two of them, at the positions they actually hold. An M4
    // votes by position, so undo needs the index rather than the identity.
    const FailedBundles second{CollectFailedBundles(state, MAINNET_THRESHOLDS, 26401)};
    BOOST_REQUIRE_EQUAL(second.removed.at(1).size(), 2U);
    BOOST_CHECK_EQUAL(second.removed.at(1).count(0), 1U);
    BOOST_CHECK_EQUAL(second.removed.at(1).count(1), 1U);
}

BOOST_AUTO_TEST_CASE(expiry_round_trips_through_the_diff)
{
    DrivechainState state;
    Sidechain sidechain{MakeProposal(1, "alpha", 0)};
    sidechain.activation_height = 0;
    state.ActivateSidechain(sidechain);
    for (uint8_t seed{1}; seed <= 3; ++seed) {
        state.ModifyPendingWithdrawals(1)->push_back(PendingWithdrawal{
            .m6id = Txid::FromUint256(uint256{seed}),
            .vote_count = seed,
            .proposal_height = seed == 2 ? 100000 : 0,
        });
    }
    Sidechain doomed{MakeProposal(2, "beta", 0)};
    state.PutProposal(doomed);

    BlockDiff block;
    block.coinbase.failed_proposals = CollectFailedProposals(state, MAINNET_THRESHOLDS, 100000);
    block.coinbase.failed_bundles = CollectFailedBundles(state, MAINNET_THRESHOLDS, 100000);

    const DrivechainState before{state};
    BOOST_REQUIRE(block.Apply(state, 100000));
    // The two at index 0 and 2 aged out; the one proposed at 100000 did not.
    BOOST_REQUIRE_EQUAL(state.GetPendingWithdrawals(1)->size(), 1U);
    BOOST_CHECK(state.GetPendingWithdrawals(1)->at(0).m6id == Txid::FromUint256(uint256{2}));
    BOOST_CHECK(state.Proposals().empty());

    UndoError undo_error{};
    BOOST_REQUIRE(block.Undo(state, undo_error));
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_SUITE_END()
