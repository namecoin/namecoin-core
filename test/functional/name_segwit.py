#!/usr/bin/env python3
# Copyright (c) 2014-2026 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for names at segwit addresses.

from test_framework.names import NameTestFramework

from test_framework.blocktools import (
  add_witness_commitment,
  create_block,
  create_coinbase,
)
from test_framework.messages import (
  CScriptWitness,
  CTransaction,
  CTxInWitness,
  CTxWitness,
)
from test_framework.util import (
  assert_equal,
  assert_greater_than,
  assert_raises_rpc_error,
  softfork_active,
)

from decimal import Decimal
import io

SEGWIT_ACTIVATION_HEIGHT = 300


class NameSegwitTest (NameTestFramework):

  def set_test_params (self):
    self.setup_name_test ([[
      "-debug",
      "-par=1",
      f"-testactivationheight=segwit@{SEGWIT_ACTIVATION_HEIGHT}"
    ]] * 1)

  def checkNameValueAddr (self, name, value, addr):
    """
    Verifies that the given name has the given value and address.
    """

    data = self.checkName (0, name, value, None, False)
    assert_equal (data['address'], addr)

  def findUnspent (self, amount):
    """
    Finds and returns an unspent input the node has with at least the
    given value.
    """

    unspents = self.node.listunspent ()
    for u in unspents:
      if u['amount'] >= amount:
        return u

    raise AssertionError ("Could not find suitable unspent output")

  def buildDummySegwitNameUpdate (self, name, value, addr):
    """
    Builds a transaction that updates the given name to the given value and
    address.  We assume that the name is at a native segwit script.  The witness
    of the transaction will be set to two dummy stack elements so that the
    program itself is "well-formed" even if it won't execute successfully.
    """

    data = self.node.name_show (name)
    u = self.findUnspent (Decimal ('0.01'))
    ins = [data, u]
    outs = {addr: Decimal ('0.01')}

    txHex = self.node.createrawtransaction (ins, outs)
    nameOp = {"op": "name_update", "name": name, "value": value}
    txHex = self.node.namerawtransaction (txHex, 0, nameOp)['hex']
    txHex = self.node.signrawtransactionwithwallet (txHex)['hex']

    tx = CTransaction ()
    tx.deserialize (io.BytesIO (bytes.fromhex (txHex)))
    tx.wit = CTxWitness ()
    tx.wit.vtxinwit.append (CTxInWitness ())
    tx.wit.vtxinwit[0].scriptWitness = CScriptWitness ()
    tx.wit.vtxinwit[0].scriptWitness.stack = [b"dummy"] * 2
    txHex = tx.serialize ().hex ()

    return txHex

  def tryUpdateSegwitName (self, name, value, addr):
    """
    Tries to update the given name to the given value and address.
    """

    txHex = self.buildDummySegwitNameUpdate (name, value, addr)
    self.node.sendrawtransaction (txHex, 0)

  def tryUpdateInBlock (self, name, value, addr, withWitness):
    """
    Tries to update the given name with a dummy witness directly in a block
    (to bypass any checks done on the mempool).
    """

    txHex = self.buildDummySegwitNameUpdate (name, value, addr)
    tx = CTransaction ()
    tx.deserialize (io.BytesIO (bytes.fromhex (txHex)))

    tip = self.node.getbestblockhash ()
    height = self.node.getblockcount () + 1
    nTime = self.node.getblockheader (tip)["mediantime"] + 1
    block = create_block (int (tip, 16), create_coinbase (height), ntime=nTime,
                          version=4)

    block.vtx.append (tx)
    add_witness_commitment (block, 0)
    block.solve ()

    blkHex = block.serialize (withWitness).hex ()
    return self.node.submitblock (blkHex)

  def run_test (self):
    self.node = self.nodes[0]

    # Register a test name to a bech32 pure-segwit address.
    addr = self.node.getnewaddress ("test", "bech32")
    name = "d/test"
    value = "{}"
    new = self.node.name_new (name)
    self.generate (self.node, 10)
    self.firstupdateName (0, name, new, value, {"dest_address": addr})
    self.generate (self.node, 5)
    self.checkNameValueAddr (name, value, addr)

    # Before segwit activation, the script should behave as anyone-can-spend.
    # It will still fail due to non-mandatory flag checks when submitted
    # into the mempool.
    assert not softfork_active (self.node, "segwit")
    assert_raises_rpc_error (-26, 'Script failed an OP_EQUALVERIFY operation',
                             self.tryUpdateSegwitName,
                             name, "wrong value", addr)
    self.generate (self.node, 1)
    self.checkNameValueAddr (name, value, addr)

    # But directly in a block, the update should work with a dummy witness.
    assert_equal (self.tryUpdateInBlock (name, "stolen", addr,
                                         withWitness=False),
                  None)
    self.checkNameValueAddr (name, "stolen", addr)

    # Activate segwit.  Since this makes the original name expire, we have
    # to re-register it.
    self.generate (self.node, 100)
    new = self.node.name_new (name)
    self.generate (self.node, 10)
    self.firstupdateName (0, name, new, value, {"dest_address": addr})
    self.generate (self.node, 5)
    self.checkNameValueAddr (name, value, addr)

    # Verify that now trying to update the name without a proper signature
    # fails differently.
    assert softfork_active (self.node, "segwit")
    assert_equal (self.tryUpdateInBlock (name, "wrong value", addr,
                                         withWitness=True),
                  'mempool-script-verify-flag-failed'
                    + ' (Script failed an OP_EQUALVERIFY operation)')
    self.checkNameValueAddr (name, value, addr)

    # Updating the name ordinarily (with signature) should work fine even
    # though it is at a segwit address.  Also spending from P2SH-segwit
    # should work fine.
    addrP2SH = self.node.getnewaddress ("test", "p2sh-segwit")
    self.node.name_update (name, "value 2", {"dest_address": addrP2SH})
    self.generate (self.node, 1)
    self.checkNameValueAddr (name, "value 2", addrP2SH)
    self.node.name_update (name, "value 3", {"dest_address": addr})
    self.generate (self.node, 1)
    self.checkNameValueAddr (name, "value 3", addr)


if __name__ == '__main__':
  NameSegwitTest (__file__).main ()
