// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/messages.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

using namespace drivechain;

namespace {
//! Build `OP_RETURN <tag || body>`, the shape every coinbase message takes.
CScript MessageScript(std::span<const unsigned char> tag, std::span<const unsigned char> body)
{
    std::vector<unsigned char> payload{tag.begin(), tag.end()};
    payload.insert(payload.end(), body.begin(), body.end());
    return CScript() << OP_RETURN << payload;
}

std::vector<unsigned char> SlotAndHash(SlotNum slot, const uint256& hash)
{
    std::vector<unsigned char> body{slot};
    body.insert(body.end(), hash.begin(), hash.end());
    return body;
}

//! Build a well-formed M8 scriptPubKey: OP_RETURN OP_PUSHBYTES_68 <tag S H P>.
CScript M8Script(SlotNum slot, const uint256& sidechain_block_hash, const uint256& prev_main_block_hash)
{
    std::vector<unsigned char> bytes{OP_RETURN, M8_SCRIPT_SIZE - 2};
    bytes.insert(bytes.end(), M8BmmRequest::TAG.begin(), M8BmmRequest::TAG.end());
    bytes.push_back(slot);
    bytes.insert(bytes.end(), sidechain_block_hash.begin(), sidechain_block_hash.end());
    bytes.insert(bytes.end(), prev_main_block_hash.begin(), prev_main_block_hash.end());
    return CScript(bytes.begin(), bytes.end());
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_messages_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(treasury_script_roundtrip)
{
    // Every slot round-trips, including the ones a script-number encoding
    // would get wrong: 0 (OP_0), 1..16 (OP_1..OP_16) and everything above 127
    // (sign-padded to two bytes).
    for (int slot{0}; slot <= 0xFF; ++slot) {
        const CScript script{TreasuryScript(static_cast<SlotNum>(slot))};
        BOOST_CHECK_EQUAL(script.size(), TREASURY_SCRIPT_SIZE);
        const auto parsed{ParseTreasuryScript(script)};
        BOOST_REQUIRE(parsed.has_value());
        BOOST_CHECK_EQUAL(int{*parsed}, slot);
    }
}

BOOST_AUTO_TEST_CASE(treasury_script_layout)
{
    const CScript script{TreasuryScript(0x80)};
    const std::vector<unsigned char> expected{OP_NOP5, 0x01, 0x80, OP_TRUE};
    BOOST_CHECK_EQUAL_COLLECTIONS(script.begin(), script.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(treasury_script_rejects_near_misses)
{
    const auto reject{[](const std::vector<unsigned char>& bytes) {
        BOOST_CHECK(!ParseTreasuryScript(CScript(bytes.begin(), bytes.end())).has_value());
    }};

    reject({});
    // Truncated: the trailing OP_TRUE is what keeps this a soft fork.
    reject({OP_NOP5, 0x01, 0x01});
    // Trailing bytes. A longer script that merely begins as a treasury output
    // is an ordinary script; without the length check it would be spendable as
    // a treasury under rules it was never meant to satisfy.
    reject({OP_NOP5, 0x01, 0x01, OP_TRUE, OP_TRUE});
    // A different NOP.
    reject({OP_NOP4, 0x01, 0x01, OP_TRUE});
    // The slot pushed as a script number rather than a raw byte.
    reject({OP_NOP5, OP_1, OP_TRUE});
    // Right shape, wrong push length.
    reject({OP_NOP5, 0x02, 0x01, 0x00, OP_TRUE});
}

BOOST_AUTO_TEST_CASE(op_return_payload)
{
    const std::vector<unsigned char> address{0xDE, 0xAD, 0xBE, 0xEF};
    const auto parsed{ParseOpReturnPayload(CScript() << OP_RETURN << address)};
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK_EQUAL_COLLECTIONS(parsed->begin(), parsed->end(), address.begin(), address.end());

    // An empty push is a payload, just not one any message tag can match.
    const auto empty{ParseOpReturnPayload(CScript() << OP_RETURN << std::vector<unsigned char>{})};
    BOOST_REQUIRE(empty.has_value());
    BOOST_CHECK(empty->empty());
}

BOOST_AUTO_TEST_CASE(op_return_payload_rejects_other_shapes)
{
    const std::vector<unsigned char> data{0x01, 0x02};

    // Not an OP_RETURN.
    BOOST_CHECK(!ParseOpReturnPayload(CScript() << OP_TRUE << data).has_value());
    // Nothing pushed.
    BOOST_CHECK(!ParseOpReturnPayload(CScript() << OP_RETURN).has_value());
    // Two pushes: the payload has to be the whole of the script.
    BOOST_CHECK(!ParseOpReturnPayload(CScript() << OP_RETURN << data << data).has_value());
    // An opcode where a data push belongs. OP_1 is a push in spirit but not a
    // data push, and GetOp leaves the payload empty rather than failing.
    BOOST_CHECK(!ParseOpReturnPayload(CScript() << OP_RETURN << OP_1).has_value());
    // A truncated push: claims 4 bytes, carries 2.
    const std::vector<unsigned char> truncated{OP_RETURN, 0x04, 0x01, 0x02};
    BOOST_CHECK(!ParseOpReturnPayload(CScript(truncated.begin(), truncated.end())).has_value());
}

BOOST_AUTO_TEST_CASE(m1_propose_sidechain)
{
    const std::vector<unsigned char> description{'s', 'i', 'd', 'e', 'c', 'h', 'a', 'i', 'n'};
    std::vector<unsigned char> body{7};
    body.insert(body.end(), description.begin(), description.end());

    const auto message{ParseCoinbaseMessage(MessageScript(M1ProposeSidechain::TAG, body))};
    BOOST_REQUIRE(message.has_value());
    const auto* m1{std::get_if<M1ProposeSidechain>(&*message)};
    BOOST_REQUIRE(m1 != nullptr);
    BOOST_CHECK_EQUAL(int{m1->slot}, 7);
    BOOST_CHECK_EQUAL_COLLECTIONS(m1->description.begin(), m1->description.end(),
                                  description.begin(), description.end());
    BOOST_CHECK(m1->ProposalId() == Hash(description));

    // The description is opaque and unbounded, so an empty one is well formed.
    const std::vector<unsigned char> slot_only{7};
    const auto empty{ParseCoinbaseMessage(MessageScript(M1ProposeSidechain::TAG, slot_only))};
    BOOST_REQUIRE(empty.has_value());
    BOOST_CHECK(std::get<M1ProposeSidechain>(*empty).description.empty());

    // ... but the slot byte is not optional.
    BOOST_CHECK(!ParseCoinbaseMessage(MessageScript(M1ProposeSidechain::TAG, {})).has_value());
}

BOOST_AUTO_TEST_CASE(m2_ack_sidechain)
{
    // Pinned deliberately: both specifications say BF where the reference
    // implementation says DF, and this patchset follows the implementation. If
    // that resolves the other way, this test is the first thing that fails.
    const std::array<unsigned char, 4> expected_tag{0xD6, 0xE1, 0xC5, 0xDF};
    BOOST_CHECK_EQUAL_COLLECTIONS(M2AckSidechain::TAG.begin(), M2AckSidechain::TAG.end(),
                                  expected_tag.begin(), expected_tag.end());

    const uint256 proposal_id{Hash(std::vector<unsigned char>{'d'})};
    const auto message{ParseCoinbaseMessage(MessageScript(M2AckSidechain::TAG, SlotAndHash(3, proposal_id)))};
    BOOST_REQUIRE(message.has_value());
    const auto* m2{std::get_if<M2AckSidechain>(&*message)};
    BOOST_REQUIRE(m2 != nullptr);
    BOOST_CHECK_EQUAL(int{m2->slot}, 3);
    BOOST_CHECK(m2->proposal_id == proposal_id);
}

BOOST_AUTO_TEST_CASE(m2_length_is_exact)
{
    const uint256 proposal_id{Hash(std::vector<unsigned char>{'d'})};

    std::vector<unsigned char> too_long{SlotAndHash(3, proposal_id)};
    too_long.push_back(0x00);
    BOOST_CHECK(!ParseCoinbaseMessage(MessageScript(M2AckSidechain::TAG, too_long)).has_value());

    std::vector<unsigned char> too_short{SlotAndHash(3, proposal_id)};
    too_short.pop_back();
    BOOST_CHECK(!ParseCoinbaseMessage(MessageScript(M2AckSidechain::TAG, too_short)).has_value());
}

BOOST_AUTO_TEST_CASE(m2_accepts_any_push_encoding)
{
    // Coinbase messages are parsed as script instructions, so the push opcode
    // is not part of the match. M8 is the exception, and the asymmetry is easy
    // to lose in a shared parser.
    const uint256 proposal_id{Hash(std::vector<unsigned char>{'d'})};
    const std::vector<unsigned char> body{SlotAndHash(3, proposal_id)};

    std::vector<unsigned char> payload{M2AckSidechain::TAG.begin(), M2AckSidechain::TAG.end()};
    payload.insert(payload.end(), body.begin(), body.end());

    // Same 37 payload bytes, pushed with OP_PUSHDATA1 instead of the minimal
    // OP_PUSHBYTES_37 that CScript would choose.
    std::vector<unsigned char> bytes{OP_RETURN, OP_PUSHDATA1, static_cast<unsigned char>(payload.size())};
    bytes.insert(bytes.end(), payload.begin(), payload.end());

    const auto message{ParseCoinbaseMessage(CScript(bytes.begin(), bytes.end()))};
    BOOST_REQUIRE(message.has_value());
    BOOST_CHECK(std::holds_alternative<M2AckSidechain>(*message));
}

BOOST_AUTO_TEST_CASE(m3_propose_bundle)
{
    const uint256 m6id{Hash(std::vector<unsigned char>{'b'})};
    const auto message{ParseCoinbaseMessage(MessageScript(M3ProposeBundle::TAG, SlotAndHash(1, m6id)))};
    BOOST_REQUIRE(message.has_value());
    const auto* m3{std::get_if<M3ProposeBundle>(&*message)};
    BOOST_REQUIRE(m3 != nullptr);
    BOOST_CHECK_EQUAL(int{m3->slot}, 1);
    BOOST_CHECK(m3->m6id == Txid::FromUint256(m6id));
}

BOOST_AUTO_TEST_CASE(m4_versions_without_votes)
{
    for (const unsigned char version : {0x00, 0x03}) {
        const std::vector<unsigned char> body{version};
        const auto message{ParseCoinbaseMessage(MessageScript(M4AckBundles::TAG, body))};
        BOOST_REQUIRE(message.has_value());
        const auto* m4{std::get_if<M4AckBundles>(&*message)};
        BOOST_REQUIRE(m4 != nullptr);
        BOOST_CHECK(m4->version == static_cast<M4AckBundles::Version>(version));
        BOOST_CHECK(m4->upvotes.empty());

        // Neither version carries a vote array, so a byte after the version
        // makes the message malformed rather than a vote.
        const std::vector<unsigned char> with_votes{version, 0x00};
        BOOST_CHECK(!ParseCoinbaseMessage(MessageScript(M4AckBundles::TAG, with_votes)).has_value());
    }
}

BOOST_AUTO_TEST_CASE(m4_one_byte_votes)
{
    const std::vector<unsigned char> body{0x01, 0x00, 0x07, M4AckBundles::ALARM_ONE_BYTE, M4AckBundles::ABSTAIN_ONE_BYTE};
    const auto message{ParseCoinbaseMessage(MessageScript(M4AckBundles::TAG, body))};
    BOOST_REQUIRE(message.has_value());
    const auto* m4{std::get_if<M4AckBundles>(&*message)};
    BOOST_REQUIRE(m4 != nullptr);
    BOOST_CHECK(m4->version == M4AckBundles::Version::VOTES_ONE_BYTE);

    // Stored exactly as encoded: the sentinels are still one-byte values.
    const std::vector<uint16_t> raw{0x0000, 0x0007, 0x00FE, 0x00FF};
    BOOST_CHECK_EQUAL_COLLECTIONS(m4->upvotes.begin(), m4->upvotes.end(), raw.begin(), raw.end());

    const std::vector<uint16_t> normalized{0x0000, 0x0007, M4AckBundles::ALARM_TWO_BYTES, M4AckBundles::ABSTAIN_TWO_BYTES};
    const std::vector<uint16_t> got{m4->NormalizedUpvotes()};
    BOOST_CHECK_EQUAL_COLLECTIONS(got.begin(), got.end(), normalized.begin(), normalized.end());

    // An empty vote array is well formed; whether it is *valid* depends on how
    // many sidechains are active, which is a connect-time question.
    const std::vector<unsigned char> no_votes{0x01};
    BOOST_REQUIRE(ParseCoinbaseMessage(MessageScript(M4AckBundles::TAG, no_votes)).has_value());
}

BOOST_AUTO_TEST_CASE(m4_two_byte_votes)
{
    // Little-endian, per BIP-300.
    const std::vector<unsigned char> body{0x02, 0x01, 0x01, 0xFE, 0xFF};
    const auto message{ParseCoinbaseMessage(MessageScript(M4AckBundles::TAG, body))};
    BOOST_REQUIRE(message.has_value());
    const auto* m4{std::get_if<M4AckBundles>(&*message)};
    BOOST_REQUIRE(m4 != nullptr);
    BOOST_CHECK(m4->version == M4AckBundles::Version::VOTES_TWO_BYTE);

    const std::vector<uint16_t> expected{0x0101, M4AckBundles::ALARM_TWO_BYTES};
    BOOST_CHECK_EQUAL_COLLECTIONS(m4->upvotes.begin(), m4->upvotes.end(), expected.begin(), expected.end());
    // Already two-byte values, so normalization is the identity.
    const std::vector<uint16_t> got{m4->NormalizedUpvotes()};
    BOOST_CHECK_EQUAL_COLLECTIONS(got.begin(), got.end(), expected.begin(), expected.end());

    // An odd trailing byte is not half a vote.
    const std::vector<unsigned char> odd{0x02, 0x01, 0x01, 0xFE};
    BOOST_CHECK(!ParseCoinbaseMessage(MessageScript(M4AckBundles::TAG, odd)).has_value());
}

BOOST_AUTO_TEST_CASE(m4_rejects_unknown_version)
{
    for (const unsigned char version : {0x04, 0x7F, 0xFF}) {
        const std::vector<unsigned char> body{version};
        BOOST_CHECK(!ParseCoinbaseMessage(MessageScript(M4AckBundles::TAG, body)).has_value());
    }
    // No version byte at all.
    BOOST_CHECK(!ParseCoinbaseMessage(MessageScript(M4AckBundles::TAG, {})).has_value());
}

BOOST_AUTO_TEST_CASE(coinbase_message_rejects_non_messages)
{
    // Not an OP_RETURN at all.
    BOOST_CHECK(!ParseCoinbaseMessage(TreasuryScript(0)).has_value());
    // An OP_RETURN carrying something that is not a message.
    const std::vector<unsigned char> junk{0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    BOOST_CHECK(!ParseCoinbaseMessage(CScript() << OP_RETURN << junk).has_value());
    // A tag one byte short of matching anything.
    const std::vector<unsigned char> short_tag{0xD6, 0xE1, 0xC5};
    BOOST_CHECK(!ParseCoinbaseMessage(CScript() << OP_RETURN << short_tag).has_value());
}

BOOST_AUTO_TEST_CASE(m7_bmm_accept)
{
    const uint256 sidechain_block_hash{Hash(std::vector<unsigned char>{'h'})};
    const auto message{ParseCoinbaseMessage(MessageScript(M7BmmAccept::TAG, SlotAndHash(2, sidechain_block_hash)))};
    BOOST_REQUIRE(message.has_value());
    const auto* m7{std::get_if<M7BmmAccept>(&*message)};
    BOOST_REQUIRE(m7 != nullptr);
    BOOST_CHECK_EQUAL(int{m7->slot}, 2);
    BOOST_CHECK(m7->sidechain_block_hash == sidechain_block_hash);

    std::vector<unsigned char> too_long{SlotAndHash(2, sidechain_block_hash)};
    too_long.push_back(0x00);
    BOOST_CHECK(!ParseCoinbaseMessage(MessageScript(M7BmmAccept::TAG, too_long)).has_value());
}

BOOST_AUTO_TEST_CASE(m8_bmm_request)
{
    const uint256 sidechain_block_hash{Hash(std::vector<unsigned char>{'h'})};
    const uint256 prev_main_block_hash{Hash(std::vector<unsigned char>{'p'})};

    const CScript script{M8Script(5, sidechain_block_hash, prev_main_block_hash)};
    BOOST_CHECK_EQUAL(script.size(), M8_SCRIPT_SIZE);

    const auto request{ParseM8Request(script)};
    BOOST_REQUIRE(request.has_value());
    BOOST_CHECK_EQUAL(int{request->slot}, 5);
    BOOST_CHECK(request->sidechain_block_hash == sidechain_block_hash);
    BOOST_CHECK(request->prev_main_block_hash == prev_main_block_hash);
}

BOOST_AUTO_TEST_CASE(m8_is_matched_byte_exactly)
{
    const uint256 sidechain_block_hash{Hash(std::vector<unsigned char>{'h'})};
    const uint256 prev_main_block_hash{Hash(std::vector<unsigned char>{'p'})};
    const CScript valid{M8Script(5, sidechain_block_hash, prev_main_block_hash)};

    // The same 68 payload bytes behind OP_PUSHDATA1. A coinbase message would
    // accept this -- see m2_accepts_any_push_encoding -- and an M8 must not.
    std::vector<unsigned char> pushdata1{OP_RETURN, OP_PUSHDATA1, M8_SCRIPT_SIZE - 2};
    pushdata1.insert(pushdata1.end(), valid.begin() + 2, valid.end());
    BOOST_CHECK(!ParseM8Request(CScript(pushdata1.begin(), pushdata1.end())).has_value());

    // A trailing byte.
    std::vector<unsigned char> trailing{valid.begin(), valid.end()};
    trailing.push_back(0x00);
    BOOST_CHECK(!ParseM8Request(CScript(trailing.begin(), trailing.end())).has_value());

    // A truncated request.
    std::vector<unsigned char> truncated{valid.begin(), valid.end() - 1};
    BOOST_CHECK(!ParseM8Request(CScript(truncated.begin(), truncated.end())).has_value());

    // Right length, wrong tag: the M7 tag is four bytes where M8's is three.
    std::vector<unsigned char> wrong_tag{valid.begin(), valid.end()};
    wrong_tag[2] = 0x01;
    BOOST_CHECK(!ParseM8Request(CScript(wrong_tag.begin(), wrong_tag.end())).has_value());

    // An M8 is not a coinbase message, and must not parse as one.
    BOOST_CHECK(!ParseCoinbaseMessage(valid).has_value());
}

BOOST_AUTO_TEST_CASE(m8_is_read_from_output_zero_only)
{
    const uint256 sidechain_block_hash{Hash(std::vector<unsigned char>{'h'})};
    const uint256 prev_main_block_hash{Hash(std::vector<unsigned char>{'p'})};
    const CScript script{M8Script(5, sidechain_block_hash, prev_main_block_hash)};

    CMutableTransaction tx;
    BOOST_CHECK(!ParseM8Request(CTransaction{tx}).has_value());

    tx.vout.emplace_back(0, script);
    BOOST_CHECK(ParseM8Request(CTransaction{tx}).has_value());

    // Moved off index 0, the same output is not a BMM request.
    CMutableTransaction shifted;
    shifted.vout.emplace_back(0, CScript() << OP_RETURN);
    shifted.vout.emplace_back(0, script);
    BOOST_CHECK(!ParseM8Request(CTransaction{shifted}).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
