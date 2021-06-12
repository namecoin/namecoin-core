// Copyright (c) 2014-2021 Daniel Kraft
// Copyright (c) 2021 yanmaani
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <base58.h>
#include <coins.h>
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
#include <net.h>
#include <primitives/transaction.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <rpc/names.h>
#include <rpc/net.h>
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
#include <wallet/rpcnames.h>
#include <wallet/rpcwallet.h>
#include <wallet/scriptpubkeyman.h>
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

  auto& node = EnsureAnyNodeContext (request.context);
  if (wallet.GetBroadcastTransactions ())
    EnsureConnman (node);

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
  return SendMoney (wallet, coinControl, nameInput, vecSend, {}, false);
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
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  RPCTypeCheck (request.params, {UniValue::VSTR, UniValue::VOBJ}, true);
  const auto& chainman = EnsureAnyChainman (request.context);

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
    const valtype ikm(key.begin(), key.end());
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

    const auto* spk = pwallet->GetScriptPubKeyMan (output);
    if (spk == nullptr)
        return false;

    auto provider = spk->GetSigningProviderWithKeys (output);
    if (provider == nullptr)
        return false;

    CTxDestination dest;
    CKeyID keyid;
    CKey key;
    if (!ExtractDestination(output, dest))
        return false; // If multisig.
    assert(IsValidDestination(dest)); // We should never get a null destination.

    keyid = GetKeyForDestination(*provider, dest);
    provider->GetKey(keyid, key);

    return getNameSalt(key, name, rand);
}

bool
saltMatchesHash(const valtype& name, const valtype& rand, const valtype& expectedHash)
{
    valtype toHash(rand);
    toHash.insert (toHash.end(), name.begin(), name.end());

    return (Hash160(toHash) == uint160(expectedHash));
}

bool existsName(const valtype& name, const ChainstateManager& chainman)
{
    LOCK(cs_main);
    const auto& coinsTip = chainman.ActiveChainstate().CoinsTip();
    CNameData oldData;
    return (coinsTip.GetName(name, oldData) && !oldData.isExpired());
}

bool existsName(const std::string& name, const ChainstateManager& chainman)
{
    return existsName(valtype(name.begin(), name.end()), chainman);
}

} // anonymous namespace

/* ************************************************************************** */

