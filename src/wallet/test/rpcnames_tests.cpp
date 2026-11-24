// Copyright (c) 2021 yanmaani
// Licensed under CC0 (Public domain)

#include <test/util/setup_common.h>
#include <addresstype.h>
#include <hash.h>
#include <key.h>
#include <key_io.h>
#include <script/names.h>
#include <script/script.h>
#include <script/solver.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <wallet/rpc/walletnames.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(rpcnames_tests, RegTestingSetup) // Keys are in regtest format

static void
TestNameSalt(const std::string& privkey_b58, const std::string& name_str, const std::string& expectedSalt_hex) {
    const CKey privkey = DecodeSecret(privkey_b58);
    const valtype name(name_str.begin(), name_str.end());
    const valtype expectedSalt = ParseHex(expectedSalt_hex);

    valtype salt(20);
    wallet::getNameSalt(privkey, name, salt);

    BOOST_CHECK(salt == expectedSalt);
}

BOOST_AUTO_TEST_CASE(name_salts)
{
    SelectParams(ChainType::REGTEST);
    TestNameSalt( // test_name_salt_addr_p2pkh
                /* private key   */ "cQDxbmQfwRV3vP1mdnVHq37nJekHLsuD3wdSQseBRA2ct4MFk5Pq",
                /* name          */ "d/wikileaks",
                /* expected salt */ "c33f6d84c93d769da2a8882ed9d4a69e2052dd9a");
    TestNameSalt( // test_name_salt_addr_p2wpkh_p2sh
                /* private key   */ "cU9hVzhpvfn91u2zTVn8uqF2ymS7ucYH8V5TmsTDmuyMHgRk9WsJ",
                /* name          */ "d/wikileaks",
                /* expected salt */ "b14763659b268460865db01b2b10b80a9cbe9ceb");
    TestNameSalt( // test_name_salt_addr_p2wpkh
                /* private key   */ "cPuQzcNEgbeYZ5at9VdGkCwkPA9r34gvEVJjuoz384rTfYpahfe7",
                /* name          */ "d/wikileaks",
                /* expected salt */ "a39038cddfcc17d391b8620639a72178dc73b19a");
}

