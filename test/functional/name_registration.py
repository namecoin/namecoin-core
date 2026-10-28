#!/usr/bin/env python3
# Copyright (c) 2014-2022 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for basic name registration and access (name_show, name_history).

from test_framework.names import NameTestFramework
from test_framework.util import *


class NameRegistrationTest (NameTestFramework):

  def set_test_params (self):
    self.setup_clean_chain = True
    self.setup_name_test ([["-namehistory", "-allowexpired"], []])

  def generateToOther (self, n):
    """
    Generates n blocks on the first node, but to an address of the second
    node (so that the first node does not get any extra coins that could mess
    up our balance tests).
    """

    addr = self.nodes[1].getnewaddress ()
    self.generatetoaddress (self.nodes[0], n, addr)

  def run_test (self):
    node = self.nodes[0]
    self.generate (node, 50)
    self.generateToOther (150)

    # Perform name_new's.  Check for too long names exception.
    newA = node.name_new ("name-0")
    newAconfl = node.name_new ("name-0")
    addr = node.getnewaddress ()
    newB = node.name_new ("name-1", {"dest_address": addr})
    node.name_new ("x" * 255)
    assert_raises_rpc_error (-8, 'name is too long', node.name_new, "x" * 256)
    self.generateToOther (5)

    # Verify that the name_new with explicit address sent really to this
    # address.  Since there is no equivalent to name_show while we only
    # have a name_new, explicitly check the raw tx.
    txHex = node.gettransaction (newB[0])['hex']
    newTx = node.decoderawtransaction (txHex)
    found = False
    for out in newTx['vout']:
      if 'nameOp' in out['scriptPubKey']:
        assert not found
        found = True
        assert_equal (out['scriptPubKey']['address'], addr)
    assert found

    # Check for exception with name_history and without -namehistory.
    self.sync_blocks ()
    assert_raises_rpc_error (-1, 'namehistory is not enabled',
                             self.nodes[1].name_history, "name-0")

    # first_update the names.  Check for too long values.
    addr = node.getnewaddress ()
    txidA = self.firstupdateName (0, "name-0", newA, "value-0",
                                  {"dest_address": addr})
    assert_raises_rpc_error (-8, 'value is too long',
                             self.firstupdateName, 0, "name-1", newB, "x" * 521)
    self.firstupdateName (0, "name-1", newB, "x" * 520)

    # Check for mempool conflict detection with registration of "name-0".
    assert_raises_rpc_error (-25, 'is already being registered',
                             self.firstupdateName,
                             0, "name-0", newAconfl, "foo")
    
    # Check that the name appears when the name_new is ripe.
    self.generateToOther (7)
    assert_raises_rpc_error (-4, 'name never existed',
                             node.name_show, "name-0")
    assert_raises_rpc_error (-4, 'name not found',
                             node.name_history, "name-0")
    self.generateToOther (1)

    data = self.checkName (0, "name-0", "value-0", 30, False)
    assert_equal (data['address'], addr)
    assert_equal (data['txid'], txidA)
    assert_equal (data['height'], 213)

    self.checkNameHistory (0, "name-0", ["value-0"])
    self.checkNameHistory (0, "name-1", ["x" * 520])

    # Verify the allow_existing option for name_new.
    assert_raises_rpc_error (-25, 'exists already',
                             node.name_new, "name-0")
    assert_raises_rpc_error (-25, 'exists already',
                             node.name_new, "name-0",
                             {"allow_existing": False})
    assert_raises_rpc_error (-3, 'is not of expected type bool',
                             node.name_new, "other",
                             {"allow_existing": 42.5})
    node.name_new ("name-0", {"allow_existing": True})

    # Check for error with rand mismatch (wrong name)
    newA = node.name_new ("test-name")
    self.generateToOther (10)
    assert_raises_rpc_error (-25, 'rand value is wrong',
                             self.firstupdateName,
                             0, "test-name-wrong", newA, "value")

    # Check for mismatch with prev tx from another node for name_firstupdate
    # and name_update.
    self.sync_blocks ()
    assert_raises_rpc_error (-6, 'Not found pre-selected input',
                             self.firstupdateName,
                             1, "test-name", newA, "value")
    self.firstupdateName (0, "test-name", newA, "test-value")

    # Check for disallowed firstupdate when the name is active.
    newSteal = node.name_new ("name-0", {"allow_existing": True})
    newSteal2 = node.name_new ("name-0", {"allow_existing": True})
    self.generateToOther (19)
    self.checkName (0, "name-0", "value-0", 1, False)
    assert_raises_rpc_error (-25, 'this name is already active',
                             self.firstupdateName,
                             0, "name-0", newSteal, "stolen")

    # Check for "stealing" of the name after expiry.
    self.generateToOther (1)
    assert newSteal[1] != newSteal2[1]
    self.firstupdateName (0, "name-0", newSteal, "stolen")
    self.checkName (0, "name-0", "value-0", 0, True)
    self.generateToOther (1)
    self.checkName (0, "name-0", "stolen", 30, False)
    self.checkNameHistory (0, "name-0", ["value-0", "stolen"])

    # Check for firstupdating an active name, but this time without the check
    # present in the RPC call itself.  This should still be prevented by the
    # mempool logic.  There was a bug that allowed these transactiosn to get
    # into the mempool, so make sure it is no longer there.
    self.firstupdateName (0, "name-0", newSteal2, "unstolen", allowActive=True)
    assert_equal (node.getrawmempool (), [])
    self.checkName (0, "name-0", "stolen", None, False)

    # Check basic updating.
    assert_raises_rpc_error (-8, 'value is too long',
                             node.name_update, "test-name", "x" * 521)
    node.name_update ("test-name", "x" * 520)
    self.checkName (0, "test-name", "test-value", None, False)
    self.generateToOther (1)
    self.checkName (0, "test-name", "x" * 520, 30, False)
    self.checkNameHistory (0, "test-name", ["test-value", "x" * 520])

    addrOther = self.nodes[1].getnewaddress ()
    node.name_update ("test-name", "sent", {"dest_address": addrOther})
    self.generateToOther (1)
    self.sync_blocks ()
    data = self.checkName (0, "test-name", "sent", 30, False)
    assert_equal (data['address'], addrOther)
    self.nodes[1].name_update ("test-name", "updated")
    self.generate (self.nodes[1], 1)
    self.sync_blocks ()
    data = self.checkName (1, "test-name", "updated", 30, False)
    self.checkNameHistory (0, "test-name",
                           ["test-value", "x" * 520, "sent", "updated"])

    # Invalid updates.
    assert_raises_rpc_error (-25, 'this name can not be updated',
                             node.name_update, "wrong-name", "foo")
    assert_raises_rpc_error (-6, 'Not found pre-selected input',
                             node.name_update, "test-name", "stolen?")

    # Update failing after expiry.  Re-registration possible.
    self.checkName (0, "name-1", "x" * 520, None, True)
    assert_raises_rpc_error (-25, 'this name can not be updated',
                             node.name_update, "name-1", "updated?")

    newSteal = node.name_new ("name-1")
    self.generateToOther (10)
    self.firstupdateName (0, "name-1", newSteal, "reregistered")
    self.generateToOther (10)
    self.checkName (0, "name-1", "reregistered", 23, False)
    self.checkNameHistory (0, "name-1", ["x" * 520, "reregistered"])

    # Test that name updates are even possible with less balance in the wallet
    # than what is locked in a name (0.01 NMC).  There was a bug preventing
    # this from working.
    balance = node.getbalance ()
    keep = Decimal ("0.001")
    addrOther = self.nodes[1].getnewaddress ()
    node.sendtoaddress (addrOther, balance - keep, "", "", True)
    self.generateToOther (1)
    assert_equal (node.getbalance (), keep)
    node.name_update ("name-1", "new value")
    self.generateToOther (1)
    assert node.getbalance () < Decimal ("0.01")
    self.checkName (0, "name-1", "new value", None, False)


if __name__ == '__main__':
  NameRegistrationTest (__file__).main ()
