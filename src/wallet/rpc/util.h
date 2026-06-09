// Copyright (c) 2017-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_RPC_UTIL_H
#define BITCOIN_WALLET_RPC_UTIL_H

#include <rpc/util.h>
#include <script/script.h>
#include <wallet/wallet.h>

#include <any>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class JSONRPCRequest;
class UniValue;
struct bilingual_str;

namespace wallet {
class LegacyScriptPubKeyMan;
enum class DatabaseStatus;
struct WalletContext;

extern const std::string HELP_REQUIRING_PASSPHRASE;

static const RPCResult RESULT_LAST_PROCESSED_BLOCK { RPCResult::Type::OBJ, "lastprocessedblock", "hash and height of the block this information was generated on",{
    {RPCResult::Type::STR_HEX, "hash", "hash of the block this information was generated on"},
    {RPCResult::Type::NUM, "height", "height of the block this information was generated on"}}
};

/**
 * Figures out what wallet, if any, to use for a JSONRPCRequest.
 *
 * @param[in] request JSONRPCRequest that wishes to access a wallet
 * @return nullptr if no wallet should be used, or a pointer to the CWallet
 */
std::shared_ptr<CWallet> GetWalletForJSONRPCRequest(const JSONRPCRequest& request);

/** Like GetWalletForJSONRPCRequest, but returns a null shared_ptr instead of
 *  throwing when no wallet context is attached, no wallet is loaded, or the
 *  selection is ambiguous.  Use this for optional wallet annotations on
 *  otherwise wallet-agnostic RPCs (e.g. the "ismine" field).  Resolving the
 *  wallet context inside the wallet translation unit avoids the typeid
 *  duplication that can occur when std::any_cast<WalletContext*> is invoked
 *  from a different static archive (e.g. the node library) on platforms with
 *  hidden-default RTTI visibility (notably macOS).  */
std::shared_ptr<CWallet> MaybeGetWalletForJSONRPCRequest(const JSONRPCRequest& request);
std::optional<std::string> GetWalletNameFromJSONRPCRequest(const JSONRPCRequest& request);
/**
 * Ensures that a wallet name is specified across the endpoint and wallet_name.
 * Throws `RPC_INVALID_PARAMETER` if none or different wallet names are specified.
 */
std::string EnsureUniqueWalletName(const JSONRPCRequest& request, std::optional<std::string_view> wallet_name);

void EnsureWalletIsUnlocked(const CWallet&);
WalletContext& EnsureWalletContext(const std::any& context);

bool GetAvoidReuseFlag(const CWallet& wallet, const UniValue& param);
std::string LabelFromValue(const UniValue& value);
//! Fetch parent descriptors of this scriptPubKey.
void PushParentDescriptors(const CWallet& wallet, const CScript& script_pubkey, UniValue& entry);

void HandleWalletError(const std::shared_ptr<CWallet>& wallet, DatabaseStatus& status, bilingual_str& error);
void AppendLastProcessedBlock(UniValue& entry, const CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);
} //  namespace wallet

#endif // BITCOIN_WALLET_RPC_UTIL_H
