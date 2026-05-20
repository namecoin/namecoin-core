#!/usr/bin/env python3
# Copyright (c) 2014-2026 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Exercise the legacy-format detection and -reindex-chainstate recovery
# path for the packed DB_NAME_EXPIRY layout.
#
# The packed layout writes a small format-version marker into the
# chainstate database the first time the daemon starts against an empty
# (or freshly-wiped) chainstate.  Subsequent startups verify that the
# marker matches the current code, and refuse to load if it does not.
#
# Pre-existing mainnet databases written before this change have no
# marker at all.  The daemon must refuse to start with a clear pointer
# at -reindex-chainstate, and -reindex-chainstate must let the user
# recover.
#
# The test uses the hidden -debugforgetnamedbversion startup flag to
# simulate an old database by erasing the marker between runs.  The
# resulting on-disk shape is indistinguishable from one written by a
# release that pre-dates the marker.

from test_framework.names import NameTestFramework
from test_framework.util import assert_equal


class NameExpireIdxReindexTest (NameTestFramework):

  def set_test_params (self):
    self.setup_clean_chain = True
    self.setup_name_test ([["-namehistory"]])

  def run_test (self):
    node = self.nodes[0]

    self.log.info ("Building a small chain with a registered name")
    self.generate (node, 200)
    newA = node.name_new ("a")
    self.generate (node, 12)
    self.firstupdateName (0, "a", newA, "value-a")
    self.generate (node, 5)
    self.checkName (0, "a", "value-a", None, False)

    tipHash = node.getbestblockhash ()
    tipHeight = node.getblockcount ()

    self.log.info ("Restarting cleanly to confirm baseline startup works")
    self.restart_node (0)
    assert_equal (node.getbestblockhash (), tipHash)
    self.checkName (0, "a", "value-a", None, False)

    self.log.info ("Simulating a legacy database by erasing the format"
                   " version marker, then restart")
    self.stop_node (0)
    self.start_node (0, extra_args=self.extra_args[0] +
                     ["-debugforgetnamedbversion"])
    self.stop_node (0)

    self.log.info ("Daemon must refuse to load the legacy database and"
                   " point at -reindex-chainstate")
    node.assert_start_raises_init_error (
      extra_args=self.extra_args[0],
      expected_msg=(
        "Error: Unsupported chainstate database format found. "
        "Please restart with -reindex-chainstate. "
        "This will rebuild the chainstate database."
      ),
    )

    self.log.info ("-reindex-chainstate recovers the database")
    self.start_node (0, extra_args=self.extra_args[0] +
                     ["-reindex-chainstate"])
    node.waitforblockheight (tipHeight)
    assert_equal (node.getbestblockhash (), tipHash)
    self.checkName (0, "a", "value-a", None, False)

    self.log.info ("After recovery, a plain restart works again, proving"
                   " the format version marker was rewritten")
    self.restart_node (0)
    assert_equal (node.getbestblockhash (), tipHash)
    self.checkName (0, "a", "value-a", None, False)


if __name__ == '__main__':
  NameExpireIdxReindexTest (__file__).main ()
