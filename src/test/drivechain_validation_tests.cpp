// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/messages.h>
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

BOOST_AUTO_TEST_SUITE_END()