BOOST_AUTO_TEST_CASE(wallet_name_salts)
{
    const CKey key1 = DecodeSecret("cQDxbmQfwRV3vP1mdnVHq37nJekHLsuD3wdSQseBRA2ct4MFk5Pq");
    const CKey key2 = DecodeSecret("cPuQzcNEgbeYZ5at9VdGkCwkPA9r34gvEVJjuoz384rTfYpahfe7");
    BOOST_REQUIRE(key1.IsValid() && key2.IsValid());
    const std::string name_str = "d/wikileaks";
    const valtype name(name_str.begin(), name_str.end());

    // The salts of both keys (their vectors in name_salts above), and the
    // commitment of a name_new that used a given salt.
    valtype salt1, salt2;
    wallet::getNameSalt(key1, name, salt1);
    wallet::getNameSalt(key2, name, salt2);
    BOOST_CHECK_EQUAL(HexStr(salt1), "c33f6d84c93d769da2a8882ed9d4a69e2052dd9a");
    BOOST_CHECK_EQUAL(HexStr(salt2), "a39038cddfcc17d391b8620639a72178dc73b19a");
    const auto commitment = [&name](const valtype& salt) {
        valtype data(salt);
        data.insert(data.end(), name.begin(), name.end());
        const uint160 hash = Hash160(data);
        return valtype(hash.begin(), hash.end());
    };

    // The P2PKH address of key1, also as the output of a name_new.
    const CScript addr = GetScriptForDestination(PKHash(key1.GetPubKey()));
    const CScript nameNew = CNameScript::buildNameNew(addr, name, valtype(20, 0));

    // A descriptor with the private key: the one key, and the salt of key1.
    {
        wallet::CWallet keystore(m_node.chain.get(), "", wallet::CreateMockableWalletDatabase());
        wallet::CreateDescriptor(keystore, "pkh(" + EncodeSecret(key1) + ")", /*success=*/true);
        LOCK(keystore.cs_wallet);
        for (const CScript& script : {addr, nameNew}) {
            const auto keys = wallet::getNameSaltKeys(&keystore, script);
            BOOST_CHECK_EQUAL(keys.size(), 1U);
            valtype rand;
            BOOST_CHECK(wallet::getNameSalt(&keystore, name, script, rand));
            BOOST_CHECK(rand == salt1);
            BOOST_CHECK(wallet::findNameSalt(keys, name, commitment(salt1), rand));
            BOOST_CHECK(rand == salt1);
            BOOST_CHECK(!wallet::findNameSalt(keys, name, commitment(salt2), rand));
        }

        // A script the wallet does not know.
        const CScript other = GetScriptForDestination(PKHash(key2.GetPubKey()));
        BOOST_CHECK(wallet::getNameSaltKeys(&keystore, other).empty());
        valtype rand;
        BOOST_CHECK(!wallet::getNameSalt(&keystore, name, other, rand));
    }

    // A watch-only descriptor of the same key: no key, so no salt either.
    {
        wallet::CWallet keystore(m_node.chain.get(), "", wallet::CreateMockableWalletDatabase());
        keystore.SetWalletFlag(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);
        wallet::CreateDescriptor(keystore, "pkh(" + HexStr(key1.GetPubKey()) + ")", /*success=*/true);
        LOCK(keystore.cs_wallet);
        BOOST_CHECK(keystore.IsMine(addr));
        for (const CScript& script : {addr, nameNew}) {
            BOOST_CHECK(wallet::getNameSaltKeys(&keystore, script).empty());
            valtype rand;
            BOOST_CHECK(!wallet::getNameSalt(&keystore, name, script, rand));
        }
    }

    const CScript multisig = GetScriptForDestination(
        ScriptHash(GetScriptForMultisig(2, {key1.GetPubKey(), key2.GetPubKey()})));
    const CScript multisigNameNew = CNameScript::buildNameNew(multisig, name, valtype(20, 0));

    // A multisig address with both private keys: the salt is the one of the
    // key with the lower key ID, and the commitments of both are found.
    {
        wallet::CWallet keystore(m_node.chain.get(), "", wallet::CreateMockableWalletDatabase());
        wallet::CreateDescriptor(keystore, "sh(multi(2," + EncodeSecret(key1) + "," + EncodeSecret(key2) + "))", /*success=*/true);
        LOCK(keystore.cs_wallet);
        BOOST_CHECK(keystore.IsMine(multisig));
        const bool key1First = key1.GetPubKey().GetID() < key2.GetPubKey().GetID();
        for (const CScript& script : {multisig, multisigNameNew}) {
            const auto keys = wallet::getNameSaltKeys(&keystore, script);
            BOOST_CHECK_EQUAL(keys.size(), 2U);
            valtype rand;
            BOOST_CHECK(wallet::getNameSalt(&keystore, name, script, rand));
            BOOST_CHECK(rand == (key1First ? salt1 : salt2));
            BOOST_CHECK(wallet::findNameSalt(keys, name, commitment(salt1), rand));
            BOOST_CHECK(rand == salt1);
            BOOST_CHECK(wallet::findNameSalt(keys, name, commitment(salt2), rand));
            BOOST_CHECK(rand == salt2);
            BOOST_CHECK(!wallet::findNameSalt(keys, name, commitment(valtype(20, 0)), rand));
        }
    }

    // The same address in the wallet of one co-signer, with key1 only: the
    // salt is the one of key1, and the commitment of key2 is not found.
    {
        wallet::CWallet keystore(m_node.chain.get(), "", wallet::CreateMockableWalletDatabase());
        wallet::CreateDescriptor(keystore, "sh(multi(2," + EncodeSecret(key1) + "," + HexStr(key2.GetPubKey()) + "))", /*success=*/true);
        LOCK(keystore.cs_wallet);
        BOOST_CHECK(keystore.IsMine(multisig));
        const auto keys = wallet::getNameSaltKeys(&keystore, multisig);
        BOOST_CHECK_EQUAL(keys.size(), 1U);
        valtype rand;
        BOOST_CHECK(wallet::getNameSalt(&keystore, name, multisig, rand));
        BOOST_CHECK(rand == salt1);
        BOOST_CHECK(wallet::findNameSalt(keys, name, commitment(salt1), rand));
        BOOST_CHECK(rand == salt1);
        BOOST_CHECK(!wallet::findNameSalt(keys, name, commitment(salt2), rand));
    }
}

BOOST_AUTO_TEST_SUITE_END()
