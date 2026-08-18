#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.
"""Test BIP300/BIP301 block validation.

Covers the rules a node can be driven into from outside: a coinbase carrying
two of a message that may only appear once, and a blind-merged-mining request
with nothing accepting it. Both make the block invalid, and both are checked
against a node that is enforcing and one that is not, since the whole point of
the design is that the second still accepts everything the first does.
"""

import copy

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, hash256
from test_framework.script import CScript, OP_NOP5, OP_RETURN, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet

M1_TAG = b"\xd5\xe0\xc4\xaf"
M4_TAG = b"\xd7\x7d\x17\x76"
M7_TAG = b"\xd1\x61\x73\x68"
M8_TAG = b"\x00\xbf\x00"
M2_TAG = b"\xd6\xe1\xc5\xdf"
M3_TAG = b"\xd4\x5a\xa9\x43"

ACTIVATION_HEIGHT = 1
# The short thresholds regtest carries: a proposal needs more than five acks
# inside ten blocks, and so does a withdrawal bundle.
ACTIVATION_THRESHOLD = 5


def message_output(tag, body):
    """An OP_RETURN output carrying a BIP300/BIP301 coinbase message."""
    return CTxOut(0, CScript([OP_RETURN, tag + body]))


def treasury_script(slot):
    """The scriptPubKey of a sidechain treasury: OP_DRIVECHAIN <slot> OP_TRUE.

    Built byte by byte rather than through CScript, because the slot is a raw
    unsigned byte and a script-number encoding would get three quarters of the
    range wrong.
    """
    return CScript(bytes([OP_NOP5, 0x01, slot, OP_TRUE]))


def address_output():
    """The opaque sidechain address a deposit must carry after its treasury."""
    return CTxOut(0, CScript([OP_RETURN, b"\xab\xcd"]))


def blinded_m6id(tx, treasury_spent):
    """The M6ID miners vote on, which is the txid of the blinded withdrawal.

    An independent implementation of the rule, written from BIP-300 rather than
    from the C++: empty the inputs, and replace the treasury change at vout[0]
    with a zero-value OP_RETURN carrying the fee as eight bytes big-endian. If
    this disagrees with the node, one of the two is wrong.
    """
    payouts = sum(out.nValue for out in tx.vout[1:])
    fee = treasury_spent - tx.vout[0].nValue - payouts
    assert fee >= 0

    blinded = copy.deepcopy(tx)
    blinded.vin = []
    blinded.wit.vtxinwit = []
    blinded.vout[0] = CTxOut(0, CScript([OP_RETURN, fee.to_bytes(8, "big")]))
    # Internal byte order, which is what the wire carries.
    return bytes.fromhex(blinded.txid_hex)[::-1]


class DrivechainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # One node enforcing, one not: BIP300 is a soft fork, so the second
        # must accept every block the first does.
        self.extra_args = [
            [f"-testactivationheight=drivechain@{ACTIVATION_HEIGHT}"],
            [],
        ]

    def deposit_tx(self, slot, amount, with_address=True, spend_treasury=None):
        """A transaction moving `amount` into a sidechain treasury."""
        utxo = self.wallet.get_utxo()
        funded = self.wallet.create_self_transfer_multi(utxos_to_spend=[utxo], num_outputs=1)["tx"]
        change = int(utxo["value"] * COIN) - amount - 1000
        if spend_treasury is not None:
            # A later deposit must also spend the treasury it replaces, and the
            # new treasury holds both.
            amount += spend_treasury[1]

        funded.vout = [CTxOut(amount, treasury_script(slot))]
        if with_address:
            funded.vout.append(address_output())
        funded.vout.append(CTxOut(change, self.wallet.get_output_script()))
        self.wallet.sign_tx(funded)

        if spend_treasury is not None:
            # Added after signing, and with an empty witness: a treasury output
            # is anyone-can-spend, so it takes neither signature nor witness,
            # and giving it one makes the script check fail before any of the
            # BIP300 rules are reached.
            funded.vin.append(CTxIn(spend_treasury[0]))
            funded.wit.vtxinwit.append(CTxInWitness())
        return funded

    def build_block(self, extra_coinbase_outputs=None, extra_txs=None):
        tip = self.nodes[0].getbestblockhash()
        height = self.nodes[0].getblockcount() + 1
        block_time = self.nodes[0].getblock(tip)["mediantime"] + 1
        block = create_block(int(tip, 16), create_coinbase(height), block_time)
        for output in extra_coinbase_outputs or []:
            block.vtx[0].vout.append(output)
        for tx in extra_txs or []:
            block.vtx.append(tx)
        # The wallet's transactions carry witnesses, so the coinbase needs the
        # commitment. It appends an output, which the message rules ignore.
        add_witness_commitment(block)
        block.solve()
        return block

    def assert_rejected(self, block, reason):
        assert_equal(self.nodes[0].submitblock(block.serialize().hex()), reason)
        assert_equal(self.nodes[0].getbestblockhash(), f"{block.hashPrevBlock:064x}")
        # A node that does not enforce these rules accepts the same block,
        # which is what makes this a soft fork rather than a chain split. It
        # may already have it by relay, so what is asserted is where it ends
        # up rather than what submitblock says.
        self.nodes[1].submitblock(block.serialize().hex())
        assert_equal(self.nodes[1].getbestblockhash(), block.hash_hex)
        self.nodes[1].invalidateblock(block.hash_hex)

    def run_test(self):
        node = self.nodes[0]

        self.log.info("An ordinary block connects with the rules active")
        self.generate(node, 3)
        assert_equal(node.getblockcount(), 3)

        self.log.info("A coinbase carrying an ordinary message is fine")
        block = self.build_block([message_output(M1_TAG, bytes([1]) + b"sidechain")])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash_hex)
        self.sync_blocks()

        self.log.info("Two identical sidechain proposals invalidate the block")
        proposal = message_output(M1_TAG, bytes([1]) + b"sidechain")
        block = self.build_block([proposal, proposal])
        self.assert_rejected(block, "bad-drivechain-duplicate-m1")

        self.log.info("Two proposals for one slot are fine if they differ")
        block = self.build_block([
            message_output(M1_TAG, bytes([1]) + b"alpha"),
            message_output(M1_TAG, bytes([1]) + b"beta"),
        ])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        self.sync_blocks()

        self.log.info("Two vote messages invalidate the block whatever they say")
        block = self.build_block([
            message_output(M4_TAG, bytes([0x00])),
            message_output(M4_TAG, bytes([0x03])),
        ])
        self.assert_rejected(block, "bad-drivechain-duplicate-m4")

        self.log.info("Two accepts for one sidechain slot invalidate the block")
        accept = message_output(M7_TAG, bytes([1]) + bytes(32))
        block = self.build_block([accept, accept])
        self.assert_rejected(block, "bad-drivechain-duplicate-m7")

        self.log.info("The chain survives a reorg through a block with messages")
        before = node.getbestblockhash()
        height_before = node.getblockcount()
        block = self.build_block([message_output(M1_TAG, bytes([2]) + b"gamma")])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        self.sync_blocks()
        node.invalidateblock(block.hash_hex)
        assert_equal(node.getbestblockhash(), before)
        assert_equal(node.getblockcount(), height_before)
        # And can be reconnected, which exercises the diff both ways.
        node.reconsiderblock(block.hash_hex)
        assert_equal(node.getbestblockhash(), block.hash_hex)

        self.log.info("A sidechain can be voted all the way into a slot")
        description = b"a sidechain"
        proposal_id = hash256(description)
        # RPC prints hashes in the reversed order Bitcoin displays them in,
        # while the wire carries them forwards.
        proposal_id_rpc = proposal_id[::-1].hex()
        block = self.build_block([message_output(M1_TAG, bytes([0]) + description)])
        assert_equal(node.submitblock(block.serialize().hex()), None)

        # Earlier blocks proposed sidechains of their own, so this picks out
        # the one being voted on rather than assuming it is alone.
        def find_proposal():
            return [p for p in node.listsidechainproposals() if p["proposal_id"] == proposal_id_rpc]

        proposal = find_proposal()
        assert_equal(len(proposal), 1)
        assert_equal(proposal[0]["slot"], 0)
        assert_equal(proposal[0]["description"], description.hex())
        assert_equal(proposal[0]["vote_count"], 0)
        # Strictly more than the threshold, which is where both prior drafts
        # of BIP-300 get the boundary wrong.
        assert_equal(proposal[0]["votes_required"], ACTIVATION_THRESHOLD + 1)
        assert_equal(node.listsidechains(), [])

        # One ack per block: a second in the same block invalidates it, and an
        # ack in the block that proposed it does not count.
        ack = message_output(M2_TAG, bytes([0]) + proposal_id)
        for expected_votes in range(1, ACTIVATION_THRESHOLD + 1):
            block = self.build_block([ack])
            assert_equal(node.submitblock(block.serialize().hex()), None)
            assert_equal(find_proposal()[0]["vote_count"], expected_votes)
            assert_equal(node.listsidechains(), [])

        self.log.info("The ack that crosses the threshold activates it")
        block = self.build_block([ack])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(find_proposal(), [])
        sidechains = node.listsidechains()
        assert_equal(len(sidechains), 1)
        assert_equal(sidechains[0]["slot"], 0)
        assert_equal(sidechains[0]["description"], description.hex())
        assert_equal(sidechains[0]["vote_count"], ACTIVATION_THRESHOLD + 1)
        assert "treasury" not in sidechains[0]
        self.sync_blocks()

        self.log.info("A withdrawal bundle can be proposed for it")
        assert_equal(node.listwithdrawalbundles(0), [])
        m6id = b"\x11" * 32
        block = self.build_block([message_output(M3_TAG, bytes([0]) + m6id)])
        assert_equal(node.submitblock(block.serialize().hex()), None)

        bundles = node.listwithdrawalbundles(0)
        assert_equal(len(bundles), 1)
        assert_equal(bundles[0]["index"], 0)
        assert_equal(bundles[0]["m6id"], m6id[::-1].hex())
        # Being proposed counts as the bundle's first upvote.
        assert_equal(bundles[0]["vote_count"], 1)
        assert_equal(bundles[0]["payable"], False)

        self.log.info("Proposing the same bundle again invalidates the block")
        block = self.build_block([message_output(M3_TAG, bytes([0]) + m6id)])
        self.assert_rejected(block, "bad-drivechain-m3-bundle-already-pending")

        self.log.info("A bundle for a slot with no sidechain invalidates the block")
        block = self.build_block([message_output(M3_TAG, bytes([9]) + m6id)])
        self.assert_rejected(block, "bad-drivechain-m3-inactive-sidechain")

        self.log.info("Bundles for an empty slot cannot be listed")
        assert_raises_rpc_error(-8, "No sidechain is active in that slot", node.listwithdrawalbundles, 9)

        self.log.info("Coins can be deposited into the sidechain treasury")
        self.wallet = MiniWallet(node)
        # Mined to the wallet's own script, so it has something to spend.
        self.generate(self.wallet, 101)

        deposit = self.deposit_tx(slot=0, amount=10 * COIN)
        block = self.build_block(extra_txs=[deposit])
        assert_equal(node.submitblock(block.serialize().hex()), None)

        treasury = node.listsidechains()[0]["treasury"]
        assert_equal(treasury["txid"], deposit.txid_hex)
        assert_equal(treasury["vout"], 0)
        assert_equal(int(treasury["amount"] * COIN), 10 * COIN)
        self.sync_blocks()

        self.log.info("A deposit without its address output invalidates the block")
        bad = self.deposit_tx(
            slot=0,
            amount=1 * COIN,
            with_address=False,
            spend_treasury=(COutPoint(int(deposit.txid_hex, 16), 0), 10 * COIN),
        )
        self.assert_rejected(self.build_block(extra_txs=[bad]), "bad-drivechain-missing-deposit-address")

        self.log.info("Taking the treasury without putting one back invalidates the block")
        theft = self.wallet.create_self_transfer_multi(
            utxos_to_spend=[self.wallet.get_utxo()], num_outputs=1
        )["tx"]
        theft.vin = [CTxIn(COutPoint(int(deposit.txid_hex, 16), 0))]
        theft.wit.vtxinwit = []
        theft.vout = [CTxOut(10 * COIN - 1000, self.wallet.get_output_script())]
        self.assert_rejected(
            self.build_block(extra_txs=[theft]), "bad-drivechain-treasury-spent-without-replacement"
        )

        self.log.info("A withdrawal needs a bundle the miners voted for")
        withdrawal = CTransaction()
        withdrawal.version = 2
        withdrawal.vin = [CTxIn(COutPoint(int(deposit.txid_hex, 16), 0))]
        withdrawal.vout = [
            CTxOut(6 * COIN, treasury_script(0)),
            CTxOut(3 * COIN, self.wallet.get_output_script()),
        ]
        m6id = blinded_m6id(withdrawal, 10 * COIN)

        # Not proposed at all.
        self.assert_rejected(
            self.build_block(extra_txs=[withdrawal]), "bad-drivechain-m6-unknown-bundle"
        )

        block = self.build_block([message_output(M3_TAG, bytes([0]) + m6id)])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        bundles = node.listwithdrawalbundles(0)
        # The node computed the same M6ID from the same transaction, which is
        # what makes the vote a commitment to these payouts and this fee.
        assert_equal(bundles[-1]["m6id"], m6id[::-1].hex())
        assert_equal(bundles[-1]["vote_count"], 1)

        # Proposed but not yet approved.
        self.assert_rejected(
            self.build_block(extra_txs=[withdrawal]), "bad-drivechain-m6-insufficient-votes"
        )

        self.log.info("Voting the bundle past the threshold makes it payable")
        index = len(node.listwithdrawalbundles(0)) - 1
        while not node.listwithdrawalbundles(0)[index]["payable"]:
            block = self.build_block([message_output(M4_TAG, bytes([0x01, index]))])
            assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.listwithdrawalbundles(0)[index]["vote_count"], ACTIVATION_THRESHOLD + 1)

        self.log.info("The approved withdrawal pays out")
        block = self.build_block(extra_txs=[withdrawal])
        assert_equal(node.submitblock(block.serialize().hex()), None)

        treasury = node.listsidechains()[0]["treasury"]
        assert_equal(treasury["txid"], withdrawal.txid_hex)
        assert_equal(int(treasury["amount"] * COIN), 6 * COIN)
        # Paid out, so no longer pending.
        assert_equal(node.listwithdrawalbundles(0), [])
        self.sync_blocks()

        self.log.info("The state survives a restart")
        tip = node.getbestblockhash()
        self.restart_node(0, self.extra_args[0])
        assert_equal(node.getbestblockhash(), tip)
        # What was voted in is still voted in, and the treasury is where the
        # withdrawal left it.
        sidechains = node.listsidechains()
        assert_equal(len(sidechains), 1)
        assert_equal(sidechains[0]["description"], description.hex())
        assert_equal(int(sidechains[0]["treasury"]["amount"] * COIN), 6 * COIN)
        assert_equal(node.listwithdrawalbundles(0), [])
        self.generate(node, 1, sync_fun=self.no_op)


if __name__ == "__main__":
    DrivechainTest(__file__).main()
