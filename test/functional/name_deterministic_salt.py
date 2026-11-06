#!/usr/bin/env python3
# Licensed under CC0 (Public domain)

# Test that name_new and name_firstupdate use deterministic salts, derived
# from a private key of the wallet for the name address, and random ones if
# the wallet has none.

import re
from decimal import Decimal

from test_framework.address import base58_to_byte
from test_framework.crypto.hkdf import hkdf_sha256
from test_framework.descriptors import descsum_create
from test_framework.extendedkey import ExtendedPrivateKey
from test_framework.names import NameTestFramework
from test_framework.util import (
    assert_equal,
    assert_not_equal,
    assert_raises_rpc_error,
)

# Keys and expected salts for the name "d/wikileaks", the same vectors as in
# src/wallet/test/rpcnames_tests.cpp.  Each key is imported as a single-key
# descriptor of the address type in its label.
SALT_VECTORS = [
    {
        "label": "p2pkh",
        "desc": "pkh({})",
        "privkey": "cQDxbmQfwRV3vP1mdnVHq37nJekHLsuD3wdSQseBRA2ct4MFk5Pq",
        "salt": "c33f6d84c93d769da2a8882ed9d4a69e2052dd9a",
    },
    {
        "label": "p2wpkh-p2sh",
        "desc": "sh(wpkh({}))",
        "privkey": "cU9hVzhpvfn91u2zTVn8uqF2ymS7ucYH8V5TmsTDmuyMHgRk9WsJ",
        "salt": "b14763659b268460865db01b2b10b80a9cbe9ceb",
    },
    {
        "label": "p2wpkh",
        "desc": "wpkh({})",
        "privkey": "cPuQzcNEgbeYZ5at9VdGkCwkPA9r34gvEVJjuoz384rTfYpahfe7",
        "salt": "a39038cddfcc17d391b8620639a72178dc73b19a",
    },
]
VECTOR_NAME = "d/wikileaks"

# The seeds of the extended keys of two co-signers of multisig addresses.
MULTISIG_SEEDS = [b"name salt co-signer a", b"name salt co-signer b"]


def name_salt(ikm, name):
    """The salt that the wallet derives for name from the private key ikm."""
    return hkdf_sha256(32, ikm, name.encode("ascii"), b"Namecoin Registration Salt")[:20].hex()


def public_salt(name):
    """The salt derived from an empty key.  Anyone can compute it from the
    name, so it must not be used as rand."""
    return name_salt(b"", name)


