// Copyright (c) 2014-2020 Daniel Kraft
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <base58.h>
#include <coins.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <init.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <names/common.h>
#include <names/encoding.h>
#include <names/main.h>
#include <names/mempool.h>
#include <node/context.h>
#include <net.h>
#include <primitives/transaction.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <rpc/names.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/names.h>
#include <txmempool.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/translation.h>
#include <util/vector.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <algorithm>
#include <memory>

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
   * Returns the script that should be used as destination.
   */
  CScript getScript ();

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

CScript DestinationAddressHelper::getScript ()
{
  if (overrideDest != nullptr)
    return GetScriptForDestination (*overrideDest);

  rdest.reset (new ReserveDestination (&wallet, wallet.m_default_address_type));
  CTxDestination dest;
  if (!rdest->GetReservedDestination (dest, false))
    throw JSONRPCError (RPC_WALLET_KEYPOOL_RAN_OUT,
                        "Error: Keypool ran out,"
                        " please call keypoolrefill first");

  return GetScriptForDestination (dest);
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
                CWallet& wallet, const CScript& nameOutScript,
                const CTxIn* nameInput, const UniValue& opt)
{
  RPCTypeCheckObj (opt,
    {
      {"sendCoins", UniValueType (UniValue::VOBJ)},
    },
    true, false);

  auto& node = EnsureNodeContext (request.context);
  if (wallet.GetBroadcastTransactions () && !node.connman)
    throw JSONRPCError (RPC_CLIENT_P2P_DISABLED,
                        "Error: Peer-to-peer functionality missing"
                        " or disabled");

  std::vector<CRecipient> vecSend;
  vecSend.push_back ({nameOutScript, NAME_LOCKED_AMOUNT, false});

  if (opt.exists ("sendCoins"))
    for (const std::string& addr : opt["sendCoins"].getKeys ())
      {
        const CTxDestination dest = DecodeDestination (addr);
        if (!IsValidDestination (dest))
          throw JSONRPCError (RPC_INVALID_ADDRESS_OR_KEY,
                              "Invalid address: " + addr);

        const CAmount nAmount = AmountFromValue (opt["sendCoins"][addr]);
        if (nAmount <= 0)
          throw JSONRPCError (RPC_TYPE_ERROR, "Invalid amount for send");

        vecSend.push_back ({GetScriptForDestination (dest), nAmount, false});
      }

  CCoinControl coinControl;
  return SendMoney (&wallet, coinControl, nameInput, vecSend, {});
}

} // anonymous namespace
/* ************************************************************************** */

UniValue
name_list (const JSONRPCRequest& request)
{
  NameOptionsHelp optHelp;
  optHelp
      .withNameEncoding ()
      .withValueEncoding ();

  RPCHelpMan ("name_list",
      "\nShows the status of all names in the wallet.\n",
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Only include this name"},
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
      }
  ).Check (request);

  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  RPCTypeCheck (request.params, {UniValue::VSTR, UniValue::VOBJ}, true);

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

  const int tipHeight = ::ChainActive ().Height ();
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
                LogPrintf ("ERROR: wallet contains tx with multiple"
                           " name outputs");
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

      const int depth = tx.GetDepthInMainChain ();
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
      addExpirationInfo (height, obj);

      mapHeights[name] = height;
      mapObjects[name] = obj;
    }
  }

  UniValue res(UniValue::VARR);
  for (const auto& item : mapObjects)
    res.push_back (item.second);

  return res;
}

/* ************************************************************************** */

