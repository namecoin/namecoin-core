// Copyright (c) 2014-2024 Daniel Kraft
// Copyright (c) 2021 yanmaani
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <base58.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/validation.h>
#include <crypto/hkdf_sha256_32.h>
#include <core_io.h>
#include <init.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <names/common.h>
#include <names/encoding.h>
#include <names/main.h>
#include <names/mempool.h>
#include <node/context.h>
#include <node/types.h>
#include <net.h>
#include <primitives/transaction.h>
#include <primitives/transaction_identifier.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <rpc/names.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <script/names.h>
#include <txmempool.h>
#include <util/moneystr.h>
#include <util/translation.h>
#include <util/vector.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/rpc/wallet.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <algorithm>
#include <memory>

namespace wallet
{

/* ************************************************************************** */
namespace
{

/**
 * A simple helper class that handles determination of the address to which
 * name outputs should be sent.  It handles the CReserveKey reservation
 * as well as parsing the explicit options given by the user (if any).
 */
class DestinationAddressHelper
{

private:

  /** Reference to the wallet that should be used.  */
  CWallet& wallet;

  /**
   * The reserve key that was used if no override is given.  When finalising
   * (after the sending succeeded), this key needs to be marked as Keep().
   */
  std::unique_ptr<ReserveDestination> rdest;

  /** Set if a valid override destination was added.  */
  std::unique_ptr<CTxDestination> overrideDest;

public:

  explicit DestinationAddressHelper (CWallet& w)
    : wallet(w)
  {}

  /**
   * Processes the given options object to see if it contains an override
   * destination.  If it does, remembers it.
   */
  void setOptions (const UniValue& opt);

  /**
   * Returns the destination that should be used as destination.
   */
  CTxDestination getDest ();

