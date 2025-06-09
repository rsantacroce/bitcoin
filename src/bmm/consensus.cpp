#include <bmm/consensus.h>
#include <arith_uint256.h>

namespace BMM {

void InitializeBMMConsensusParams(BMMConsensusParams& params)
{
    // Initialize BMM difficulty adjustment parameters
    params.nBMMDifficultyAdjustmentInterval = 2016;  // Same as Bitcoin
    params.nBMMTargetTimespan = 14 * 24 * 60 * 60;   // 14 days
    params.nBMMTargetSpacing = 10 * 60;              // 10 minutes

    // Initialize BMM proof of work parameters
    params.bmmPowLimit = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    params.fBMMPowAllowMinDifficultyBlocks = false;
    params.fBMMPowNoRetargeting = false;

    // Initialize sidechain parameters
    params.nSidechainActivationHeight = 0;  // To be set based on deployment
    params.nSidechainMaturity = 100;        // Number of confirmations required for sidechain blocks

    // Initialize BMM-specific flags
    params.fBMMEnabled = true;
    params.fSidechainEnabled = true;
}

} // namespace BMM 