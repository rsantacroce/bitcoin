// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/messages.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <vector>

using namespace drivechain;

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

BOOST_AUTO_TEST_SUITE_END()