  /**
   * Marks the key as used if one has been reserved.  This should be called
   * when sending succeeded.
   */
  void finalise ();

};

void DestinationAddressHelper::setOptions (const UniValue& opt)
{
  RPCTypeCheckObj (opt,
    {
      {"destAddress", UniValueType (UniValue::VSTR)},
    },
    true, false);
  if (!opt.exists ("destAddress"))
    return;

  CTxDestination dest = DecodeDestination (opt["destAddress"].get_str ());
  if (!IsValidDestination (dest))
    throw JSONRPCError (RPC_INVALID_ADDRESS_OR_KEY, "invalid address");
  overrideDest.reset (new CTxDestination (std::move (dest)));
}

CTxDestination DestinationAddressHelper::getDest ()
{
  if (overrideDest != nullptr)
    return *overrideDest;

  rdest.reset (new ReserveDestination (&wallet, wallet.m_default_address_type));
  const auto op_dest = rdest->GetReservedDestination (false);
  if (!op_dest)
    throw JSONRPCError (RPC_WALLET_KEYPOOL_RAN_OUT,
                        strprintf ("Failed to generate mining address: %s",
                                   util::ErrorString (op_dest).original));

  return *op_dest;
}

void DestinationAddressHelper::finalise ()
{
  if (rdest != nullptr)
    rdest->KeepDestination ();
}

/**
 * Sends a name output to the given name script.  This is the "final" step that
 * is common between name_new, name_firstupdate and name_update.  This method
 * also implements the "sendCoins" option, if included.
 */
UniValue
SendNameOutput (const JSONRPCRequest& request,
                CWallet& wallet,
                const CTxDestination& dest, const CScript& nameOp,
                const CTxIn* nameInput, const UniValue& opt)
{
  RPCTypeCheckObj (opt,
    {
      {"sendCoins", UniValueType (UniValue::VOBJ)},
    },
    true, false);

  auto& node = EnsureAnyNodeContext (request);
  if (wallet.GetBroadcastTransactions ())
    EnsureConnman (node);

  std::vector<CRecipient> vecSend;
  vecSend.push_back ({dest, NAME_LOCKED_AMOUNT, false, nameOp});

  if (opt.exists ("sendCoins"))
    for (const std::string& addr : opt["sendCoins"].getKeys ())
      {
        const CTxDestination coinDest = DecodeDestination (addr);
        if (!IsValidDestination (coinDest))
          throw JSONRPCError (RPC_INVALID_ADDRESS_OR_KEY,
                              "Invalid address: " + addr);

        const CAmount nAmount = AmountFromValue (opt["sendCoins"][addr]);
        if (nAmount <= 0)
          throw JSONRPCError (RPC_TYPE_ERROR, "Invalid amount for send");

        vecSend.push_back ({coinDest, nAmount, false});
      }

  CCoinControl coinControl;
  return SendMoney (wallet, coinControl, nameInput, vecSend, {}, false);
}

/**
 * Checks whether a name exists (is registered and not expired).
 * Acquires cs_main internally.
 */
bool
existsName (const ChainstateManager& chainman, const valtype& name)
{
  LOCK (cs_main);
  CNameData data;
  const auto& coinsTip = chainman.ActiveChainstate ().CoinsTip ();
  return coinsTip.GetName (name, data) && !data.isExpired (chainman.ActiveHeight ());
}

} // anonymous namespace
/* ************************************************************************** */

RPCHelpMan
name_list ()
{
  NameOptionsHelp optHelp;
  optHelp
      .withNameEncoding ()
      .withValueEncoding ();

  return RPCHelpMan ("name_list",
      "Shows the status of all names in the wallet.\n",
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only include this name"},
          optHelp.buildRpcArg (),
      },
      RPCResult {RPCResult::Type::ARR, "", "",
          {
              NameInfoHelp ()
                .withExpiration ()
                .finish ()
          }
      },
      RPCExamples {
          HelpExampleCli ("name_list", "")
        + HelpExampleCli ("name_list", "\"myname\"")
        + HelpExampleRpc ("name_list", "")
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  const auto& chainman = EnsureChainman (EnsureAnyNodeContext (request));

  UniValue options(UniValue::VOBJ);
  if (request.params.size () >= 2)
    options = request.params[1].get_obj ();

  valtype nameFilter;
  if (request.params.size () >= 1 && !request.params[0].isNull ())
    nameFilter = DecodeNameFromRPCOrThrow (request.params[0], options);

  std::map<valtype, int> mapHeights;
  std::map<valtype, UniValue> mapObjects;

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  {
  LOCK2 (pwallet->cs_wallet, cs_main);

  const int tipHeight = chainman.ActiveHeight ();
  for (const auto& item : pwallet->mapWallet)
    {
      const CWalletTx& tx = item.second;
      if (!tx.tx->IsNamecoin ())
        continue;

      CNameScript nameOp;
      int nOut = -1;
      for (unsigned i = 0; i < tx.tx->vout.size (); ++i)
        {
          const CNameScript cur(tx.tx->vout[i].scriptPubKey);
          if (cur.isNameOp ())
            {
              if (nOut != -1)
                LogDebug (BCLog::NAMES, "%s: wallet contains tx with multiple name outputs", __func__);
              else
                {
                  nameOp = cur;
                  nOut = i;
                }
            }
        }

      if (nOut == -1 || !nameOp.isAnyUpdate ())
        continue;

      const valtype& name = nameOp.getOpName ();
      if (!nameFilter.empty () && nameFilter != name)
        continue;

      const int depth = pwallet->GetTxDepthInMainChain(tx);
      if (depth <= 0)
        continue;
      const int height = tipHeight - depth + 1;

      const auto mit = mapHeights.find (name);
      if (mit != mapHeights.end () && mit->second > height)
        continue;

      UniValue obj
        = getNameInfo (options, name, nameOp.getOpValue (),
                       COutPoint (tx.GetHash (), nOut),
                       nameOp.getAddress ());
      addOwnershipInfo (nameOp.getAddress (), pwallet, obj);
      addExpirationInfo (chainman, height, obj);

      mapHeights[name] = height;
      mapObjects[name] = obj;
    }
  }

  UniValue res(UniValue::VARR);
  for (const auto& item : mapObjects)
    res.push_back (item.second);

  return res;
}
  );
}

/**
 * Generate a salt using HKDF for a given name + private key combination.
 * Indirectly used in name_new and name_firstupdate.
 * Refactored out to make testing easier.
 * @return True on success, false otherwise
 */
bool
getNameSalt(const CKey& key, const valtype& name, valtype& rand)
{
    const valtype ikm(reinterpret_cast<const unsigned char*> (key.begin()),
                      reinterpret_cast<const unsigned char*> (key.end()));
    const std::string salt(reinterpret_cast<const char*>(name.data()), name.size());
    const std::string info("Namecoin Registration Salt");
    CHKDF_HMAC_SHA256_L32 hkdf32(ikm.data(), ikm.size(), salt);
    unsigned char tmp[32];
    hkdf32.Expand32(info, tmp);

    rand = valtype(tmp, tmp + 20);
    return true;
}

