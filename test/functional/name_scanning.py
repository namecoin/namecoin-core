#!/usr/bin/env python3
# Copyright (c) 2014-2021 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for the name_scan RPC method.

from test_framework.names import NameTestFramework
from test_framework.util import *

class NameScanningTest (NameTestFramework):

  def set_test_params (self):
    self.setup_name_test ([[]] * 1)

  def run_test (self):
    self.node = self.nodes[0]

    # Mine a block so that we're no longer in initial download.
    self.generate (self.node, 1)

    # Initially, all should be empty.
    assert_equal (self.node.name_scan (), [])
    assert_equal (self.node.name_scan ("foo", 10), [])

    # Register some names with various data, heights and expiration status.
    # Using both "aa" and "b" ensures that we can also check for the expected
    # comparison order between string length and lexicographic ordering.
    newA = self.node.name_new ("d/a")
    newAA = self.node.name_new ("d/aa")
    newB = self.node.name_new ("d/b")
    newC = self.node.name_new ("d/c")
    self.generate (self.node, 15)

    self.firstupdateName (0, "d/a", newA, "wrong value")
    self.firstupdateName (0, "d/aa", newAA, "value aa")
    self.firstupdateName (0, "d/b", newB, "value b")
    self.generate (self.node, 15)
    self.firstupdateName (0, "d/c", newC, "value c")
    self.node.name_update ("d/a", "value a")
    self.generate (self.node, 20)

    # Check the expected name_scan data values.
    scan = self.node.name_scan ()
    assert_equal (len (scan), 4)
    self.checkNameData (scan[0], "d/a", "value a", 11, False)
    self.checkNameData (scan[1], "d/b", "value b", -4, True)
    self.checkNameData (scan[2], "d/c", "value c", 11, False)
    self.checkNameData (scan[3], "d/aa", "value aa", -4, True)

    # Check for expected names in various basic name_scan calls.
    self.checkList (self.node.name_scan (), ["d/a", "d/b", "d/c", "d/aa"])
    self.checkList (self.node.name_scan ("", 0), [])
    self.checkList (self.node.name_scan ("", -1), [])
    self.checkList (self.node.name_scan ("d/b"), ["d/b", "d/c", "d/aa"])
    self.checkList (self.node.name_scan ("d/zz"), [])
    self.checkList (self.node.name_scan ("", 2), ["d/a", "d/b"])
    self.checkList (self.node.name_scan ("d/b", 1), ["d/b"])

    # Verify encoding for start argument.
    self.checkList (self.node.name_scan ("642f63", 10, {"name_encoding": "hex"}),
                    ["642f63", "642f6161"])

    # Verify filtering based on number of confirmations.
    self.checkList (self.node.name_scan ("", 100, {"min_conf": 35}),
                    ["d/b", "d/aa"])
    self.checkList (self.node.name_scan ("", 100, {"min_conf": 36}), [])
    self.checkList (self.node.name_scan ("", 100, {"max_conf": 19}), [])
    self.checkList (self.node.name_scan ("", 100, {"max_conf": 20}),
                    ["d/a", "d/c"])

    # Verify interaction with filtering and count.
    self.checkList (self.node.name_scan ("", 1, {"max_conf": 20}), ["d/a"])
    self.checkList (self.node.name_scan ("", 2, {"max_conf": 20}),
                    ["d/a", "d/c"])

    # Error checks for confirmation options.
    assert_raises_rpc_error (-8, "min_conf must be >= 1",
                             self.node.name_scan, "", 100, {"min_conf": 0})
    assert_raises_rpc_error (-8, "min_conf must be >= 1",
                             self.node.name_scan, "", 100, {"min_conf": -42})
    self.node.name_scan ("", 100, {"min_conf": 1})
    assert_raises_rpc_error (-8, "max_conf must not be negative",
                             self.node.name_scan, "", 100, {"max_conf": -1})
    self.node.name_scan ("", 100, {"max_conf": 0})

    # Verify filtering based on prefix.
    self.checkList (self.node.name_scan ("", 100, {"prefix": ""}),
                    ["d/a", "d/b", "d/c", "d/aa"])
    self.checkList (self.node.name_scan ("", 100, {"prefix": "d/a"}),
                    ["d/a", "d/aa"])

    # Check prefix and name_encoding.
    options = {
        "prefix": "642f61",
        "name_encoding": "hex",
    }
    self.checkList (self.node.name_scan ("", 100, options),
                    ["642f61", "642f6161"])
    assert_raises_rpc_error (-1000, "Name/value is invalid for encoding ascii",
                             self.node.name_scan, "", 100, {"prefix": "äöü"})

    # Verify filtering based on regexp.
    self.checkList (self.node.name_scan ("", 100, {"regexp": "[ac]"}),
                    ["d/a", "d/c", "d/aa"])

    # Multiple filters are combined using "and".
    options = {
        "prefix": "d/a",
        "max_conf": 20,
    }
    self.checkList (self.node.name_scan ("", 100, options), ["d/a"])

    # Include a name with invalid UTF-8 to make sure it doesn't break
    # the regexp check.
    self.restart_node (0, extra_args=["-nameencoding=hex"])
    hexName = "642f00ff"
    new = self.node.name_new (hexName)
    self.generate (self.node, 10)
    self.firstupdateName (0, hexName, new, "{}")
    self.generate (self.node, 5)
    fullHexList = ['642f61', '642f62', '642f63', hexName, '642f6161']
    self.checkList (self.node.name_scan (), fullHexList)
    self.checkList (self.node.name_scan ("", 100, {"regexp": "a"}),
                    ['642f61', '642f6161'])

  def checkList (self, data, names):
    """
    Check that the result in 'data' contains the names
    given in the array 'names'.
    """

    def walker (e):
      return e['name']
    dataNames = list (map (walker, data))

    assert_equal (dataNames, names)


if __name__ == '__main__':
  NameScanningTest (__file__).main ()
