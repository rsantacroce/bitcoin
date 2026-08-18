// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_M6ID_H
#define BITCOIN_DRIVECHAIN_M6ID_H

#include <consensus/amount.h>
#include <drivechain/messages.h>
#include <primitives/transaction_identifier.h>

#include <optional>

class CTransaction;

namespace drivechain {

//! Why an M6 could not be blinded.
enum class M6Error {
    //! vout[0] is not a treasury output, so this is not a withdrawal.
    NO_TREASURY_OUTPUT,
    //! A withdrawal spends the treasury and nothing else, so it has exactly
    //! one input.
    INPUT_COUNT,
    //! The payouts do not sum to a representable amount.
    AMOUNT_OVERFLOW,
    //! The outputs spend more than the treasury being spent held.
    INSUFFICIENT_TREASURY,
};

//! A withdrawal reduced to the form miners vote on.
struct BlindedM6 {
    //! The slot whose treasury this withdrawal spends.
    SlotNum slot;
    //! txid of the blinded transaction: the identifier an M3 proposes and an M4
    //! votes for.
    Txid m6id;
    //! The fee the withdrawal pays, `F_total`. Committed to by the M6ID, so a
    //! miner cannot alter it after the vote.
    CAmount fee;
};

/**
 * Blind a withdrawal transaction and compute its M6ID.
 *
 * Bundles are voted on for months before the treasury output they will
 * eventually spend exists, so the vote cannot commit to the final txid. It
 * commits instead to a blinded form, which differs from the final M6 in exactly
 * two ways:
 *
 *   - its vin is empty; and
 *   - vout[0] is a zero-value `OP_RETURN <F_total>`, where the fee is a 64-bit
 *     unsigned integer encoded as 8 bytes big-endian, occupying the index the
 *     treasury output occupies in the final M6.
 *
 * Constructing the final M6 is therefore the inverse: put the treasury output
 * back at vout[0] and add the single treasury input. Nothing else changes,
 * which is what lets one vote bind the payouts and the fee while leaving the
 * treasury outpoint free.
 *
 * `treasury_spent` is the value of the treasury output this transaction spends,
 * `T_prev`. The fee is recovered from it as `F_total = T_prev - T_new -
 * P_total`, since a treasury output alone does not reveal what it paid.
 *
 * Returns nullopt and sets `error` if `m6` cannot be a withdrawal at all.
 *
 * BIP-300, "M6 — Withdrawal (L2 → L1)".
 */
std::optional<BlindedM6> BlindM6(const CTransaction& m6, CAmount treasury_spent, M6Error& error);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_M6ID_H
