// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/db.h>

#include <dbwrapper.h>
#include <drivechain/diff.h>
#include <drivechain/state.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace drivechain {
namespace {
//! The whole BIP-300 state.
constexpr uint8_t DB_STATE{'S'};
//! Hash of the block the stored state reflects.
constexpr uint8_t DB_TIP{'T'};
//! Per-block undo data, keyed by block hash.
constexpr uint8_t DB_BLOCK_DIFF{'D'};
} // namespace

bool DrivechainDB::ReadState(DrivechainState& state, uint256& tip) const
{
    // Both or neither. A state without the block it belongs to cannot be used,
    // since there is no way to tell what still needs connecting.
    if (!m_db.Read(DB_TIP, tip)) return false;
    return m_db.Read(DB_STATE, state);
}

bool DrivechainDB::ReadBlockDiff(const uint256& block_hash, BlockDiff& diff) const
{
    return m_db.Read(std::make_pair(DB_BLOCK_DIFF, block_hash), diff);
}

void DrivechainDB::Flush(const DrivechainState& state,
                         const uint256& tip,
                         const std::map<uint256, BlockDiff>& store_diffs,
                         const std::vector<uint256>& erase_diffs,
                         bool fsync)
{
    CDBBatch batch{m_db};
    batch.Write(DB_STATE, state);
    batch.Write(DB_TIP, tip);
    for (const auto& [block_hash, diff] : store_diffs) {
        batch.Write(std::make_pair(DB_BLOCK_DIFF, block_hash), diff);
    }
    for (const uint256& block_hash : erase_diffs) {
        batch.Erase(std::make_pair(DB_BLOCK_DIFF, block_hash));
    }
    m_db.WriteBatch(batch, fsync);
}

} // namespace drivechain