UniValue
name_new (const JSONRPCRequest& request)
{
  NameOptionsHelp optHelp;
  optHelp
      .withNameEncoding ()
      .withWriteOptions ()
      .withArg ("allowExisting", RPCArg::Type::BOOL, "false",
                "If set, then the name_new is sent even if the name exists already");

  RPCHelpMan ("name_new",
      "\nStarts registration of the given name.  Must be followed up with name_firstupdate to finish the registration."
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
      }
  ).Check (request);

  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  RPCTypeCheck (request.params, {UniValue::VSTR, UniValue::VOBJ});

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
      LOCK (cs_main);
      CNameData oldData;
      const auto& coinsTip = ::ChainstateActive ().CoinsTip ();
      if (coinsTip.GetName (name, oldData) && !oldData.isExpired ())
        throw JSONRPCError (RPC_TRANSACTION_ERROR, "this name exists already");
    }

  valtype rand(20);
  GetRandBytes (&rand[0], rand.size ());

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  LOCK (pwallet->cs_wallet);

  EnsureWalletIsUnlocked (pwallet);

  DestinationAddressHelper destHelper(*pwallet);
  destHelper.setOptions (options);

  const CScript newScript
      = CNameScript::buildNameNew (destHelper.getScript (), name, rand);

  const UniValue txidVal
      = SendNameOutput (request, *pwallet, newScript, nullptr, options);
  destHelper.finalise ();

  const std::string randStr = HexStr (rand);
  const std::string txid = txidVal.get_str ();
  LogPrintf ("name_new: name=%s, rand=%s, tx=%s\n",
             EncodeNameForMessage (name), randStr.c_str (), txid.c_str ());

  UniValue res(UniValue::VARR);
  res.push_back (txid);
  res.push_back (randStr);

  return res;
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
getNamePrevout (const uint256& txid, CTxOut& txOut, CTxIn& txIn)
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

      Coin coin;
      if (!::ChainstateActive ().CoinsTip ().GetCoin (outp, coin))
        continue;

      if (!coin.out.IsNull ()
          && CNameScript::isNameScript (coin.out.scriptPubKey))
        {
          txOut = coin.out;
          txIn = CTxIn (outp);
          return true;
        }
    }

  return false;
}

}  // anonymous namespace

