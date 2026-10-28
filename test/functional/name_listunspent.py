#!/usr/bin/env python3
# Copyright (c) 2018-2021 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for correct handling of names in "listunspent".

from test_framework.names import NameTestFramework
from test_framework.util import *

class NameListUnspentTest (NameTestFramework):

  def set_test_params (self):
    self.setup_name_test ([["-allowexpired"]] * 2)

  def lookupName (self, ind, name, **kwargs):
    """Wrapper around lookup that gets txid and vout from the name."""

    pending = self.nodes[ind].name_pending (name)
    if len (pending) > 0:
      data = pending[0]
    else:
      data = self.nodes[ind].name_show (name)

    return self.lookup (ind, data['txid'], data['vout'], **kwargs)

  def lookup (self, ind, txid, vout,
              addressFilter=None,
              allowUnconfirmed=False,
              includeNames=False):
    """
    Runs listunspent on the given node and returns the unspent result matching
    the txid:vout combination or None.
    """

    min_conf = 1
    if allowUnconfirmed:
      min_conf = 0

    opt = {"includeNames": includeNames}
    res = self.nodes[ind].listunspent (min_conf, None, addressFilter, None, opt)

    for out in res:
      if (out['txid'], out['vout']) == (txid, vout):
        return out
    return None

  def run_test (self):

    # Create a name new for testing.  Figure out the vout by looking at the
    # raw transaction.
    addrA = self.nodes[0].getnewaddress ()
    new = self.nodes[0].name_new ("testname", {"dest_address": addrA})
    txid = new[0]
    raw = self.nodes[0].getrawtransaction (txid, 1)
    vout = None
    for i in range (len (raw['vout'])):
      if 'nameOp' in raw['vout'][i]['scriptPubKey']:
        vout = i
        break
    assert vout is not None

    # Check expected behaviour for listunspent with the unconfirmed name_new.
    assert self.lookup (0, txid, vout, allowUnconfirmed=True) is None
    assert self.lookup (0, txid, vout, includeNames=True) is None
    unspent = self.lookup (0, txid, vout,
                           addressFilter=[addrA],
                           allowUnconfirmed=True,
                           includeNames=True)
    assert unspent is not None
    assert_equal (unspent['confirmations'], 0)
    assert 'nameOp' in unspent
    assert_equal (unspent['nameOp']['op'], 'name_new')

    # Name new's don't expire.  Verify that we get it after confirmation
    # correctly as well.
    self.generate (self.nodes[0], 50)
    unspent = self.lookup (0, txid, vout, includeNames=True)
    assert unspent is not None
    assert_equal (unspent['confirmations'], 50)
    assert 'nameOp' in unspent

    # Firstupdate the name and check that briefly.
    self.firstupdateName (0, "testname", new, "value")
    self.generate (self.nodes[0], 1)
    unspent = self.lookupName (0, "testname", includeNames=True)
    assert unspent is not None
    assert 'nameOp' in unspent
    assert_equal (unspent['nameOp']['op'], 'name_firstupdate')
    assert_equal (unspent['nameOp']['name'], 'testname')

    # Update the name, sending to another node.
    addrB = self.nodes[1].getnewaddress ()
    self.nodes[0].name_update ("testname", "value", {"dest_address": addrB})
    self.sync_mempools ()

    # Node 0 should no longer have the unspent output.
    assert self.lookupName (0, "testname",
                            allowUnconfirmed=True,
                            includeNames=True) is None
    assert self.lookupName (0, "testname",
                            allowUnconfirmed=True,
                            includeNames=True) is None

    # Node 1 should now see the output as unconfirmed.
    assert self.lookupName (1, "testname", allowUnconfirmed=True) is None
    assert self.lookupName (1, "testname", includeNames=True) is None
    unspent = self.lookupName (1, "testname",
                               addressFilter=[addrB],
                               allowUnconfirmed=True,
                               includeNames=True)
    assert unspent is not None
    assert_equal (unspent['confirmations'], 0)
    assert 'nameOp' in unspent
    assert_equal (unspent['nameOp']['op'], 'name_update')
    assert_equal (unspent['nameOp']['name'], 'testname')

    # Mine blocks and verify node 1 seeing the name correctly.
    self.generate (self.nodes[1], 30)
    assert_equal (self.nodes[1].name_show ("testname")['expired'], False)
    assert_equal (self.nodes[1].name_show ("testname")['expires_in'], 1)
    assert self.lookupName (1, "testname") is None
    assert self.lookupName (1, "testname",
                            addressFilter=[addrA],
                            includeNames=True) is None
    unspent = self.lookupName (1, "testname", includeNames=True)
    assert unspent is not None
    assert_equal (unspent['confirmations'], 30)
    assert 'nameOp' in unspent
    assert_equal (unspent['nameOp']['op'], 'name_update')
    assert_equal (unspent['nameOp']['name'], 'testname')

    # One more block and the name expires.  Then it should no longer show up.
    self.generate (self.nodes[1], 1)
    assert_equal (self.nodes[1].name_show ("testname")['expired'], True)
    assert self.lookupName (1, "testname", includeNames=True) is None

if __name__ == '__main__':
  NameListUnspentTest (__file__).main ()
