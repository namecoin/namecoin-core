#!/usr/bin/env python3
# Copyright (c) 2025 The Namecoin developers
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Test the registername RPC method.

from test_framework.names import NameTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class RegisterNameTest(NameTestFramework):

    def set_test_params(self):
        self.setup_name_test([[]] * 1)
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 200)

        self.log.info("Test basic registername with auto name_new.")
        result = node.registername("d/regtest", "hello-world")
        assert "txid" in result
        assert "rand" in result
        txid = result["txid"]
        rand_val = result["rand"]
        assert_equal(len(txid), 64)
        assert_equal(len(rand_val), 40)

        self.log.info("Check name_new is in mempool.")
        mempool = node.getrawmempool()
        assert txid in mempool

        self.log.info("Mine name_new + firstupdate into a block.")
        self.generate(node, 1)

        self.log.info("Wait for maturity (12 blocks after name_new confirms).")
        self.generate(node, 12)

        self.log.info("Mine the firstupdate.")
        self.generate(node, 1)

        self.log.info("Verify the name is registered.")
        self.checkName(0, "d/regtest", "hello-world", 30, False)

        self.log.info("Test that registering an existing name fails.")
        assert_raises_rpc_error(-25, "exists already",
                                node.registername, "d/regtest", "new-val")

        self.log.info("Test registername with default (empty) value.")
        result2 = node.registername("d/regtest2")
        assert "txid" in result2
        assert "rand" in result2
        self.generate(node, 14)
        self.checkName(0, "d/regtest2", "", 30, False)

        self.log.info("Test that too-long name is rejected.")
        assert_raises_rpc_error(-8, "name is too long",
                                node.registername, "x" * 256)

        self.log.info("Test that too-long value is rejected.")
        assert_raises_rpc_error(-8, "value is too long",
                                node.registername, "d/longval", "x" * 521)

        self.log.info("Test registername with existing name_new commitment.")
        new_data = node.name_new("d/existing_new")
        self.generate(node, 12)

        result3 = node.registername("d/existing_new", "reuse-commitment")
        assert "txid" in result3
        assert "rand" in result3
        self.generate(node, 1)
        self.checkName(0, "d/existing_new", "reuse-commitment", 30, False)

        self.log.info("Test return format is JSON object (not array).")
        result4 = node.registername("d/format_test", "test-value")
        assert isinstance(result4, dict)
        assert "txid" in result4
        assert "rand" in result4
        assert isinstance(result4["txid"], str)
        assert isinstance(result4["rand"], str)

        self.log.info("All tests passed!")


if __name__ == '__main__':
    RegisterNameTest().main()
