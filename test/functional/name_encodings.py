#!/usr/bin/env python3
# Copyright (c) 2018 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Tests the name/value encoding options in the RPC interface.

from test_framework.names import NameTestFramework
from test_framework.util import *

from decimal import Decimal


def strToHex (string):
  return bytes_to_hex_str (string.encode ('latin1'))


class NameEncodingsTest (NameTestFramework):

  # A basic name that is valid as ASCII and used for tests with differently
  # encoded values.
  name = "d/test"

  # A basic value that is valid as ASCII and used for tests where the
  # name is encoded.
  value = "test-value"

  # Running counter.  This is used to compute a suffix for registered names
  # to make sure they do not clash with names of previous tests.
  nameCounter = 0

  # Keep track of the currently set encodings.
  nameEncoding = 'ascii'
  valueEncoding = 'ascii'

  def set_test_params (self):
    # We need -namehistory for the test, so use node 1.  Thus we have to
    # have two nodes here.
    self.setup_name_test ([[]] * 2)

  def setEncodings (self, nameEnc='ascii', valueEnc='ascii'):
    args = ["-nameencoding=%s" % nameEnc, "-valueencoding=%s" % valueEnc]
    self.restart_node (1, extra_args=args)

    self.nameEncoding = nameEnc
    self.valueEncoding = valueEnc

  def uniqueName (self, baseName, enc='ascii'):
    """
    Uses the running nameCounter to change the baseName into a unique
    name by adding some suffix (depending on the encoding).  The suffix
    will be valid for the encoding, so that a valid name stays valid and
    an invalid name is still invalid (due to the baseName itself).
    """

    suff = "%09d" % self.nameCounter
    if enc == 'hex':
      suff = strToHex (suff)

    self.nameCounter += 1

    return baseName + suff

  def nameRawTx (self, nameInp, nameOp):
    """
    Constructs, signs and sends a raw transaction using namerawtransaction
    with the given (if any) previous name input.

    Returned are the result of namerawtransaction (for name_new), to which the
    final txid and name vout are added.
    """

    ins = []
    if nameInp is not None:
      ins.append ({"txid": nameInp['txid'], "vout": nameInp['vout']})
    addr = self.node.getnewaddress ()
    out = {addr: Decimal ('0.01')}

    rawTx = self.node.createrawtransaction (ins, out)
    nameOpData = self.node.namerawtransaction (rawTx, 0, nameOp)

    rawTx = self.node.fundrawtransaction (nameOpData['hex'])['hex']
    vout = self.rawtxOutputIndex (1, rawTx, addr)
    assert vout is not None

    signed = self.node.signrawtransactionwithwallet (rawTx)
    assert signed['complete']
    txid = self.node.sendrawtransaction (signed['hex'])

    nameOpData['txid'] = txid
    nameOpData['vout'] = vout
    return nameOpData

  def verifyAndMinePendingUpdate (self, name, value, txid):
    """
    Verifies that there is a pending transaction that (first)updates
    name to value with the given txid.  The tx is mined as well, and then
    all the read-only RPCs are checked for it (name_show, name_list, ...).
    """

    data = self.node.name_pending (name)
    assert_equal (len (data), 1)
    assert_equal (data[0]['name_encoding'], self.nameEncoding)
    assert_equal (data[0]['name'], name)
    assert_equal (data[0]['value_encoding'], self.valueEncoding)
    assert_equal (data[0]['value'], value)
    assert_equal (data[0]['txid'], txid)

    self.node.generate (1)
    data = self.node.name_show (name)
    assert_equal (data['name_encoding'], self.nameEncoding)
    assert_equal (data['name'], name)
    assert_equal (data['value_encoding'], self.valueEncoding)
    assert_equal (data['value'], value)
    assert_equal (data['txid'], txid)

    assert_equal (self.node.name_history (name)[-1], data)
    assert_equal (self.node.name_scan (name, 1)[0], data)
    assert_equal (self.node.name_list (name)[0], data)

    found = False
    for d in self.node.name_filter ():
      if d['txid'] == txid:
        assert_equal (d, data)
        assert not found
        found = True
    assert found

  def validName (self, baseName, encoding):
    """
    Runs tests asserting that the given string is valid as name in the
    given encoding.
    """

    self.setEncodings (nameEnc=encoding)

    name = self.uniqueName (baseName, encoding)
    new = self.node.name_new (name)
    self.node.generate (1)
    txid = self.firstupdateName (1, name, new, self.value)
    self.node.generate (11)
    self.verifyAndMinePendingUpdate (name, self.value, txid)

    txid = self.node.name_update (name, self.value)
    self.verifyAndMinePendingUpdate (name, self.value, txid)

    self.node.sendtoname (name, 1)

    # Redo the whole life-cycle now also with raw transactions (with a new
    # unique name).
    name = self.uniqueName (baseName, encoding)

    new = self.nameRawTx (None, {
      "op": "name_new",
      "name": name,
    })
    self.node.generate (1)
    first = self.nameRawTx (new, {
      "op": "name_firstupdate",
      "name": name,
      "value": self.value,
      "rand": new['rand'],
    })
    self.node.generate (11)
    self.verifyAndMinePendingUpdate (name, self.value, first['txid'])

    upd = self.nameRawTx (first, {
      "op": "name_update",
      "name": name,
      "value": self.value,
    })
    self.verifyAndMinePendingUpdate (name, self.value, upd['txid'])

  def validValue (self, value, encoding):
    """
    Runs tests asserting that the given string is valid as value in the
    given encoding.
    """

    self.setEncodings (valueEnc=encoding)

    name = self.uniqueName (self.name, encoding)
    new = self.node.name_new (name)
    self.node.generate (1)
    txid = self.firstupdateName (1, name, new, value)
    self.node.generate (11)
    self.verifyAndMinePendingUpdate (name, value, txid)

    txid = self.node.name_update (name, value)
    self.verifyAndMinePendingUpdate (name, value, txid)

    # Redo the whole life-cycle now also with raw transactions (with a new
    # unique name).
    name = self.uniqueName (self.name, encoding)

    new = self.nameRawTx (None, {
      "op": "name_new",
      "name": name,
    })
    self.node.generate (1)
    first = self.nameRawTx (new, {
      "op": "name_firstupdate",
      "name": name,
      "value": value,
      "rand": new['rand'],
    })
    self.node.generate (11)
    self.verifyAndMinePendingUpdate (name, value, first['txid'])

    upd = self.nameRawTx (first, {
      "op": "name_update",
      "name": name,
      "value": value,
    })
    self.verifyAndMinePendingUpdate (name, value, upd['txid'])

  def invalidName (self, name, encoding):
    """
    Runs tests to check that the various RPC methods treat the given name
    as invalid in the encoding.
    """

    self.setEncodings (nameEnc=encoding)

    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_new, name)
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_firstupdate,
                             name, "00", 32 * "00", self.value)
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_update, name, self.value)

    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_pending, name)
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_show, name)
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_history, name)
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_scan, name)
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_list, name)

    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.sendtoname, name, 1)

    nameNew = {
      "op": "name_new",
      "name": name,
    }
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.nameRawTx, None, nameNew)
    nameFirst = {
      "op": "name_firstupdate",
      "name": name,
      "value": self.value,
      "rand": "00",
    }
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.nameRawTx, None, nameFirst)
    nameUpd = {
      "op": "name_update",
      "name": name,
      "value": self.value,
    }
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.nameRawTx, None, nameUpd)

  def invalidValue (self, value, encoding):
    """
    Runs tests to check that the various RPC methods treat the given value
    as invalid in the encoding.
    """

    self.setEncodings (valueEnc=encoding)

    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_firstupdate,
                             self.name, "00", 32 * "00", value)
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_update, self.name, value)

    nameFirst = {
      "op": "name_firstupdate",
      "name": self.name,
      "value": value,
      "rand": "00",
    }
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.nameRawTx, None, nameFirst)
    nameUpd = {
      "op": "name_update",
      "name": self.name,
      "value": value,
    }
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.nameRawTx, None, nameUpd)

  def run_test (self):
    self.node = self.nodes[1]

    # Note:  The tests here are mainly important to verify that strings
    # are encoded/decoded at all.  The different possibilities for valid
    # and invalid strings in detail are tested already in the unit test.
    # But since we have the utility methods available, we might just as well
    # call them with different values instead of just once or twice.

    self.log.info ("Testing valid names...")
    self.validName ("d/abc", "ascii")
    self.validName ("d/\x00äöü\n", "utf8")
    self.validName ("0011ff", "hex")
    self.validName ("", "hex")

    self.log.info ("Testing valid values...")
    self.validValue ('{"foo":"bar"}', "ascii")
    self.validValue ('{"foo":"\x00äöü\n"}', "utf8")
    self.validValue (strToHex ('{"foo":"\x00\xff"}'), "hex")
    self.validValue ('', "hex")

    self.log.info ("Testing invalid names...")
    self.invalidName ("d/\n", "ascii")
    self.invalidName ("d/äöü", "ascii")
    self.invalidName ("d/\xff", "ascii")
    # We cannot actually test invalid UTF-8 on the "input side", because a
    # string with arbitrary bytes gets UTF-8 encoded when sending to JSON RPC.
    self.invalidName ("d", "hex")
    self.invalidName ("xx", "hex")

    self.log.info ("Testing invalid values...")
    self.invalidValue ('{"foo":"\n"}', "ascii")
    self.invalidValue ('{"foo":"äöü"}', "ascii")
    self.invalidValue ('{"foo":"\xff"}', "ascii")
    self.invalidValue ('d', "hex")
    self.invalidValue ('xx', "hex")

    self.test_outputSide ()
    self.test_walletTx ()
    self.test_rpcOption ()

  def test_outputSide (self):
    """
    Tests encodings purely on the output-side.  This test registers some
    names/values using hex encoding, and then verifies how those names look like
    when queried with another encoding.
    """

    self.log.info ("Different encodings on the output side...")

    nameAscii = self.uniqueName ("d/test", "ascii")
    valueAscii = "{}"
    nameHex = self.uniqueName ("0011ff", "hex")
    valueHex = strToHex ('{"msg":"\x00\xff"}')

    self.setEncodings (nameEnc="hex", valueEnc="hex")
    newAscii = self.node.name_new (strToHex (nameAscii))
    newHex = self.node.name_new (nameHex)
    self.node.generate (10)
    txidAscii = self.firstupdateName (1, strToHex (nameAscii), newAscii,
                                      valueHex)
    txidHex = self.firstupdateName (1, nameHex, newHex, strToHex (valueAscii))
    self.node.generate (5)

    # Test name_show with ASCII encoding.
    self.setEncodings (nameEnc="ascii", valueEnc="ascii")
    data = self.node.name_show (nameAscii)
    assert_equal (data['name_encoding'], 'ascii')
    assert_equal (data['name'], nameAscii)
    assert 'name_error' not in data
    assert_equal (data['value_encoding'], 'ascii')
    assert 'value' not in data
    assert_equal (data['value_error'], 'invalid data for ascii')
    assert_equal (data['txid'], txidAscii)

    # Test the non-ASCII name in name_scan output.
    found = False
    for data in self.node.name_scan ():
      if data['txid'] != txidHex:
        continue
      assert not found
      found = True
      assert_equal (data['name_encoding'], 'ascii')
      assert 'name' not in data
      assert_equal (data['name_error'], 'invalid data for ascii')
      assert_equal (data['value_encoding'], 'ascii')
      assert_equal (data['value'], valueAscii)
      assert 'value_error' not in data
    assert found

    # Test script and transaction decoding.
    txAscii = self.node.getrawtransaction (txidAscii, 1)
    found = False
    for out in txAscii['vout']:
      if not 'nameOp' in out['scriptPubKey']:
        continue

      assert not found
      found = True

      op = out['scriptPubKey']['nameOp']
      assert_equal (op['op'], 'name_firstupdate')
      assert_equal (op['name_encoding'], 'ascii')
      assert_equal (op['name'], nameAscii)
      assert 'name_error' not in op
      assert_equal (op['value_encoding'], 'ascii')
      assert 'value' not in op
      assert_equal (op['value_error'], 'invalid data for ascii')

      script = self.node.decodescript (out['scriptPubKey']['hex'])
      assert_equal (script['nameOp'], op)
    assert found

  def testNameForWalletTx (self, baseName, enc, msgFmt):
    """
    Registers a name and then verifies that the value returned from
    gettransaction / listtransactions as "name operation" matches
    what we expect.

    The expected msgFmt is a format string where the actually used name
    is filled in for %s.
    """

    self.setEncodings (nameEnc=enc)
    name = self.uniqueName (baseName, enc)
    updMsg = msgFmt % name

    new = self.node.name_new (name)
    self.node.generate (12)
    txid = self.firstupdateName (1, name, new, self.value)
    self.node.generate (1)

    data = self.node.gettransaction (txid)
    assert_equal (len (data['details']), 1)
    assert_equal (data['details'][0]['name'], "update: %s" % updMsg)

    found = False
    for tx in self.node.listtransactions ():
      if tx['txid'] != txid:
        continue
      assert not found
      found = True
      assert_equal (tx['name'], "update: %s" % updMsg)
    assert found

  def test_walletTx (self):
    """
    Tests the name displayed in listtransactions / gettransaction in the
    wallet when it is a name update.
    """

    self.log.info ("Testing name update in wallet...")
    self.testNameForWalletTx ("d/test", "ascii", "'%s'")
    self.testNameForWalletTx ("00ff", "hex", "0x%s")

  def readRpcOption (self, nameAscii, nameUtf8, valueAscii, valueUtf8):
    """
    Tests overriding the name/value encoding through the "options" RPC
    argument for "read" methods.

    This is not a "standalone test" but rather called from test_rpcOption.
    This allows us to reuse the registered names/values from there.
    """

    self.log.info ("Testing options-override for read RPCs...")
    self.setEncodings ()

    # Type check for the encoding options.
    assert_raises_rpc_error (-3, "Expected type string",
                             self.node.name_scan, nameAscii, 1,
                             {"nameEncoding": 42})
    assert_raises_rpc_error (-3, "Expected type string",
                             self.node.name_scan, nameAscii, 1,
                             {"valueEncoding": 42})

    # name_scan
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_scan, nameUtf8, 1)

    res = self.node.name_scan (nameUtf8, 1, {"nameEncoding": "utf8"})
    assert_equal (len (res), 1)
    res = res[0]
    assert_equal (res['name_encoding'], 'utf8')
    assert_equal (res['name'], nameUtf8)
    assert_equal (res['value_encoding'], 'ascii')
    assert_equal (res['value'], valueAscii)

    res = self.node.name_scan (nameAscii, 1)
    assert_equal (len (res), 1)
    res = res[0]
    assert_equal (res['name_encoding'], 'ascii')
    assert_equal (res['name'], nameAscii)
    assert_equal (res['value_encoding'], 'ascii')
    assert 'value' not in res
    assert_equal (res['value_error'], 'invalid data for ascii')

    res = self.node.name_scan (nameAscii, 1, {"valueEncoding": "utf8"})
    assert_equal (len (res), 1)
    res = res[0]
    assert_equal (res['name_encoding'], 'ascii')
    assert_equal (res['name'], nameAscii)
    assert_equal (res['value_encoding'], 'utf8')
    assert_equal (res['value'], valueUtf8)

  def test_rpcOption (self):
    """
    Tests overriding the name/value encoding through the "options" RPC
    argument for "write" methods.

    The end of the function also calls a separate routine that verifies
    options overrides for "read" RPCs.
    """

    self.log.info ("Testing options-override for write RPCs...")

    # We set the default encodings to ASCII and then use a name/value
    # that is not valid in ASCII with overrides to utf8 for the test.
    self.setEncodings ()
    nameAscii = self.uniqueName ("d/abc", "ascii")
    nameUtf8 = self.uniqueName ("d/äöü", "utf8")
    valueAscii = "{}"
    valueUtf8 = '{"foo":"äöü"}'

    # Type check for the encoding options.
    assert_raises_rpc_error (-3, "Expected type string",
                             self.node.name_update, nameAscii, valueAscii,
                             {"nameEncoding": 42})
    assert_raises_rpc_error (-3, "Expected type string",
                             self.node.name_update, nameAscii, valueAscii,
                             {"valueEncoding": 42})

    # name_new both names, verify expected behaviour.
    newAscii = self.node.name_new (nameAscii)
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_new, nameUtf8)
    newUtf8 = self.node.name_new (nameUtf8, {"nameEncoding": "utf8"})
    self.node.generate (12)

    # firstupdate the names and verify expected behaviour.
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.firstupdateName,
                             1, nameAscii, newAscii, valueUtf8)
    self.firstupdateName (1, nameAscii, newAscii, valueUtf8,
                          {"valueEncoding": "utf8"})
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.firstupdateName,
                             1, nameUtf8, newUtf8, valueAscii)
    self.firstupdateName (1, nameUtf8, newUtf8, valueAscii,
                          {"nameEncoding": "utf8"})
    self.node.generate (1)

    # update the names and verify also that.
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_update, nameAscii, valueUtf8)
    self.node.name_update (nameAscii, valueUtf8, {"valueEncoding": "utf8"})
    assert_raises_rpc_error (-1000, "Name/value is invalid",
                             self.node.name_update, nameUtf8, valueAscii)
    self.node.name_update (nameUtf8, valueAscii, {"nameEncoding": "utf8"})
    self.node.generate (1)

    # Verify using name_show just to make sure all worked as expected and did
    # not silently just do something wrong.  For this, we change the configured
    # encodings so that we can retrieve utf8 names and values.
    self.setEncodings (nameEnc="utf8", valueEnc="utf8")
    data = self.node.name_show (nameAscii)
    assert_equal (data["value"], valueUtf8)
    data = self.node.name_show (nameUtf8)
    assert_equal (data["value"], valueAscii)

    # Call tests for read-only RPCs.
    self.readRpcOption (nameAscii, nameUtf8, valueAscii, valueUtf8)


if __name__ == '__main__':
  NameEncodingsTest ().main ()