UniValue
name_firstupdate (const JSONRPCRequest& request)
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

  /* We can not use RPCHelpMan::Check here, as we have that "hidden" sixth
     argument for "allow active" names (in tests).  */
  if (request.fHelp || request.params.size () < 4 || request.params.size () > 6)
    throw std::runtime_error (
        RPCHelpMan ("name_firstupdate",
            "\nFinishes the registration of a name.  Depends on name_new being already issued."
                + HELP_REQUIRING_PASSPHRASE,
            {
                {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to register"},
                {"rand", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The rand value of name_new"},
                {"tx", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The name_new txid"},
                {"value", RPCArg::Type::STR, RPCArg::Optional::NO, "Value for the name"},
                optHelp.buildRpcArg (),
            },
            RPCResult {RPCResult::Type::STR_HEX, "", "the transaction ID"},
            RPCExamples {
                HelpExampleCli ("name_firstupdate", "\"myname\", \"555844f2db9c7f4b25da6cb8277596de45021ef2\" \"a77ceb22aa03304b7de64ec43328974aeaca211c37dd29dcce4ae461bb80ca84\", \"my-value\"")
              + HelpExampleRpc ("name_firstupdate", "\"myname\", \"555844f2db9c7f4b25da6cb8277596de45021ef2\" \"a77ceb22aa03304b7de64ec43328974aeaca211c37dd29dcce4ae461bb80ca84\", \"my-value\"")
            }
        ).ToString ());

  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  RPCTypeCheck (request.params,
                {UniValue::VSTR, UniValue::VSTR, UniValue::VSTR, UniValue::VSTR,
                 UniValue::VOBJ});

  UniValue options(UniValue::VOBJ);
  if (request.params.size () >= 5)
    options = request.params[4].get_obj ();

  const valtype name = DecodeNameFromRPCOrThrow (request.params[0], options);
  if (name.size () > MAX_NAME_LENGTH)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the name is too long");

  const valtype rand = ParseHexV (request.params[1], "rand");
  if (rand.size () > 20)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "invalid rand value");

  const uint256 prevTxid = ParseHashV (request.params[2], "txid");

  const valtype value = DecodeValueFromRPCOrThrow (request.params[3], options);
  if (value.size () > MAX_VALUE_LENGTH_UI)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the value is too long");

  {
    LOCK (mempool.cs);
    if (mempool.registersName (name))
      throw JSONRPCError (RPC_TRANSACTION_ERROR,
                          "this name is already being registered");
  }

  if (request.params.size () < 6 || !request.params[5].get_bool ())
    {
      LOCK (cs_main);
      CNameData oldData;
      const auto& coinsTip = ::ChainstateActive ().CoinsTip ();
      if (coinsTip.GetName (name, oldData) && !oldData.isExpired ())
        throw JSONRPCError (RPC_TRANSACTION_ERROR,
                            "this name is already active");
    }

  CTxOut prevOut;
  CTxIn txIn;
  {
    LOCK (cs_main);
    if (!getNamePrevout (prevTxid, prevOut, txIn))
      throw JSONRPCError (RPC_TRANSACTION_ERROR, "previous txid not found");
  }

  const CNameScript prevNameOp(prevOut.scriptPubKey);
  assert (prevNameOp.isNameOp ());
  if (prevNameOp.getNameOp () != OP_NAME_NEW)
    throw JSONRPCError (RPC_TRANSACTION_ERROR, "previous tx is not name_new");

  valtype toHash(rand);
  toHash.insert (toHash.end (), name.begin (), name.end ());
  if (uint160 (prevNameOp.getOpHash ()) != Hash160 (toHash))
    throw JSONRPCError (RPC_TRANSACTION_ERROR, "rand value is wrong");

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  LOCK (pwallet->cs_wallet);

  EnsureWalletIsUnlocked (pwallet);

  DestinationAddressHelper destHelper(*pwallet);
  destHelper.setOptions (options);

  const CScript nameScript
    = CNameScript::buildNameFirstupdate (destHelper.getScript (), name, value,
                                         rand);

  const UniValue txidVal
      = SendNameOutput (request, *pwallet, nameScript, &txIn, options);
  destHelper.finalise ();

  return txidVal;
}

/* ************************************************************************** */

UniValue
name_update (const JSONRPCRequest& request)
{
  NameOptionsHelp optHelp;
  optHelp
      .withNameEncoding ()
      .withValueEncoding ()
      .withWriteOptions ();

  RPCHelpMan ("name_update",
      "\nUpdates a name and possibly transfers it."
          + HELP_REQUIRING_PASSPHRASE,
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to update"},
          {"value", RPCArg::Type::STR, RPCArg::Optional::NO, "Value for the name"},
          optHelp.buildRpcArg (),
      },
      RPCResult {RPCResult::Type::STR_HEX, "", "the transaction ID"},
      RPCExamples {
          HelpExampleCli ("name_update", "\"myname\", \"new-value\"")
        + HelpExampleRpc ("name_update", "\"myname\", \"new-value\"")
      }
  ).Check (request);

  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  RPCTypeCheck (request.params,
                {UniValue::VSTR, UniValue::VSTR, UniValue::VOBJ});

  UniValue options(UniValue::VOBJ);
  if (request.params.size () >= 3)
    options = request.params[2].get_obj ();

  const valtype name = DecodeNameFromRPCOrThrow (request.params[0], options);
  if (name.size () > MAX_NAME_LENGTH)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the name is too long");

  const valtype value = DecodeValueFromRPCOrThrow (request.params[1], options);
  if (value.size () > MAX_VALUE_LENGTH_UI)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "the value is too long");

  /* For finding the name output to spend, we first check if there are
     pending operations on the name in the mempool.  If there are, then we
     build upon the last one to get a valid chain.  If there are none, then we
     look up the last outpoint from the name database instead.  */

  const unsigned chainLimit = gArgs.GetArg ("-limitnamechains",
                                            DEFAULT_NAME_CHAIN_LIMIT);
  COutPoint outp;
  {
    LOCK (mempool.cs);

    const unsigned pendingOps = mempool.pendingNameChainLength (name);
    if (pendingOps >= chainLimit)
      throw JSONRPCError (RPC_TRANSACTION_ERROR,
                          "there are already too many pending operations"
                          " on this name");

    if (pendingOps > 0)
      outp = mempool.lastNameOutput (name);
  }

  if (outp.IsNull ())
    {
      LOCK (cs_main);

      CNameData oldData;
      const auto& coinsTip = ::ChainstateActive ().CoinsTip ();
      if (!coinsTip.GetName (name, oldData) || oldData.isExpired ())
        throw JSONRPCError (RPC_TRANSACTION_ERROR,
                            "this name can not be updated");

      outp = oldData.getUpdateOutpoint ();
    }

  assert (!outp.IsNull ());
  const CTxIn txIn(outp);

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  LOCK (pwallet->cs_wallet);

  EnsureWalletIsUnlocked (pwallet);

  DestinationAddressHelper destHelper(*pwallet);
  destHelper.setOptions (options);

  const CScript nameScript
    = CNameScript::buildNameUpdate (destHelper.getScript (), name, value);

  const UniValue txidVal
      = SendNameOutput (request, *pwallet, nameScript, &txIn, options);
  destHelper.finalise ();

  return txidVal;
}

/* ************************************************************************** */

