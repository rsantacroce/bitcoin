// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/messages.h>

#include <script/script.h>

#include <optional>
#include <vector>

namespace drivechain {

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

} // namespace drivechain