RPCHelpMan
name_autoregister()
{
    NameOptionsHelp optHelp;
    optHelp
        .withNameEncoding()
        .withWriteOptions()
        .withArg("allowExisting", RPCArg::Type::BOOL, "false",
                 "If set, then the name_new is sent even if the name exists already")
        .withArg("delegate", RPCArg::Type::BOOL, "false",
                 "If set, register a dd/ or idd/ name and delegate the name there. Name must be in d/ or id/ namespace.");

    return RPCHelpMan("name_autoregister",
        "\nAutomatically registers the given name; performs the first half and queues the second."
            + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to register"},
            {"value", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Value for the name"},
            optHelp.buildRpcArg(),
        },
        RPCResult {RPCResult::Type::ARR_FIXED, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "the txid, used in name_firstupdate"},
                {RPCResult::Type::STR_HEX, "rand", "random value, as in name_firstupdate"},
            },
        },
        RPCExamples {
            HelpExampleCli("name_autoregister", "\"myname\"")
            + HelpExampleRpc("name_autoregister", "\"myname\"")
        },
        [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet)
        return NullUniValue;
    CWallet* const pwallet = wallet.get();

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VSTR, UniValue::VOBJ});
    const auto& chainman = EnsureAnyChainman(request.context);

    // build a name_new
    // broadcast it
    // build a name_fu with nSequence
    // queue it
    // return the txid and rand from name_new, as an array
    //
    // if delegate=true:
    //   build nn1 = d/something
    //   build nn2 = dd/something (if already exist, something else)
    //   broadcast nn1,nn2
    //   build nfu1 = "d/something points to dd/xxx"
    //   build nfu2 = "d/xxx is the value"
    //   queue nfu1,nfu2
    //   return txid from nn1

    UniValue options(UniValue::VOBJ);
    if (request.params.size () >= 3)
        options = request.params[2].get_obj();
    RPCTypeCheckObj(options,
        {
            {"allowExisting", UniValueType(UniValue::VBOOL)},
            {"delegate", UniValueType(UniValue::VBOOL)},
        },
        true, false);

    const valtype name = DecodeNameFromRPCOrThrow(request.params[0], options);
    if (name.size() > MAX_NAME_LENGTH)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "the name is too long");

    const bool isDefaultVal = (request.params.size() < 2 || request.params[1].isNull());
    const valtype value = isDefaultVal ?
        valtype():
        DecodeValueFromRPCOrThrow(request.params[1], options);

    if (value.size() > MAX_VALUE_LENGTH_UI)
      throw JSONRPCError(RPC_INVALID_PARAMETER, "the value is too long");

    if (!options["allowExisting"].isTrue())
    {
        LOCK(cs_main);
        if (existsName(name, chainman))
            throw JSONRPCError(RPC_TRANSACTION_ERROR, "this name exists already");
    }

    // TODO: farm out to somewhere else for namespace parsing

    bool isDelegated = options["delegate"].isTrue();
    std::string delegatedName;
    std::string delegatedValue;

    if (isDelegated)
    {
        bool isDomain;
        bool isIdentity;
        std::string nameStr(name.begin(), name.end());
        isDomain   = nameStr.rfind("d/", 0) == 0;
        isIdentity = nameStr.rfind("id/", 0) == 0;

        if (!isDomain && !isIdentity)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "delegation requested, but name neither d/ nor id/");

        assert(!(isDomain && isIdentity));

        size_t slashIdx = nameStr.find_first_of('/');
        assert(slashIdx != std::string::npos);

        std::string mainLabel = nameStr.substr(slashIdx, std::string::npos);

        std::string prefix = isDomain ? "dd" : "idd";

        std::string suffix("");

        // Attempt to generate name like dd/name, dd/name5, dd/name73
        do {
            delegatedName = prefix + mainLabel + suffix;
            valtype rand(1);
            GetRandBytes(&rand[0], rand.size());
            suffix += std::string(1, '0' + (rand[0] % 10));
        } while (existsName(delegatedName, chainman) && delegatedName.size() <= MAX_NAME_LENGTH);

        // Fallback. This could happen if the base name is 254 characters, for instance
        while (existsName(delegatedName, chainman) || delegatedName.size() > MAX_NAME_LENGTH) {
            // Attempt to generate name like dd/f7f5fdbd
            valtype rand(4);
            GetRandBytes(&rand[0], rand.size());
            delegatedName = strprintf("%s/%hh02x%hh02x%hh02x%hh02x", prefix, rand[0], rand[1], rand[2], rand[3]);
            // TODO: Escape properly for JSON.
        }

        delegatedValue = strprintf("{\"import\":\"%s\"}", delegatedName);
    }

    UniValue res(UniValue::VARR);

    /* Make sure the results are valid at least up to the most recent block
       the user could have gotten from another RPC command prior to now.  */
    pwallet->BlockUntilSyncedToCurrentChain();

    auto issue_nn = [&] (const valtype name, bool push) {
        LOCK(pwallet->cs_wallet);
        EnsureWalletIsUnlocked(*pwallet);

        DestinationAddressHelper destHelper(*pwallet);
        destHelper.setOptions(options);

        const CScript output = destHelper.getScript();

        valtype rand(20);
        if (!getNameSalt(pwallet, name, output, rand))
            GetRandBytes(&rand[0], rand.size());

        const CScript newScript
            = CNameScript::buildNameNew(output, name, rand);

        const UniValue txidVal
            = SendNameOutput(request, *pwallet, newScript, nullptr, options);
        destHelper.finalise();

        const std::string randStr = HexStr(rand);
        const std::string txid = txidVal.get_str();
        LogPrintf("name_new: name=%s, rand=%s, tx=%s\n",
                 EncodeNameForMessage(name), randStr.c_str(), txid.c_str());

        if (push) {
            res.push_back(txid);
            res.push_back(randStr);
        }

        return std::make_pair(uint256S(txid), rand);
    };

    auto queue_nfu = [&] (const valtype name, const valtype value, const auto info) {
        LOCK(pwallet->cs_wallet);
        const int TWELVE_PLUS_ONE = 13;

        uint256 txid = info.first;
        valtype rand = info.second;

        const CWalletTx* wtx = pwallet->GetWalletTx(txid);
        CTxIn txIn;
        for (unsigned int i = 0; i < wtx->tx->vout.size(); i++)
            if (CNameScript::isNameScript(wtx->tx->vout[i].scriptPubKey))
                txIn = CTxIn(COutPoint(txid, i), wtx->tx->vout[i].scriptPubKey, /* nSequence */ TWELVE_PLUS_ONE);
                // nSequence = 13 => only broadcast name_firstupdate when name_new is mature (12 blocks)
                // Note: nSequence is basically ornamental here, see comment below

        EnsureWalletIsUnlocked(*pwallet);

        DestinationAddressHelper destHelper(*pwallet);
        destHelper.setOptions(options);

        // if delegated, use delegationValue for value, and use value in dd/ name
        const CScript nameScript
            = CNameScript::buildNameFirstupdate(destHelper.getScript(), name, value, rand);

        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;
        bilingual_str error;
        CTransactionRef tx;
        FeeCalculation fee_calc_out;

        CCoinControl coin_control;

        std::vector<CRecipient> recipients;
        recipients.push_back({nameScript, NAME_LOCKED_AMOUNT, false});

        const bool created_ok = pwallet->CreateTransaction(recipients, &txIn, tx, nFeeRequired, nChangePosRet, error, coin_control, fee_calc_out, /* sign */ false);
        if (!created_ok)
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, error.original);
            // Not sure if this can ever happen.

        // No need to sign; sigature will be discarded.

        // Kludge: Since CreateTransaction discards nSequence of txIn, manually add it back in again.

        CMutableTransaction mtx(*tx);

        for (unsigned int i = 0; i < mtx.vin.size(); i++)
            if (mtx.vin[i].prevout == txIn.prevout)
                mtx.vin[i].nSequence = TWELVE_PLUS_ONE;

        // Sign it for real
        bool complete = pwallet->SignTransaction(mtx);
        if (!complete)
            throw JSONRPCError(RPC_WALLET_ERROR, "Error signing transaction");
            // This should never happen.

        // TODO: Mark all inputs unspendable.

        const bool queued_ok = pwallet->WriteQueuedTransaction(mtx.GetHash(), mtx);
        if (!queued_ok)
            throw JSONRPCError(RPC_WALLET_ERROR, "Error queueing transaction");

        destHelper.finalise();
    };

    auto info = issue_nn(name, true);
    if (isDelegated) {
        auto info2 = issue_nn(valtype(delegatedName.begin(), delegatedName.end()), false);
        queue_nfu(name, valtype(delegatedValue.begin(), delegatedValue.end()), info);
        queue_nfu(valtype(delegatedName.begin(), delegatedName.end()), value, info2);
    }
    else {
        queue_nfu(name, value, info);
    }

    return res;
}
  );
}

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
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();

  RPCTypeCheck (request.params, {UniValue::VSTR, UniValue::VOBJ});
  const auto& chainman = EnsureAnyChainman (request.context);

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

  if (!options["allowExisting"].isTrue () &&
      existsName (name, chainman))
    throw JSONRPCError (RPC_TRANSACTION_ERROR, "this name exists already");

  /* Make sure the results are valid at least up to the most recent block
     the user could have gotten from another RPC command prior to now.  */
  pwallet->BlockUntilSyncedToCurrentChain ();

  LOCK (pwallet->cs_wallet);

  EnsureWalletIsUnlocked (*pwallet);

  DestinationAddressHelper destHelper(*pwallet);
  destHelper.setOptions (options);

  const CScript output = destHelper.getScript ();

  valtype rand(20);
  if (!getNameSalt (pwallet, name, output, rand))
      GetRandBytes (&rand[0], rand.size ());

  const CScript newScript
      = CNameScript::buildNameNew (output, name, rand);

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
            "\nFinishes the registration of a name.  Depends on name_new being already issued."
                + HELP_REQUIRING_PASSPHRASE,
            {
                {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to register"},
                {"rand", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED_NAMED_ARG, "The rand value of name_new"},
                {"tx", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED_NAMED_ARG, "The name_new txid"},
                {"value", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Value for the name"},
                optHelp.buildRpcArg (),
                {"allow_active", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED_NAMED_ARG, "Disable check for the name being active"},
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

  RPCTypeCheck (request.params,
                {UniValue::VSTR, UniValue::VSTR, UniValue::VSTR, UniValue::VSTR,
                 UniValue::VOBJ}, true);
  const auto& node = EnsureAnyNodeContext (request.context);
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

  if ((request.params.size () < 6 || !request.params[5].get_bool ()) &&
      existsName (name, chainman))
    throw JSONRPCError (RPC_TRANSACTION_ERROR,
                        "this name is already active");

  uint256 prevTxid = uint256::ZERO; // if it can't find a txid, force an error
  if (fixedTxid)
    {
      prevTxid = ParseHashV (request.params[2], "txid");
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
                LogPrintf ("ERROR: wallet contains tx with multiple"
                           " name outputs");
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

  if (prevTxid == uint256::ZERO)
    throw JSONRPCError (RPC_TRANSACTION_ERROR, "scan for previous txid failed");

  CTxOut prevOut;
  CTxIn txIn;
  {
    LOCK (cs_main);
    if (!getNamePrevout (prevTxid, prevOut, txIn))
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

  const CScript nameScript
    = CNameScript::buildNameFirstupdate (destHelper.getScript (), name, value,
                                         rand);

  const UniValue txidVal
      = SendNameOutput (request, *pwallet, nameScript, &txIn, options);
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
      "\nUpdates a name and possibly transfers it."
          + HELP_REQUIRING_PASSPHRASE,
      {
          {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name to update"},
          {"value", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Value for the name"},
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

  RPCTypeCheck (request.params,
                {UniValue::VSTR, UniValue::VSTR, UniValue::VOBJ}, true);
  const auto& node = EnsureAnyNodeContext (request.context);
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

  const unsigned chainLimit = gArgs.GetArg ("-limitnamechains",
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
      LOCK (cs_main);

      CNameData oldData;
      const auto& coinsTip = chainman.ActiveChainstate ().CoinsTip ();
      if (!coinsTip.GetName (name, oldData) || oldData.isExpired ())
        throw JSONRPCError (RPC_TRANSACTION_ERROR,
                            "this name can not be updated");
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

  const CScript nameScript
    = CNameScript::buildNameUpdate (destHelper.getScript (), name, value);

  const UniValue txidVal
      = SendNameOutput (request, *pwallet, nameScript, &txIn, options);
  destHelper.finalise ();

  return txidVal;
}
  );
}

/* ************************************************************************** */

RPCHelpMan
queuerawtransaction ()
{
  return RPCHelpMan ("queuerawtransaction",
      "\nQueue a transaction for future broadcast.",
      {
          {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The hex string of the raw transaction"},
      },
      RPCResult {RPCResult::Type::STR_HEX, "", "the transaction ID"},
      RPCExamples {
          HelpExampleCli("queuerawtransaction", "txhex") +
          HelpExampleRpc("queuerawtransaction", "txhex")
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet) return NullUniValue;

  RPCTypeCheck (request.params,
                {UniValue::VSTR});

  // parse transaction from parameter
  CMutableTransaction mtxParsed;
  if (!DecodeHexTx(mtxParsed, request.params[0].get_str(), true, true))
    throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
  CTransactionRef txParsed(MakeTransactionRef(mtxParsed));
  const uint256& hashTx = txParsed->GetHash();

  // Validate transaction
  NodeContext& node = EnsureAnyNodeContext(request.context);
  ChainstateManager& chainman = EnsureChainman(node);
  CTxMemPool& mempool = EnsureMemPool(node);

  {
    LOCK (cs_main);
    // Check validity
    MempoolAcceptResult result = AcceptToMemoryPool(chainman.ActiveChainstate(), mempool, txParsed,
      /* bypass_limits */ false, /* test_accept */ true);
    // If it can be broadcast immediately, do that and return early.
    if (result.m_result_type == MempoolAcceptResult::ResultType::VALID)
    {
      std::string unused_err_string;
      // Don't check max fee.
      const TransactionError err = BroadcastTransaction(node, txParsed, unused_err_string,
        /* max_tx_fee */ 0, /* relay */ true, /* wait_callback */ false);
      assert(err == TransactionError::OK);

      return hashTx.GetHex();
    }

    // Otherwise, it's not valid right now.
    if (result.m_state.GetResult() == TxValidationResult::TX_CONSENSUS)
        /* We only want to avoid unconditionally invalid transactions.
         * Blocking e.g. orphan transactions is not desirable. */
      throw JSONRPCError (RPC_WALLET_ERROR, strprintf("Invalid transaction (%s)", result.m_state.GetRejectReason()));
  }

  // After these checks, add it to the queue.
  {
    LOCK (wallet->cs_wallet);
    if (!wallet->WriteQueuedTransaction(hashTx, mtxParsed))
    {
      throw JSONRPCError (RPC_WALLET_ERROR, "Error queueing transaction");
    }
  }

  return hashTx.GetHex();
}
  );
}

/* ************************************************************************** */

RPCHelpMan
dequeuetransaction ()
{
  return RPCHelpMan ("dequeuetransaction",
      "\nRemove a transaction from the queue.",
      {
          {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction ID of the transaction to be dequeued"},
      },
      RPCResult {RPCResult::Type::NONE, "", ""},
      RPCExamples {
          HelpExampleCli("dequeuetransaction", "txid") +
          HelpExampleRpc("dequeuetransaction", "txid")
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet) return NullUniValue;

  RPCTypeCheck (request.params,
                {UniValue::VSTR});

  const uint256& txid = ParseHashV (request.params[0], "txid");

  LOCK (wallet->cs_wallet);

  if (!wallet->EraseQueuedTransaction(txid))
  {
    throw JSONRPCError (RPC_WALLET_ERROR, "Error dequeueing transaction");
  }

  return NullUniValue;
}
  );
}

/* ************************************************************************** */

RPCHelpMan
listqueuedtransactions ()
{
  return RPCHelpMan{"listqueuedtransactions",
      "\nList the transactions that are queued for future broadcast.\n",
      {
      },
      RPCResult{
          RPCResult::Type::OBJ_DYN, "", "JSON object with transaction ID's as keys",
          {
              {RPCResult::Type::OBJ, "", "",
              {
                  {RPCResult::Type::STR_HEX, "transaction", "The hex string of the raw transaction."},
              }},
          }
      },
      RPCExamples{
          HelpExampleCli("listqueuedtransactions", "") +
          HelpExampleRpc("listqueuedtransactions", "")
      },
      [&] (const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet) return NullUniValue;

  LOCK (wallet->cs_wallet);

  UniValue result(UniValue::VOBJ);

  for (const auto& i : wallet->queuedTransactionMap)
  {
    const uint256& txid = i.first;
    const CMutableTransaction& tx = i.second;

    const std::string txStr = EncodeHexTx(CTransaction(tx), RPCSerializationFlags());

    UniValue entry(UniValue::VOBJ);
    entry.pushKV("transaction", txStr);

    result.pushKV(txid.GetHex(), entry);
  }

  return result;
}
  };
}

/* ************************************************************************** */

RPCHelpMan
sendtoname ()
{
  return RPCHelpMan{"sendtoname",
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
  std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest (request);
  if (!wallet)
    return NullUniValue;
  CWallet* const pwallet = wallet.get ();
  const auto& chainman = EnsureAnyChainman (request.context);

  if (chainman.ActiveChainstate ().IsInitialBlockDownload ())
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

  EnsureWalletIsUnlocked(*pwallet);

  std::vector<CRecipient> recipients;
  const CAmount amount = AmountFromValue (request.params[1]);
  recipients.push_back ({data.getAddress (), amount, fSubtractFeeFromAmount});

  return SendMoney(*pwallet, coin_control, nullptr, recipients, mapValue, false);
}
  };
}