class NameDeterministicSaltTest(NameTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.setup_name_test ([[]])

    def import_vector_keys(self):
        """Import the vector keys into the (descriptor) wallet and return
        their addresses as a dict keyed by label.  The descriptors are not
        ranged, so they are never active; the keys are only used through
        destAddress."""
        node = self.nodes[0]

        requests = []
        for v in SALT_VECTORS:
            requests.append({
                "desc": descsum_create(v["desc"].format(v["privkey"])),
                "timestamp": "now",
            })
        for res in node.importdescriptors(requests):
            assert_equal(res["success"], True)

        addresses = {}
        for v in SALT_VECTORS:
            pubDesc = node.getdescriptorinfo(v["desc"].format(v["privkey"]))["descriptor"]
            [addr] = node.deriveaddresses(pubDesc)
            assert_equal(node.getaddressinfo(addr)["ismine"], True)
            addresses[v["label"]] = addr

        return addresses

    def run_test(self):
        node = self.nodes[0]
        assert(node.listwallets() != [])
        assert_equal(node.getwalletinfo()["descriptors"], True)

        addresses = self.import_vector_keys()
        vectorSalt = {v["label"]: v["salt"] for v in SALT_VECTORS}
        self.generate (node, 200)

        self.log.info("Salts of name_new match the known vectors.")
        vectorNew = {}
        for label, addr in addresses.items():
            new = node.name_new(VECTOR_NAME, {"destAddress": addr})
            assert_equal(new[1], vectorSalt[label])
            vectorNew[label] = new

        self.log.info("Begin registration of more names.")
        # Names registered to the imported keys, one per address type.
        typedNew = {}
        for label, addr in addresses.items():
            name = "d/salt-" + label
            typedNew[name] = node.name_new(name, {"destAddress": addr})
        # A name at a fresh address of the wallet's active descriptors
        # (the usual case).
        defaultNew = node.name_new("d/default")
        self.generate (node, 12)

        self.log.info("Leave both rand and txid blank (null).")
        for name in list(typedNew) + ["d/default"]:
            node.name_firstupdate (name)
        self.generate (node, 1)
        for name in list(typedNew) + ["d/default"]:
            self.checkName(0, name, "", 30, False)
        # The wallet found the right name_new for each name and derived
        # the rand that name_new used.
        for name, new in typedNew.items():
            self.assertFirstupdate(name, new[0], new[1])
        self.assertFirstupdate("d/default", defaultNew[0], defaultNew[1])
        self.log.info("Names registered; no value provided.")

        self.log.info("Now let's register a name and give it the TXID, but leave it to figure out the rand value.")

        # There are three name_new outputs for VECTOR_NAME in the wallet,
        # so pass the txid of one of them (the P2WPKH one).
        txid = vectorNew["p2wpkh"][0]
        node.name_firstupdate (VECTOR_NAME, None, txid)
        self.generate (node, 1)
        self.checkName(0, VECTOR_NAME, "", 30, False)
        self.assertFirstupdate(VECTOR_NAME, txid, vectorSalt["p2wpkh"])

        new_name = node.name_new('d/name2')
        self.generate (node, 12)
        node.name_firstupdate ("d/name2", None, new_name[0])
        self.generate (node, 1)
        self.checkName(0, "d/name2", "", 30, False)
        self.assertFirstupdate("d/name2", new_name[0], new_name[1])

        self.log.info("Now let's register a name and give it the rand value, but leave it to figure out the txid.")

        new_name = node.name_new('d/name3')
        self.generate (node, 12)
        node.name_firstupdate ("d/name3", new_name[1])
        self.generate (node, 1)
        self.checkName(0, "d/name3", "", 30, False)
        self.assertFirstupdate("d/name3", new_name[0], new_name[1])

        self.log.info("Neither gave any exceptions.")

        self.log.info("Now let's register a name and give it a wrong (but existing) TXID.")

        node.name_new('d/name4')
        new_name2 = node.name_new('d/dummy')
        self.generate (node, 12)
        assert_raises_rpc_error(-25, "generated rand for txid does not match", node.name_firstupdate, "d/name4", None, new_name2[0])

        self.log.info("Gave exception, as expected.")

        self.log.info("Now let's give it the TXID of a name operation that is not a name_new.")
        # The name output of the name_firstupdate of d/name2 is unspent.
        firstTxid = node.name_show("d/name2")["txid"]
        for rand in [None, "00" * 20]:
            assert_raises_rpc_error(-25, "previous tx is not name_new", node.name_firstupdate, "d/name5", rand, firstTxid)

        self.log.info("A name_new to an address of another wallet is not matched.")
        node.createwallet("other")
        foreignAddr = node.get_wallet_rpc("other").getnewaddress()
        node.unloadwallet("other")
        assert_equal(node.getaddressinfo(foreignAddr)["ismine"], False)
        foreignNew = node.name_new("d/foreign", {"destAddress": foreignAddr})
        self.generate (node, 12)
        assert_raises_rpc_error(-25, "scan for previous txid failed", node.name_firstupdate, "d/foreign")
        assert_raises_rpc_error(-25, "could not generate rand for txid", node.name_firstupdate, "d/foreign", None, foreignNew[0])

        self.test_multisig_wallet()

        self.log.info("Register expired names again, without rand and txid.")
        # The wallet keeps the spent name_new of the first registration,
        # which matches the name as well.  After a restart the scan sees the
        # wallet transactions in a different order, so use several names.
        names = ["d/again-%d" % i for i in range(10)]
        for name in names:
            node.name_new(name)
        self.generate (node, 12)
        for name in names:
            node.name_firstupdate (name)
        self.generate (node, 1)
        self.generate (node, 30)
        for name in names:
            assert_equal(node.name_show(name, {"allowExpired": True})["expired"], True)
        secondNew = {}
        for name in names:
            secondNew[name] = node.name_new(name)
        self.generate (node, 12)
        self.restart_node(0)
        for name in names:
            node.name_firstupdate (name)
        self.generate (node, 1)
        for name in names:
            self.checkName(0, name, "", 30, False)
            self.assertFirstupdate(name, secondNew[name][0], secondNew[name][1])

        self.log.info("Deriving rand needs an unlocked wallet.")
        node.encryptwallet("passphrase")
        node.walletpassphrase("passphrase", 600)
        lockedNew = node.name_new("d/locked")
        self.generate (node, 12)
        node.walletlock()
        assert_raises_rpc_error(-13, "walletpassphrase", node.name_firstupdate, "d/locked")
        assert_raises_rpc_error(-13, "walletpassphrase", node.name_firstupdate, "d/locked", None, lockedNew[0])
        node.walletpassphrase("passphrase", 600)
        node.name_firstupdate ("d/locked")
        self.generate (node, 1)
        self.checkName(0, "d/locked", "", 30, False)
        self.assertFirstupdate("d/locked", lockedNew[0], lockedNew[1])

    def test_multisig_wallet(self):
        """Two wallets of the co-signers of 1-of-2 multisig addresses, each
        with its own private key and the public key of the other.  The one
        that sends name_new derives rand from its key and finishes the
        registration without it; the other needs the rand."""
        node = self.nodes[0]

        self.log.info("name_new of a multisig wallet derives rand from the wallet's key.")
        # name_salt is the function the wallet uses.
        for v in SALT_VECTORS:
            key = base58_to_byte(v["privkey"])[0][:32]
            assert_equal(name_salt(key, VECTOR_NAME), v["salt"])

        xprvs = [ExtendedPrivateKey.from_seed(seed) for seed in MULTISIG_SEEDS]
        a, b = [k.to_string() for k in xprvs]
        A, B = [k.pubkey().to_string() for k in xprvs]
        node.createwallet(wallet_name="cosigner-a", blank=True)
        node.createwallet(wallet_name="cosigner-b", blank=True)
        walletA = node.get_wallet_rpc("cosigner-a")
        walletB = node.get_wallet_rpc("cosigner-b")
        # The same addresses in both wallets: the keys are given in the same
        # order, private in the own wallet and public in the other one.
        for rpc, keyA, keyB in [(walletA, a, B), (walletB, A, b)]:
            requests = []
            for desc, internal in [(f"sh(multi(1,{keyA}/0/*,{keyB}/0/*))", False),
                                   (f"sh(multi(1,{keyA}/1/*,{keyB}/1/*))", True),
                                   (f"wsh(multi(1,{keyA}/2/*,{keyB}/2/*))", False)]:
                requests.append({
                    "desc": descsum_create(desc),
                    "timestamp": "now",
                    "active": True,
                    "internal": internal,
                })
            for res in rpc.importdescriptors(requests):
                assert_equal(res["success"], True)
        mainWallet = node.get_wallet_rpc(self.default_wallet_name)
        mainWallet.sendtoaddress(walletA.getnewaddress(), 10)
        self.generate (node, 1)
        assert_equal(walletA.getbalance(), walletB.getbalance())

        # The default address type is legacy, i.e. the P2SH multisig
        # descriptor.  The P2WSH one is given as destAddress.
        destAddress = {
            "d/multisig-p2sh": None,
            "d/multisig-p2wsh": walletA.getnewaddress("", "bech32"),
        }
        pending = {}
        for name, addr in destAddress.items():
            opt = {} if addr is None else {"destAddress": addr}
            new = walletA.name_new(name, opt)
            tx = walletA.gettransaction(txid=new[0], verbose=True)["decoded"]
            [nameOut] = [out for out in tx["vout"] if "nameOp" in out["scriptPubKey"]]
            nameAddr = nameOut["scriptPubKey"]["address"]
            info = walletA.getaddressinfo(nameAddr)
            assert_equal(info["ismine"], True)
            assert_equal(info["script"], "multisig")
            # P2SH for the default address, P2WSH for the bech32 one.
            assert_equal(info["isscript"], True)
            assert_equal(info["iswitness"], addr is not None)
            # The rand is the salt of A's key for the address.  The key
            # origins in the descriptor of the address give its path.
            paths = re.findall(r"\[[0-9a-f]{8}(/[0-9]+/[0-9]+)\]", info["desc"])
            assert_equal(len(paths), 2)
            assert_equal(paths[0], paths[1])
            keyA = xprvs[0].derive_path("m" + paths[0]).key.get_bytes()
            assert_equal(new[1], name_salt(keyA, name))
            assert_not_equal(new[1], public_salt(name))
            # B owns the address as well, but holds the other key.
            assert_equal(walletB.getaddressinfo(nameAddr)["ismine"], True)
            pending[name] = new
        self.generate (node, 12)

        self.log.info("The sending wallet finishes without rand, the co-signer cannot.")
        for name, new in pending.items():
            assert_raises_rpc_error(-25, "scan for previous txid failed", walletB.name_firstupdate, name)
            assert_raises_rpc_error(-25, "generated rand for txid does not match", walletB.name_firstupdate, name, None, new[0])
        # One name through the scan, the other one with the txid given.
        walletA.name_firstupdate("d/multisig-p2sh")
        walletA.name_firstupdate("d/multisig-p2wsh", None, pending["d/multisig-p2wsh"][0])
        self.generate (node, 1)
        for name, new in pending.items():
            self.checkName(0, name, "", 30, False)
            self.assertFirstupdate(name, new[0], new[1], walletA)

        self.log.info("The co-signer finishes with the rand that name_new returned.")
        new = walletA.name_new("d/multisig-cosigner")
        self.generate (node, 12)
        walletB.name_firstupdate("d/multisig-cosigner", new[1], new[0])
        self.generate (node, 1)
        self.checkName(0, "d/multisig-cosigner", "", 30, False)
        self.assertFirstupdate("d/multisig-cosigner", new[0], new[1], walletB)

        self.log.info("Registrations started with the public salt can be finished.")
        # Earlier versions of name_new used the public salt as rand for such
        # addresses.  Send such name_new outputs with raw transactions, two
        # to the wallet and one to an address of another wallet.
        publicNew = {}
        for name in ["d/public-scan", "d/public-txid"]:
            publicNew[name] = self.sendRawNameNew(walletA, name, walletA.getnewaddress(), public_salt(name))
        foreignTxid = self.sendRawNameNew(walletA, "d/public-foreign", mainWallet.getnewaddress(), public_salt("d/public-foreign"))
        self.generate (node, 12)
        walletA.name_firstupdate("d/public-scan")
        walletA.name_firstupdate("d/public-txid", None, publicNew["d/public-txid"])
        # The public salt is only accepted for outputs of the wallet.
        assert_raises_rpc_error(-25, "scan for previous txid failed", walletA.name_firstupdate, "d/public-foreign")
        assert_raises_rpc_error(-25, "could not generate rand for txid", walletA.name_firstupdate, "d/public-foreign", None, foreignTxid)
        self.generate (node, 1)
        for name, txid in publicNew.items():
            self.checkName(0, name, "", 30, False)
            self.assertFirstupdate(name, txid, public_salt(name), walletA)

        node.unloadwallet("cosigner-a")
        node.unloadwallet("cosigner-b")

    def sendRawNameNew(self, rpc, name, addr, rand):
        """Send a name_new of name with the given rand to addr in a raw
        transaction funded by the wallet of rpc.  Returns the txid."""
        tx = rpc.createrawtransaction([], {addr: Decimal("0.01")})
        nameTx = rpc.namerawtransaction(tx, 0, {"op": "name_new", "name": name, "rand": rand})
        assert_equal(nameTx["rand"], rand)
        tx = rpc.fundrawtransaction(nameTx["hex"])["hex"]
        signed = rpc.signrawtransactionwithwallet(tx)
        assert_equal(signed["complete"], True)
        return rpc.sendrawtransaction(signed["hex"])

    def assertFirstupdate(self, name, newTxid, rand, rpc=None):
        """Check that the confirmed name_firstupdate of name spends the
        name_new output of the transaction newTxid and reveals rand.  The
        transaction is looked up with rpc (default: the wallet of node 0)."""
        node = self.nodes[0]
        if rpc is None:
            rpc = node
        data = node.name_show(name)
        tx = rpc.gettransaction(txid=data["txid"], verbose=True)["decoded"]
        assert newTxid in [txin["txid"] for txin in tx["vin"]]
        nameOp = tx["vout"][data["vout"]]["scriptPubKey"]["nameOp"]
        assert_equal(nameOp["op"], "name_firstupdate")
        assert_equal(nameOp["rand"], rand)


if __name__ == '__main__':
    NameDeterministicSaltTest(__file__).main()
