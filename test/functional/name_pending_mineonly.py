#!/usr/bin/env python3
# Copyright (c) 2026 The Namecoin developers
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for the {"mineOnly": true} option of name_pending.
#
# Two nodes share a mempool.  Node 0 first registers a name and then
# transfers it to an address owned by node 1 via name_update.  While the
# update sits in the mempool, name_pending must respect mineOnly:true on
# both sides.
#
# Wallet-scoped RPC endpoints are used so that the wallet context is always
# attached to the request.

from test_framework.names import NameTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class NamePendingMineOnlyTest (NameTestFramework):

  def set_test_params (self):
    self.setup_name_test ([[]] * 2)

  def run_test (self):
    node0 = self.nodes[0].get_wallet_rpc (self.default_wallet_name)
    node1 = self.nodes[1].get_wallet_rpc (self.default_wallet_name)

    # ----- Setup: register name "a" on node 0 -----
    newA = node0.name_new ("a")
    self.generate (self.nodes[0], 12)
    self.firstupdateName (0, "a", newA, "value-a")
    self.generate (self.nodes[0], 1)
    self.sync_blocks ()

    # ----- Pending update owned by node 0 -----
    txMine0 = node0.name_update ("a", "value-a-2")
    self.sync_mempools ()

    # Default behavior unchanged: both nodes see the pending op.
    assert_equal (len (node0.name_pending ()), 1)
    assert_equal (len (node1.name_pending ()), 1)

    # mineOnly:false explicitly equals the default.
    assert_equal (
        [e['txid'] for e in node0.name_pending (None, {"mineOnly": False})],
        [e['txid'] for e in node0.name_pending ()])

    # mineOnly:true returns only entries owned by the calling wallet.
    mine0 = node0.name_pending (None, {"mineOnly": True})
    assert_equal (len (mine0), 1)
    assert_equal (mine0[0]['name'], 'a')
    assert_equal (mine0[0]['txid'], txMine0)
    assert_equal (mine0[0]['ismine'], True)
    assert_equal (node1.name_pending (None, {"mineOnly": True}), [])

    # mineOnly composes with the name filter.
    assert_equal (
        node0.name_pending ('a', {"mineOnly": True})[0]['txid'], txMine0)
    assert_equal (node0.name_pending ('does not exist',
                                      {"mineOnly": True}), [])
    assert_equal (node1.name_pending ('a', {"mineOnly": True}), [])

    # Mine, sync, and clear the mempool before the next scenario.
    self.generate (self.nodes[0], 1)
    self.sync_blocks ()
    assert_equal (node0.name_pending (), [])
    assert_equal (node1.name_pending (), [])

    # ----- Pending transfer: ownership flips between wallets -----
    addrOther = node1.getnewaddress ()
    txTransfer = node0.name_update ("a", "sent-a", {"destAddress": addrOther})
    self.sync_mempools ()

    # The pending output's destination is owned by node 1, so:
    #   * node 0 -> mineOnly:true -> []
    #   * node 1 -> mineOnly:true -> 1 entry, ismine=true
    assert_equal (node0.name_pending (None, {"mineOnly": True}), [])
    pending1 = node1.name_pending (None, {"mineOnly": True})
    assert_equal (len (pending1), 1)
    assert_equal (pending1[0]['txid'], txTransfer)
    assert_equal (pending1[0]['name'], 'a')
    assert_equal (pending1[0]['ismine'], True)

    # ----- Error: mineOnly when no wallet is loaded -----
    # Unload the default wallet and confirm mineOnly errors out cleanly
    # rather than silently returning everything.
    self.nodes[0].unloadwallet (self.default_wallet_name)
    assert_raises_rpc_error (
        -18, "mineOnly requires a wallet to be loaded",
        self.nodes[0].name_pending, None, {"mineOnly": True})
    # Default behavior (no mineOnly) still works without a wallet.
    assert isinstance (self.nodes[0].name_pending (), list)
    self.nodes[0].loadwallet (self.default_wallet_name)


if __name__ == '__main__':
  NamePendingMineOnlyTest (__file__).main ()