namespace
{

/**
 * Generate a salt using HKDF for a given name + txout combination.
 * Used in name_new and name_firstupdate.
 * @return True on success, false otherwise
 */
bool
getNameSalt(CWallet* const pwallet, const valtype& name, const CScript& output, valtype& rand)
{
    AssertLockHeld(pwallet->cs_wallet);

    for (const auto* spk : pwallet->GetScriptPubKeyMans (output))
      {
        if (spk == nullptr)
            continue;

        auto provider = spk->GetSigningProviderWithKeys (output);
        if (provider == nullptr)
            continue;

        CTxDestination dest;
        CKeyID keyid;
        CKey key;
        if (!ExtractDestination(output, dest))
            continue; // If multisig.
        assert(IsValidDestination(dest)); // We should never get a null destination.

        keyid = GetKeyForDestination(*provider, dest);
        provider->GetKey(keyid, key);

        return wallet::getNameSalt(key, name, rand);
      }

    return false;
}

bool
saltMatchesHash(const valtype& name, const valtype& rand, const valtype& expectedHash)
{
    valtype toHash(rand);
    toHash.insert (toHash.end(), name.begin(), name.end());

    return (Hash160(toHash) == uint160(expectedHash));
}

} // anonymous namespace

/* ************************************************************************** */

RPCHelpMan
name_new ()
{
  NameOptionsHelp optHelp;
  optHelp
      .withNameEncoding ()
      .withWriteOptions ()
      .withArg ("allowExisting", RPCArg::Type::BOOL, "false",
                "If set, then the name_new is sent even if the name exists already");

  return RPCHelpMan ("name_new",
      "Starts registration of the given name.  Must be followed up with name_firstupdate to finish the registration."
          + HELP_REQUIRING_PASSPHRASE,
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to register"},
          optHelp.buildRpcArg (),
      },
      RPCResult {RPCResult::Type::ARR_FIXED, "", "",
          {
              {RPCResult::Type::STR_HEX, "txid", "the txid, required for name_firstupdate"},
              {RPCResult::Type::STR_HEX, "rand", "random value, for name_firstupdate"},
          },
      },
      RPCExamples {
          HelpExampleCli ("name_new", "\"myname\"")
        + HelpExampleRpc ("name_new", "\"myname\"")
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  const auto& chainman = EnsureChainman (EnsureAnyNodeContext (request));

  UniValue options(UniValue::VOBJ);
  if (request.params.size () >= 2)
    options = request.params[1].get_obj ();
  RPCTypeCheckObj (options,
    {
      {"allowExisting", UniValueType (UniValue::VBOOL)},
    },
    true, false);

  const valtype name = DecodeNameFromRPCOrThrow (request.params[0], options);
  if (name.size () > MAX_NAME_LENGTH)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the name is too long");

  if (!options["allowExisting"].isTrue ())
    {
      if (existsName (chainman, name))
        throw JSONRPCError (RPC_TRANSACTION_ERROR, "this name exists already");
    }

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  LOCK (pwallet->cs_wallet);

  EnsureWalletIsUnlocked (*pwallet);

  DestinationAddressHelper destHelper(*pwallet);
  destHelper.setOptions (options);

  const CTxDestination dest = destHelper.getDest ();

  valtype rand(20);
  if (!getNameSalt (pwallet, name, GetScriptForDestination (dest), rand))
      GetRandBytes (rand);

  const CScript nameOp
      = CNameScript::buildNameNew (CScript (), name, rand);

  const UniValue txidVal
      = SendNameOutput (request, *pwallet, dest, nameOp, nullptr, options);
  destHelper.finalise ();

  const std::string randStr = HexStr (rand);
  const std::string txid = txidVal.get_str ();
  LogInfo ("name_new: name=%s, rand=%s, tx=%s\n",
           EncodeNameForMessage (name), randStr.c_str (), txid.c_str ());

  UniValue res(UniValue::VARR);
  res.push_back (txid);
  res.push_back (randStr);

  return res;
}
  );
}

/* ************************************************************************** */

namespace
{

/**
 * Helper routine to fetch the name output of a previous transaction.  This
 * is required for name_firstupdate.
 * @param txid Previous transaction ID.
 * @param txOut Set to the corresponding output.
 * @param txIn Set to the CTxIn to include in the new tx.
 * @return True if the output could be found.
 */
bool
getNamePrevout (Chainstate& chainState, const Txid& txid,
                CTxOut& txOut, CTxIn& txIn)
{
  AssertLockHeld (cs_main);

  // Maximum number of outputs that are checked for the NAME_NEW prevout.
  constexpr unsigned MAX_NAME_PREVOUT_TRIALS = 1000;

  // Unfortunately, with the change of the txdb to be based on outputs rather
  // than full transactions, we can no longer just look up the txid and iterate
  // over all outputs.  Since this is only necessary for a corner case, we just
  // keep trying with indices until we find the output (up to a maximum number
  // of trials).

  for (unsigned i = 0; i < MAX_NAME_PREVOUT_TRIALS; ++i)
    {
      const COutPoint outp(txid, i);

      const auto coin = chainState.CoinsTip ().GetCoin (outp);
      if (!coin)
        continue;

      if (!coin->out.IsNull ()
          && CNameScript::isNameScript (coin->out.scriptPubKey))
        {
          txOut = coin->out;
          txIn = CTxIn (outp);
          return true;
        }
    }

  return false;
}

}  // anonymous namespace

