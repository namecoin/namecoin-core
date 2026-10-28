#!/usr/bin/env python3
# Copyright (c) 2014-2026 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC tests for the handling of names in the wallet.

from test_framework.names import NameTestFramework
from test_framework.util import *

from decimal import Decimal

nameFee = Decimal ("0.01")
initialBalance = Decimal ("2500")
zero = Decimal ("0")

class NameWalletTest (NameTestFramework):

  spentA = zero
  spentB = zero

  def set_test_params (self):
    self.setup_clean_chain = True
    self.setup_name_test ([["-allowexpired"]] * 2)

  def generateToOther (self, ind, n):
    """
    Generates n new blocks with test node ind, paying to an address in neither
    test wallet.  This ensures that we do not mess up the balances.
    """

    addr = "my8oiLCytSojEzK8Apgt7bYjGUD4s6jvRv"
    self.generatetoaddress (self.nodes[ind], n, addr)

  def getFee (self, ind, txid, extra=zero):
    """
    Returns and checks the fee of a transaction.  There may be an additional
    fee for the locked coin, and the paytxfee times the tx size.
    The tx size is queried from the node with the given index by the txid.
    """

    info = self.nodes[ind].gettransaction (txid)
    totalFee = -info['fee']
    assert totalFee >= extra

    absFee = totalFee - extra

    return totalFee

  def checkBalance (self, ind, spent):
    """
    Checks the balance of the node with index ind.  It should be
    the initial balance minus "spent".
    """

    bal = self.nodes[ind].getbalance ()
    assert_equal (bal, initialBalance - spent)
    assert_equal (self.nodes[ind].getbalance (), bal)

  def checkBalances (self, spentA=zero, spentB=zero):
    """
    Checks balances of the two test nodes.  The expected spent amounts
    for them are stored in self.spentA and self.spentB, and increased
    prior to the check by the arguments passed in.
    """

    self.spentA += spentA
    self.spentB += spentB

    self.checkBalance (0, self.spentA)
    self.checkBalance (1, self.spentB)

  def checkTx (self, ind, txid, amount, fee, details):
    """
    Calls 'gettransaction' and compares the result to the
    expected data given in the arguments.  "details" is an array
    containing all the tx sent/received entries expected.
    Each array element is an array itself with the fields:

      [category, nameop, amount, fee]

    nameop can be None if no "name" key is expected.
    """

    data = self.nodes[ind].gettransaction (txid)
    assert_equal (data['amount'], amount)

    if fee is None:
      assert 'fee' not in data
    else:
      assert_equal (data['fee'], fee)

    # Bring the details returned in the same format as our expected
    # argument.  Furthermore, check that each entry has an address
    # set (but don't compare the address, since we don't know it).
    detailsGot = []
    for d in data['details']:
      assert 'address' in d
      if 'name' in d:
        nameOp = d['name']
        if nameOp[:3] == 'new':
          nameOp = 'new'
      else:
        # None is not sortable in Python3, so use "none" instead.
        nameOp = "none"
      if 'fee' in d:
        fee = d['fee']
      else:
        fee = None
      detailsGot.append ([d['category'], nameOp, d['amount'], fee])

    # Compare.  Sort to get rid of differences in the order.
    detailsGot.sort ()
    details.sort ()
    assert_equal (detailsGot, details)

  def run_test (self):
    self.generate (self.nodes[0], 50)
    self.sync_blocks ()
    self.generate (self.nodes[1], 50)
    self.generateToOther (1, 150)
    self.sync_blocks ()
    self.checkBalances ()

    # Check that we use legacy addresses.
    # FIXME: Remove once we have segwit.
    addr = self.nodes[0].getnewaddress ()
    info = self.nodes[0].getaddressinfo (addr)
    assert not info['isscript']
    assert not info['iswitness']

    # Register and update a name.  Check changes to the balance.
    newA = self.nodes[0].name_new ("name-a")
    newFee = self.getFee (0, newA[0], nameFee)
    self.generateToOther (0, 5)
    self.checkBalances (newFee)
    firstA = self.firstupdateName (0, "name-a", newA, "value")
    firstFee = self.getFee (0, firstA)
    self.generateToOther (0, 10)
    self.checkBalances (firstFee)
    updA = self.nodes[0].name_update ("name-a", "new value")
    updFee = self.getFee (0, updA)
    self.generateToOther (0, 1)
    self.checkBalances (updFee)

    # Check the transactions.
    self.checkTx (0, newA[0], zero, -newFee,
                  [['send', 'new', zero, -newFee]])
    self.checkTx (0, firstA, zero, -firstFee,
                  [['send', "update: 'name-a'", zero, -firstFee]])
    self.checkTx (0, updA, zero, -updFee,
                  [['send', "update: 'name-a'", zero, -updFee]])

    # Send a name from 0 to 1 by firstupdate and update.
    addr = self.nodes[1].getnewaddress ()
    newB = self.nodes[0].name_new ("name-b")
    fee = self.getFee (0, newB[0], nameFee)
    newC = self.nodes[0].name_new ("name-c")
    fee += self.getFee (0, newC[0], nameFee)
    self.generateToOther (0, 5)
    self.checkBalances (fee)
    firstB = self.firstupdateName (0, "name-b", newB, "value",
                                   {"dest_address": addr})
    fee = self.getFee (0, firstB)
    firstC = self.firstupdateName (0, "name-c", newC, "value")
    fee += self.getFee (0, firstC)
    self.generateToOther (0, 10)
    self.checkBalances (fee)
    updC = self.nodes[0].name_update ("name-c", "new value",
                                      {"dest_address": addr})
    fee = self.getFee (0, updC)
    self.generateToOther (0, 1)
    self.checkBalances (fee)

    # Check the receiving transactions on node 1.
    self.sync_blocks ()
    self.checkTx (1, firstB, zero, None,
                  [['receive', "update: 'name-b'", zero, None]])
    self.checkTx (1, updC, zero, None,
                  [['receive', "update: 'name-c'", zero, None]])

    # Use the rawtx API to build a simultaneous name update and currency send.
    # This is done as an atomic name trade.  Note, though, that the
    # logic is a bit confused by "coin join" transactions and thus
    # possibly not exactly what one would expect.
    price = Decimal ("1.0")
    fee = Decimal ("0.01")
    txid = self.atomicTrade ("name-a", "enjoy", price, fee, 0, 1)
    self.generateToOther (0, 1)

    self.sync_blocks ()
    self.checkBalances (-price, price + fee)
    self.checkTx (0, txid, price, None,
                  [['receive', "none", price, None]])
    self.checkTx (1, txid, -price, -fee,
                  [['send', "none", -price, -fee],
                   ['send', "update: 'name-a'", zero, -fee]])

    # Test sendtoname RPC command.
    addr = self.nodes[0].getnewaddress ()
    newDest = self.nodes[0].name_new ("destination")
    self.generateToOther (0, 5)
    self.checkBalances (self.getFee (0, newDest[0], nameFee))
    txid = self.firstupdateName (0, "destination", newDest, "value",
                                 {"dest_address": addr})
    self.generateToOther (0, 10)
    self.checkName (0, "destination", "value", None, False)
    self.checkBalances (self.getFee (0, txid))

    self.sync_blocks ()
    assert_raises_rpc_error (-5, 'name not found',
                             self.nodes[1].sendtoname, "non-existant", 10)

    txid = self.nodes[1].sendtoname ("destination", 10)
    fee = self.getFee (1, txid)
    self.generateToOther (1, 1)
    self.sync_blocks ()
    self.checkBalances (-10, 10 + fee)

    txid = self.nodes[1].sendtoname ("destination", 10, "foo", "bar", True)
    fee = self.getFee (1, txid)
    self.generateToOther (1, 1)
    self.sync_blocks ()
    self.checkBalances (-10 + fee, 10)

    self.generateToOther (1, 30)
    self.checkName (1, "destination", "value", None, True)
    assert_raises_rpc_error (-5, 'the name is expired',
                             self.nodes[1].sendtoname, "destination", 10)


if __name__ == '__main__':
  NameWalletTest (__file__).main ()
