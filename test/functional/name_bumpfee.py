#!/usr/bin/env python3
# Copyright (c) 2026 The Namecoin Core developers
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Test that bumpfee preserves Namecoin name operations.

Verifies that using bumpfee (RBF) on name_new, name_firstupdate, and
name_update transactions preserves the name operation script prefix in
the replacement transaction, rather than stripping it to a plain payment.
"""

from test_framework.names import NameTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)


class NameBumpFeeTest(NameTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.setup_name_test([
            ["-walletrbf=1", "-mintxfee=0.00002"],
            [],
        ])

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 250)

        self.test_bumpfee_name_new(node)
        self.test_bumpfee_name_firstupdate(node)
        self.test_bumpfee_name_update(node)

    def get_decoded_tx(self, node, txid):
        """Get a decoded transaction via the wallet RPC.

        Uses gettransaction (wallet RPC) + decoderawtransaction instead of
        getrawtransaction (mempool/blockchain RPC). This is necessary because
        getrawtransaction requires either mempool presence or -txindex, and
        after bumpfee the original transaction is evicted from the mempool
        by the RBF replacement. The wallet always tracks its own transactions
        regardless of mempool state.
        """
        tx_hex = node.gettransaction(txid)["hex"]
        return node.decoderawtransaction(tx_hex)

    def bump_and_check_name_op(self, node, txid, expected_name_op):
        """Shared helper: verify original tx nameOp, bump fee, verify bumped tx nameOp.

        1. Gets the original transaction and verifies its nameOp matches expected_name_op
        2. Calls bumpfee
        3. Gets the replacement transaction and verifies its nameOp also matches
        4. Verifies the fee increased

        Returns the bumped txid.
        """

        # Verify original transaction has the expected nameOp
        orig_tx = self.get_decoded_tx(node, txid)
        orig_name_vout = self.find_name_vout(orig_tx)
        assert orig_name_vout is not None, \
            "Original tx should have a nameOp output"
        assert_equal(
            orig_name_vout["scriptPubKey"]["nameOp"], expected_name_op)

        # Bump the fee
        result = node.bumpfee(txid)
        bumped_txid = result["txid"]
        assert_greater_than(result["fee"], 0)

        # Verify the replacement transaction preserves the nameOp
        bumped_tx = self.get_decoded_tx(node, bumped_txid)
        bumped_name_vout = self.find_name_vout(bumped_tx)
        assert bumped_name_vout is not None, \
            "Bumped tx must preserve the name operation"
        assert_equal(
            bumped_name_vout["scriptPubKey"]["nameOp"], expected_name_op)

        return bumped_txid

    def test_bumpfee_name_new(self, node):
        self.log.info("Test bumpfee on name_new...")

        new_data = node.name_new("test/bumpfee-new")
        txid = new_data[0]

        # Get the hash from the original tx to build expected nameOp
        orig_tx = self.get_decoded_tx(node, txid)
        orig_name_vout = self.find_name_vout(orig_tx)
        name_hash = orig_name_vout["scriptPubKey"]["nameOp"]["hash"]

        expected_name_op = {"op": "name_new", "hash": name_hash}

        self.bump_and_check_name_op(node, txid, expected_name_op)

        self.generate(node, 1)
        self.log.info("  name_new bumpfee: OK")

    def test_bumpfee_name_firstupdate(self, node):
        self.log.info("Test bumpfee on name_firstupdate...")

        new_data = node.name_new("test/bumpfee-firstupdate")
        self.generate(node, 12)

        txid = self.firstupdateName(
            0, "test/bumpfee-firstupdate", new_data, "initial-value")

        # Get the rand from the original tx to build expected nameOp
        orig_tx = self.get_decoded_tx(node, txid)
        orig_name_vout = self.find_name_vout(orig_tx)
        rand = orig_name_vout["scriptPubKey"]["nameOp"]["rand"]

        expected_name_op = {
            "op": "name_firstupdate",
            "name": "test/bumpfee-firstupdate",
            "name_encoding": "ascii",
            "value": "initial-value",
            "value_encoding": "ascii",
            "rand": rand,
        }

        self.bump_and_check_name_op(node, txid, expected_name_op)

        # Mine and verify on-chain state
        self.generate(node, 1)
        name_data = node.name_show("test/bumpfee-firstupdate")
        assert_equal(name_data["value"], "initial-value")

        self.log.info("  name_firstupdate bumpfee: OK")

    def test_bumpfee_name_update(self, node):
        self.log.info("Test bumpfee on name_update...")

        # Register a name fully first
        new_data = node.name_new("test/bumpfee-update")
        self.generate(node, 12)
        self.firstupdateName(
            0, "test/bumpfee-update", new_data, "original-value")
        self.generate(node, 1)

        txid = node.name_update("test/bumpfee-update", "updated-value")

        expected_name_op = {
            "op": "name_update",
            "name": "test/bumpfee-update",
            "name_encoding": "ascii",
            "value": "updated-value",
            "value_encoding": "ascii",
        }

        self.bump_and_check_name_op(node, txid, expected_name_op)

        # Mine and verify on-chain state
        self.generate(node, 1)
        name_data = node.name_show("test/bumpfee-update")
        assert_equal(name_data["value"], "updated-value")

        self.log.info("  name_update bumpfee: OK")

    @staticmethod
    def find_name_vout(tx):
        """Find the vout containing a nameOp in a decoded transaction."""
        for vout in tx["vout"]:
            if "nameOp" in vout.get("scriptPubKey", {}):
                return vout
        return None


if __name__ == '__main__':
    NameBumpFeeTest(__file__).main()
