#include <bmm/bmm.h>
#include <arith_uint256.h>
#include <chain.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>

namespace BMM {

bool CheckBMMProofOfWork(const uint256& hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

unsigned int GetNextBMMWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight + 1) % BMM_DIFFICULTY_ADJUSTMENT_INTERVAL != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then it MUST be a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + BMM_TARGET_SPACING * 2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % BMM_DIFFICULTY_ADJUSTMENT_INTERVAL != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexLast->nHeight - (BMM_DIFFICULTY_ADJUSTMENT_INTERVAL - 1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextBMMWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int CalculateNextBMMWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < BMM_TARGET_TIMESPAN/4)
        nActualTimespan = BMM_TARGET_TIMESPAN/4;
    if (nActualTimespan > BMM_TARGET_TIMESPAN*4)
        nActualTimespan = BMM_TARGET_TIMESPAN*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= BMM_TARGET_TIMESPAN;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool ValidateBMMBlock(const CBlockHeader& block, const Consensus::Params& params)
{
    // Check proof of work
    if (!CheckBMMProofOfWork(block.GetHash(), block.nBits, params))
        return false;

    // Additional BMM-specific validations can be added here
    // For example, validating the sidechain block commitment

    return true;
}

} // namespace BMM 