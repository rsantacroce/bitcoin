// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_PARAMS_H
#define BITCOIN_DRIVECHAIN_PARAMS_H

#include <algorithm>
#include <cstdint>
#include <limits>

//! The vote counts and ages BIP-300 measures proposals and bundles against.
//!
//! Deliberately free of Core includes: these are consensus parameters, so
//! consensus/params.h carries a copy, and that header is included nearly
//! everywhere.
namespace drivechain {

//! Activation height meaning BIP-300 is not deployed on this network.
//!
//! Every network but regtest carries this today. Choosing a mainnet activation
//! is a deployment decision nobody has made: BIP-300 says it deploys "when/if a
//! majority of hashrate runs the enforcer client", which is a signalling
//! mechanism rather than a height, and picking one here would be inventing an
//! answer.
inline constexpr int NOT_DEPLOYED{std::numeric_limits<int>::max()};

/**
 * BIP-300's constants, and the comparisons made against them.
 *
 * The comparisons live here rather than at their call sites because every one
 * of them is a place a previous specification got the boundary wrong. All vote
 * thresholds are **strict**: a bundle needs 13151 acks, not 13150. All age
 * windows are **inclusive**. Both prior drafts describe the withdrawal
 * threshold in prose as "reaches 13150 or greater"; the reference
 * implementation compares strictly, and since a bundle starts at one ack and
 * can gain at most one per block, the difference is a full block of hashrate
 * approval.
 *
 * BIP-300, "Constants".
 */
struct Thresholds {
    //! A bundle older than this is discarded and pays nothing.
    uint16_t withdrawal_bundle_max_age;
    //! A bundle whose vote count exceeds this may be paid out.
    uint16_t withdrawal_bundle_inclusion_threshold;
    //! Windows and bars for overwriting a slot that already holds a sidechain.
    uint16_t used_slot_proposal_max_age;
    uint16_t used_slot_activation_threshold;
    //! Windows and bars for claiming an empty slot. On mainnet this is a 90%
    //! hashrate bar (1815 of 2016), not the bare majority the original BIP-300
    //! draft implied -- a deliberate policy difference, not editorial drift.
    uint16_t unused_slot_proposal_max_age;
    uint16_t unused_slot_activation_threshold;

    //! Whether a proposal with `vote_count` acks at `age` blocks old activates.
    constexpr bool ProposalActivates(uint16_t vote_count, int32_t age, bool slot_is_used) const
    {
        const int32_t max_age{slot_is_used ? used_slot_proposal_max_age : unused_slot_proposal_max_age};
        const int32_t threshold{slot_is_used ? used_slot_activation_threshold : unused_slot_activation_threshold};
        return vote_count > threshold && age <= max_age;
    }

    /**
     * Whether a proposal is discarded at `age` blocks old.
     *
     * Two ways to fail, and only the first is in BIP-300. The second is the
     * reference implementation discarding a proposal once it has accumulated
     * enough non-ack blocks that it can no longer reach the threshold inside
     * the remaining window. That is consensus-visible -- it changes when a
     * later M2 is ignored, and when the same proposal can be made afresh -- so
     * it is reproduced here and recorded as a divergence in doc/drivechain.md
     * rather than quietly dropped.
     */
    constexpr bool ProposalFailed(uint16_t vote_count, int32_t age, bool slot_is_used) const
    {
        const int32_t max_age{slot_is_used ? used_slot_proposal_max_age : unused_slot_proposal_max_age};
        const int32_t threshold{slot_is_used ? used_slot_activation_threshold : unused_slot_activation_threshold};
        if (age > max_age) return true;

        const int32_t max_fails{std::max(0, max_age - threshold)};
        const int32_t fails{std::max(0, age - vote_count)};
        return age > max_fails && fails >= max_fails;
    }

    //! Whether a bundle with this many upvotes may be paid out.
    constexpr bool BundleIsPayable(uint16_t vote_count) const
    {
        return vote_count > withdrawal_bundle_inclusion_threshold;
    }

    //! Whether a bundle this old is discarded.
    constexpr bool BundleExpired(int32_t age) const
    {
        return age > withdrawal_bundle_max_age;
    }

    friend bool operator==(const Thresholds& a, const Thresholds& b) = default;
};

//! BIP-300 as specified. A sidechain activation takes up to 2016 blocks and a
//! withdrawal up to 26300, so a full cycle is months.
inline constexpr Thresholds MAINNET_THRESHOLDS{
    .withdrawal_bundle_max_age = 26300,
    .withdrawal_bundle_inclusion_threshold = 13150,
    .used_slot_proposal_max_age = 26300,
    .used_slot_activation_threshold = 13150,
    .unused_slot_proposal_max_age = 2016,
    .unused_slot_activation_threshold = 1815,
};

//! Tiny values so a regtest session can exercise a whole activation and
//! withdrawal cycle in seconds rather than weeks.
inline constexpr Thresholds SHORT_THRESHOLDS{
    .withdrawal_bundle_max_age = 10,
    .withdrawal_bundle_inclusion_threshold = 5,
    .used_slot_proposal_max_age = 10,
    .used_slot_activation_threshold = 5,
    .unused_slot_proposal_max_age = 10,
    .unused_slot_activation_threshold = 5,
};

//! A middle setting for a dry-run network: an activation votes for roughly six
//! hours and a withdrawal for half a day inside a one-day window, so a full
//! cycle takes about a day and a half while the voting still plays out over
//! enough blocks to be meaningful.
inline constexpr Thresholds DRYNET_THRESHOLDS{
    .withdrawal_bundle_max_age = 144,
    .withdrawal_bundle_inclusion_threshold = 72,
    .used_slot_proposal_max_age = 144,
    .used_slot_activation_threshold = 72,
    .unused_slot_proposal_max_age = 36,
    .unused_slot_activation_threshold = 30,
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_PARAMS_H
