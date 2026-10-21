// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <test/util/random.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1261130161; // Block #30240
    CBlockIndex pindexLast;
    pindexLast.nHeight = 32255;
    pindexLast.nTime = 1262152739;  // Block #32255
    pindexLast.nBits = 0x1d00ffff;

    // Here (and below): expected_nbits is calculated in
    // CalculateNextWorkRequired(); redoing the calculation here would be just
    // reimplementing the same code that is written in pow.cpp. Rather than
    // copy that code, we just hardcode the expected result.
    unsigned int expected_nbits = 0x1d00d86aU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1231006505; // Block #0
    CBlockIndex pindexLast;
    pindexLast.nHeight = 2015;
    pindexLast.nTime = 1233061996;  // Block #2015
    pindexLast.nBits = 0x1d00ffff;
    unsigned int expected_nbits = 0x1d00ffffU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the lower bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1279008237; // Block #66528
    CBlockIndex pindexLast;
    pindexLast.nHeight = 68543;
    pindexLast.nTime = 1279297671;  // Block #68543
    pindexLast.nBits = 0x1c05a3f4;
    unsigned int expected_nbits = 0x1c0168fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that reducing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits-1;
    BOOST_CHECK(!PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

/* Test the constraint on the upper bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1263163443; // NOTE: Not an actual block time
    CBlockIndex pindexLast;
    pindexLast.nHeight = 46367;
    pindexLast.nTime = 1269211443;  // Block #46367
    pindexLast.nBits = 0x1c387f6f;
    unsigned int expected_nbits = 0x1d00e1fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that increasing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits+1;
    BOOST_CHECK(!PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits{~0x00800000U};
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p2 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p3 = &blocks[m_rng.randrange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

namespace {
/** Header chain built period by period, for testing the DAA ricochet guardrails. */
class RetargetChain
{
public:
    explicit RetargetChain(const Consensus::Params& params) : m_params{params} {}

    /** Difficulty relative to the reference difficulty 1.0. */
    static uint32_t Bits(double difficulty)
    {
        arith_uint256 target{BaseTarget()};
        target *= 1'000'000;
        target /= arith_uint256{static_cast<uint64_t>(difficulty * 1'000'000)};
        return target.GetCompact();
    }

    static double Difficulty(uint32_t bits)
    {
        arith_uint256 target;
        target.SetCompact(bits);
        return BaseTarget().getdouble() / target.getdouble();
    }

    /** Add a period mined at `bits`, taking `ratio` times the target timespan. */
    void AddPeriod(uint32_t bits, double ratio = 1.0)
    {
        const int interval = m_params.DifficultyAdjustmentInterval();
        const int64_t timespan = static_cast<int64_t>(m_params.nPowTargetTimespan * ratio);
        for (int i = 0; i < interval; ++i) {
            auto& block = m_blocks.emplace_back(std::make_unique<CBlockIndex>());
            block->nHeight = m_blocks.size() - 1;
            block->pprev = m_blocks.size() > 1 ? m_blocks[m_blocks.size() - 2].get() : nullptr;
            block->nBits = bits;
            block->nTime = m_time + (i == interval - 1 ? timespan : 0);
            block->BuildSkip();
        }
        m_time += timespan;
    }

    /** Add a period mined at whatever difficulty the chain requires. */
    void AddRetargetedPeriod(double ratio = 1.0) { AddPeriod(Next(), ratio); }

    /** nBits required for the next period. */
    uint32_t Next() const
    {
        const CBlockIndex* last = m_blocks.back().get();
        const CBlockIndex* first = last->GetAncestor(last->nHeight - (m_params.DifficultyAdjustmentInterval() - 1));
        const uint32_t bits = CalculateNextWorkRequired(last, first->GetBlockTime(), m_params);
        BOOST_CHECK(PermittedDifficultyTransition(m_params, last->nHeight + 1, last->nBits, bits));
        return bits;
    }

private:
    static arith_uint256 BaseTarget() { return UintToArith256(uint256{"00000000ffff0000000000000000000000000000000000000000000000000000"}) >> 16; }

    const Consensus::Params& m_params;
    std::vector<std::unique_ptr<CBlockIndex>> m_blocks;
    uint32_t m_time{1'600'000'000};
};

Consensus::Params GuardrailParams(const ArgsManager& args)
{
    Consensus::Params params{CreateChainParams(args, ChainType::MAIN)->GetConsensus()};
    params.DaaGuardrailHeight = 0;
    params.EcashHeight = std::numeric_limits<int>::max();
    return params;
}

/** Difficulty required after periods at `history` difficulties, the last one taking `ratio` of the target timespan. */
double NextDifficulty(const Consensus::Params& params, std::vector<double> history, double ratio)
{
    RetargetChain chain{params};
    for (size_t i = 0; i < history.size(); ++i) {
        chain.AddPeriod(RetargetChain::Bits(history[i]), i + 1 == history.size() ? ratio : 1.0);
    }
    return RetargetChain::Difficulty(chain.Next());
}
} // namespace

BOOST_AUTO_TEST_CASE(daa_guardrail_streaks_untouched)
{
    const auto params{GuardrailParams(*m_node.args)};
    // Steady rise, and a rise followed by a small dip: no guardrail armed
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 1.1, 1.2}, 1 / 1.5), 1.8, 0.01);
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 1.2, 1.15}, 1 / 1.5), 1.725, 0.01);
    // Steady fall, and a fall followed by a small rise
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 0.9, 0.8}, 2.0), 0.4, 0.01);
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 0.8, 0.96}, 2.0), 0.48, 0.01);
}

