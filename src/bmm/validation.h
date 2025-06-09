#ifndef BITCOIN_BMM_VALIDATION_H
#define BITCOIN_BMM_VALIDATION_H

#include <bmm/bmm.h>
#include <bmm/consensus.h>
#include <primitives/block.h>
#include <validation.h>

namespace BMM {

// BMM-specific validation functions
bool CheckBMMBlockHeader(const CBlockHeader& block, BlockValidationState& state, const Consensus::Params& params);
bool CheckBMMBlock(const CBlock& block, BlockValidationState& state, const Consensus::Params& params);
bool ValidateBMMBlockHeader(const CBlockHeader& block, const Consensus::Params& params);

// BMM-specific block processing
bool ProcessBMMBlock(const CBlock& block, BlockValidationState& state, const Consensus::Params& params);
bool ConnectBMMBlock(const CBlock& block, BlockValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view);

// BMM-specific chain state management
bool UpdateBMMChainState(const CBlock& block, CBlockIndex* pindex, CCoinsViewCache& view);
bool ValidateBMMChainState(const CBlockIndex* pindex, const Consensus::Params& params);

} // namespace BMM

#endif // BITCOIN_BMM_VALIDATION_H 