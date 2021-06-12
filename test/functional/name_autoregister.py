#!/usr/bin/env python3
# Licensed under CC0 (Public domain)

# Test that name_autoregister works

from test_framework.names import NameTestFramework
from test_framework.util import *

class NameAutoregisterTest(NameTestFramework):

    def set_test_params(self):
        self.setup_name_test ([[]] * 1)
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0] # alias
        node.generate(200) # get out of IBD

        self.log.info("Register a name.")
        node.name_autoregister("d/name", "value")
        node.generate(1)
        assert(len(node.listqueuedtransactions()) == 1)
        self.log.info("Queue contains 1 transaction.")
        self.log.info("Wait 12 blocks.")
        node.generate(12)
        assert(len(node.listqueuedtransactions()) == 0)
        self.log.info("Queue is empty.")
        self.log.info("Wait 1 more block so transaction can get mined.")
        node.generate(1)
        self.checkNameData(node.name_show("d/name"), "d/name", "value", 30, False)
        self.log.info("Name is registered.")

        self.log.info("Check delegation: normal case.")
        node.name_autoregister("d/delegated", "value", {"delegate": True})
        assert(len(node.listqueuedtransactions()) == 2)
        node.generate(14)
        assert(len(node.listqueuedtransactions()) == 0)
        self.checkNameData(node.name_show("d/delegated"), "d/delegated", '{"import":"dd/delegated"}', 30, False)
        self.checkNameData(node.name_show("dd/delegated"), "dd/delegated", 'value', 30, False)

        self.log.info("Check delegation: appending digits.")
        node.name_autoregister("dd/delegated2", "value")
        node.generate(14)
        node.name_autoregister("d/delegated2", "value", {"delegate": True})
        node.generate(14)
        self.log.info("Value is: " + node.name_show("d/delegated2")['value'])
        assert(node.name_show("d/delegated2")['value'] != '{"import":"dd/delegated2"}')

if __name__ == '__main__':
    NameAutoregisterTest ().main ()
