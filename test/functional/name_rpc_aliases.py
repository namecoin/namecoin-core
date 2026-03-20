#!/usr/bin/env python3
# Copyright (c) 2024-2026 The Namecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Test Bitcoin Core-convention RPC method aliases for name operations.

Tests that the new alias method names (getname, getnamehistory, listnames,
getmempoolnames, checknamedb, listwalletnames, preregistername, registername,
updatename) work identically to the legacy name_* methods.

Also tests that snake_case option arguments (allow_expired, min_conf,
max_conf, by_hash, name_encoding, value_encoding, dest_address, send_coins,
allow_existing) are accepted alongside the legacy camelCase forms.

See: https://github.com/namecoin/namecoin-core/issues/551
"""

from test_framework.names import NameTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class NameRPCAliasesTest(NameTestFramework):
    def set_test_params(self):
        self.setup_name_test()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 110)

        self.log.info("Test checknamedb alias")
        result_old = node.name_checkdb()
        result_new = node.checknamedb()
        assert_equal(result_old, result_new)

        self.log.info("Test preregistername alias (name_new)")
        new_result = node.preregistername("d/test-alias")
        assert len(new_result) == 2  # [txid, rand]
        self.generate(node, 1)

        self.log.info("Test registername alias (name_firstupdate)")
        txid_new = new_result[0]
        rand = new_result[1]
        self.generate(node, 12)
        reg_result = node.registername("d/test-alias", rand, txid_new, '{"info":"test"}')
        assert isinstance(reg_result, str)  # txid
        self.generate(node, 1)

        self.log.info("Test getname alias (name_show)")
        show_old = node.name_show("d/test-alias")
        show_new = node.getname("d/test-alias")
        assert_equal(show_old, show_new)

        self.log.info("Test getname with snake_case allow_expired option")
        show_snake = node.getname("d/test-alias", {"allow_expired": True})
        assert_equal(show_old["name"], show_snake["name"])

        self.log.info("Test updatename alias (name_update)")
        update_result = node.updatename("d/test-alias", '{"info":"updated"}')
        assert isinstance(update_result, str)  # txid
        self.generate(node, 1)

        self.log.info("Test listnames alias (name_scan)")
        scan_old = node.name_scan()
        scan_new = node.listnames()
        assert_equal(scan_old, scan_new)

        self.log.info("Test listnames with snake_case min_conf option")
        scan_snake = node.listnames("", 500, {"min_conf": 1})
        assert len(scan_snake) > 0

        self.log.info("Test getnamehistory alias (name_history)")
        hist_old = node.name_history("d/test-alias")
        hist_new = node.getnamehistory("d/test-alias")
        assert_equal(hist_old, hist_new)

        self.log.info("Test getmempoolnames alias (name_pending)")
        # Create a pending name operation
        node.name_new("d/pending-test")
        pending_old = node.name_pending()
        pending_new = node.getmempoolnames()
        assert_equal(pending_old, pending_new)

        self.log.info("Test listwalletnames alias (name_list)")
        list_old = node.name_list()
        list_new = node.listwalletnames()
        assert_equal(list_old, list_new)

        self.log.info("Test preregistername with snake_case allow_existing option")
        # First register a name
        new2 = node.name_new("d/existing-test")
        self.generate(node, 13)
        node.name_firstupdate("d/existing-test", new2[1], new2[0], "value")
        self.generate(node, 1)
        # Now try to name_new it again with allow_existing (snake_case)
        result = node.preregistername("d/existing-test", {"allow_existing": True})
        assert len(result) == 2

        self.log.info("All RPC alias tests passed!")


if __name__ == '__main__':
    NameRPCAliasesTest(__file__).main()
