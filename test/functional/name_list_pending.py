#!/usr/bin/env python3
# Copyright (c) 2026 The Namecoin developers
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for the {"includePending": true} option of name_list.
#
# name_list normally returns only confirmed wallet names.  With
# includePending:true it also returns the wallet's unconfirmed name
# operations from the mempool, marked with {"pending": true} and an
# "op" field.  A pending entry replaces the confirmed entry for the
# same name.

from test_framework.names import NameTestFramework
from test_framework.util import assert_equal


class NameListPendingTest (NameTestFramework):

  def set_test_params (self):
    self.setup_name_test ([[]] * 1)

  def run_test (self):
    node = self.nodes[0].get_wallet_rpc (self.default_wallet_name)

    # Register two names "a" and "b".
    newA = node.name_new ("a")
    newB = node.name_new ("b")
    self.generate (self.nodes[0], 12)
    self.firstupdateName (0, "a", newA, "value-a-1")
    self.firstupdateName (0, "b", newB, "value-b-1")
    self.generate (self.nodes[0], 1)

    # ----- Confirmed-only baseline -----
    confirmed = sorted (node.name_list (), key=lambda e: e['name'])
    assert_equal ([e['name'] for e in confirmed], ['a', 'b'])
    assert_equal ([e['value'] for e in confirmed], ['value-a-1', 'value-b-1'])
    for e in confirmed:
      assert 'pending' not in e
      assert 'op' not in e

    # includePending without any pending tx is identical to default.
    assert_equal (
        sorted (node.name_list (None, {"includePending": True}),
                key=lambda e: e['name']),
        confirmed)

    # ----- Pending name_update -----
    txUpd = node.name_update ("a", "value-a-2")
    assert txUpd in self.nodes[0].getrawmempool ()

    # Default name_list still shows the confirmed value for "a".
    default_after = sorted (node.name_list (), key=lambda e: e['name'])
    assert_equal ([e['value'] for e in default_after],
                  ['value-a-1', 'value-b-1'])
    for e in default_after:
      assert 'pending' not in e

    # includePending shows the pending value for "a", confirmed for "b".
    pending_view = sorted (
        node.name_list (None, {"includePending": True}),
        key=lambda e: e['name'])
    assert_equal ([e['name'] for e in pending_view], ['a', 'b'])

    a_entry = pending_view[0]
    assert_equal (a_entry['name'], 'a')
    assert_equal (a_entry['value'], 'value-a-2')
    assert_equal (a_entry['pending'], True)
    assert_equal (a_entry['op'], 'name_update')
    assert_equal (a_entry['txid'], txUpd)
    assert_equal (a_entry['ismine'], True)

    b_entry = pending_view[1]
    assert_equal (b_entry['name'], 'b')
    assert_equal (b_entry['value'], 'value-b-1')
    assert 'pending' not in b_entry, b_entry

    # The name filter composes with includePending.
    only_a = node.name_list ('a', {"includePending": True})
    assert_equal (len (only_a), 1)
    assert_equal (only_a[0]['name'], 'a')
    assert_equal (only_a[0]['pending'], True)

    only_b = node.name_list ('b', {"includePending": True})
    assert_equal (len (only_b), 1)
    assert 'pending' not in only_b[0]

    # ----- After confirmation, pending becomes confirmed -----
    self.generate (self.nodes[0], 1)
    confirmed_after = sorted (
        node.name_list (None, {"includePending": True}),
        key=lambda e: e['name'])
    assert_equal ([e['value'] for e in confirmed_after],
                  ['value-a-2', 'value-b-1'])
    for e in confirmed_after:
      assert 'pending' not in e
      assert 'op' not in e


if __name__ == '__main__':
  NameListPendingTest (__file__).main ()
