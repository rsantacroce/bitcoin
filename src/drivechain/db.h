// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_DB_H
#define BITCOIN_DRIVECHAIN_DB_H

#include <dbwrapper.h>
#include <drivechain/diff.h>
#include <drivechain/state.h>
#include <uint256.h>

#include <map>
#include <vector>

namespace drivechain {

/**
 * Persistent storage for the BIP-300 state and the per-block diffs that undo
 * it.
 *
 * The state is written as a single value rather than one key per entry, which
 * is a deliberate departure from CCoinsViewDB. The BIP-300 state is bounded by
 * 256 slots and holds a handful of proposals and bundles each, so it is orders
 * of magnitude smaller than the UTXO set: the cache and dirty-tracking
 * machinery that makes the coins database work would buy nothing here and cost
 * a great deal of surface to review. Writing it whole also makes the flush
 * atomic by construction, which is the property that actually matters.
 *
 * Undo data lives here too, keyed by block hash, rather than in CBlockUndo.
 * Extending CBlockUndo would change the rev*.dat format and force every user to
 * reindex, and it would couple this format to a structure that upstream is free
 * to change.
 */
class DrivechainDB
{
public:
    explicit DrivechainDB(DBParams params) : m_db{std::move(params)} {}

    //! Read the persisted state and the hash of the block it reflects.
    //! Returns false if nothing has been written yet, which is the normal
    //! state of a fresh datadir.
    bool ReadState(DrivechainState& state, uint256& tip) const;

    //! Read the diff produced by connecting `block_hash`.
    bool ReadBlockDiff(const uint256& block_hash, BlockDiff& diff) const;

    /**
     * Write the state, the block it reflects, and any diffs to store or drop,
     * as one atomic batch.
     *
     * Atomicity is the point. If the state could reach disk without the diff
     * that undoes it -- or at a different height than the chainstate -- an
     * unclean shutdown would leave a node that cannot tell which of its
     * databases to believe.
     *
     * Set `fsync` for a flush that must survive a power loss, as CDBWrapper
     * users do. A write failure raises dbwrapper_error rather than returning,
     * which is how the rest of Core treats a database that will not take a
     * write.
     */
    void Flush(const DrivechainState& state,
               const uint256& tip,
               const std::map<uint256, BlockDiff>& store_diffs = {},
               const std::vector<uint256>& erase_diffs = {},
               bool fsync = false);

private:
    CDBWrapper m_db;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_DB_H