BOOST_AUTO_TEST_CASE(daa_guardrail_upper)
{
    const auto params{GuardrailParams(*m_node.args)};
    // Rule 1: rose to 1.2, fell 6.7% to 1.12: the next rise is capped at 90% of the way back to 1.2
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 1.2, 1.12}, 1 / 1.2), 1.12 + 0.9 * 0.08, 0.01);
    // Rule 2: rose to 1.2, fell 16.7% to 1.0: capped at 80% of the way back
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 1.2, 1.0}, 1 / 1.5), 1.0 + 0.8 * 0.2, 0.01);
    // A rise below the cap, or a further fall, is untouched
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 1.2, 1.12}, 1.12 / 1.15), 1.15, 0.01);
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 1.2, 1.0}, 2.0), 0.5, 0.01);
}

BOOST_AUTO_TEST_CASE(daa_guardrail_lower)
{
    const auto params{GuardrailParams(*m_node.args)};
    // Rule 3: fell to 0.8, rose 30% to 1.04: the next fall is floored at 90% of the way back to 0.8
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 0.8, 1.04}, 2.0), 1.04 - 0.9 * 0.24, 0.01);
    // Rule 4: fell to 0.6, rose 66.7% to 1.0: floored at 80% of the way back
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 0.6, 1.0}, 4.0), 1.0 - 0.8 * 0.4, 0.01);
    // A fall above the floor, or a further rise, is untouched
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 0.8, 1.04}, 1.04 / 0.9), 0.9, 0.01);
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 0.6, 1.0}, 0.5), 2.0, 0.01);
}

BOOST_AUTO_TEST_CASE(daa_guardrail_only_next_adjustment)
{
    const auto params{GuardrailParams(*m_node.args)};
    RetargetChain chain{params};
    chain.AddPeriod(RetargetChain::Bits(1.0));
    chain.AddPeriod(RetargetChain::Bits(1.2));
    chain.AddPeriod(RetargetChain::Bits(1.0), 1 / 1.5);
    // The cap (rule 2) applies once...
    chain.AddRetargetedPeriod(1 / 1.5);
    // ...after which the rise of 16% arms nothing, so the next rise is untouched.
    BOOST_CHECK_CLOSE(RetargetChain::Difficulty(chain.Next()), 1.16 * 1.5, 0.01);
}

BOOST_AUTO_TEST_CASE(daa_guardrail_both_hit)
{
    const auto params{GuardrailParams(*m_node.args)};
    RetargetChain chain{params};
    chain.AddPeriod(RetargetChain::Bits(1.0));
    chain.AddPeriod(RetargetChain::Bits(1.6));
    chain.AddPeriod(RetargetChain::Bits(1.0), 0.5);
    // Rule 2 caps the rise to 2.0 at the upper guardrail 1.0 + 0.8 * 0.6 = 1.48
    const uint32_t upper_bits{chain.Next()};
    BOOST_CHECK_CLOSE(RetargetChain::Difficulty(upper_bits), 1.48, 0.01);
    chain.AddPeriod(upper_bits, 4.0);
    // Rule 3 arms the lower guardrail 1.48 - 0.9 * 0.48 = 1.048, which the fall to 0.37 hits.
    // Rule 5: both guardrails hit in a row, so settle on their average.
    BOOST_CHECK_CLOSE(RetargetChain::Difficulty(chain.Next()), (1.48 + 1.048) / 2, 0.01);
}

BOOST_AUTO_TEST_CASE(daa_guardrail_lower_then_upper_not_averaged)
{
    const auto params{GuardrailParams(*m_node.args)};
    RetargetChain chain{params};
    chain.AddPeriod(RetargetChain::Bits(1.0));
    chain.AddPeriod(RetargetChain::Bits(0.6));
    chain.AddPeriod(RetargetChain::Bits(1.0), 4.0);
    // Rule 4 floors the fall to 0.25 at the lower guardrail 1.0 - 0.8 * 0.4 = 0.68
    const uint32_t lower_bits{chain.Next()};
    BOOST_CHECK_CLOSE(RetargetChain::Difficulty(lower_bits), 0.68, 0.01);
    chain.AddPeriod(lower_bits, 0.25);
    // Rule 2 caps the rise to 2.72 at 0.68 + 0.8 * 0.32 = 0.936. Rule 5 only applies upper then lower.
    BOOST_CHECK_CLOSE(RetargetChain::Difficulty(chain.Next()), 0.936, 0.01);
}

BOOST_AUTO_TEST_CASE(daa_guardrail_activation)
{
    auto params{GuardrailParams(*m_node.args)};
    const int interval = params.DifficultyAdjustmentInterval();
    // Both moves arming the guardrail must happen at or after the activation height
    params.DaaGuardrailHeight = interval;
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 1.2, 1.0}, 1 / 1.5), 1.16, 0.01);
    params.DaaGuardrailHeight = interval + 1;
    BOOST_CHECK_CLOSE(NextDifficulty(params, {1.0, 1.2, 1.0}, 1 / 1.5), 1.5, 0.01);
    // Disabled on main before the eCash fork
    const auto& main_params{CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus()};
    BOOST_CHECK_EQUAL(main_params.DaaGuardrailHeight, main_params.EcashHeight);
}

void sanity_check_chainparams(const ArgsManager& args, ChainType chain_type)
{
    const auto chainParams = CreateChainParams(args, chain_type);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg && pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // check max target * 4*nPowTargetTimespan doesn't overflow -- see pow.cpp:CalculateNextWorkRequired()
    if (!consensus.fPowNoRetargeting) {
        arith_uint256 targ_max{UintToArith256(uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
        targ_max /= consensus.nPowTargetTimespan*4;
        BOOST_CHECK(UintToArith256(consensus.powLimit) < targ_max);
    }
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET4_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET4);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::SIGNET);
}

BOOST_AUTO_TEST_SUITE_END()
