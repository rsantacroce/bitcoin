#ifndef BITCOIN_BMM_CONSENSUS_H
#define BITCOIN_BMM_CONSENSUS_H

#include <consensus/params.h>
#include <uint256.h>

namespace BMM {

// BMM-specific consensus parameters
struct BMMConsensusParams {
    // BMM difficulty adjustment parameters
    int nBMMDifficultyAdjustmentInterval;
    int64_t nBMMTargetTimespan;
    int64_t nBMMTargetSpacing;
    
    // BMM proof of work parameters
    uint256 bmmPowLimit;
    bool fBMMPowAllowMinDifficultyBlocks;
    bool fBMMPowNoRetargeting;
    
    // Sidechain parameters
    int nSidechainActivationHeight;
    int nSidechainMaturity;
    
    // BMM-specific flags
    bool fBMMEnabled;
    bool fSidechainEnabled;
};

// Initialize BMM consensus parameters
void InitializeBMMConsensusParams(BMMConsensusParams& params);

} // namespace BMM

#endif // BITCOIN_BMM_CONSENSUS_H 