#!/usr/bin/env python3
# Copyright (c) 2014-2026 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Test the -prunenamehistory node-local option.  The flag drops
# DB_NAME_HISTORY entries for fully-expired names whose latest update
# became expired more than the configured number of blocks ago.

from test_framework.names import NameTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class NamePruneHistoryTest (NameTestFramework):

  def set_test_params (self):
    self.setup_clean_chain = True
    # Three nodes:
    #   0: history on, pruning at 10 blocks past expiry (unit under test)
    #   1: history on, pruning disabled (control / unpruned reference)
    #   2: history disabled, pruning argument tolerated (must warn but
    #      not break the node)
    self.setup_name_test ([
      ["-namehistory", "-prunenamehistory=10", "-allowexpired"],
      ["-namehistory", "-allowexpired"],
      ["-prunenamehistory=10", "-allowexpired"],
    ])

  def run_test (self):
    nodePrune = self.nodes[0]
    nodeKeep = self.nodes[1]
    nodeNoHistory = self.nodes[2]

    self.generate (nodePrune, 200)
    self.sync_blocks (self.nodes)

    # Register two names.  d/prune-me will be left to expire,
    # d/keep-me will be renewed before expiry.
    newA = nodePrune.name_new ("d/prune-me")
    newB = nodePrune.name_new ("d/keep-me")
    self.generate (nodePrune, 12)
    self.sync_blocks (self.nodes)

    self.firstupdateName (0, "d/prune-me", newA, "v1")
    self.firstupdateName (0, "d/keep-me", newB, "v1")
    self.generate (nodePrune, 2)
    self.sync_blocks (self.nodes)

    nodePrune.name_update ("d/prune-me", "v2")
    nodePrune.name_update ("d/keep-me", "v2")
    updateBlk = self.generate (nodePrune, 1)[0]
    self.sync_blocks (self.nodes)

    # Sanity:  both history-tracking nodes see the [v1, v2] timeline.
    self.checkNameHistory (0, "d/prune-me", ["v1", "v2"])
    self.checkNameHistory (1, "d/prune-me", ["v1", "v2"])
    self.checkNameHistory (0, "d/keep-me",  ["v1", "v2"])
    self.checkNameHistory (1, "d/keep-me",  ["v1", "v2"])

    # Renew d/keep-me before it expires.
    self.generate (nodePrune, 27)
    self.sync_blocks (self.nodes)
    nodePrune.name_update ("d/keep-me", "v3")
    self.generate (nodePrune, 1)
    self.sync_blocks (self.nodes)
    self.checkName (0, "d/keep-me", "v3", 30, False)

    # d/prune-me expires (regtest expiration depth = 30) at the block
    # whose height is updateBlk_height + 30.  At that point the history
    # is not yet old enough to prune (configured grace = 10 blocks).
    self.checkName (0, "d/prune-me", "v2", 2, False)
    self.generate (nodePrune, 2)
    self.sync_blocks (self.nodes)
    self.checkName (0, "d/prune-me", "v2", 0, True)
    self.checkNameHistory (0, "d/prune-me", ["v1", "v2"])
    self.checkNameHistory (1, "d/prune-me", ["v1", "v2"])

    # Now mine 10 more blocks so the grace period elapses.  The pruning
    # path is triggered exactly when the current height reaches the
    # latest update's height + expirationDepth + pruneDepth.
    self.generate (nodePrune, 10)
    self.sync_blocks (self.nodes)

    # Node 0 has pruned: name_history returns a single entry, namely the
    # current (expired) DB_NAME row.  Its value is "v2".
    historyPruned = nodePrune.name_history ("d/prune-me")
    assert_equal (len (historyPruned), 1)
    assert_equal (historyPruned[0]['value'], "v2")
    assert_equal (historyPruned[0]['expired'], True)

    # Node 1 still has the full record.
    self.checkNameHistory (1, "d/prune-me", ["v1", "v2"])

    # d/keep-me must be untouched on both nodes: it has a live DB_NAME
    # row that's not expired, so the prune candidate set does not
    # include it.  Each node sees v1, v2, v3 (the live head is appended
    # by name_history).
    self.checkNameHistory (0, "d/keep-me", ["v1", "v2", "v3"])
    self.checkNameHistory (1, "d/keep-me", ["v1", "v2", "v3"])

    # Node 2 has -namehistory off so name_history must refuse.
    assert_raises_rpc_error (-1, "-namehistory is not enabled",
                             nodeNoHistory.name_history, "d/prune-me")

    # ----------------------------------------------------------------
    # Reorg corner case:  pruning is irreversible.  Roll the chain back
    # past the prune point and confirm the node does not crash or
    # corrupt its database.  The pruned history is not restored; this
    # is the documented trade-off of the option.
    # ----------------------------------------------------------------
    tipBefore = nodePrune.getbestblockhash ()
    nodePrune.invalidateblock (updateBlk)

    # The update to "v2" is undone.  d/prune-me reverts to its v1 state.
    self.checkName (0, "d/prune-me", "v1", None, False)

    # History on node 0 was already pruned: name_history just reports
    # the current (now v1) data.
    historyAfterReorg = nodePrune.name_history ("d/prune-me")
    assert_equal ([e['value'] for e in historyAfterReorg], ["v1"])

    # Reconsider the original tip; everything must come back cleanly.
    nodePrune.reconsiderblock (updateBlk)
    assert_equal (nodePrune.getbestblockhash (), tipBefore)

    # Run name_checkdb to make sure the LevelDB state is self-consistent
    # even after the reorg-with-pruned-history dance.
    assert_equal (nodePrune.name_checkdb (), True)


if __name__ == '__main__':
  NamePruneHistoryTest (__file__).main ()