UniValue
queuerawtransaction (const JSONRPCRequest& request)
{
  // TODO: Use NameOptionsHelp for name encoding.  Requires making
  // NameOptionsHelp support OBJ args.
  RPCHelpMan ("queuerawtransaction",
      "\nQueue a transaction for future broadcast.",
      {
          {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The hex string of the raw transaction"},
          {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "JSON with options",
              {
                  {"sendWhen", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Conditions upon which the transaction will be broadcast",
                      {
                          {"txid", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Transaction ID to watch for confirmation"},
                          {"name", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Name to watch for confirmation (most recent update)"},
                          {"confirmations", RPCArg::Type::NUM, RPCArg::Optional::NO, "Queued transaction will be broadcast when the txid or name has this many confirmations"},
                      },
                  },
              },
          },
      },
      RPCResult {RPCResult::Type::STR_HEX, "", "the transaction ID"},
      RPCExamples {
          "\nRegistration: queue a name_firstupdate txhex when a name_new txid is mature\n"
        + HelpExampleCli("queuerawtransaction", "txhex '{ \"sendWhen\": { \"txid\": \"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\", \"confirmations\": 12 } }'") +
          "\nRenewal: queue a name_update txhex when a name_anyupdate txid has ~2 months remaining\n"
        + HelpExampleRpc("queuerawtransaction", "txhex, { \"sendWhen\": { \"txid\": \"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\", \"confirmations\": 27360 } } ") +
          "\nSnipe: queue a name_firstupdate txhex when a name has expired\n"
        + HelpExampleRpc("queuerawtransaction", "txhex, { \"sendWhen\": { \"name\": \"d/example\", \"confirmations\": 35999 } } ")
      }
  ).Check (request);

  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet) return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  RPCTypeCheck (request.params,
                {UniValue::VSTR, UniValue::VOBJ});

  // parse transaction from parameter
  // we only use this for error checking and returning a txid
  CMutableTransaction mtxParsed;
  if (!DecodeHexTx(mtxParsed, request.params[0].get_str()))
    throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
  CTransactionRef txParsed(MakeTransactionRef(std::move(mtxParsed)));
  const uint256& hashTx = txParsed->GetHash();

  // No need to catch InvalidNameString here since we already verified above
  // that it's valid hex.
  const valtype tx = DecodeName (request.params[0].get_str (), NameEncoding::HEX);

  UniValue options(UniValue::VOBJ);
  if (request.params.size () >= 2)
    options = request.params[1].get_obj ();

  RPCTypeCheckObj (options,
    {
      {"sendWhen", UniValueType (UniValue::VOBJ)},
    },
    true, false);

  UniValue sendWhen(UniValue::VOBJ);
  if (options.exists ("sendWhen"))
    sendWhen = options["sendWhen"].get_obj ();

  RPCTypeCheckObj (sendWhen,
    {
      {"txid", UniValueType (UniValue::VSTR)},
      {"name", UniValueType (UniValue::VSTR)},
      {"confirmations", UniValueType (UniValue::VNUM)},
    },
    true, false);

  uint256 triggerTxid;
  if (sendWhen.exists ("txid"))
    triggerTxid = ParseHashV (sendWhen["txid"], "sendWhen txid");

  valtype triggerName;
  if (sendWhen.exists ("name"))
    triggerName = DecodeNameFromRPCOrThrow (sendWhen["name"], options);

  int32_t triggerDepth = 0;
  if (sendWhen.exists ("confirmations"))
    triggerDepth = sendWhen["confirmations"].get_int();

  LOCK2 (cs_main, pwallet->cs_wallet);

  if (!pwallet->WriteQueuedTransaction(hashTx.GetHex(), tx, triggerTxid, triggerName, triggerDepth))
  {
    throw JSONRPCError (RPC_WALLET_ERROR, "Error queueing transaction");
  }

  return hashTx.GetHex();
}

/* ************************************************************************** */

UniValue
listqueuedtransactions (const JSONRPCRequest& request)
{
  RPCHelpMan{"listqueuedtransactions",
      "\nList the transactions that are queued for future broadcast.\n",
      {
      },
      RPCResult{
          RPCResult::Type::OBJ_DYN, "", "JSON object with transaction ID's as keys",
          {
              {RPCResult::Type::OBJ, "", "",
              {
                  {RPCResult::Type::STR_HEX, "transaction", "The hex string of the raw transaction."},
                  {RPCResult::Type::OBJ, "sendWhen", "Conditions upon which the transaction will be broadcast.",
                  {
                      {RPCResult::Type::STR_HEX, "txid", "Transaction ID to watch for confirmation."},
                      {RPCResult::Type::STR, "name", "Name to watch for confirmation (most recent update)."},
                      {RPCResult::Type::NUM, "confirmations", "Queued transaction will be broadcast when the txid or name has this many confirmations."},
                  }},
              }},
          }
      },
      RPCExamples{
          HelpExampleCli("listqueuedtransactions", "") +
          HelpExampleRpc("listqueuedtransactions", "")
      },
  }.Check(request);

  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet) return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  if (request.fHelp || request.params.size () > 0)
    throw std::runtime_error (
        "listqueuedtransactions\n"
        "\nList the transactions that are queued for future broadcast.\n"
      );

  LOCK2 (cs_main, pwallet->cs_wallet);

  UniValue result(UniValue::VOBJ);

  for (const auto& i : wallet->queuedTransactionMap)
  {
    const auto& npd = i.second;
    const std::string& txidHex = i.first;
    const uint256& triggerTxid = npd.getTriggerTxid();
    const valtype& triggerName = npd.getTriggerName();
    int32_t triggerDepth = npd.getTriggerDepth();
    const valtype& tx = npd.getTx();

    const std::string triggerTxidStr = triggerTxid.GetHex();
    const std::string triggerNameStr = EncodeName (triggerName, ConfiguredNameEncoding ());
    const std::string txStr = EncodeName (tx, NameEncoding::HEX);

    UniValue sendWhen(UniValue::VOBJ);
    if (triggerTxid.IsNull())
    {
        sendWhen.pushKV("txid", NullUniValue);
    }
    else {
        sendWhen.pushKV("txid", triggerTxidStr);
    }
    // TODO: Return NullUniValue if trigger name isn't set
    sendWhen.pushKV("name", triggerNameStr);
    sendWhen.pushKV("confirmations", triggerDepth);

    UniValue entry(UniValue::VOBJ);
    entry.pushKV("sendWhen", sendWhen);
    entry.pushKV("transaction", txStr);

    result.pushKV(txidHex, entry);
  }

  return result;
}

/* ************************************************************************** */

UniValue
sendtoname (const JSONRPCRequest& request)
{
  RPCHelpMan{"sendtoname",
      "\nSend an amount to the owner of a name.\n"
      "\nIt is an error if the name is expired."
          + HELP_REQUIRING_PASSPHRASE,
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to send to."},
          {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1"},
          {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment used to store what the transaction is for.\n"
  "                             This is not part of the transaction, just kept in your wallet."},
          {"comment_to", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment to store the name of the person or organization\n"
  "                             to which you're sending the transaction. This is not part of the \n"
  "                             transaction, just kept in your wallet."},
          {"subtractfeefromamount", RPCArg::Type::BOOL, /* default */ "false", "The fee will be deducted from the amount being sent.\n"
  "                             The recipient will receive less coins than you enter in the amount field."},
          {"replaceable", RPCArg::Type::BOOL, /* default */ "fallback to wallet's default", "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
      },
          RPCResult {RPCResult::Type::STR_HEX, "", "the transaction ID"},
          RPCExamples{
              HelpExampleCli ("sendtoname", "\"id/foobar\" 0.1")
      + HelpExampleCli ("sendtoname", "\"id/foobar\" 0.1 \"donation\" \"seans outpost\"")
      + HelpExampleCli ("sendtoname", "\"id/foobar\" 0.1 \"\" \"\" true")
      + HelpExampleRpc ("sendtoname", "\"id/foobar\", 0.1, \"donation\", \"seans outpost\"")
          },
      }.Check (request);

  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  if (::ChainstateActive ().IsInitialBlockDownload ())
    throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD,
                       "Namecoin is downloading blocks...");

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
  if (!::ChainstateActive ().CoinsTip ().GetName (name, data))
    {
      std::ostringstream msg;
      msg << "name not found: " << EncodeNameForMessage (name);
      throw JSONRPCError (RPC_INVALID_ADDRESS_OR_KEY, msg.str ());
    }
  if (data.isExpired ())
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

  EnsureWalletIsUnlocked(pwallet);

  std::vector<CRecipient> recipients;
  const CAmount amount = AmountFromValue (request.params[1]);
  recipients.push_back ({data.getAddress (), amount, fSubtractFeeFromAmount});

  return SendMoney(pwallet, coin_control, nullptr, recipients, mapValue);
}
