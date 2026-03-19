#!/usr/bin/env python3
# Copyright (c) 2021 yanmaani
# Licensed under CC0 (Public domain)

# Test the name_autoregister RPC.

from test_framework.names import NameTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class NameAutoregisterTest(NameTestFramework):

    def set_test_params(self):
        self.setup_name_test([[]] * 1)
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        node.generate(200)

        self.log.info("Test basic name_autoregister.")
        result = node.name_autoregister("d/autotest", "hello-world")
        assert "txid" in result
        assert "rand" in result
        txid = result["txid"]
        rand = result["rand"]
        assert_equal(len(txid), 64)
        assert_equal(len(rand), 40)

        self.log.info("Check name_new is in mempool.")
        assert txid in node.getrawmempool()

        self.log.info("Check name_firstupdate is queued.")
        queued = node.listqueuedtransactions()
        assert_equal(len(queued), 1)

        self.log.info("Check that re-registering the same name fails.")
        # The name_new is in the mempool, not yet mined, so existsName
        # won't see it yet. But let's mine it and then try.
        node.generate(1)
        # name_new is confirmed but name is not yet active (no firstupdate).
        # However, we should be able to test the already-exists check
        # after the name fully registers.

        self.log.info("Wait for maturity (12 blocks after name_new confirms).")
        node.generate(12)

        self.log.info("Check that the queued firstupdate has been broadcast.")
        queued = node.listqueuedtransactions()
        assert_equal(len(queued), 0)

        self.log.info("Mine the firstupdate.")
        node.generate(1)

        self.log.info("Verify the name is registered.")
        self.checkName(0, "d/autotest", "hello-world", 30, False)

        self.log.info("Test that registering an existing name fails.")
        assert_raises_rpc_error(-25, "exists already",
                                node.name_autoregister, "d/autotest")

        self.log.info("Test name_autoregister with default (empty) value.")
        result2 = node.name_autoregister("d/autotest2")
        assert "txid" in result2
        node.generate(14)
        self.checkName(0, "d/autotest2", "", None, False)

        self.log.info("Test that too-long name is rejected.")
        assert_raises_rpc_error(-8, "name is too long",
                                node.name_autoregister, "x" * 256)

        self.log.info("Test that too-long value is rejected.")
        assert_raises_rpc_error(-8, "value is too long",
                                node.name_autoregister, "d/longval", "x" * 521)

        self.log.info("Test coin locking - name_new output should be locked.")
        result3 = node.name_autoregister("d/locktest", "lock-value")
        txid3 = result3["txid"]
        locked = node.listlockunspent()
        found = False
        for entry in locked:
            if entry["txid"] == txid3:
                found = True
                break
        assert found, "name_new output should be locked"

        self.log.info("All tests passed!")


if __name__ == '__main__':
    NameAutoregisterTest().main()
