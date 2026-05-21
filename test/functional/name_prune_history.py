#!/usr/bin/env python3
# Copyright (c) 2014-2026 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Test the -prunenamehistory node-local option.  The flag trims
# individual DB_NAME_HISTORY entries once they are more than
# (expirationDepth + pruneDepth) blocks deep, applied uniformly to
# entries of live, expired, and renewed names.  The trim is driven by a
# dedicated history-tail index that records every archived history
# entry by its own write height (not by the name's current live
# height), so renewed names see their early history dropped on
# schedule even while they remain actively renewed.

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

    # Regtest consensus: NameExpirationDepth = 30 blocks.
    # Pruning grace configured to 10 blocks above.
    EXP_DEPTH = 30
    PRUNE_DEPTH = 10
    HORIZON = EXP_DEPTH + PRUNE_DEPTH

    # PruneExpiredHistory is invoked from ConnectBlock with
    # nHeight = pindex->nHeight + 1, mirroring ExpireNames.  So when a
    # block at chain height N is connected, the trim cut-off is
    # (N + 1) - HORIZON, i.e. an archive at height H is first visited
    # by the trim hook at chain height H + HORIZON - 1.
    def trim_block_for_archive (h):
      return h + HORIZON - 1

    self.generate (nodePrune, 200)
    self.sync_blocks (self.nodes)

    # Three names exercising the key cases:
    #
    #   d/prune-me     : firstupdated, then updated once.  Its single
    #                    history entry sits at the firstupdate height
    #                    and should be dropped once that height ages
    #                    past HORIZON.  The name itself is never
    #                    renewed beyond v2, so by the time the trim
    #                    catches it, name_show reports it as expired.
    #
    #   d/renew-thrice : firstupdated, updated to v2, then renewed to
    #                    v3 and v4 at later heights.  History-tail
    #                    index entries land at THREE distinct heights
    #                    (firstupdate, the v2 height, the v3 height).
    #                    Under option-2 semantics these are visited
    #                    independently and the early entries drop on
    #                    schedule even though the live row is still
    #                    active.  This is the case the previous PR
    #                    iteration (live-row-driven trim) could not
    #                    handle.
    #
    #   d/safe-tail    : same shape as d/renew-thrice but its latest
    #                    renewal happens late enough that not all of
    #                    its history entries cross the threshold in
    #                    this test.  Used to confirm that the trim
    #                    leaves a non-empty tail when only some
    #                    entries are ripe.
    newA = nodePrune.name_new ("d/prune-me")
    newB = nodePrune.name_new ("d/renew-thrice")
    newC = nodePrune.name_new ("d/safe-tail")
    self.generate (nodePrune, 12)
    self.sync_blocks (self.nodes)

    self.firstupdateName (0, "d/prune-me",     newA, "v1")
    self.firstupdateName (0, "d/renew-thrice", newB, "v1")
    self.firstupdateName (0, "d/safe-tail",    newC, "v1")
    firstBlk = self.generate (nodePrune, 1)[0]
    self.sync_blocks (self.nodes)
    heightFirst = nodePrune.getblockcount ()

    # Update to v2.  This is the moment each name's v1 entry gets
    # archived into DB_NAME_HISTORY -> history-tail index gains a key
    # at heightFirst for each of the three names.
    nodePrune.name_update ("d/prune-me",     "v2")
    nodePrune.name_update ("d/renew-thrice", "v2")
    nodePrune.name_update ("d/safe-tail",    "v2")
    self.generate (nodePrune, 1)
    self.sync_blocks (self.nodes)
    heightV2 = nodePrune.getblockcount ()
    assert_equal (heightV2, heightFirst + 1)

    # Sanity: both history-tracking nodes see [v1, v2].
    self.checkNameHistory (0, "d/prune-me",     ["v1", "v2"])
    self.checkNameHistory (1, "d/prune-me",     ["v1", "v2"])
    self.checkNameHistory (0, "d/renew-thrice", ["v1", "v2"])
    self.checkNameHistory (0, "d/safe-tail",    ["v1", "v2"])

    # Renew d/renew-thrice to v3, then v4, spaced one block apart.
    # After this, the history-tail index has keys for d/renew-thrice
    # at {heightFirst, heightV2, heightV3} (one per archived entry).
    nodePrune.name_update ("d/renew-thrice", "v3")
    self.generate (nodePrune, 1)
    self.sync_blocks (self.nodes)
    heightV3 = nodePrune.getblockcount ()
    nodePrune.name_update ("d/renew-thrice", "v4")
    self.generate (nodePrune, 1)
    self.sync_blocks (self.nodes)
    heightV4 = nodePrune.getblockcount ()

    self.checkNameHistory (0, "d/renew-thrice", ["v1", "v2", "v3", "v4"])
    self.checkNameHistory (1, "d/renew-thrice", ["v1", "v2", "v3", "v4"])

    # Renew d/safe-tail much later, so its v2 archive sits high enough
    # to survive the first round of trims.  Mine some filler blocks so
    # the v3 renewal lands well above the eventual cut-off.
    self.generate (nodePrune, 25)
    self.sync_blocks (self.nodes)
    nodePrune.name_update ("d/safe-tail", "v3")
    self.generate (nodePrune, 1)
    self.sync_blocks (self.nodes)
    heightSafeV3 = nodePrune.getblockcount ()

    self.checkNameHistory (0, "d/safe-tail", ["v1", "v2", "v3"])
    self.checkNameHistory (1, "d/safe-tail", ["v1", "v2", "v3"])

    # ----------------------------------------------------------------
    # First trim crossing: bring the chain to trim_block_for_archive
    # (heightFirst), the block at which the trim hook's cut-off first
    # equals heightFirst.  The history-tail index has keys at
    # heightFirst for all three names (their v1 entries archived when
    # the v2 update fired).  After the trim, the v1 archive drops from
    # all three; the v2 archives (at heightV2) and any later archives
    # are untouched because their index keys live at later heights.
    # ----------------------------------------------------------------
    target = trim_block_for_archive (heightFirst)
    while nodePrune.getblockcount () < target:
      self.generate (nodePrune, 1)
    self.sync_blocks (self.nodes)

    # d/prune-me: had only v1 archived.  History now empty -> RPC
    # name_history returns just the live row (v2, expired).
    historyPruneMe = nodePrune.name_history ("d/prune-me")
    assert_equal ([e['value'] for e in historyPruneMe], ["v2"])
    assert_equal (historyPruneMe[0]['expired'], True)
    # Control node has the full history.
    self.checkNameHistory (1, "d/prune-me", ["v1", "v2"])

    # d/renew-thrice: v1 archive dropped, v2 and v3 archives kept.
    self.checkNameHistory (0, "d/renew-thrice", ["v2", "v3", "v4"])
    self.checkNameHistory (1, "d/renew-thrice", ["v1", "v2", "v3", "v4"])
    # Live row preserved.
    showThrice = nodePrune.name_show ("d/renew-thrice")
    assert_equal (showThrice["value"], "v4")

    # d/safe-tail: v1 archive dropped, v2 archive kept.
    self.checkNameHistory (0, "d/safe-tail", ["v2", "v3"])
    self.checkNameHistory (1, "d/safe-tail", ["v1", "v2", "v3"])

    # ----------------------------------------------------------------
    # Second trim crossing: mine to trim_block_for_archive(heightV2)
    # so the cut-off catches up to heightV2.  Both d/renew-thrice and
    # d/safe-tail were updated to v2 in the same block, so this
    # catches both their v2 archives.  d/renew-thrice's v2 archive
    # came from the v3 renewal (which had to happen for v2 to be
    # archived at all), so the index key is firmly there.  Same for
    # d/safe-tail's v2 archive (came from the late v3 renewal).
    # ----------------------------------------------------------------
    target = trim_block_for_archive (heightV2)
    while nodePrune.getblockcount () < target:
      self.generate (nodePrune, 1)
    self.sync_blocks (self.nodes)

    # d/renew-thrice: v1 and v2 archives gone, v3 archive kept.
    self.checkNameHistory (0, "d/renew-thrice", ["v3", "v4"])

    # d/safe-tail: v2 archive drops too (same heightV2).  Now only
    # v3's name_history representation is the live row (v3); no
    # archived entries remain on node 0.
    historySafe = nodePrune.name_history ("d/safe-tail")
    assert_equal ([e['value'] for e in historySafe], ["v3"])

    # The unpruned node keeps the full chronology.
    self.checkNameHistory (1, "d/renew-thrice", ["v1", "v2", "v3", "v4"])
    self.checkNameHistory (1, "d/safe-tail",    ["v1", "v2", "v3"])

    # ----------------------------------------------------------------
    # Third trim crossing: mine to trim_block_for_archive(heightV3),
    # catching d/renew-thrice's v3 archive.  After this,
    # d/renew-thrice's history-tail index is empty; name_history is
    # just the live row.  The DB_NAME row is preserved -- name_show
    # still returns v4.
    # ----------------------------------------------------------------
    target = trim_block_for_archive (heightV3)
    while nodePrune.getblockcount () < target:
      self.generate (nodePrune, 1)
    self.sync_blocks (self.nodes)

    historyThrice = nodePrune.name_history ("d/renew-thrice")
    assert_equal ([e['value'] for e in historyThrice], ["v4"])
    showThriceAfter = nodePrune.name_show ("d/renew-thrice")
    assert_equal (showThriceAfter["value"], "v4")
    # By construction the trim only fires after the live row is more
    # than HORIZON blocks deep, so name_show necessarily reports it as
    # expired at this point.  That's the documented "audit trail of
    # past states; current state lives elsewhere" semantic.
    assert_equal (showThriceAfter["expired"], True)

    # Node 2 has -namehistory off so name_history must refuse.
    assert_raises_rpc_error (-1, "-namehistory is not enabled",
                             nodeNoHistory.name_history, "d/renew-thrice")

    # ----------------------------------------------------------------
    # Reorg corner case:  pruning is irreversible.  Roll the chain back
    # past a trim block and confirm the node does not crash or corrupt
    # its database.  The dropped history is not restored; this is the
    # documented trade-off of the option.
    # ----------------------------------------------------------------
    tipBefore = nodePrune.getbestblockhash ()
    # Invalidate the v2 block (firstBlk + 1).  This unwinds all the
    # later updates too.  d/prune-me reverts to v1.
    invalidateTarget = nodePrune.getblockhash (heightV2)
    nodePrune.invalidateblock (invalidateTarget)
    self.checkName (0, "d/prune-me", "v1", None, False)
    # History on node 0 was already trimmed: name_history just reports
    # the current (now v1) data.
    historyAfterReorg = nodePrune.name_history ("d/prune-me")
    assert_equal ([e['value'] for e in historyAfterReorg], ["v1"])
    # Reconsider so we restore the original tip cleanly.
    nodePrune.reconsiderblock (invalidateTarget)
    assert_equal (nodePrune.getbestblockhash (), tipBefore)

    # Run name_checkdb to make sure the LevelDB state is self-consistent
    # even after the reorg-with-pruned-history dance.  This also
    # exercises the new DB_NAME_HISTORY_EXPIRY validation in
    # CCoinsViewDB::ValidateNameDB.
    assert_equal (nodePrune.name_checkdb (), True)
    assert_equal (nodeKeep.name_checkdb (), True)
    assert_equal (nodeNoHistory.name_checkdb (), True)


if __name__ == '__main__':
  NamePruneHistoryTest (__file__).main ()
