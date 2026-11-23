#!/usr/bin/env python3
# Copyright (c) 2014-2021 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC tests for name operations and the rawtx API.

from test_framework.names import NameTestFramework
from test_framework.util import *

from decimal import Decimal

class NameRawTxTest (NameTestFramework):

  def set_test_params (self):
    self.setup_name_test ([[]] * 2)

  def run_test (self):
    # Decode name_new.
    new = self.nodes[0].name_new ("my-name")
    self.generate (self.nodes[0], 10)
    data = self.decodeNameTx (0, new[0])
    assert_equal (data['op'], "name_new")
    assert 'hash' in data

    # Decode name_firstupdate.
    first = self.firstupdateName (0, "my-name", new, "initial value")
    self.generate (self.nodes[0], 5)
    data = self.decodeNameTx (0, first)
    assert_equal (data['op'], "name_firstupdate")
    assert_equal (data['name'], "my-name")
    assert_equal (data['value'], "initial value")
    assert_equal (data['rand'], new[1])

    # Decode name_update.
    upd = self.nodes[0].name_update ("my-name", "new value")
    self.generate (self.nodes[0], 1)
    data = self.decodeNameTx (0, upd)
    assert_equal (data['op'], "name_update")
    assert_equal (data['name'], "my-name")
    assert_equal (data['value'], "new value")

    # Go through the full name "life cycle" (name_new, name_firstupdate and
    # name_update) with raw transactions.
    newOp = {"op": "name_new", "name": "raw-test-name"}
    newAddr = self.nodes[0].getnewaddress ()
    newOutp, newData = self.rawNameOp (0, None, newAddr, newOp)
    self.generate (self.nodes[0], 10)

    firstOp = {"op": "name_firstupdate", "rand": newData["rand"],
               "name": "raw-test-name", "value": "first value"}
    firstAddr = self.nodes[0].getnewaddress ()
    firstOutp, _ = self.rawNameOp (0, newOutp, firstAddr, firstOp)
    self.generate (self.nodes[0], 5)
    self.checkName (0, "raw-test-name", "first value", None, False)

    updOp = {"op": "name_update", "name": "raw-test-name", "value": "new value"}
    updAddr = self.nodes[0].getnewaddress ()
    self.rawNameOp (0, firstOutp, updAddr, updOp)
    self.generate (self.nodes[0], 1)
    self.checkName (0, "raw-test-name", "new value", None, False)

    # Verify range check of vout in namerawtransaction.
    tx = self.nodes[0].createrawtransaction ([], {})
    assert_raises_rpc_error (-8, "vout is out of range",
                             self.nodes[0].namerawtransaction, tx, 0, {})
    assert_raises_rpc_error (-8, "vout is out of range",
                             self.nodes[0].namerawtransaction, tx, -1, {})

    # Perform a rawtx name update together with an atomic currency transaction.
    # We send the test name from 0 to 1 and some coins from 1 to 0.  In other
    # words, perform an atomic name trade.
    self.sync_blocks ()
    balanceA = self.nodes[0].getbalance ()
    balanceB = self.nodes[1].getbalance ()
    price = Decimal ("1.0")
    fee = Decimal ("0.01")

    self.atomicTrade ("my-name", "enjoy", price, fee, 0, 1)
    self.generate (self.nodes[0], 1)
    self.sync_blocks ()

    data = self.checkName (0, "my-name", "enjoy", None, False)
    info = self.nodes[1].getaddressinfo (data["address"])
    assert info['ismine']
    assert_equal (self.nodes[0].name_list ("my-name"), [])
    data = self.nodes[0].name_list ("my-name", {"showTransferred": True})
    assert_equal (len (data), 1)
    assert_equal (data[0]["name"], "my-name")
    assert_equal (data[0]["ismine"], False)
    data = self.nodes[1].name_list ("my-name")
    assert_equal (len (data), 1)
    assert_equal (data[0]["name"], "my-name")
    assert_equal (data[0]["ismine"], True)

    assert_equal (balanceA + price, self.nodes[0].getbalance ())
    # Node 1 gets a block matured, take this into account.
    assert_equal (balanceB - price - fee + Decimal ("50"),
                  self.nodes[1].getbalance ())

    # Try to construct and relay a transaction that updates two names at once.
    # This used to crash the client, #116.  It should lead to an error (as such
    # a transaction is invalid), but not a crash.
    newA = self.nodes[0].name_new ("a")
    newB = self.nodes[0].name_new ("b")
    self.generate (self.nodes[0], 10)
    self.firstupdateName (0, "a", newA, "value a")
    self.firstupdateName (0, "b", newB, "value b")
    self.generate (self.nodes[0], 5)

    inA, outA = self.constructUpdateTx (0, "a", "new value a")
    inB, outB = self.constructUpdateTx (0, "b", "new value b")

    tx = outA[:8]      # version
    tx += '02'         # number of txin
    tx += inA[10:-10]  # first txin
    tx += inB[10:-10]  # second txin
    tx += '02'         # number of txout
    tx += outA[12:-8]  # first txout
    tx += outB[12:-8]  # second txout
    tx += '00' * 4     # locktime

    signed = self.nodes[0].signrawtransactionwithwallet (tx)
    assert_raises_rpc_error (-26, None,
                             self.nodes[0].sendrawtransaction, signed['hex'])

  def decodeNameTx (self, ind, txid):
    """
    Retrieves the transaction data for the given txid (assuming it is in the
    node's wallet) and finds the output that is a name operation.  Returns
    the decoded nameop entry.
    """

    txHex = self.nodes[ind].gettransaction (txid)['hex']
    data = self.nodes[ind].decoderawtransaction (txHex)
    res = None
    for out in data['vout']:
      if 'nameOp' in out['scriptPubKey']:
        assert res is None
        res = out['scriptPubKey']['nameOp']

        # Extra check:  Verify that the address is decoded correctly.
        addr = out['scriptPubKey']['address']
        assert_equal (out['scriptPubKey']['type'], "pubkeyhash")
        validation = self.nodes[ind].validateaddress (addr)
        assert validation['isvalid']

    assert res is not None
    return res

  def rawNameOp (self, ind, nameIn, toAddr, op):
    """
    Utility method to construct and send a name-operation transaction with
    the raw-transactions interface.  It uses the provided input (if not None)
    for the name and finds other inputs to fund the tx.  It sends the name
    to toAddr with the operation defined by op.
    """

    vin = []
    if nameIn is not None:
      vin.append (nameIn)

    nameAmount = Decimal ("0.01")
    vout = {toAddr: nameAmount}

    tx = self.nodes[ind].createrawtransaction (vin, vout)
    tx = self.nodes[ind].fundrawtransaction (tx, {"feeRate": 0.01})

    nameInd = self.rawtxOutputIndex (ind, tx['hex'], toAddr)
    nameTx = self.nodes[ind].namerawtransaction (tx['hex'], nameInd, op)

    tx = self.nodes[ind].signrawtransactionwithwallet (nameTx['hex'])
    txid = self.nodes[ind].sendrawtransaction (tx['hex'])

    return {"txid": txid, "vout": nameInd}, nameTx


  def constructUpdateTx (self, ind, name, val):
    """
    Construct a name_update raw transaction for the given name.  The target
    address is newly constructed.  Returned are the hex-encoded input
    and output, so that one can mix-and-match them.
    """

    addr = self.nodes[ind].getnewaddress ()
    data = self.nodes[ind].name_show (name)
    txo = self.nodes[ind].gettxout (data['txid'], data['vout'])
    amount = txo['value']

    vin = [{"txid": data['txid'], "vout": data['vout']}]
    txin = self.nodes[ind].createrawtransaction (vin, {})

    vout = {addr: amount}
    txout = self.nodes[ind].createrawtransaction ([], vout)
    nameop = {"op": "name_update", "name": name, "value": val, "address": addr}
    txout = self.nodes[ind].namerawtransaction (txout, 0, nameop)

    return txin, txout['hex']


if __name__ == '__main__':
  NameRawTxTest (__file__).main ()
