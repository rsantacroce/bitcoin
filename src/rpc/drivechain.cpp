// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <drivechain/params.h>
#include <drivechain/state.h>
#include <kernel/cs_main.h>
#include <rpc/blockchain.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sync.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <validation.h>

#include <vector>

using node::NodeContext;

//! Everything the sidechain services need to read out of a node's BIP300 state.
//!
//! Consensus does not read any of this back, so nothing here can make a block
//! valid or invalid. It exists because a sidechain has no other way to learn
//! whether its slot is still occupied, how close a withdrawal is to being
//! payable, or which treasury output to spend next.
namespace {

//! The height a BIP300 age is measured from, or -1 when the rules are not on.
int DrivechainTipHeight(const ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);
    const CBlockIndex* tip{chainman.ActiveChain().Tip()};
    return tip ? tip->nHeight : -1;
}

UniValue SidechainEntry(const drivechain::Sidechain& sidechain, int tip_height)
{
    UniValue entry{UniValue::VOBJ};
    entry.pushKV("slot", int{sidechain.slot});
    // Opaque to consensus, so it is handed back exactly as it was proposed
    // rather than interpreted.
    entry.pushKV("description", HexStr(sidechain.description));
    entry.pushKV("proposal_id", sidechain.Id().description_hash.ToString());
    entry.pushKV("vote_count", int{sidechain.vote_count});
    entry.pushKV("proposal_height", sidechain.proposal_height);
    entry.pushKV("age", std::max(0, tip_height - sidechain.proposal_height));
    if (sidechain.activation_height != drivechain::NO_HEIGHT) {
        entry.pushKV("activation_height", sidechain.activation_height);
    }
    return entry;
}

RPCHelpMan listsidechains()
{
    return RPCHelpMan{
        "listsidechains",
        "Returns the sidechains occupying a slot, and the treasury of each.\n",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                 {
                     {RPCResult::Type::NUM, "slot", "The sidechain slot, 0 to 255"},
                     {RPCResult::Type::STR_HEX, "description", "The sidechain description, opaque to consensus"},
                     {RPCResult::Type::STR_HEX, "proposal_id", "sha256d of the description"},
                     {RPCResult::Type::NUM, "vote_count", "Acks the proposal had when it activated"},
                     {RPCResult::Type::NUM, "proposal_height", "Height of the block that proposed it"},
                     {RPCResult::Type::NUM, "age", "Blocks since it was proposed"},
                     {RPCResult::Type::NUM, "activation_height", "Height at which it activated"},
                     {RPCResult::Type::OBJ, "treasury", /*optional=*/true, "The slot's treasury output, if it has one",
                      {
                          {RPCResult::Type::STR_HEX, "txid", "Transaction holding the treasury"},
                          {RPCResult::Type::NUM, "vout", "Output index of the treasury"},
                          {RPCResult::Type::STR_AMOUNT, "amount", "Value held in the treasury"},
                      }},
                 }},
            }},
        RPCExamples{HelpExampleCli("listsidechains", "") + HelpExampleRpc("listsidechains", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            ChainstateManager& chainman{EnsureAnyChainman(request.context)};
            LOCK(cs_main);
            const drivechain::DrivechainState& state{chainman.ActiveChainstate().DrivechainTip()};
            const int tip_height{DrivechainTipHeight(chainman)};

            UniValue result{UniValue::VARR};
            for (const auto& [slot, sidechain] : state.ActiveSidechains()) {
                UniValue entry{SidechainEntry(sidechain, tip_height)};
                if (const drivechain::Ctip* ctip{state.GetCtip(slot)}) {
                    UniValue treasury{UniValue::VOBJ};
                    treasury.pushKV("txid", ctip->outpoint.hash.ToString());
                    treasury.pushKV("vout", static_cast<uint64_t>(ctip->outpoint.n));
                    treasury.pushKV("amount", ValueFromAmount(ctip->value));
                    entry.pushKV("treasury", std::move(treasury));
                }
                result.push_back(std::move(entry));
            }
            return result;
        },
    };
}

