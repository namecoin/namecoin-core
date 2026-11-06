// Copyright (c) 2021 yanmaani
// Licensed under CC0 (Public domain)

#ifndef BITCOIN_WALLET_RPC_WALLETNAMES_H
#define BITCOIN_WALLET_RPC_WALLETNAMES_H

#include <key.h>
#include <script/script.h>
#include <wallet/wallet.h>

#include <vector>

namespace wallet
{

bool getNameSalt(const CKey& key, const valtype& name, valtype& rand);

/**
 * Returns the private keys the wallet has for the address of the given output
 * script (with or without a name prefix), ordered by key ID: at most one for a
 * single-key address, this wallet's keys among the signers of a multisig
 * address, none in a watch-only or external-signer wallet.
 */
std::vector<CKey> getNameSaltKeys(CWallet* pwallet, const CScript& output)
    EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet);

/**
 * Derives the rand value for a name_new to the given output script from the
 * first of those keys.  Returns false if the wallet has no private key for it.
 */
bool getNameSalt(CWallet* pwallet, const valtype& name, const CScript& output, valtype& rand)
    EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet);

/**
 * Finds, among the salts of the given keys for name, the one that matches the
 * commitment hash of a name_new.  Returns false if none does.
 */
bool findNameSalt(const std::vector<CKey>& keys, const valtype& name, const valtype& hash, valtype& rand);

} // namespace wallet

#endif //BITCOIN_WALLET_RPC_WALLETNAMES_H
