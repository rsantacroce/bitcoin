// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then it MUST be a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

namespace {
// Difficulty is proportional to work, so the guardrails are computed in work
// units, where "X% of the distance" is a plain linear interpolation.
arith_uint256 TargetToWork(const arith_uint256& target)
{
    return (~target / (target + 1)) + 1;
}

arith_uint256 WorkToTarget(const arith_uint256& work)
{
    return ~work / work;
}

enum class GuardrailType { NONE, UPPER, LOWER };

struct Guardrail {
    GuardrailType type{GuardrailType::NONE};
    //! Difficulty, in work units, the guardrail sits at
    arith_uint256 work;
};

// Work of the period starting at period_start. The first block of a period
// is never a testnet min-difficulty block, so it carries the real target.
arith_uint256 PeriodWork(const CBlockIndex* pindexLast, int period_start)
{
    arith_uint256 target;
    target.SetCompact(pindexLast->GetAncestor(period_start)->nBits);
    return TargetToWork(target);
}

/**
 * Guardrail armed for the retarget at `height` (rules 1-4 in README-DAA.md).
 * It only depends on the difficulty of the three preceding periods, which
 * are read from the block index, so no extra state has to be tracked.
 */
Guardrail GetGuardrail(const CBlockIndex* pindexLast, int height, const Consensus::Params& params)
{
    const int interval = params.DifficultyAdjustmentInterval();
    // Both moves must be guardrail-era moves, and there must be a period before them.
    if (height - 2 * interval < params.DaaGuardrailHeight || height - 3 * interval < 0) return {};

    const arith_uint256 work_last = PeriodWork(pindexLast, height - interval);
    const arith_uint256 work_prev = PeriodWork(pindexLast, height - 2 * interval);
    const arith_uint256 work_before = PeriodWork(pindexLast, height - 3 * interval);

    // Guardrail at num/10 of the distance from the last difficulty back
    // towards the previous one, which the last move bounced away from.
    const auto guardrail = [&](GuardrailType type, int num) {
        return Guardrail{type, (work_last * (10 - num) + work_prev * num) / 10};
    };

    if (work_prev > work_before) {
        // Rose, then fell by more than 10% (rule 2) or 5% (rule 1): cap the next rise
        if (work_last * 10 < work_prev * 9) return guardrail(GuardrailType::UPPER, 8);
        if (work_last * 20 < work_prev * 19) return guardrail(GuardrailType::UPPER, 9);
    } else if (work_prev < work_before) {
        // Fell, then rose by more than 50% (rule 4) or 25% (rule 3): floor the next fall
        if (work_last * 2 > work_prev * 3) return guardrail(GuardrailType::LOWER, 8);
        if (work_last * 4 > work_prev * 5) return guardrail(GuardrailType::LOWER, 9);
    }
    return {};
}

bool HitsGuardrail(const Guardrail& guardrail, const arith_uint256& work)
{
    return (guardrail.type == GuardrailType::UPPER && work > guardrail.work) ||
           (guardrail.type == GuardrailType::LOWER && work < guardrail.work);
}

// The unconstrained retarget: scale by the clamped timespan of the period.
arith_uint256 RetargetTarget(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;

    // Special difficulty rule for Testnet4
    if (params.enforce_BIP94) {
        // Here we use the first block of the difficulty period. This way
        // the real difficulty is always preserved in the first block as
        // it is not allowed to use the min-difficulty exception.
        int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
        const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
        bnNew.SetCompact(pindexFirst->nBits);
    } else {
        bnNew.SetCompact(pindexLast->nBits);
    }

    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew;
}
} // namespace

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew = RetargetTarget(pindexLast, nFirstBlockTime, params);

    // DAA ricochet guardrails, see README-DAA.md
    const int height = pindexLast->nHeight + 1;
    const Guardrail guardrail = GetGuardrail(pindexLast, height, params);
    const arith_uint256 work = TargetToWork(bnNew);
    if (HitsGuardrail(guardrail, work)) {
        arith_uint256 new_work = guardrail.work;
        if (guardrail.type == GuardrailType::LOWER) {
            // Rule 5: the previous retarget hit an upper guardrail and this one
            // hits a lower one, so settle on the average of the two.
            const int interval = params.DifficultyAdjustmentInterval();
            const int prev_height = height - interval;
            const Guardrail prev_guardrail = GetGuardrail(pindexLast, prev_height, params);
            if (prev_guardrail.type == GuardrailType::UPPER && prev_height != params.EcashHeight) {
                const CBlockIndex* prev_last = pindexLast->GetAncestor(prev_height - 1);
                const int64_t prev_first_time = pindexLast->GetAncestor(prev_height - interval)->GetBlockTime();
                const arith_uint256 prev_work = TargetToWork(RetargetTarget(prev_last, prev_first_time, params));
                if (HitsGuardrail(prev_guardrail, prev_work)) {
                    new_work = (prev_guardrail.work + guardrail.work) / 2;
                }
            }
        }
        bnNew = WorkToTarget(new_work);
    }

    // eCash fork activation difficulty reset
    if (pindexLast->nHeight + 1 == params.EcashHeight)
        bnNew.SetCompact(params.EcashForkBits);

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

// Check that on difficulty adjustments, the new difficulty does not increase
// or decrease beyond the permitted limits.
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{
    if (params.fPowAllowMinDifficultyBlocks) return true;

    if (height % params.DifficultyAdjustmentInterval() == 0) {
        int64_t smallest_timespan = params.nPowTargetTimespan/4;
        int64_t largest_timespan = params.nPowTargetTimespan*4;

        const arith_uint256 pow_limit = UintToArith256(params.powLimit);
        arith_uint256 observed_new_target;
        observed_new_target.SetCompact(new_nbits);

        // Calculate the largest difficulty value possible:
        arith_uint256 largest_difficulty_target;
        largest_difficulty_target.SetCompact(old_nbits);
        largest_difficulty_target *= largest_timespan;
        largest_difficulty_target /= params.nPowTargetTimespan;

        if (largest_difficulty_target > pow_limit) {
            largest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 maximum_new_target;
        maximum_new_target.SetCompact(largest_difficulty_target.GetCompact());
        if (maximum_new_target < observed_new_target) return false;

        // Calculate the smallest difficulty value possible:
        arith_uint256 smallest_difficulty_target;
        smallest_difficulty_target.SetCompact(old_nbits);
        smallest_difficulty_target *= smallest_timespan;
        smallest_difficulty_target /= params.nPowTargetTimespan;

        if (smallest_difficulty_target > pow_limit) {
            smallest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 minimum_new_target;
        minimum_new_target.SetCompact(smallest_difficulty_target.GetCompact());
        if (minimum_new_target > observed_new_target) return false;
    } else if (old_nbits != new_nbits) {
        return false;
    }
    return true;
}

// Bypasses the actual proof of work check during fuzz testing with a simplified validation checking whether
// the most significant bit of the last byte of the hash is set.
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) return (hash.data()[31] & 0x80) == 0;
    return CheckProofOfWorkImpl(hash, nBits, params);
}

std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(pow_limit))
        return {};

    return bnTarget;
}

bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    auto bnTarget{DeriveTarget(nBits, params.powLimit)};
    if (!bnTarget) return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
