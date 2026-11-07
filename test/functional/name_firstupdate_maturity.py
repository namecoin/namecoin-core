#!/usr/bin/env python3
# Copyright (c) 2014-2026 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Test that name_firstupdate is rejected from the mempool when the
# name_new has insufficient confirmations (anti-frontrunning protection).

from test_framework.names import NameTestFramework
from test_framework.util import assert_equal


class NameFirstupdateMaturityTest (NameTestFramework):

  def set_test_params (self):
    self.setup_clean_chain = True
    self.setup_name_test ([[]])

  def run_test (self):
    node = self.nodes[0]
    self.generate (node, 200)

    self.test_immature_rejected_from_mempool (node)
    self.test_mature_accepted_into_mempool (node)
    self.test_unconfirmed_name_new_rejected (node)
    self.test_boundary_exactly_at_depth (node)
    self.test_wallet_rebroadcast_after_maturity (node)

  def test_immature_rejected_from_mempool (self, node):
    """name_firstupdate must not enter mempool when name_new has < 12 confs."""
    self.log.info ("Testing immature name_firstupdate rejected from mempool...")

    new_data = node.name_new ("test-immature")
    # Mine only 1 block — name_new has 1 confirmation, needs 12
    self.generate (node, 1)

    # name_firstupdate goes through CommitTransaction which doesn't throw,
    # but the tx should NOT enter the mempool
    self.firstupdateName (0, "test-immature", new_data, "value")
    assert_equal (node.getrawmempool (), [])

    # Mine up to 11 confirmations — still not enough
    self.generate (node, 10)
    self.firstupdateName (0, "test-immature", new_data, "value")
    assert_equal (node.getrawmempool (), [])

  def test_mature_accepted_into_mempool (self, node):
    """name_firstupdate succeeds once name_new has >= 12 confirmations."""
    self.log.info ("Testing mature name_firstupdate accepted...")

    new_data = node.name_new ("test-mature")
    self.generate (node, 12)

    txid = self.firstupdateName (0, "test-mature", new_data, "value-mature")
    assert txid in node.getrawmempool ()

    self.generate (node, 1)
    self.checkName (0, "test-mature", "value-mature", 30, False)

  def test_unconfirmed_name_new_rejected (self, node):
    """name_firstupdate with an unconfirmed name_new must not enter mempool."""
    self.log.info ("Testing unconfirmed name_new rejected...")

    new_data = node.name_new ("test-unconfirmed")
    # Don't mine — name_new is still in mempool (0 confirmations)

    self.firstupdateName (0, "test-unconfirmed", new_data, "value")
    # Filter mempool: only name_new should be there, no name_firstupdate
    for txid in node.getrawmempool ():
      tx = node.getrawtransaction (txid, True)
      for vout in tx.get ("vout", []):
        nameOp = vout.get ("scriptPubKey", {}).get ("nameOp", {})
        assert nameOp.get ("op") != "name_firstupdate", \
            "Immature name_firstupdate should not be in mempool"

    # Clean up: mine to confirm the name_new
    self.generate (node, 1)

  def test_boundary_exactly_at_depth (self, node):
    """Test the exact boundary: rejected at depth 11, accepted at depth 12."""
    self.log.info ("Testing exact maturity boundary...")

    new_data = node.name_new ("test-boundary")
    self.generate (node, 11)

    # At depth 11: should NOT enter mempool
    self.firstupdateName (0, "test-boundary", new_data, "value")
    assert_equal (node.getrawmempool (), [])

    # Mine one more block (depth 12): should be accepted
    self.generate (node, 1)
    txid = self.firstupdateName (0, "test-boundary", new_data, "value-boundary")
    assert txid in node.getrawmempool ()

    self.generate (node, 1)

  def test_wallet_rebroadcast_after_maturity (self, node):
    """Verify that a rejected immature firstupdate can succeed after maturity."""
    self.log.info ("Testing resubmission after maturity...")

    new_data = node.name_new ("test-rebroadcast")
    self.generate (node, 1)

    # Rejected — immature
    self.firstupdateName (0, "test-rebroadcast", new_data, "value")
    assert_equal (node.getrawmempool (), [])

    # Mine remaining blocks to reach maturity
    self.generate (node, 11)

    # Now try again — should succeed
    txid = self.firstupdateName (
        0, "test-rebroadcast", new_data, "value-rebroadcast")
    assert txid in node.getrawmempool ()

    self.generate (node, 1)
    self.checkName (0, "test-rebroadcast", "value-rebroadcast", 30, False)


if __name__ == '__main__':
  NameFirstupdateMaturityTest (__file__).main ()
