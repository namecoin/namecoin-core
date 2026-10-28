#!/usr/bin/env python3
# Copyright (c) 2014-2021 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for name_list.

from test_framework.names import NameTestFramework
from test_framework.util import *

class NameListTest (NameTestFramework):

  def set_test_params (self):
    self.setup_name_test ([[]] * 2)

  def run_test (self):
    assert_equal (self.nodes[0].name_list (), [])
    assert_equal (self.nodes[1].name_list (), [])

    newA = self.nodes[0].name_new ("name")
    self.generate (self.nodes[0], 10)
    self.firstupdateName (0, "name", newA, "value")
    self.generate (self.nodes[0], 5)

    arr = self.nodes[0].name_list ()
    assert_equal (len (arr), 1)
    self.checkNameStatus (arr[0], "name", "value", False, True)

    self.sync_blocks ()
    assert_equal (self.nodes[1].name_list ("name"), [])

    # Transfer the name away and check that name_list updates accordingly.
    addrB = self.nodes[1].getnewaddress ()
    self.nodes[0].name_update ("name", "enjoy", {"dest_address":addrB})
    arr = self.nodes[0].name_list ()
    assert_equal (len (arr), 1)
    self.checkNameStatus (arr[0], "name", "value", False, True)

    self.generate (self.nodes[0], 1)
    arr = self.nodes[0].name_list ()
    assert_equal (len (arr), 1)
    self.checkNameStatus (arr[0], "name", "enjoy", False, False)

    self.sync_blocks ()
    arr = self.nodes[1].name_list ()
    assert_equal (len (arr), 1)
    self.checkNameStatus (arr[0], "name", "enjoy", False, True)

    # Updating the name in the new wallet shouldn't change the
    # old wallet's name_list entry.
    self.nodes[1].name_update ("name", "new value")
    self.generate (self.nodes[1], 1)
    arr = self.nodes[1].name_list ("name")
    assert_equal (len (arr), 1)
    self.checkNameStatus (arr[0], "name", "new value", False, True)

    self.sync_blocks ()
    arr = self.nodes[0].name_list ()
    assert_equal (len (arr), 1)
    self.checkNameStatus (arr[0], "name", "enjoy", False, False)

    # Transfer it back and see that it updates in wallet A.
    addrA = self.nodes[0].getnewaddress ()
    self.nodes[1].name_update ("name", "sent", {"dest_address": addrA})
    self.generate (self.nodes[1], 1)

    self.sync_blocks ()
    arr = self.nodes[0].name_list ()
    assert_equal (len (arr), 1)
    self.checkNameStatus (arr[0], "name", "sent", False, True)

    # Let the name expire.
    self.generate (self.nodes[0], 40)
    arr = self.nodes[0].name_list ()
    assert_equal (len (arr), 1)
    self.checkNameStatus (arr[0], "name", "sent", True, True)

  def checkNameStatus (self, data, name, value, expired, mine):
    """
    Check a name_list entry for the expected data.
    """

    self.checkNameData (data, name, value, None, expired)
    assert_equal (data['ismine'], mine)

if __name__ == '__main__':
  NameListTest (__file__).main ()
