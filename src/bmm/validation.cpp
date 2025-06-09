#include <bmm/validation.h>
#include <bmm/bmm.h>
#include <bmm/consensus.h>
#include <chain.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <uint256.h>
#include <util/check.h>

namespace BMM {

bool CheckBMMBlockHeader(const CBlockHeader& block, BlockValidationState& state, const Consensus::Params& params)
{
    // Check proof of work
    if (!CheckBMMProofOfWork(block.GetHash(), block.nBits, params)) {
        return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "high-hash", "proof of work failed");
    }

    // Check timestamp
    if (block.GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60) {
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "time-too-new");
    }

    return true;
}

bool CheckBMMBlock(const CBlock& block, BlockValidationState& state, const Consensus::Params& params)
{
    // Check block header
    if (!CheckBMMBlockHeader(block, state, params)) {
        return false;
    }

    // Check merkle root
    bool mutated;
    uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
    if (block.hashMerkleRoot != hashMerkleRoot2) {
        return state.Invalid(BlockValidationResult::BLOCK_MUTATED, "bad-txnmrklroot", "hashMerkleRoot mismatch");
    }

    // Check for merkle tree malleability
    if (mutated) {
        return state.Invalid(BlockValidationResult::BLOCK_MUTATED, "bad-txns-duplicate", "duplicate transaction");
    }

    // Check transactions
    for (const auto& tx : block.vtx) {
        if (!tx->IsCoinBase() && !tx->IsCoinStake()) {
            if (!CheckTransaction(*tx, state)) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-validateinputs", strprintf("Transaction check failed (tx hash %s) %s", tx->GetHash().ToString(), state.ToString()));
            }
        }
    }

    // Check for duplicate coinbase/stake
    if (block.vtx.size() > 0 && !block.vtx[0]->IsCoinBase()) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-cb-missing", "first tx is not coinbase");
    }

    return true;
}

bool ValidateBMMBlockHeader(const CBlockHeader& block, const Consensus::Params& params)
{
    BlockValidationState state;
    return CheckBMMBlockHeader(block, state, params);
}

bool ProcessBMMBlock(const CBlock& block, BlockValidationState& state, const Consensus::Params& params)
{
    // Basic block validation
    if (!CheckBMMBlock(block, state, params)) {
        return false;
    }

    // Additional BMM-specific processing can be added here
    // For example, validating sidechain commitments

    return true;
}

bool ConnectBMMBlock(const CBlock& block, BlockValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view)
{
    // Update UTXO set
    for (const auto& tx : block.vtx) {
        if (!tx->IsCoinBase()) {
            for (const auto& txin : tx->vin) {
                view.SpendCoin(txin.prevout);
            }
        }
    }

    // Add new coins
    for (const auto& tx : block.vtx) {
        const uint256& txid = tx->GetHash();
        for (size_t i = 0; i < tx->vout.size(); i++) {
            const CTxOut& out = tx->vout[i];
            if (!out.IsNull()) {
                COutPoint outpoint(txid, i);
                Coin coin(out, pindex->nHeight, tx->IsCoinBase());
                view.AddCoin(outpoint, std::move(coin), tx->IsCoinBase());
            }
        }
    }

    return true;
}

bool UpdateBMMChainState(const CBlock& block, CBlockIndex* pindex, CCoinsViewCache& view)
{
    // Update chain state with BMM-specific logic
    // This could include updating sidechain state, etc.
    return true;
}

bool ValidateBMMChainState(const CBlockIndex* pindex, const Consensus::Params& params)
{
    // Validate chain state with BMM-specific rules
    // This could include checking sidechain commitments, etc.
    return true;
}

} // namespace BMM 