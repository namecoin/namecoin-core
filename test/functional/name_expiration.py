#!/usr/bin/env python3
# Copyright (c) 2014-2021 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Test that expiring / unexpiring names works as desired, in particular
# with respect to clearing the UTXO set and mempool.

from test_framework.names import NameTestFramework
from test_framework.util import *

class NameExpirationTest (NameTestFramework):

  def set_test_params (self):
    self.setup_clean_chain = True
    self.setup_name_test ([["-namehistory", "-allowexpired"]])

  def checkUTXO (self, name, shouldBeThere):
    """
    Query for a name's coin in the UTXO set and check that it is either
    there or not.
    """

    data = self.node.name_show (name)
    txo = self.node.gettxout (data['txid'], data['vout'])

    if shouldBeThere:
      assert txo is not None
      assert_equal (txo['scriptPubKey']['nameOp']['name'], name)
    else:
      assert txo is None

  def run_test (self):
    self.node = self.nodes[0]
    self.generate (self.node, 200)

    # Start the registration of two names which will be used.  name-long
    # will expire and be reregistered on a "long chain".  name-short will
    # be updated before expiration on that chain.
    #
    # On another, "short" chain, we let both names expire.
    #
    # In the end, we reorg back to the "long" chain and we verify that the
    # mempool and UTXO set behave as they should.
    newLong = self.node.name_new ("name-long")
    newLong2 = self.node.name_new ("name-long")
    newShort = self.node.name_new ("name-short")
    self.generate (self.node, 12)

    # Register the names.  name-long should expire one block before
    # name-short, so that the situation described above works out.
    updLong = self.firstupdateName (0, "name-long", newLong, "value")
    self.generate (self.node, 2)
    updShort = self.firstupdateName (0, "name-short", newShort, "value")
    self.generate (self.node, 27)
    self.checkName (0, "name-long", "value", 2, False)
    self.checkName (0, "name-short", "value", 4, False)

    # Check that the UTXO entries are there.
    self.checkUTXO ("name-long", True)
    self.checkUTXO ("name-short", True)

    # Create the "long" chain.  Let name-short expire there but renew
    # name-long instead.
    self.node.name_update ("name-long", "renewed")
    undoBlk = self.generate (self.node, 5)[0]
    self.checkName (0, "name-long", "renewed", 26, False)
    self.checkName (0, "name-short", "value", -1, True)
    self.checkNameHistory (0, "name-long", ["value", "renewed"])
    self.checkNameHistory (0, "name-short", ["value"])
    self.checkUTXO ("name-long", True)
    self.checkUTXO ("name-short", False)
    self.generate (self.node, 10)
    self.node.invalidateblock (undoBlk)

    # Let name-long expire on the "short" chain.
    assert_equal (self.node.getrawmempool (), [])
    self.generate (self.node, 2)
    self.checkName (0, "name-long", "value", 0, True)
    self.checkName (0, "name-short", "value", 2, False)
    self.checkUTXO ("name-long", False)
    self.checkUTXO ("name-short", True)

    # Snatch up name-long and update name-short just-in-time.  Note that
    # "just-in-time" is "expires_in == 2", since when creating the block,
    # it will be "expires_in == 1" already!
    updLong2 = self.firstupdateName (0, "name-long", newLong2, "value 2")
    renewShort = self.node.name_update ("name-short", "renewed")
    self.generate (self.node, 1)
    self.checkName (0, "name-long", "value 2", 30, False)
    self.checkName (0, "name-short", "renewed", 30, False)
    self.checkNameHistory (0, "name-long", ["value", "value 2"])
    self.checkNameHistory (0, "name-short", ["value", "renewed"])

    # Reorg back to the long chain.
    self.node.reconsiderblock (undoBlk)
    self.checkName (0, "name-long", "renewed", 16, False)
    self.checkName (0, "name-short", "value", -11, True)
    self.checkNameHistory (0, "name-long", ["value", "renewed"])
    self.checkNameHistory (0, "name-short", ["value"])
    self.checkUTXO ("name-long", True)
    self.checkUTXO ("name-short", False)

    # Check that the conflicting tx's are removed from the mempool.
    assert_equal (self.node.getrawmempool (), [])
    data = self.node.gettransaction (updLong2)
    assert data['confirmations'] <= 0
    data = self.node.gettransaction (renewShort)
    assert data['confirmations'] <= 0

    # Redo the same stuff but now without actually mining the conflicted tx
    # on the short chain.  Make sure that the mempool cleaning works as expected
    # also in this case.
    #
    # name-unexpired will unexpire in the chain reorg, which means that we
    # will try to re-register it on the short chain.
    #
    # name-expired will expire in the chain reorg, which means that we try
    # to update it on the short chain (but that will be too late for the
    # long one after the reorg).

    newUnexpired = self.node.name_new ("name-unexpired")
    newExpired = self.node.name_new ("name-expired")
    newSnatch = self.node.name_new ("name-unexpired")
    self.generate (self.node, 12)

    self.firstupdateName (0, "name-unexpired", newUnexpired, "value")
    self.generate (self.node, 2)
    self.firstupdateName (0, "name-expired", newExpired, "value")
    self.generate (self.node, 27)
    self.checkName (0, "name-unexpired", "value", 2, False)
    self.checkName (0, "name-expired", "value", 4, False)

    # Build the "long chain".
    self.node.name_update ("name-unexpired", "renewed")
    undoBlk = self.generate (self.node, 20)[0]
    self.checkName (0, "name-unexpired", "renewed", 11, False)
    self.checkName (0, "name-expired", "value", -16, True)
    self.node.invalidateblock (undoBlk)

    # Build the "short chain".  Make sure that the mempool does not accidentally
    # contain the renewal transaction from before (it should not, because the
    # reorg is too long to keep transactions in the mempool).
    assert_equal (self.node.getrawmempool (), [])
    self.generate (self.node, 2)
    self.checkName (0, "name-unexpired", "value", 0, True)
    self.checkName (0, "name-expired", "value", 2, False)

    updExpired = self.firstupdateName (0, "name-unexpired", newSnatch,
                                       "value 2")
    updUnexpired = self.node.name_update ("name-expired", "renewed")
    mempoolShort = self.node.getrawmempool ()
    assert updExpired in mempoolShort
    assert updUnexpired in mempoolShort

    # Perform the reorg back to the "long chain".
    self.node.reconsiderblock (undoBlk)
    assert_equal (self.node.getrawmempool (), [])

    # Round-trip check across a daemon restart.  Register a small batch
    # of names that all share the same expiration height, restart the
    # node, and verify that the expire index round-trips cleanly through
    # the on-disk store.  Then mine the block at which they expire and
    # confirm they really do.
    self.log.info ("daemon-restart round-trip of the expire index")

    batchNames = ["round-trip-1", "round-trip-2", "round-trip-3",
                  "round-trip-4"]
    batchNew = {nm: self.node.name_new (nm) for nm in batchNames}
    self.generate (self.node, 12)
    for nm in batchNames:
      self.firstupdateName (0, nm, batchNew[nm], "v-" + nm)
    self.generate (self.node, 1)

    for nm in batchNames:
      self.checkName (0, nm, "v-" + nm, None, False)
      self.checkUTXO (nm, True)

    expiresIn = self.node.name_show (batchNames[0])['expires_in']

    # Restart and verify nothing is lost across the on-disk round-trip.
    self.restart_node (0)
    self.node = self.nodes[0]
    for nm in batchNames:
      data = self.node.name_show (nm)
      assert_equal (data['value'], "v-" + nm)
      assert_equal (data['expired'], False)
      assert_equal (data['expires_in'], expiresIn)
      self.checkUTXO (nm, True)

    # Mine past the expiration cliff.  Every name in the batch should
    # expire in the same block (they share a creation height).
    self.generate (self.node, expiresIn)
    for nm in batchNames:
      self.checkName (0, nm, "v-" + nm, 0, True)
      self.checkUTXO (nm, False)

    # One more restart, this time after the expiration, just to make
    # sure the post-expiration state also survives a flush + reopen.
    self.restart_node (0)
    self.node = self.nodes[0]
    for nm in batchNames:
      self.checkName (0, nm, "v-" + nm, 0, True)
      self.checkUTXO (nm, False)


if __name__ == '__main__':
  NameExpirationTest (__file__).main ()
