// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/validation.h>

#include <drivechain/messages.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>

namespace drivechain {

std::string BlockErrorString(BlockError error)
{
    switch (error) {
    case BlockError::DUPLICATE_M1: return "bad-drivechain-duplicate-m1";
    case BlockError::DUPLICATE_M2: return "bad-drivechain-duplicate-m2";
    case BlockError::DUPLICATE_M4: return "bad-drivechain-duplicate-m4";
    case BlockError::DUPLICATE_M7: return "bad-drivechain-duplicate-m7";
    case BlockError::STATE_MISMATCH: return "drivechain-state-mismatch";
    }
    return "bad-drivechain-unknown";
}

bool CollectCoinbaseMessages(const CTransaction& coinbase, CoinbaseMessages& out, BlockError& error)
{
    out = CoinbaseMessages{};

    // The duplicate rules are not all the same shape, and the differences are
    // deliberate. An M1 is a duplicate only of an identical proposal, since a
    // coinbase may legitimately propose two different sidechains for one slot.
    // An M2 is a duplicate of any other M2 for the same slot, whatever it acks,
    // because a slot gets one vote per block. An M4 is a duplicate of any other
    // M4 at all. An M7 is a duplicate of any other M7 for the same slot.
    std::set<SidechainProposalId> proposed;
    std::set<SlotNum> acked_slots;
    std::set<SlotNum> accepted_slots;

    for (uint32_t vout{0}; vout < coinbase.vout.size(); ++vout) {
        const std::optional<CoinbaseMessage> message{ParseCoinbaseMessage(coinbase.vout[vout].scriptPubKey)};
        // Not a message: an ordinary output, and the block stays valid.
        if (!message) continue;

        if (const auto* m1{std::get_if<M1ProposeSidechain>(&*message)}) {
            const SidechainProposalId id{.slot = m1->slot, .description_hash = m1->ProposalId()};
            if (!proposed.insert(id).second) {
                error = BlockError::DUPLICATE_M1;
                return false;
            }
        } else if (const auto* m2{std::get_if<M2AckSidechain>(&*message)}) {
            if (!acked_slots.insert(m2->slot).second) {
                error = BlockError::DUPLICATE_M2;
                return false;
            }
        } else if (std::holds_alternative<M4AckBundles>(*message)) {
            if (out.has_m4) {
                error = BlockError::DUPLICATE_M4;
                return false;
            }
            out.has_m4 = true;
        } else if (const auto* m7{std::get_if<M7BmmAccept>(&*message)}) {
            if (!accepted_slots.insert(m7->slot).second) {
                error = BlockError::DUPLICATE_M7;
                return false;
            }
        }
        // M3 has no duplicate rule of its own here. Two M3s naming the same
        // bundle for the same slot are still rejected, by the rule that a
        // bundle already pending cannot be proposed again -- the first one
        // makes it pending, and the second one then breaks that rule.

        out.messages.emplace_back(*message, vout);
    }

    return true;
}

} // namespace drivechain
