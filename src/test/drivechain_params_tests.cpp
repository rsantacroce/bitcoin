// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/params.h>
#include <drivechain/params.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace drivechain;

BOOST_FIXTURE_TEST_SUITE(drivechain_params_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(mainnet_constants)
{
    // Pinned against BIP-300's Constants section. These are consensus, and a
    // typo in one of them is a chain split rather than a bug.
    BOOST_CHECK_EQUAL(MAINNET_THRESHOLDS.withdrawal_bundle_max_age, 26300);
    BOOST_CHECK_EQUAL(MAINNET_THRESHOLDS.withdrawal_bundle_inclusion_threshold, 13150);
    BOOST_CHECK_EQUAL(MAINNET_THRESHOLDS.used_slot_proposal_max_age, 26300);
    BOOST_CHECK_EQUAL(MAINNET_THRESHOLDS.used_slot_activation_threshold, 13150);
    BOOST_CHECK_EQUAL(MAINNET_THRESHOLDS.unused_slot_proposal_max_age, 2016);
    BOOST_CHECK_EQUAL(MAINNET_THRESHOLDS.unused_slot_activation_threshold, 1815);
}

BOOST_AUTO_TEST_CASE(vote_thresholds_are_strict)
{
    // "Reaches 13150 or greater" is how both prior drafts describe this in
    // prose. It is wrong: the comparison is strict, so a bundle needs 13151.
    // The difference is a full block of hashrate approval.
    BOOST_CHECK(!MAINNET_THRESHOLDS.BundleIsPayable(13149));
    BOOST_CHECK(!MAINNET_THRESHOLDS.BundleIsPayable(13150));
    BOOST_CHECK(MAINNET_THRESHOLDS.BundleIsPayable(13151));

    // Same for claiming an empty slot: 1815 acks is not enough, 1816 is.
    BOOST_CHECK(!MAINNET_THRESHOLDS.ProposalActivates(1815, 2016, /*slot_is_used=*/false));
    BOOST_CHECK(MAINNET_THRESHOLDS.ProposalActivates(1816, 2016, /*slot_is_used=*/false));

    // And for overwriting an occupied one.
    BOOST_CHECK(!MAINNET_THRESHOLDS.ProposalActivates(13150, 26300, /*slot_is_used=*/true));
    BOOST_CHECK(MAINNET_THRESHOLDS.ProposalActivates(13151, 26300, /*slot_is_used=*/true));
}

BOOST_AUTO_TEST_CASE(age_windows_are_inclusive)
{
    // A proposal at exactly its maximum age can still activate; one block
    // later it cannot.
    BOOST_CHECK(MAINNET_THRESHOLDS.ProposalActivates(1816, 2016, /*slot_is_used=*/false));
    BOOST_CHECK(!MAINNET_THRESHOLDS.ProposalActivates(1816, 2017, /*slot_is_used=*/false));

    BOOST_CHECK(!MAINNET_THRESHOLDS.BundleExpired(26300));
    BOOST_CHECK(MAINNET_THRESHOLDS.BundleExpired(26301));
}

BOOST_AUTO_TEST_CASE(a_proposal_fails_once_it_cannot_win)
{
    // BIP-300 only discards a proposal when it runs out of window. The
    // reference implementation also discards one that can no longer reach the
    // threshold in the blocks remaining, which is consensus-visible: it
    // decides when a later ack is ignored, and when the same proposal may be
    // made afresh.
    //
    // For an empty mainnet slot the window is 2016 and the bar is 1815, so a
    // proposal can afford 201 blocks without an ack.
    constexpr int32_t MAX_FAILS{2016 - 1815};

    // Acked in every block so far: still alive, however old.
    BOOST_CHECK(!MAINNET_THRESHOLDS.ProposalFailed(/*vote_count=*/2000, /*age=*/2000, false));

    // 201 missed blocks is one too many, and only counts once the proposal is
    // old enough for the shortfall to be decisive.
    BOOST_CHECK(!MAINNET_THRESHOLDS.ProposalFailed(/*vote_count=*/0, /*age=*/MAX_FAILS, false));
    BOOST_CHECK(MAINNET_THRESHOLDS.ProposalFailed(/*vote_count=*/0, /*age=*/MAX_FAILS + 1, false));

    // A proposal with 100 acks by block 301 has missed 201, so it is done even
    // though 1715 blocks of window remain.
    BOOST_CHECK(MAINNET_THRESHOLDS.ProposalFailed(/*vote_count=*/100, /*age=*/301, false));
    BOOST_CHECK(!MAINNET_THRESHOLDS.ProposalFailed(/*vote_count=*/101, /*age=*/301, false));

    // Running out of window fails it regardless of votes.
    BOOST_CHECK(!MAINNET_THRESHOLDS.ProposalFailed(/*vote_count=*/2016, /*age=*/2016, false));
    BOOST_CHECK(MAINNET_THRESHOLDS.ProposalFailed(/*vote_count=*/2016, /*age=*/2017, false));
}

BOOST_AUTO_TEST_CASE(test_presets_keep_the_same_shape)
{
    // The short presets exist so a test network can run a whole cycle quickly.
    // They must still be usable: a bar no higher than its window, and a bundle
    // that can actually reach payable.
    for (const Thresholds& thresholds : {SHORT_THRESHOLDS, DRYNET_THRESHOLDS, MAINNET_THRESHOLDS}) {
        BOOST_CHECK(thresholds.unused_slot_activation_threshold < thresholds.unused_slot_proposal_max_age);
        BOOST_CHECK(thresholds.used_slot_activation_threshold < thresholds.used_slot_proposal_max_age);
        BOOST_CHECK(thresholds.withdrawal_bundle_inclusion_threshold < thresholds.withdrawal_bundle_max_age);

        // A bundle gains at most one vote per block and starts at one, so it
        // must be able to cross the bar before its window closes.
        BOOST_CHECK(thresholds.BundleIsPayable(thresholds.withdrawal_bundle_inclusion_threshold + 1));
        BOOST_CHECK(!thresholds.BundleExpired(thresholds.withdrawal_bundle_inclusion_threshold + 1));
    }
}

BOOST_AUTO_TEST_CASE(no_network_activates_by_default)
{
    // Choosing an activation is a deployment decision nobody has made: BIP-300
    // says it deploys "when/if a majority of hashrate runs the enforcer
    // client", which is a signalling mechanism rather than a height. Until
    // that is settled the rules are off everywhere, including regtest, where
    // a test turns them on with -testactivationheight=drivechain@N.
    BOOST_CHECK_EQUAL(CChainParams::Main()->GetConsensus().drivechain_activation_height, drivechain::NOT_DEPLOYED);
    BOOST_CHECK_EQUAL(CChainParams::TestNet()->GetConsensus().drivechain_activation_height, drivechain::NOT_DEPLOYED);
    BOOST_CHECK_EQUAL(CChainParams::TestNet4()->GetConsensus().drivechain_activation_height, drivechain::NOT_DEPLOYED);
    BOOST_CHECK_EQUAL(CChainParams::SigNet({})->GetConsensus().drivechain_activation_height, drivechain::NOT_DEPLOYED);
    BOOST_CHECK_EQUAL(CChainParams::RegTest({})->GetConsensus().drivechain_activation_height, drivechain::NOT_DEPLOYED);

    // Regtest still carries the short thresholds, so a test that turns the
    // rules on gets a whole activation and withdrawal cycle in seconds.
    BOOST_CHECK(CChainParams::RegTest({})->GetConsensus().drivechain_thresholds == SHORT_THRESHOLDS);
}

BOOST_AUTO_TEST_CASE(regtest_can_set_the_activation_height)
{
    CChainParams::RegTestOptions options;
    options.activation_heights[Consensus::BuriedDeployment::DEPLOYMENT_DRIVECHAIN] = 42;
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().drivechain_activation_height, 42);
}

BOOST_AUTO_TEST_CASE(mainnet_carries_the_specified_thresholds)
{
    // Stated even though the rules are off, so that turning them on is a
    // one-line change rather than an invitation to invent constants.
    const auto mainnet{CChainParams::Main()};
    BOOST_CHECK(mainnet->GetConsensus().drivechain_thresholds == MAINNET_THRESHOLDS);
}

BOOST_AUTO_TEST_SUITE_END()
