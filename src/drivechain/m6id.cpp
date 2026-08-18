// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/m6id.h>

#include <consensus/amount.h>
#include <crypto/common.h>
#include <drivechain/messages.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace drivechain {

std::optional<BlindedM6> BlindM6(const CTransaction& m6, CAmount treasury_spent, M6Error& error)
{
    if (m6.vout.empty()) {
        error = M6Error::NO_TREASURY_OUTPUT;
        return std::nullopt;
    }
    const std::optional<SlotNum> slot{ParseTreasuryScript(m6.vout[0].scriptPubKey)};
    if (!slot) {
        error = M6Error::NO_TREASURY_OUTPUT;
        return std::nullopt;
    }
    if (m6.vin.size() != 1) {
        error = M6Error::INPUT_COUNT;
        return std::nullopt;
    }

    // The payouts are every output except the treasury change at index 0.
    //
    // The reference implementation checks these sums for 64-bit overflow,
    // where this checks the money range. The two cannot disagree on any
    // transaction that reaches consensus: CheckTransaction has already
    // rejected an out-of-range output or total, so a block containing one
    // never gets this far. Checking the money range instead keeps the
    // arithmetic below well defined on a signed type.
    CAmount payouts{0};
    for (size_t i{1}; i < m6.vout.size(); ++i) {
        const CAmount value{m6.vout[i].nValue};
        if (value < 0 || !MoneyRange(value) || !MoneyRange(payouts + value)) {
            error = M6Error::AMOUNT_OVERFLOW;
            return std::nullopt;
        }
        payouts += value;
    }

    const CAmount treasury_change{m6.vout[0].nValue};
    if (treasury_change < 0 || !MoneyRange(treasury_change) || !MoneyRange(treasury_change + payouts)) {
        error = M6Error::AMOUNT_OVERFLOW;
        return std::nullopt;
    }

    // T_new = T_prev - P_total - F_total, so the fee is whatever the treasury
    // lost that the payouts did not gain.
    const CAmount spent{treasury_change + payouts};
    if (treasury_spent < spent) {
        error = M6Error::INSUFFICIENT_TREASURY;
        return std::nullopt;
    }
    const CAmount fee{treasury_spent - spent};

    // Spec divergence: BIP-300 says the blinded form MUST have a non-zero total
    // payout. Neither the reference implementation nor this code enforces it,
    // so a withdrawal that pays out nothing and burns the difference as fee is
    // accepted. Left as-is deliberately; see doc/drivechain.md.

    CMutableTransaction blinded{m6};
    blinded.vin.clear();
    std::vector<unsigned char> fee_bytes(8);
    WriteBE64(fee_bytes.data(), static_cast<uint64_t>(fee));
    blinded.vout[0] = CTxOut{0, CScript() << OP_RETURN << fee_bytes};

    return BlindedM6{
        .slot = *slot,
        .m6id = CTransaction{blinded}.GetHash(),
        .fee = fee,
    };
}

} // namespace drivechain