RPCHelpMan listsidechainproposals()
{
    return RPCHelpMan{
        "listsidechainproposals",
        "Returns the sidechain proposals still gathering votes.\n"
        "A slot may have several at once, each with its own count.\n",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                 {
                     {RPCResult::Type::NUM, "slot", "The slot the proposal wants"},
                     {RPCResult::Type::STR_HEX, "description", "The sidechain description, opaque to consensus"},
                     {RPCResult::Type::STR_HEX, "proposal_id", "sha256d of the description, which an M2 acks"},
                     {RPCResult::Type::NUM, "vote_count", "Acks accumulated so far"},
                     {RPCResult::Type::NUM, "proposal_height", "Height of the block that proposed it"},
                     {RPCResult::Type::NUM, "age", "Blocks since it was proposed"},
                     {RPCResult::Type::NUM, "votes_required", "Acks needed to activate, which is one more than the threshold"},
                 }},
            }},
        RPCExamples{HelpExampleCli("listsidechainproposals", "") + HelpExampleRpc("listsidechainproposals", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            ChainstateManager& chainman{EnsureAnyChainman(request.context)};
            LOCK(cs_main);
            const drivechain::DrivechainState& state{chainman.ActiveChainstate().DrivechainTip()};
            const drivechain::Thresholds& thresholds{chainman.GetConsensus().drivechain_thresholds};
            const int tip_height{DrivechainTipHeight(chainman)};

            UniValue result{UniValue::VARR};
            for (const auto& [id, proposal] : state.Proposals()) {
                UniValue entry{SidechainEntry(proposal, tip_height)};
                // The bar depends on whether the slot is already occupied, and
                // it is strict, so activation needs one more than it.
                const bool slot_is_used{state.IsActive(proposal.slot)};
                const uint16_t threshold{slot_is_used ? thresholds.used_slot_activation_threshold
                                                      : thresholds.unused_slot_activation_threshold};
                entry.pushKV("votes_required", int{threshold} + 1);
                result.push_back(std::move(entry));
            }
            return result;
        },
    };
}

RPCHelpMan listwithdrawalbundles()
{
    return RPCHelpMan{
        "listwithdrawalbundles",
        "Returns the withdrawal bundles pending for a sidechain slot, in the order miners vote on them.\n",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot"},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                 {
                     {RPCResult::Type::NUM, "index", "Position in the slot's list, which is what an M4 votes by"},
                     {RPCResult::Type::STR_HEX, "m6id", "The blinded withdrawal's txid"},
                     {RPCResult::Type::NUM, "vote_count", "Upvotes accumulated so far"},
                     {RPCResult::Type::NUM, "proposal_height", "Height of the block that proposed it"},
                     {RPCResult::Type::NUM, "age", "Blocks since it was proposed"},
                     {RPCResult::Type::NUM, "votes_required", "Upvotes needed before it may be paid out"},
                     {RPCResult::Type::BOOL, "payable", "Whether it has enough upvotes to be paid out now"},
                 }},
            }},
        RPCExamples{HelpExampleCli("listwithdrawalbundles", "0") + HelpExampleRpc("listwithdrawalbundles", "0")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            const int slot_arg{request.params[0].getInt<int>()};
            if (slot_arg < 0 || slot_arg > 255) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Sidechain slot must be between 0 and 255");
            }
            const drivechain::SlotNum slot{static_cast<drivechain::SlotNum>(slot_arg)};

            ChainstateManager& chainman{EnsureAnyChainman(request.context)};
            LOCK(cs_main);
            const drivechain::DrivechainState& state{chainman.ActiveChainstate().DrivechainTip()};
            const drivechain::PendingWithdrawals* pending{state.GetPendingWithdrawals(slot)};
            if (pending == nullptr) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "No sidechain is active in that slot");
            }
            const drivechain::Thresholds& thresholds{chainman.GetConsensus().drivechain_thresholds};
            const int tip_height{DrivechainTipHeight(chainman)};

            UniValue result{UniValue::VARR};
            for (size_t index{0}; index < pending->size(); ++index) {
                const drivechain::PendingWithdrawal& bundle{(*pending)[index]};
                UniValue entry{UniValue::VOBJ};
                entry.pushKV("index", static_cast<uint64_t>(index));
                entry.pushKV("m6id", bundle.m6id.ToString());
                entry.pushKV("vote_count", int{bundle.vote_count});
                entry.pushKV("proposal_height", bundle.proposal_height);
                entry.pushKV("age", std::max(0, tip_height - bundle.proposal_height));
                entry.pushKV("votes_required", int{thresholds.withdrawal_bundle_inclusion_threshold} + 1);
                entry.pushKV("payable", thresholds.BundleIsPayable(bundle.vote_count));
                result.push_back(std::move(entry));
            }
            return result;
        },
    };
}

} // namespace

void RegisterDrivechainRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"drivechain", &listsidechains},
        {"drivechain", &listsidechainproposals},
        {"drivechain", &listwithdrawalbundles},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
