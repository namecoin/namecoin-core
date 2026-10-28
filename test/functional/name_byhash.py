#!/usr/bin/env python3
# Copyright (c) 2019-2021 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for lookups of names by hash rather than preimage.

from test_framework.names import NameTestFramework
from test_framework.util import *

import hashlib


class NameByHashTest (NameTestFramework):

  def set_test_params (self):
    # We start without -namehashindex initially so that we can test the
    # "not enabled" error first.
    self.setup_name_test ([["-namehistory"]] * 1)
    self.setup_clean_chain = True

  def run_test (self):
    node = self.nodes[0]
    self.generate (node, 200)

    name = "testname"
    nameHex = name.encode ("ascii").hex ()
    singleHash = hashlib.new ("sha256", name.encode ("ascii")).digest ()
    doubleHashHex = hashlib.new ("sha256", singleHash).hexdigest ()
    by_hashOptions = {"name_encoding": "hex", "by_hash": "sha256d"}

    # Start by setting up a test name.
    new = node.name_new (name)
    self.generate (node, 10)
    self.firstupdateName (0, name, new, "value")
    self.generate (node, 5)

    # Check looking up "direct".
    res = node.name_show (name, {"by_hash": "direct"})
    assert_equal (res["name"], name)
    assert_equal (res["value"], "value")

    # -namehashindex is not enabled yet.
    assert_raises_rpc_error (-1, 'namehashindex is not enabled',
                             node.name_show, doubleHashHex, by_hashOptions)
    assert_equal (node.getindexinfo ("namehash"), {})

    # Restart the node and enable indexing.
    self.restart_node (0, extra_args=["-namehashindex", "-namehistory"])
    self.wait_until (
        lambda: all (i["synced"] for i in node.getindexinfo ().values ()))
    assert_equal (node.getindexinfo ("namehash"), {
      "namehash": {
        "synced": True,
        "best_block_height": node.getblockcount (),
      }
    })

    # Now the lookup by hash should work.
    res = node.name_show (doubleHashHex, by_hashOptions)
    assert_equal (res["name"], nameHex)
    assert_equal (res["value"], "value")
    res = node.name_history (doubleHashHex, by_hashOptions)
    assert_equal (len (res), 1)
    assert_equal (res[0]["name"], nameHex)

    # Unknown name by hash.
    assert_raises_rpc_error (-4, "name hash not found",
                             node.name_show, "42" * 32, by_hashOptions)

    # General errors with the parameters.
    assert_raises_rpc_error (-8, "Invalid value for by_hash",
                             node.name_show, doubleHashHex, {"by_hash": "foo"})
    assert_raises_rpc_error (-8, "must be 32 bytes long",
                             node.name_show, "abcd", {"by_hash": "sha256d"})


if __name__ == '__main__':
  NameByHashTest (__file__).main ()
