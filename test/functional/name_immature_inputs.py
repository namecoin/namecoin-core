#!/usr/bin/env python3
# Copyright (c) 2018-2021 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Test for handling of immature inputs for name operations.

from test_framework.names import NameTestFramework
from test_framework.util import *

class NameImmatureInputsTest (NameTestFramework):

  def set_test_params (self):
    # We need two nodes so that getblocktemplate doesn't complain about
    # 'not being connected'.  But only node 0 is actually used throughout
    # the test.
    self.setup_name_test ([["-debug=names"]] * 2)

  def dependsOn (self, ind, child, parent):
    """
    Checks whether the child transaction (given by txid) depends on an output
    from the parent txid.
    """

    child = self.nodes[ind].getrawtransaction (child, 1)
    for vin in child['vin']:
      if vin['txid'] == parent:
        return True

    return False

  def run_test (self):

    # The first part of this test registers a name using the standard RPC
    # interface.  This should work as soon as the name_new has at least one
    # confirmation (but not when it is unconfirmed).  The name should appear
    # as soon as the name_new has matured.
    new = self.nodes[0].name_new ("a")
    assert_raises_rpc_error (-25, '', self.firstupdateName,
                             0, "a", new, "value")
    self.generate (self.nodes[0], 1)
    first = self.firstupdateName (0, "a", new, "value")
    self.generate (self.nodes[0], 11)
    assert_raises_rpc_error (-4, 'name never existed',
                             self.nodes[0].name_show, "a")
    self.generate (self.nodes[0], 1)
    self.checkName (0, "a", "value", 30, False)

    # Next, we want to make sure that things still work fine even if we relay
    # the name_firstupdate transaction while the name_new is still unconfirmed.
    # We can do that by constructing it using the raw tx API.
    #
    # That previously failed:
    #   https://github.com/namecoin/namecoin-core/issues/50

    addr = self.nodes[0].getnewaddress ()
    new = self.nodes[0].name_new ("b", {"dest_address": addr})
    newTx = self.nodes[0].getrawtransaction (new[0])
    nameInd = self.rawtxOutputIndex (0, newTx, addr)

    nameAmount = Decimal ('0.01')
    ins = [{"txid": new[0], "vout": nameInd}]
    txRaw = self.nodes[0].createrawtransaction (ins, {addr: nameAmount})
    op = {"op": "name_firstupdate", "name": "b", "value": "value",
          "rand": new[1]}
    txRaw = self.nodes[0].namerawtransaction (txRaw, 0, op)['hex']
    txRaw = self.nodes[0].fundrawtransaction (txRaw)['hex']
    signed = self.nodes[0].signrawtransactionwithwallet (txRaw)
    assert signed['complete']
    first = self.nodes[0].sendrawtransaction (signed['hex'])

    assert_equal (set ([new[0], first]), set (self.nodes[0].getrawmempool ()))
    self.nodes[0].getblocktemplate ({'rules': ['segwit']})
    self.generate (self.nodes[0], 1)
    assert_equal ([first], self.nodes[0].getrawmempool ())
    self.generate (self.nodes[0], 11)
    assert_raises_rpc_error (-4, 'name never existed',
                             self.nodes[0].name_show, "b")
    self.generate (self.nodes[0], 1)
    self.checkName (0, "b", "value", 30, False)

    # It should be possible to use unconfirmed *currency* outputs in a name
    # firstupdate, though (so that multiple name registrations are possible
    # even if one has only a single currency output in the wallet).

    newC = self.nodes[0].name_new ("c")
    newD = self.nodes[0].name_new ("d")
    self.generate (self.nodes[0], 12)

    balance = self.nodes[0].getbalance ()
    self.nodes[0].sendtoaddress (addr, balance, None, None, True)
    assert_equal (1, len (self.nodes[0].listunspent (0)))
    firstC = self.firstupdateName (0, "c", newC, "value")
    firstD = self.firstupdateName (0, "d", newD, "value")
    assert self.dependsOn (0, firstD, firstC)
    self.generate (self.nodes[0], 1)
    self.checkName (0, "c", "value", 30, False)
    self.checkName (0, "d", "value", 30, False)

    assert_equal (1, len (self.nodes[0].listunspent ()))
    updC = self.nodes[0].name_update ("c", "new value")
    updD = self.nodes[0].name_update ("d", "new value")
    assert self.dependsOn (0, updD, updC)
    self.generate (self.nodes[0], 1)
    self.checkName (0, "c", "new value", 30, False)
    self.checkName (0, "d", "new value", 30, False)

if __name__ == '__main__':
  NameImmatureInputsTest (__file__).main ()
