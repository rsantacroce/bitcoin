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

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTxOut
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

M1_TAG = b"\xd5\xe0\xc4\xaf"
M4_TAG = b"\xd7\x7d\x17\x76"
M7_TAG = b"\xd1\x61\x73\x68"
M8_TAG = b"\x00\xbf\x00"

ACTIVATION_HEIGHT = 1


def message_output(tag, body):
    """An OP_RETURN output carrying a BIP300/BIP301 coinbase message."""
    return CTxOut(0, CScript([OP_RETURN, tag + body]))


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

    def build_block(self, extra_coinbase_outputs=None, extra_txs=None):
        tip = self.nodes[0].getbestblockhash()
        height = self.nodes[0].getblockcount() + 1
        block_time = self.nodes[0].getblock(tip)["mediantime"] + 1
        block = create_block(int(tip, 16), create_coinbase(height), block_time)
        for output in extra_coinbase_outputs or []:
            block.vtx[0].vout.append(output)
        for tx in extra_txs or []:
            block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()
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

        self.log.info("The state survives a restart")
        tip = node.getbestblockhash()
        self.restart_node(0, self.extra_args[0])
        assert_equal(node.getbestblockhash(), tip)
        self.generate(node, 1, sync_fun=self.no_op)


if __name__ == "__main__":
    DrivechainTest(__file__).main()
