// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <drivechain/m6id.h>
#include <drivechain/messages.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <vector>

using namespace drivechain;

namespace {
constexpr SlotNum SLOT{1};

//! A withdrawal: one input spending the treasury, the treasury change at
//! vout[0], and the payouts after it.
CMutableTransaction WithdrawalTx(const COutPoint& treasury, CAmount change, const std::vector<CAmount>& payouts)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(treasury);
    tx.vout.emplace_back(change, TreasuryScript(SLOT));
    for (const CAmount payout : payouts) {
        tx.vout.emplace_back(payout, CScript() << OP_TRUE);
    }
    return tx;
}

COutPoint SomeOutPoint(uint8_t seed)
{
    return COutPoint{Txid::FromUint256(uint256{seed}), seed};
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_m6id_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(blinded_form_recovers_the_fee)
{
    // T_prev 10000, change 6000, payouts 3000 => fee 1000.
    const CMutableTransaction tx{WithdrawalTx(SomeOutPoint(1), 6000, {3000})};

    M6Error error{};
    const auto blinded{BlindM6(CTransaction{tx}, 10000, error)};
    BOOST_REQUIRE(blinded.has_value());
    BOOST_CHECK_EQUAL(int{blinded->slot}, int{SLOT});
    BOOST_CHECK_EQUAL(blinded->fee, 1000);
}

BOOST_AUTO_TEST_CASE(blinded_form_matches_its_definition)
{
    const CMutableTransaction tx{WithdrawalTx(SomeOutPoint(1), 6000, {3000, 400})};

    M6Error error{};
    const auto blinded{BlindM6(CTransaction{tx}, 10000, error)};
    BOOST_REQUIRE(blinded.has_value());
    BOOST_CHECK_EQUAL(blinded->fee, 600);

    // Built by hand from the specification: empty vin, and vout[0] replaced by
    // a zero-value OP_RETURN carrying the fee as 8 bytes big-endian.
    CMutableTransaction expected{tx};
    expected.vin.clear();
    const std::vector<unsigned char> fee_bytes{0, 0, 0, 0, 0, 0, 0x02, 0x58}; // 600
    expected.vout[0] = CTxOut{0, CScript() << OP_RETURN << fee_bytes};

    BOOST_CHECK(blinded->m6id == CTransaction{expected}.GetHash());
}

BOOST_AUTO_TEST_CASE(m6id_does_not_depend_on_the_treasury_outpoint)
{
    // This is the entire purpose of blinding: miners vote on a bundle months
    // before the treasury output it will spend exists, so the same payouts and
    // the same fee must yield the same M6ID whatever the treasury outpoint
    // turns out to be.
    const CMutableTransaction first{WithdrawalTx(SomeOutPoint(1), 6000, {3000})};
    const CMutableTransaction second{WithdrawalTx(SomeOutPoint(2), 6000, {3000})};
    BOOST_CHECK(CTransaction{first}.GetHash() != CTransaction{second}.GetHash());

    M6Error error{};
    const auto blinded_first{BlindM6(CTransaction{first}, 10000, error)};
    const auto blinded_second{BlindM6(CTransaction{second}, 10000, error)};
    BOOST_REQUIRE(blinded_first.has_value());
    BOOST_REQUIRE(blinded_second.has_value());
    BOOST_CHECK(blinded_first->m6id == blinded_second->m6id);
}

BOOST_AUTO_TEST_CASE(m6id_binds_the_fee)
{
    // Same payouts, different treasury change, so a different fee against the
    // same treasury: the vote must not carry over.
    const CMutableTransaction tx{WithdrawalTx(SomeOutPoint(1), 6000, {3000})};

    M6Error error{};
    const auto cheap{BlindM6(CTransaction{tx}, 10000, error)};
    const auto dear{BlindM6(CTransaction{tx}, 11000, error)};
    BOOST_REQUIRE(cheap.has_value());
    BOOST_REQUIRE(dear.has_value());
    BOOST_CHECK_EQUAL(cheap->fee, 1000);
    BOOST_CHECK_EQUAL(dear->fee, 2000);
    BOOST_CHECK(cheap->m6id != dear->m6id);
}

BOOST_AUTO_TEST_CASE(zero_fee_is_valid)
{
    const CMutableTransaction tx{WithdrawalTx(SomeOutPoint(1), 6000, {4000})};

    M6Error error{};
    const auto blinded{BlindM6(CTransaction{tx}, 10000, error)};
    BOOST_REQUIRE(blinded.has_value());
    BOOST_CHECK_EQUAL(blinded->fee, 0);
}

BOOST_AUTO_TEST_CASE(rejects_transactions_that_are_not_withdrawals)
{
    M6Error error{};

    // No outputs at all.
    BOOST_CHECK(!BlindM6(CTransaction{CMutableTransaction{}}, 10000, error).has_value());
    BOOST_CHECK(error == M6Error::NO_TREASURY_OUTPUT);

    // The treasury change is not at index 0.
    CMutableTransaction shifted{WithdrawalTx(SomeOutPoint(1), 6000, {3000})};
    std::swap(shifted.vout[0], shifted.vout[1]);
    BOOST_CHECK(!BlindM6(CTransaction{shifted}, 10000, error).has_value());
    BOOST_CHECK(error == M6Error::NO_TREASURY_OUTPUT);

    // A withdrawal spends the treasury and nothing else.
    CMutableTransaction two_inputs{WithdrawalTx(SomeOutPoint(1), 6000, {3000})};
    two_inputs.vin.emplace_back(SomeOutPoint(2));
    BOOST_CHECK(!BlindM6(CTransaction{two_inputs}, 10000, error).has_value());
    BOOST_CHECK(error == M6Error::INPUT_COUNT);

    CMutableTransaction no_input{WithdrawalTx(SomeOutPoint(1), 6000, {3000})};
    no_input.vin.clear();
    BOOST_CHECK(!BlindM6(CTransaction{no_input}, 10000, error).has_value());
    BOOST_CHECK(error == M6Error::INPUT_COUNT);
}

BOOST_AUTO_TEST_CASE(rejects_spending_more_than_the_treasury_held)
{
    const CMutableTransaction tx{WithdrawalTx(SomeOutPoint(1), 6000, {3000})};

    M6Error error{};
    // Exactly the outputs is fine; a satoshi less is not.
    BOOST_CHECK(BlindM6(CTransaction{tx}, 9000, error).has_value());
    BOOST_CHECK(!BlindM6(CTransaction{tx}, 8999, error).has_value());
    BOOST_CHECK(error == M6Error::INSUFFICIENT_TREASURY);
}

BOOST_AUTO_TEST_CASE(rejects_out_of_range_payouts)
{
    CMutableTransaction tx{WithdrawalTx(SomeOutPoint(1), 6000, {MAX_MONEY, MAX_MONEY})};

    M6Error error{};
    BOOST_CHECK(!BlindM6(CTransaction{tx}, MAX_MONEY, error).has_value());
    BOOST_CHECK(error == M6Error::AMOUNT_OVERFLOW);
}

BOOST_AUTO_TEST_SUITE_END()
