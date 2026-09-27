#!/usr/bin/env python3
# Licensed under CC0 (Public domain)

# Test that name_new and name_firstupdate use deterministic salts.

from test_framework.descriptors import descsum_create
from test_framework.names import NameTestFramework
from test_framework.util import (
    assert_equal,
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

        self.log.info("A name_new to an address of another wallet is not matched.")
        node.createwallet("other")
        foreignAddr = node.get_wallet_rpc("other").getnewaddress()
        node.unloadwallet("other")
        assert_equal(node.getaddressinfo(foreignAddr)["ismine"], False)
        foreignNew = node.name_new("d/foreign", {"destAddress": foreignAddr})
        self.generate (node, 12)
        assert_raises_rpc_error(-25, "scan for previous txid failed", node.name_firstupdate, "d/foreign")
        assert_raises_rpc_error(-25, "could not generate rand for txid", node.name_firstupdate, "d/foreign", None, foreignNew[0])

    def assertFirstupdate(self, name, newTxid, rand):
        """Check that the confirmed name_firstupdate of name spends the
        name_new output of the transaction newTxid and reveals rand."""
        node = self.nodes[0]
        data = node.name_show(name)
        tx = node.gettransaction(txid=data["txid"], verbose=True)["decoded"]
        assert newTxid in [txin["txid"] for txin in tx["vin"]]
        nameOp = tx["vout"][data["vout"]]["scriptPubKey"]["nameOp"]
        assert_equal(nameOp["op"], "name_firstupdate")
        assert_equal(nameOp["rand"], rand)


if __name__ == '__main__':
    NameDeterministicSaltTest(__file__).main()
