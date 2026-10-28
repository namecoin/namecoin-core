#!/usr/bin/env python3
# Copyright (c) 2018-2022 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for the "ismine" field in the various name RPCs.

from test_framework.names import NameTestFramework
from test_framework.util import *

class NameIsMineTest (NameTestFramework):

  def set_test_params (self):
    self.setup_clean_chain = True
    self.setup_name_test ([[], ['-namehistory']])

  def verifyExpectedIsMineInList (self, arr):
    """
    Goes through the array of name info, as returned by name_scan or name_list.
    It verifies that exactly the two expected names are there, and the
    ismine field matches the expectation for them.
    """

    assert_equal (len (arr), 2)

    for n in arr:
      if n['name'] == "d/a":
        assert_equal (n['ismine'], True)
      elif n['name'] == "d/b":
        assert_equal (n['ismine'], False)
      else:
        raise AssertionError ("Unexpected name in array: %s" % n['name'])

  def run_test (self):
    self.node = self.nodes[1]
    self.generate (self.node, 110)

    # Register two names.  One of them is then sent to an address not owned
    # by the node and one is updated into a pending operation.  That then
    # gives us the basic setup to test the "ismine" field in all the
    # circumstances.
    newA = self.node.name_new ("d/a")
    newB = self.node.name_new ("d/b")
    self.generate (self.node, 10)
    self.firstupdateName (1, "d/a", newA, "value")
    self.firstupdateName (1, "d/b", newB, "value")
    self.generate (self.node, 5)
    otherAddr = self.nodes[0].getnewaddress ()
    self.node.name_update ("d/b", "new value", {"dest_address": otherAddr})
    self.generate (self.node, 1)
    self.node.name_update ("d/a", "new value")

    # name_show
    assert_equal (self.node.name_show ("d/a")['ismine'], True)
    assert_equal (self.node.name_show ("d/b")['ismine'], False)

    # name_history
    hist = self.node.name_history ("d/a")
    assert_equal (len (hist), 1)
    assert_equal (hist[0]['ismine'], True)
    hist = self.node.name_history ("d/b")
    assert_equal (len (hist), 2)
    assert_equal (hist[0]['ismine'], True)
    assert_equal (hist[1]['ismine'], False)

    # name_pending
    pending = self.node.name_pending ()
    assert_equal (len (pending), 1)
    p = pending[0]
    assert_equal (p['name'], "d/a")
    assert_equal (p['ismine'], True)

    # name_scan, name_list
    self.verifyExpectedIsMineInList (self.node.name_scan ())
    self.verifyExpectedIsMineInList (self.node.name_list ())

    # Test the multiwallet situation.  If no wallet is specified for the RPC,
    # then ismine should not be set at all.
    self.node.createwallet ("extra")
    defaultRpc = self.node.get_wallet_rpc ("default_wallet")
    extraRpc = self.node.get_wallet_rpc ("extra")
    self.verifyExpectedIsMineInList (defaultRpc.name_list ())
    for nm in ["d/a", "d/b"]:
      assert 'ismine' not in self.node.name_show (nm)
      assert_equal (extraRpc.name_show (nm)['ismine'], False)

if __name__ == '__main__':
  NameIsMineTest (__file__).main ()