RPCHelpMan
name_firstupdate ()
{
  /* There is an undocumented sixth argument that can be used to disable
     the check for already existing names here (it will still be checked
     by the mempool and tx validation logic, of course).  This is used
     by the regtests to catch a bug that was previously present but
     has presumably no other use.  */

  NameOptionsHelp optHelp;
  optHelp
      .withNameEncoding ()
      .withValueEncoding ()
      .withWriteOptions ();

  return RPCHelpMan ("name_firstupdate",
            "Finishes the registration of a name.  Depends on name_new being already issued."
                + HELP_REQUIRING_PASSPHRASE,
            {
                {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to register"},
                {"rand", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The rand value of name_new"},
                {"tx", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The name_new txid"},
                {"value", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Value for the name"},
                optHelp.buildRpcArg (),
                {"allow_active", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Disable check for the name being active"},
            },
            RPCResult {RPCResult::Type::STR_HEX, "", "the transaction ID"},
            RPCExamples {
                HelpExampleCli ("name_firstupdate", "\"myname\", \"555844f2db9c7f4b25da6cb8277596de45021ef2\" \"a77ceb22aa03304b7de64ec43328974aeaca211c37dd29dcce4ae461bb80ca84\", \"my-value\"")
              + HelpExampleRpc ("name_firstupdate", "\"myname\", \"555844f2db9c7f4b25da6cb8277596de45021ef2\" \"a77ceb22aa03304b7de64ec43328974aeaca211c37dd29dcce4ae461bb80ca84\", \"my-value\"")
            },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  const auto& node = EnsureAnyNodeContext (request);
  const auto& chainman = EnsureChainman (node);

  UniValue options(UniValue::VOBJ);
  if (request.params.size () >= 5)
    options = request.params[4].get_obj ();

  const valtype name = DecodeNameFromRPCOrThrow (request.params[0], options);
  if (name.size () > MAX_NAME_LENGTH)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the name is too long");

  const bool fixedRand = (request.params.size () >= 2 && !request.params[1].isNull ());
  const bool fixedTxid = (request.params.size () >= 3 && !request.params[2].isNull ());
  valtype rand(20);
  if (fixedRand)
    {
      rand = ParseHexV (request.params[1], "rand");
      if (rand.size () > 20)
        throw JSONRPCError (RPC_INVALID_PARAMETER, "invalid rand value");
    }

  const bool isDefaultVal = (request.params.size () < 4 || request.params[3].isNull ());
  const valtype value = isDefaultVal ?
      valtype ():
      DecodeValueFromRPCOrThrow (request.params[3], options);

  if (value.size () > MAX_VALUE_LENGTH_UI)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the value is too long");

  {
    auto& mempool = EnsureMemPool (node);
    LOCK (mempool.cs);
    if (mempool.registersName (name))
      throw JSONRPCError (RPC_TRANSACTION_ERROR,
                          "this name is already being registered");
  }

  if (request.params.size () < 6 || !request.params[5].get_bool ())
    {
      if (existsName (chainman, name))
        throw JSONRPCError (RPC_TRANSACTION_ERROR,
                            "this name is already active");
    }

  Txid prevTxid;
  prevTxid.SetNull (); // if it can't find a txid, force an error
  if (fixedTxid)
    {
      const auto id = Txid::FromHex (request.params[2].get_str ());
      if (!id)
        throw JSONRPCError (RPC_INVALID_PARAMETER, "invalid txid");
      prevTxid = *id;
    }
  else
    {
      // Code slightly duplicates name_scan, but not enough to be able to refactor.
      /* Make sure the results are valid at least up to the most recent block
         the user could have gotten from another RPC command prior to now.  */
      pwallet->BlockUntilSyncedToCurrentChain ();

      LOCK2 (pwallet->cs_wallet, cs_main);

      for (const auto& item : pwallet->mapWallet)
        {
          const CWalletTx& tx = item.second;
          if (!tx.tx->IsNamecoin ())
            continue;

          CScript output;
          CNameScript nameOp;
          bool found = false;
          for (CTxOut curOutput : tx.tx->vout)
            {
              CScript curScript = curOutput.scriptPubKey;
              const CNameScript cur(curScript);
              if (!cur.isNameOp ())
                continue;
              if (cur.getNameOp () != OP_NAME_NEW)
                continue;
              if (found) {
                LogDebug (BCLog::NAMES, "%s: wallet contains tx with multiple name outputs", __func__);
                continue;
              }
              nameOp = cur;
              found = true;
              output = curScript;
            }

          if (!found)
            continue; // no name outputs found

          if (!fixedRand)
            {
              if (!getNameSalt (pwallet, name, output, rand)) // we don't have the private key for that output
                continue;
            }

          if (!saltMatchesHash (name, rand, nameOp.getOpHash ()))
            continue;

          // found it
          prevTxid = tx.GetHash ();

          break; // if there be more than one match, the behavior is undefined
        }
    }

  if (prevTxid.IsNull ())
    throw JSONRPCError (RPC_TRANSACTION_ERROR, "scan for previous txid failed");

  CTxOut prevOut;
  CTxIn txIn;
  {
    LOCK (cs_main);
    if (!getNamePrevout (chainman.ActiveChainstate (), prevTxid, prevOut, txIn))
      throw JSONRPCError (RPC_TRANSACTION_ERROR, "previous txid not found");
  }

  const CNameScript prevNameOp(prevOut.scriptPubKey);

  if (!fixedRand)
    {
      LOCK (pwallet->cs_wallet);
      bool saltOK = getNameSalt (pwallet, name, prevOut.scriptPubKey, rand);
      if (!saltOK)
          throw JSONRPCError (RPC_TRANSACTION_ERROR, "could not generate rand for txid");
      if (!saltMatchesHash (name, rand, prevNameOp.getOpHash ()))
        throw JSONRPCError (RPC_TRANSACTION_ERROR, "generated rand for txid does not match");
    }

  assert (prevNameOp.isNameOp ());
  if (prevNameOp.getNameOp () != OP_NAME_NEW)
    throw JSONRPCError (RPC_TRANSACTION_ERROR, "previous tx is not name_new");

  if (!saltMatchesHash (name, rand, prevNameOp.getOpHash ()))
    throw JSONRPCError (RPC_TRANSACTION_ERROR, "rand value is wrong");

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  LOCK (pwallet->cs_wallet);

  EnsureWalletIsUnlocked (*pwallet);

  DestinationAddressHelper destHelper(*pwallet);
  destHelper.setOptions (options);

  const CScript nameOp
    = CNameScript::buildNameFirstupdate (CScript (), name, value, rand);

  const UniValue txidVal
      = SendNameOutput (request, *pwallet, destHelper.getDest (), nameOp,
                        &txIn, options);
  destHelper.finalise ();

  return txidVal;
}
  );
}

/* ************************************************************************** */

RPCHelpMan
name_update ()
{
  NameOptionsHelp optHelp;
  optHelp
      .withNameEncoding ()
      .withValueEncoding ()
      .withWriteOptions ();

  return RPCHelpMan ("name_update",
      "Updates a name and possibly transfers it."
          + HELP_REQUIRING_PASSPHRASE,
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to update"},
          {"value", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Value for the name"},
          optHelp.buildRpcArg (),
      },
      RPCResult {RPCResult::Type::STR_HEX, "", "the transaction ID"},
      RPCExamples {
          HelpExampleCli ("name_update", "\"myname\", \"new-value\"")
        + HelpExampleRpc ("name_update", "\"myname\", \"new-value\"")
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  const auto& node = EnsureAnyNodeContext (request);
  const auto& chainman = EnsureChainman (node);

  UniValue options(UniValue::VOBJ);
  if (request.params.size () >= 3)
    options = request.params[2].get_obj ();

  const valtype name = DecodeNameFromRPCOrThrow (request.params[0], options);
  if (name.size () > MAX_NAME_LENGTH)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the name is too long");

  const bool isDefaultVal = request.params.size() < 2 || request.params[1].isNull();

  valtype value;
  if (!isDefaultVal) {
      value = DecodeValueFromRPCOrThrow (request.params[1], options);
      if (value.size () > MAX_VALUE_LENGTH_UI)
          throw JSONRPCError (RPC_INVALID_PARAMETER, "the value is too long");
  }

  /* For finding the name output to spend and its value, we first check if
     there are pending operations on the name in the mempool.  If there
     are, then we build upon the last one to get a valid chain.  If there
     are none, then we look up the last outpoint from the name database
     instead. */
  // TODO: Use name_show for this instead.

  const unsigned chainLimit = gArgs.GetIntArg ("-limitnamechains",
                                               DEFAULT_NAME_CHAIN_LIMIT);
  COutPoint outp;
  {
    auto& mempool = EnsureMemPool (node);
    LOCK (mempool.cs);

    const unsigned pendingOps = mempool.pendingNameChainLength (name);
    if (pendingOps >= chainLimit)
      throw JSONRPCError (RPC_TRANSACTION_ERROR,
                          "there are already too many pending operations"
                          " on this name");

    if (pendingOps > 0)
      {
        outp = mempool.lastNameOutput (name);
        if (isDefaultVal)
          {
            const auto& tx = mempool.mapTx.find(outp.hash)->GetTx();
            value = CNameScript(tx.vout[outp.n].scriptPubKey).getOpValue();
          }
      }
  }

  if (outp.IsNull ())
    {
      if (!existsName (chainman, name))
        throw JSONRPCError (RPC_TRANSACTION_ERROR,
                            "this name can not be updated");

      LOCK (cs_main);
      CNameData oldData;
      const auto& coinsTip = chainman.ActiveChainstate ().CoinsTip ();
      const bool found = coinsTip.GetName (name, oldData);
      assert (found);
      if (isDefaultVal)
        value = oldData.getValue();
      outp = oldData.getUpdateOutpoint ();
    }
  assert (!outp.IsNull ());
  const CTxIn txIn(outp);

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  LOCK (pwallet->cs_wallet);

  EnsureWalletIsUnlocked (*pwallet);

  DestinationAddressHelper destHelper(*pwallet);
  destHelper.setOptions (options);

  const CScript nameOp
    = CNameScript::buildNameUpdate (CScript (), name, value);

  const UniValue txidVal
      = SendNameOutput (request, *pwallet, destHelper.getDest (), nameOp,
                        &txIn, options);
  destHelper.finalise ();

  return txidVal;
}
  );
}

/* ************************************************************************** */

namespace
{

/**
 * Helper class for name_autoregister.  Encapsulates the state and logic
 * for creating the name_new transaction, building the name_firstupdate
 * transaction, committing it to the wallet for automatic rebroadcast,
 * and locking the name_new output.
 */
class AutoregisterHelper
{

private:

  const JSONRPCRequest& request;
  CWallet& wallet;
  ChainstateManager& chainman;
  NodeContext& node;

  valtype name;
  valtype value;

  /** The txid of the name_new transaction.  */
  Txid newTxid;

  /** The random salt used in the name_new.  */
  valtype rand;

  /** The destination address helper.  */
  DestinationAddressHelper destHelper;

public:

  AutoregisterHelper (const JSONRPCRequest& req, CWallet& w,
                      ChainstateManager& cm, NodeContext& n,
                      const valtype& nm, const valtype& val,
                      const UniValue& options)
    : request(req), wallet(w), chainman(cm), node(n),
      name(nm), value(val), destHelper(w)
  {
    destHelper.setOptions (options);
  }

  /** Create and broadcast the name_new transaction.  */
  void createNameNew (const UniValue& options);

  /** Build, sign, and commit the name_firstupdate transaction.  */
  void createAndCommitFirstUpdate ();

  /** Lock the name script output of the name_new transaction.  */
  void lockNameNewOutput ();

  /** Finalise the destination address reservation.  */
  void finalise () { destHelper.finalise (); }

  Txid getNewTxid () const { return newTxid; }
  const valtype& getRand () const { return rand; }

};

void
AutoregisterHelper::createNameNew (const UniValue& options)
{
  const CTxDestination dest = destHelper.getDest ();
  const CScript destScript = GetScriptForDestination (dest);

  rand.resize (20);
  if (!getNameSalt (&wallet, name, destScript, rand))
    GetRandBytes (rand);

  const CScript nameOp
    = CNameScript::buildNameNew (CScript (), name, rand);

  const UniValue txidVal
    = SendNameOutput (request, wallet, dest, nameOp, nullptr, options);

  newTxid = Txid::FromUint256 (ParseHashV (txidVal, "txid"));
}

void
AutoregisterHelper::createAndCommitFirstUpdate ()
{
  const CTxDestination dest = destHelper.getDest ();

  /* Build the name_firstupdate script.  */
  const CScript nameOp
    = CNameScript::buildNameFirstupdate (CScript (), name, value, rand);

  /* Find the name_new output to spend from the wallet transaction.
     The name_new was just broadcast and is not yet in CoinsTip,
     so we look it up directly from the wallet.  */
  CTxIn txIn;
  {
    const CWalletTx* wtx = wallet.GetWalletTx (newTxid);
    if (!wtx)
      throw JSONRPCError (RPC_TRANSACTION_ERROR,
                          "could not find name_new in wallet");

    bool found = false;
    for (unsigned int i = 0; i < wtx->tx->vout.size (); ++i)
      {
        if (CNameScript::isNameScript (wtx->tx->vout[i].scriptPubKey))
          {
            txIn = CTxIn (COutPoint (newTxid, i));
            found = true;
            break;
          }
      }
    if (!found)
      throw JSONRPCError (RPC_TRANSACTION_ERROR,
                          "could not find name_new output");
  }

  /* Create the firstupdate transaction via the standard wallet path.
     The mempool will reject this while the name_new is immature
     (< MIN_FIRSTUPDATE_DEPTH confirmations).  CommitTransaction
     stores the tx in the wallet regardless, and blockConnected
     retries on each new block via m_pending_rebroadcast until the
     name_new matures and the firstupdate is accepted.  */
  std::vector<CRecipient> vecSend;
  vecSend.push_back ({dest, NAME_LOCKED_AMOUNT, false, nameOp});

  CCoinControl coinControl;
  auto res = CreateTransaction (wallet, vecSend, &txIn,
                                /*change_pos=*/std::nullopt, coinControl,
                                /*sign=*/true);
  if (!res)
    throw JSONRPCError (RPC_WALLET_ERROR,
                        "failed to create name_firstupdate: "
                        + util::ErrorString (res).original);

  /* Commit the transaction to the wallet and broadcast it.
     CommitTransaction adds the tx to the wallet regardless of whether
     the mempool accepts it.  Since the name_new is immature at this
     point, the mempool will reject it; CommitTransaction sets
     m_pending_rebroadcast so that blockConnected retries on each new
     block until the name_new matures.  */
  wallet.CommitTransaction (res->tx, {}, {});
}

void
AutoregisterHelper::lockNameNewOutput ()
{
  const CWalletTx* wtx = wallet.GetWalletTx (newTxid);
  if (!wtx)
    return;

  for (unsigned int i = 0; i < wtx->tx->vout.size (); ++i)
    {
      if (CNameScript::isNameScript (wtx->tx->vout[i].scriptPubKey))
        {
          wallet.LockCoin (COutPoint (newTxid, i));
          break;
        }
    }
}

} // anonymous namespace

RPCHelpMan
name_autoregister ()
{
  NameOptionsHelp optHelp;
  optHelp
      .withNameEncoding ()
      .withValueEncoding ()
      .withWriteOptions ();

  return RPCHelpMan ("name_autoregister",
      "\nRegisters a name atomically.  Performs name_new and name_firstupdate"
      "\nin a single call.  The firstupdate transaction is committed to the"
      "\nwallet and broadcast; the wallet's automatic rebroadcast mechanism"
      "\nhandles retries if the mempool does not accept it immediately."
          + HELP_REQUIRING_PASSPHRASE,
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to register"},
          {"value", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Value for the name"},
          optHelp.buildRpcArg (),
      },
      RPCResult {RPCResult::Type::OBJ, "", "",
          {
              {RPCResult::Type::STR_HEX, "txid", "the txid of the name_new transaction"},
              {RPCResult::Type::STR_HEX, "rand", "the random value used"},
          },
      },
      RPCExamples {
          HelpExampleCli ("name_autoregister", "\"myname\"")
        + HelpExampleCli ("name_autoregister", "\"myname\" \"my-value\"")
        + HelpExampleRpc ("name_autoregister", "\"myname\", \"my-value\"")
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  RPCTypeCheck (request.params, {UniValue::VSTR, UniValue::VSTR, UniValue::VOBJ},
                true);
  auto& node = EnsureAnyNodeContext (request);
  auto& chainman = EnsureChainman (node);

  UniValue options(UniValue::VOBJ);
  if (request.params.size () >= 3)
    options = request.params[2].get_obj ();

  const valtype name = DecodeNameFromRPCOrThrow (request.params[0], options);
  if (name.size () > MAX_NAME_LENGTH)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the name is too long");

  if (existsName (chainman, name))
    throw JSONRPCError (RPC_TRANSACTION_ERROR, "this name exists already");

  const bool isDefaultVal = (request.params.size () < 2 || request.params[1].isNull ());
  const valtype value = isDefaultVal
    ? valtype ()
    : DecodeValueFromRPCOrThrow (request.params[1], options);

  if (value.size () > MAX_VALUE_LENGTH_UI)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the value is too long");

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  LOCK (pwallet->cs_wallet);
  EnsureWalletIsUnlocked (*pwallet);

  AutoregisterHelper helper(request, *pwallet, chainman, node,
                            name, value, options);

  helper.createNameNew (options);
  helper.createAndCommitFirstUpdate ();
  helper.lockNameNewOutput ();
  helper.finalise ();

  const std::string randStr = HexStr (helper.getRand ());
  const std::string txid = helper.getNewTxid ().GetHex ();

  LogPrintf ("name_autoregister: name=%s, rand=%s, tx=%s\n",
             EncodeNameForMessage (name), randStr.c_str (), txid.c_str ());

  UniValue res(UniValue::VOBJ);
  res.pushKV ("txid", txid);
  res.pushKV ("rand", randStr);

  return res;
}
  );
}

/* ************************************************************************** */

RPCHelpMan
sendtoname ()
{
  return RPCHelpMan{"sendtoname",
      "Send an amount to the owner of a name.\n"
      "\nIt is an error if the name is expired."
          + HELP_REQUIRING_PASSPHRASE,
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to send to."},
          {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1"},
          {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A comment used to store what the transaction is for.\n"
  "                             This is not part of the transaction, just kept in your wallet."},
          {"comment_to", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A comment to store the name of the person or organization\n"
  "                             to which you're sending the transaction. This is not part of the \n"
  "                             transaction, just kept in your wallet."},
          {"subtractfeefromamount", RPCArg::Type::BOOL, RPCArg::Default{false}, "The fee will be deducted from the amount being sent.\n"
  "                             The recipient will receive less coins than you enter in the amount field."},
          {"replaceable", RPCArg::Type::BOOL, RPCArg::DefaultHint{"fallback to wallet's default"}, "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
      },
          RPCResult {RPCResult::Type::STR_HEX, "", "the transaction ID"},
          RPCExamples{
              HelpExampleCli ("sendtoname", "\"id/foobar\" 0.1")
      + HelpExampleCli ("sendtoname", "\"id/foobar\" 0.1 \"donation\" \"seans outpost\"")
      + HelpExampleCli ("sendtoname", "\"id/foobar\" 0.1 \"\" \"\" true")
      + HelpExampleRpc ("sendtoname", "\"id/foobar\", 0.1, \"donation\", \"seans outpost\"")
          },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest (request);
  if (!pwallet)
    return NullUniValue;
  const auto& chainman = EnsureChainman (EnsureAnyNodeContext (request));

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain();

  LOCK(pwallet->cs_wallet);

  /* sendtoname does not support an options argument (e.g. to override the
     configured name/value encodings).  That would just add to the already
     long list of rarely used arguments.  Also, this function is inofficially
     deprecated anyway, see
     https://github.com/namecoin/namecoin-core/issues/12.  */
  const UniValue NO_OPTIONS(UniValue::VOBJ);

  const valtype name = DecodeNameFromRPCOrThrow (request.params[0], NO_OPTIONS);

  CNameData data;
  {
    LOCK (cs_main);
    if (!chainman.ActiveChainstate ().CoinsTip ().GetName (name, data))
      {
        std::ostringstream msg;
        msg << "name not found: " << EncodeNameForMessage (name);
        throw JSONRPCError (RPC_INVALID_ADDRESS_OR_KEY, msg.str ());
      }
  }
  if (data.isExpired (chainman.ActiveHeight ()))
    throw JSONRPCError (RPC_INVALID_ADDRESS_OR_KEY, "the name is expired");

  /* The code below is strongly based on sendtoaddress.  Make sure to
     keep it in sync.  */

  // Wallet comments
  mapValue_t mapValue;
  if (request.params.size() > 2 && !request.params[2].isNull()
        && !request.params[2].get_str().empty())
      mapValue["comment"] = request.params[2].get_str();
  if (request.params.size() > 3 && !request.params[3].isNull()
        && !request.params[3].get_str().empty())
      mapValue["to"] = request.params[3].get_str();

  bool fSubtractFeeFromAmount = false;
  if (!request.params[4].isNull())
      fSubtractFeeFromAmount = request.params[4].get_bool();

  CCoinControl coin_control;
  if (!request.params[5].isNull()) {
      coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
  }

  EnsureWalletIsUnlocked(*pwallet);

  std::vector<CRecipient> recipients;
  const CNoDestination dest(data.getAddress ());
  const CAmount amount = AmountFromValue (request.params[1]);
  recipients.push_back ({dest, amount, fSubtractFeeFromAmount});

  return SendMoney(*pwallet, coin_control, nullptr, recipients, mapValue, false);
}
  };
}

} // namespace wallet
