// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/messages.h>

#include <crypto/common.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace drivechain {
namespace {
//! Consume `tag` from the front of `payload`, returning what follows.
std::optional<std::span<const unsigned char>> MatchTag(std::span<const unsigned char> payload,
                                                       std::span<const unsigned char> tag)
{
    if (payload.size() < tag.size()) return std::nullopt;
    if (!std::equal(tag.begin(), tag.end(), payload.begin())) return std::nullopt;
    return payload.subspan(tag.size());
}

//! Parse `<slot> <32-byte hash>` and nothing else. M2 and M3 share this body.
std::optional<std::pair<SlotNum, uint256>> ParseSlotAndHash(std::span<const unsigned char> body)
{
    if (body.size() != 1 + 32) return std::nullopt;
    return std::make_pair(SlotNum{body[0]}, uint256{body.subspan(1, 32)});
}

std::optional<M4AckBundles> ParseM4Body(std::span<const unsigned char> body)
{
    if (body.empty()) return std::nullopt;

    M4AckBundles m4;
    m4.version = static_cast<M4AckBundles::Version>(body[0]);
    const std::span<const unsigned char> votes{body.subspan(1)};

    switch (m4.version) {
    case M4AckBundles::Version::REPEAT_PREVIOUS:
    case M4AckBundles::Version::UPVOTE_LEADING_BY_50:
        // Neither carries a vote array, so anything after the version byte
        // means this is not a well-formed M4.
        if (!votes.empty()) return std::nullopt;
        return m4;
    case M4AckBundles::Version::VOTES_ONE_BYTE:
        m4.upvotes.reserve(votes.size());
        for (const unsigned char vote : votes) {
            m4.upvotes.push_back(vote);
        }
        return m4;
    case M4AckBundles::Version::VOTES_TWO_BYTE:
        // A trailing odd byte is not a vote, so the message is malformed
        // rather than short by one.
        if (votes.size() % 2 != 0) return std::nullopt;
        m4.upvotes.reserve(votes.size() / 2);
        for (size_t i{0}; i < votes.size(); i += 2) {
            m4.upvotes.push_back(ReadLE16(votes.data() + i));
        }
        return m4;
    }
    // Version byte outside 0x00..0x03.
    return std::nullopt;
}
} // namespace

CScript TreasuryScript(SlotNum slot)
{
    CScript script;
    script << OP_DRIVECHAIN;
    // Not `script << slot`: that would encode the slot as a script number,
    // which sign-pads values above 127 and uses OP_1..OP_16 for small ones.
    // The slot is a raw byte behind a fixed single-byte push.
    script.push_back(0x01);
    script.push_back(slot);
    script << OP_TRUE;
    return script;
}

std::optional<SlotNum> ParseTreasuryScript(const CScript& script)
{
    if (script.size() != TREASURY_SCRIPT_SIZE) return std::nullopt;
    if (script[0] != OP_DRIVECHAIN) return std::nullopt;
    if (script[1] != 0x01) return std::nullopt;
    if (script[3] != OP_TRUE) return std::nullopt;
    return script[2];
}

std::optional<std::vector<unsigned char>> ParseOpReturnPayload(const CScript& script)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;

    if (!script.GetOp(pc, opcode) || opcode != OP_RETURN) return std::nullopt;

    std::vector<unsigned char> payload;
    if (!script.GetOp(pc, opcode, payload)) return std::nullopt;
    // GetOp only fills `payload` for data pushes. Anything above OP_PUSHDATA4
    // is an opcode, and leaves `payload` empty rather than failing, so the
    // check has to be explicit.
    if (opcode > OP_PUSHDATA4) return std::nullopt;

    if (pc != script.end()) return std::nullopt;

    return payload;
}

uint256 M1ProposeSidechain::ProposalId() const
{
    return Hash(description);
}

std::vector<uint16_t> M4AckBundles::NormalizedUpvotes() const
{
    if (version != Version::VOTES_ONE_BYTE) return upvotes;

    std::vector<uint16_t> normalized;
    normalized.reserve(upvotes.size());
    for (const uint16_t vote : upvotes) {
        switch (vote) {
        case ABSTAIN_ONE_BYTE: normalized.push_back(ABSTAIN_TWO_BYTES); break;
        case ALARM_ONE_BYTE: normalized.push_back(ALARM_TWO_BYTES); break;
        default: normalized.push_back(vote); break;
        }
    }
    return normalized;
}

std::optional<CoinbaseMessage> ParseCoinbaseMessage(const CScript& script)
{
    const auto payload{ParseOpReturnPayload(script)};
    if (!payload) return std::nullopt;

    if (const auto body{MatchTag(*payload, M1ProposeSidechain::TAG)}) {
        // The description is whatever follows the slot, and may be empty.
        if (body->empty()) return std::nullopt;
        return M1ProposeSidechain{
            .slot = (*body)[0],
            .description = {body->begin() + 1, body->end()},
        };
    }
    if (const auto body{MatchTag(*payload, M2AckSidechain::TAG)}) {
        const auto parsed{ParseSlotAndHash(*body)};
        if (!parsed) return std::nullopt;
        return M2AckSidechain{.slot = parsed->first, .proposal_id = parsed->second};
    }
    if (const auto body{MatchTag(*payload, M3ProposeBundle::TAG)}) {
        const auto parsed{ParseSlotAndHash(*body)};
        if (!parsed) return std::nullopt;
        return M3ProposeBundle{.slot = parsed->first, .m6id = Txid::FromUint256(parsed->second)};
    }
    if (const auto body{MatchTag(*payload, M4AckBundles::TAG)}) {
        const auto parsed{ParseM4Body(*body)};
        if (!parsed) return std::nullopt;
        return *parsed;
    }
    if (const auto body{MatchTag(*payload, M7BmmAccept::TAG)}) {
        const auto parsed{ParseSlotAndHash(*body)};
        if (!parsed) return std::nullopt;
        return M7BmmAccept{.slot = parsed->first, .sidechain_block_hash = parsed->second};
    }

    return std::nullopt;
}

std::optional<M8BmmRequest> ParseM8Request(const CScript& script)
{
    // A byte-exact match, not a parse of instructions. The push opcode is
    // pinned by the fixed message length, so OP_PUSHDATA1 does not qualify.
    if (script.size() != M8_SCRIPT_SIZE) return std::nullopt;
    if (script[0] != OP_RETURN) return std::nullopt;
    if (script[1] != M8_SCRIPT_SIZE - 2) return std::nullopt;

    const std::span<const unsigned char> payload{script.data() + 2, M8_SCRIPT_SIZE - 2};
    const auto body{MatchTag(payload, M8BmmRequest::TAG)};
    if (!body) return std::nullopt;

    return M8BmmRequest{
        .slot = (*body)[0],
        .sidechain_block_hash = uint256{body->subspan(1, 32)},
        .prev_main_block_hash = uint256{body->subspan(33, 32)},
    };
}

std::optional<M8BmmRequest> ParseM8Request(const CTransaction& tx)
{
    if (tx.vout.empty()) return std::nullopt;
    return ParseM8Request(tx.vout[0].scriptPubKey);
}

} // namespace drivechain
