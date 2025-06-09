#ifndef BITCOIN_BMM_BMM_H
#define BITCOIN_BMM_BMM_H

#include <primitives/block.h>
#include <uint256.h>
#include <consensus/params.h>

class CBlockIndex;

namespace BMM {

// BMM-specific constants
static const int BMM_DIFFICULTY_ADJUSTMENT_INTERVAL = 2016;
static const int BMM_TARGET_TIMESPAN = 14 * 24 * 60 * 60; // 14 days
static const int BMM_TARGET_SPACING = 10 * 60; // 10 minutes

// BMM block header structure
struct BMMBlockHeader {
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    uint256 hashSidechainBlock;  // Commitment to sidechain block
};

// BMM validation functions
bool CheckBMMProofOfWork(const uint256& hash, unsigned int nBits, const Consensus::Params& params);
unsigned int GetNextBMMWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params);
bool ValidateBMMBlock(const CBlockHeader& block, const Consensus::Params& params);

// BMM difficulty adjustment
unsigned int CalculateNextBMMWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params);

} // namespace BMM

#endif // BITCOIN_BMM_BMM_H 