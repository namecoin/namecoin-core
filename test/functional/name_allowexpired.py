#!/usr/bin/env python3
# Licensed under CC0 (Public domain)

# Test that name_show displays expired names if allow_expired = true,
# and otherwise throws "not found" errors (if allow_expired = false).

from test_framework.names import NameTestFramework
from test_framework.util import *

class NameExpirationTest(NameTestFramework):

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_name_test([
            ["-allowexpired"],
            ["-noallowexpired"]
        ])

    def run_test(self):
        idx_allow = 0
        idx_disallow = 1
        node = self.nodes[idx_allow]
        node_disallow = self.nodes[idx_disallow]
        self.generate (node, 200)

        self.log.info("Begin registration of two names.")
        # "d/active" and (2) "d/expired".
        # "d/active" will be renewed.
        # "d/expired" will be let to lapse.
        #
        # To look up "d/expired" should either succeed or throw an error,
        # depending on the values of (1) the JSON option allow_expired
        # and (2) the command-line parameter -allowexpired.
        # Looking up "d/active" should always succeed regardless.
        new_active = node.name_new("d/active")
        new_expired = node.name_new("d/expired")
        self.generate (node, 12)

        self.log.info("Register the names.")
        self.firstupdateName(0, "d/active", new_active, "value")
        self.firstupdateName(0, "d/expired", new_expired, "value")
        self.generate (node, 1)
        # names on regtest expire after 30 blocks
        self.log.info("Wait 1 block, make sure domains registered.")
        self.checkName(0, "d/active", "value", 30, False)
        self.checkName(0, "d/expired", "value", 30, False)
        
        self.log.info("Let half a registration interval pass.")
        self.generate (node, 15)
        
        self.log.info("Renew d/active.")
        node.name_update("d/active", "renewed")
        # Don't renew d/expired. 
        self.log.info("Let d/expired lapse.")
        self.generate (node, 16)
        # 30 - 15 = 15

        self.log.info("Check default behaviors.")
        self.sync_blocks(self.nodes)
        self.checkName(idx_allow, "d/expired", "value", -1, True)
        assert_raises_rpc_error(-4, 'name expired',
            node_disallow.name_show, "d/expired")

        self.log.info("Check positive JSON overrides.")
        # checkName only accepts one parameter, use checkNameData
        self.checkNameData(
            node.name_show("d/expired", {"allow_expired": True}),
            "d/expired", "value", -1, True)
        self.checkNameData(
            node_disallow.name_show("d/expired", {"allow_expired": True}),
            "d/expired", "value", -1, True)

        self.log.info("Check negative JSON overrides.")
        assert_raises_rpc_error(-4, 'name expired',
            node.name_show, "d/expired", {"allow_expired": False})
        assert_raises_rpc_error(-4, 'name expired',
            node_disallow.name_show, "d/expired", {"allow_expired": False})

if __name__ == '__main__':
    NameExpirationTest (__file__).main ()
