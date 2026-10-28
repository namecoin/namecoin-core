#!/usr/bin/env python3
# Copyright (c) 2014-2025 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for multisig handling with names.

from test_framework.names import NameTestFramework

from test_framework.messages import (
  COIN,
  COutPoint,
  CTransaction,
  CTxIn,
  CTxOut,
  NAMECOIN_TX_VERSION,
)
from test_framework.script import (
  CScript,
  OP_2DROP,
  OP_DROP,
  OP_NAME_UPDATE,
  OP_TRUE,
)
from test_framework.util import (
  assert_equal,
  assert_greater_than,
  assert_raises_rpc_error,
  softfork_active,
)

from decimal import Decimal
import codecs
import io


class NameMultisigTest (NameTestFramework):

  def set_test_params (self):
    # For now, BIP16 is active (we do not yet have access to the parsed
    # options).  If --bip16-active is false, we restart the node later on.
    # Since Segwit assumes that BIP16 is active and we do not need Segwit
    # for this test at all, just disable it always.
    self.node_args = ["-acceptnonstdtxn=1", "-testactivationheight=segwit@1000000"]
    # Nodes 0 and 1 hold the two keys for the 2-of-2 multisig, and node 2
    # has a watch-only wallet with the actual multisig address in it.
    self.setup_name_test ([self.node_args] * 3)

  def add_options (self, parser):
    super ().add_options (parser)
    parser.add_argument ("--bip16-active", dest="activated", default=False,
                         action="store_true",
                         help="Test behaviour with BIP16 active")

  def getNewPubkey (self, ind):
    """
    Get a new address of one of the nodes and return directly
    the full pubkey.
    """

    addr = self.nodes[ind].getnewaddress ()
    data = self.nodes[ind].getaddressinfo (addr)

    return data['pubkey']

  def checkNameWithHeight (self, ind, name, value, height):
    """
    Verifies that the given name as the given value and update height.
    """

    data = self.checkName (ind, name, value, None, False)
    assert_equal (data['height'], height)

  def getP2SH (self, ind, script):
    """
    Takes a CScript instance and returns the corresponding P2SH address.
    """

    scrHex = script.hex ()
    data = self.nodes[ind].decodescript (scrHex)
    return data['p2sh']

  def findUnspent (self, ind, amount):
    """
    Finds and returns an unspent input the node has with at least the
    given value.
    """

    unspents = self.nodes[ind].listunspent ()
    for u in unspents:
      if u['amount'] >= amount:
        return u

    raise AssertionError ("Could not find suitable unspent output")

  def setScriptSigOps (self, txHex, ind, scriptSigOps):
    """
    Update the given hex transaction by setting the scriptSig for the
    input with the given index.
    """

    tx = CTransaction ()
    tx.deserialize (io.BytesIO (bytes.fromhex (txHex)))
    tx.vin[ind].scriptSig = CScript (scriptSigOps)

    return tx.serialize ().hex ()

  def updateAnyoneCanSpendName (self, ind, name, value, addr, scriptSigOps):
    """
    Updates the given name to the given value and address.  The name input will
    be signed by the given array of script ops for the scriptSig.
    """

    data = self.nodes[ind].name_show (name)
    u = self.findUnspent (ind, Decimal ('0.01'))
    ins = [data, u]
    outs = {addr: Decimal ('0.01')}

    tx = self.nodes[ind].createrawtransaction (ins, outs)
    nameOp = {"op": "name_update", "name": name, "value": value}
    tx = self.nodes[ind].namerawtransaction (tx, 0, nameOp)['hex']

    tx = self.setScriptSigOps (tx, 0, scriptSigOps)
    signed = self.nodes[ind].signrawtransactionwithwallet (tx)
    assert signed['complete']
    self.nodes[ind].sendrawtransaction (signed['hex'], 0)

  def test_2of2_multisig (self):
    """
    Tests that holding a name in a 2-of-2 multisig address works as expected.
    One key holder alone cannot sign, but both together can update the name.
    This verifies basic usage of P2SH multisig for names.
    """

    self.log.info ("Testing name held by 2-of-2 multisig...")

    # Construct a 2-of-2 multisig address shared between two nodes.
    pubkeyA = self.getNewPubkey (0)
    pubkeyB = self.getNewPubkey (1)
    self.nodes[2].createwallet (wallet_name="multisig", disable_private_keys=True)
    multisigRpc = self.nodes[2].get_wallet_rpc ("multisig")
    desc = f"sh(multi(2,{pubkeyA},{pubkeyB}))"
    info = self.nodes[2].getdescriptorinfo (desc)
    desc = f"{desc}#{info['checksum']}"
    multisigRpc.importdescriptors ([{"desc": desc, "timestamp": "now"}])
    [p2sh] = self.nodes[2].deriveaddresses (desc)

    # Register a new name to that address.
    new = self.nodes[0].name_new ("name")
    self.generate (self.nodes[0], 10)
    self.firstupdateName (0, "name", new, "value", {"dest_address": p2sh})
    self.generate (self.nodes[0], 5)
    self.sync_blocks ()
    data = self.checkName (0, "name", "value", None, False)
    assert_equal (data['address'], p2sh)

    # Straight-forward name updating should fail (for both nodes).
    assert_raises_rpc_error (-6, None,
                             self.nodes[0].name_update, "name", "new value")
    assert_raises_rpc_error (-6, None,
                             self.nodes[1].name_update, "name", "new value")

    # Find some other input to add as fee.
    unspents = self.nodes[0].listunspent ()
    assert len (unspents) > 0
    feeInput = unspents[0]
    changeAddr = self.nodes[0].getnewaddress ()
    nameAmount = Decimal ("0.01")
    changeAmount = feeInput['amount'] - nameAmount

    # Construct the name update as raw transaction.
    addr = self.nodes[1].getnewaddress ()
    inputs = [{"txid": data['txid'], "vout": data['vout']}, feeInput]
    outputs = {changeAddr: changeAmount, addr: nameAmount}
    txRaw = self.nodes[0].createrawtransaction (inputs, outputs)
    op = {"op": "name_update", "name": "name", "value": "it worked"}
    nameInd = self.rawtxOutputIndex (0, txRaw, addr)
    txRaw = self.nodes[0].namerawtransaction (txRaw, nameInd, op)

    # Convert to PSBT and process it with the watch-only wallet to add
    # data about the script.
    psbt = self.nodes[0].converttopsbt (txRaw['hex'])
    psbt = multisigRpc.walletprocesspsbt (psbt)

    # Sign it partially.
    psbt = self.nodes[0].walletprocesspsbt (psbt['psbt'])
    assert not psbt['complete']

    # Sign it fully.
    psbt = self.nodes[1].walletprocesspsbt (psbt['psbt'])
    assert psbt['complete']
    tx = self.nodes[1].finalizepsbt (psbt['psbt'])
    assert tx['complete']
    tx = tx['hex']

    # Manipulate the signature to invalidate it.  This checks whether or
    # not the OP_MULTISIG is actually verified (vs just the script hash
    # compared to the redeem script).
    txData = bytearray (bytes.fromhex (tx))
    txData[44] = (txData[44] + 10) % 256
    txManipulated = txData.hex ()

    # Send the tx.  The manipulation should be caught (independently of
    # when strict P2SH checks are enabled, since they are enforced
    # mandatorily in the mempool).
    assert_raises_rpc_error (-26, None,
                             self.nodes[0].sendrawtransaction, txManipulated)
    self.nodes[0].sendrawtransaction (tx)
    self.generate (self.nodes[0], 1)
    self.sync_blocks ()

    # Check that it was transferred correctly.
    self.checkName (1, "name", "it worked", None, False)
    self.nodes[1].name_update ("name", "changed")
    self.generate (self.nodes[1], 1)
    self.checkName (1, "name", "changed", None, False)

  def test_namescript_p2sh (self):
    """
    Tests how name prefixes interact with P2SH outputs and redeem scripts.
    """

    self.log.info ("Testing name prefix and P2SH interactions...")

    # This test only needs a single node and no syncing.
    node = self.nodes[0]

    name = "d/p2sh"
    value = "value"
    new = node.name_new (name)
    self.generate (node, 12)
    self.firstupdateName (0, name, new, value)
    self.generate (node, 1)
    baseHeight = node.getblockcount ()
    self.checkNameWithHeight (0, name, value, baseHeight)

    # Prepare some scripts and P2SH addresses we use later.  We build the
    # name script prefix for an update to our testname, so that we can build
    # P2SH redeem scripts with (or without) it.

    nameBytes = codecs.encode (name, 'ascii')
    valueBytes = codecs.encode (value, 'ascii')
    updOps = [OP_NAME_UPDATE, nameBytes, valueBytes, OP_2DROP, OP_DROP]
    anyoneOps = [OP_TRUE]

    updScript = CScript (updOps)
    anyoneScript = CScript (anyoneOps)
    updAndAnyoneScript = CScript (updOps + anyoneOps)

    anyoneAddr = self.getP2SH (0, anyoneScript)
    updAndAnyoneAddr = self.getP2SH (0, updAndAnyoneScript)

    # Send the name to the anyone-can-spend name-update script directly.
    # This is expected to update the name (verifies the update script is good).

    tx = CTransaction ()
    tx.version = NAMECOIN_TX_VERSION
    data = node.name_show (name)
    tx.vin.append (CTxIn (COutPoint (int (data['txid'], 16), data['vout'])))
    tx.vout.append (CTxOut (COIN // 100, updAndAnyoneScript))
    txHex = tx.serialize ().hex ()

    txHex = node.fundrawtransaction (txHex)['hex']
    signed = node.signrawtransactionwithwallet (txHex)
    assert signed['complete']
    node.sendrawtransaction (signed['hex'])

    self.generate (node, 1)
    self.checkNameWithHeight (0, name, value, baseHeight + 1)

    # Send the name to the anyone-can-spend P2SH address.  This should just
    # work fine and update the name.
    self.updateAnyoneCanSpendName (0, name, "value2", anyoneAddr, [])
    self.generate (node, 1)
    self.checkNameWithHeight (0, name, "value2", baseHeight + 2)

    # Send a coin to the P2SH address with name prefix.  This should just
    # work fine but not update the name.  We should be able to spend the coin
    # again from that address.

    txid = node.sendtoaddress (updAndAnyoneAddr, 2)
    tx = node.getrawtransaction (txid)
    ind = self.rawtxOutputIndex (0, tx, updAndAnyoneAddr)
    self.generate (node, 1)

    ins = [{"txid": txid, "vout": ind}]
    addr = node.getnewaddress ()
    out = {addr: 1}
    tx = node.createrawtransaction (ins, out)
    tx = self.setScriptSigOps (tx, 0, [updAndAnyoneScript])

    node.sendrawtransaction (tx, 0)
    self.generate (node, 1)
    self.checkNameWithHeight (0, name, "value2", baseHeight + 2)

    found = False
    for u in node.listunspent ():
      if u['address'] == addr and u['amount'] == 1:
        found = True
        break
    if not found:
      raise AssertionError ("Coin not sent to expected address")

    # Send the name to the P2SH address with name prefix and then spend it
    # again.  Spending should work fine, and the name should just be updated
    # ordinarily; the name prefix of the redeem script should have no effect.
    self.updateAnyoneCanSpendName (0, name, "value3", updAndAnyoneAddr,
                                   [anyoneScript])
    self.generate (node, 1)
    self.checkNameWithHeight (0, name, "value3", baseHeight + 5)
    self.updateAnyoneCanSpendName (0, name, "value4", anyoneAddr,
                                   [updAndAnyoneScript])
    self.generate (node, 1)
    self.checkNameWithHeight (0, name, "value4", baseHeight + 6)

  def run_test (self):
    if not self.options.activated:
      self.log.info ("Disabling BIP16 for the test")
      self.node_args.append ("-testactivationheight=bip16@1000000")
      for i in range (3):
        self.restart_node (i, extra_args=self.node_args)
      self.connect_nodes (0, 1)
      self.connect_nodes (0, 2)
      self.connect_nodes (1, 2)

    assert_equal (softfork_active (self.nodes[0], "bip16"),
                  self.options.activated)

    self.test_2of2_multisig ()
    self.test_namescript_p2sh ()


if __name__ == '__main__':
  NameMultisigTest (__file__).main ()
