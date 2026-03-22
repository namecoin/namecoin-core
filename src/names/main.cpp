// Copyright (c) 2014-2025 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <names/main.h>

#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <hash.h>
#include <logging.h>
#include <names/encoding.h>
#include <script/interpreter.h>
#include <script/names.h>
#include <txmempool.h>
#include <uint256.h>
#include <undo.h>
#include <util/strencodings.h>
#include <validation.h>

#include <string>
#include <vector>

namespace
{

/**
 * Check whether a name at nPrevHeight is expired at nHeight.  Also
 * heights of MEMPOOL_HEIGHT are supported.  For nHeight == MEMPOOL_HEIGHT,
 * we check at the current best tip's height.
 * @param nPrevHeight The name's output.
 * @param nHeight The height at which it should be updated.
 * @return True iff the name is expired.
 */
bool
isExpired (unsigned nPrevHeight, unsigned nHeight)
{
  assert (nHeight != MEMPOOL_HEIGHT);
  if (nPrevHeight == MEMPOOL_HEIGHT)
    return false;

  const Consensus::Params& params = Params ().GetConsensus ();
  return nPrevHeight + params.rules->NameExpirationDepth (nHeight) <= nHeight;
}

} // anonymous namespace

/* ************************************************************************** */
/* CNameData.  */

bool
CNameData::isExpired (unsigned h) const
{
  return ::isExpired (nHeight, h);
}

/* ************************************************************************** */
/* CNameTxUndo.  */

void
CNameTxUndo::fromOldState (const valtype& nm, const CCoinsView& view)
{
  name = nm;
  isNew = !view.GetName (name, oldData);
}

void
CNameTxUndo::apply (CCoinsViewCache& view) const
{
  if (isNew)
    view.DeleteName (name);
  else
    view.SetName (name, oldData, true);
}

/* ************************************************************************** */

bool
CheckNameTransaction (const CTransaction& tx, unsigned nHeight,
                      const CCoinsView& view,
                      TxValidationState& state, const script_verify_flags flags)
{
  /* Ignore historic bugs.  */
  CChainParams::BugType type;
  if (Params ().IsHistoricBug (tx.GetHash (), nHeight, type))
    return true;

  /* As a first step, try to locate inputs and outputs of the transaction
     that are name scripts.  At most one input and output should be
     a name operation.  */

  int nameIn = -1;
  CNameScript nameOpIn;
  Coin coinIn;
  for (unsigned i = 0; i < tx.vin.size (); ++i)
    {
      const COutPoint& prevout = tx.vin[i].prevout;
      const auto coin = view.GetCoin (prevout);
      if (!coin)
        return state.Invalid (TxValidationResult::TX_MISSING_INPUTS,
                              "bad-txns-inputs-missingorspent",
                              "Failed to fetch name input coin");

      const CNameScript op(coin->out.scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameIn != -1)
            return state.Invalid (TxValidationResult::TX_CONSENSUS,
                                  "tx-multiple-name-inputs",
                                  "Multiple name inputs");
          nameIn = i;
          nameOpIn = op;
          coinIn = *coin;
        }
    }

  int nameOut = -1;
  CNameScript nameOpOut;
  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameOut != -1)
            return state.Invalid (TxValidationResult::TX_CONSENSUS,
                                  "tx-multiple-name-outputs",
                                  "Multiple name outputs");
          nameOut = i;
          nameOpOut = op;
        }
    }

  /* Check that no name inputs/outputs are present for a non-Namecoin tx.
     If that's the case, all is fine.  For a Namecoin tx instead, there
     should be at least an output (for NAME_NEW, no inputs are expected).  */

  if (!tx.IsNamecoin ())
    {
      if (nameIn != -1)
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nonname-with-name-input",
                              "Non-name transaction has name input");
      if (nameOut != -1)
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nonname-with-name-output",
                              "Non-name transaction has name output");

      return true;
    }

  assert (tx.IsNamecoin ());
  if (nameOut == -1)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-name-without-name-output",
                          "Name transaction has no name output");

  /* Reject "greedy names".  */
  const Consensus::Params& params = Params ().GetConsensus ();
  if (tx.vout[nameOut].nValue < params.rules->MinNameCoinAmount(nHeight))
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-name-greedy",
                          "Greedy name operation");

  /* Handle NAME_NEW now, since this is easy and different from the other
     operations.  */

  if (nameOpOut.getNameOp () == OP_NAME_NEW)
    {
      if (nameIn != -1)
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-namenew-with-name-input",
                              "NAME_NEW with previous name input");

      if (nameOpOut.getOpHash ().size () != 20)
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-namenew-wrong-size",
                              "NAME_NEW's hash has the wrong size");

      return true;
    }

  /* Now that we have ruled out NAME_NEW, check that we have a previous
     name input that is being updated.  */

  assert (nameOpOut.isAnyUpdate ());
  if (nameIn == -1)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-nameupdate-without-name-input",
                          "Name update has no previous name input");
  const valtype& name = nameOpOut.getOpName ();

  if (name.size () > MAX_NAME_LENGTH)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-name-invalid",
                          "Invalid name");
  if (nameOpOut.getOpValue ().size () > MAX_VALUE_LENGTH)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-value-invalid",
                          "Invalid value");

  /* Process NAME_UPDATE next.  */

  if (nameOpOut.getNameOp () == OP_NAME_UPDATE)
    {
      if (!nameOpIn.isAnyUpdate ())
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nameupdate-invalid-prev",
                              "Name input for NAME_UPDATE is not an update");

      if (name != nameOpIn.getOpName ())
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nameupdate-name-mismatch",
                              "NAME_UPDATE name mismatch to name input");

      /* If the name input is pending, then no further checks with respect
         to the name input in the name database are done.  Otherwise, we verify
         that the name input matches the name database; this is redundant
         as UTXO handling takes care of it anyway, but we do it for
         an extra safety layer.  */
      const unsigned inHeight = coinIn.nHeight;
      if (inHeight == MEMPOOL_HEIGHT)
        return true;

      CNameData oldName;
      if (!view.GetName (name, oldName))
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nameupdate-nonexistant",
                              "NAME_UPDATE name does not exist");
      if (oldName.isExpired (nHeight))
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nameupdate-expired",
                              "NAME_UPDATE on an expired name");
      assert (inHeight == oldName.getHeight ());
      assert (tx.vin[nameIn].prevout == oldName.getUpdateOutpoint ());

      return true;
    }

  /* Finally, NAME_FIRSTUPDATE.  */

  assert (nameOpOut.getNameOp () == OP_NAME_FIRSTUPDATE);
  if (nameOpIn.getNameOp () != OP_NAME_NEW)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-firstupdate-nonnew-input",
                          "NAME_FIRSTUPDATE input is not a NAME_NEW");

  /* Enforce NAME_NEW maturity unconditionally — including mempool
     admission.  Broadcasting an immature name_firstupdate would reveal
     the name being registered and defeat the anti-frontrunning design.

     If the name_new is still unconfirmed (MEMPOOL_HEIGHT), it is
     definitely not mature.  The wallet's ResubmitWalletTransactions
     will automatically retry broadcast once the name_new has enough
     confirmations.  */
  {
    const unsigned inHeight = static_cast<unsigned> (coinIn.nHeight);
    if (inHeight == MEMPOOL_HEIGHT
        || inHeight + MIN_FIRSTUPDATE_DEPTH > nHeight)
      return state.Invalid (TxValidationResult::TX_PREMATURE_SPEND,
                            "tx-firstupdate-immature",
                            "NAME_FIRSTUPDATE on immature NAME_NEW");
  }

  const bool requireLongSalt
      = static_cast<bool> (flags & SCRIPT_VERIFY_NAMES_LONG_SALT);
  if (requireLongSalt && nameOpOut.getOpRand ().size () < 20)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-firstupdate-invalid-rand",
                          "NAME_FIRSTUPDATE rand value is too short");
  if (nameOpOut.getOpRand ().size () > 20)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-firstupdate-invalid-rand",
                          "NAME_FIRSTUPDATE rand value is too large");

  {
    valtype toHash(nameOpOut.getOpRand ());
    toHash.insert (toHash.end (), name.begin (), name.end ());
    const uint160 hash = Hash160 (toHash);
    if (hash != uint160 (nameOpIn.getOpHash ()))
      return state.Invalid (TxValidationResult::TX_CONSENSUS,
                            "tx-firstupdate-hash-mismatch",
                            "NAME_FIRSTUPDATE mismatch in hash / rand value");
  }

  CNameData oldName;
  if (view.GetName (name, oldName) && !oldName.isExpired (nHeight))
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-firstupdate-existing-name",
                          "NAME_FIRSTUPDATE on existing name");

  /* We don't have to specifically check that miners don't create blocks with
     conflicting NAME_FIRSTUPDATE's, since the mining's CCoinsViewCache
     takes care of this with the check above already.  */

  return true;
}

void
ApplyNameTransaction (const CTransaction& tx, unsigned nHeight,
                      CCoinsViewCache& view, CBlockUndo& undo)
{
  assert (nHeight != MEMPOOL_HEIGHT);

  /* Handle historic bugs that should *not* be applied.  Names that are
     outputs should be marked as unspendable in this case.  Otherwise,
     we get an inconsistency between the UTXO set and the name database.  */
  CChainParams::BugType type;
  const Txid txHash = tx.GetHash ();
  if (Params ().IsHistoricBug (txHash, nHeight, type)
      && type != CChainParams::BUG_FULLY_APPLY)
    {
      if (type == CChainParams::BUG_FULLY_IGNORE)
        for (unsigned i = 0; i < tx.vout.size (); ++i)
          {
            const CNameScript op(tx.vout[i].scriptPubKey);
            if (op.isNameOp () && op.isAnyUpdate ())
              view.SpendCoin (COutPoint (txHash, i));
          }
      return;
    }

  /* This check must be done *after* the historic bug fixing above!  Some
     of the names that must be handled above are actually produced by
     transactions *not* marked as Namecoin tx.  */
  if (!tx.IsNamecoin ())
    return;

  /* Changes are encoded in the outputs.  We don't have to do any checks,
     so simply apply all these.  */

  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp () && op.isAnyUpdate ())
        {
          const valtype& name = op.getOpName ();
          LogDebug (BCLog::NAMES, "Updating name at height %d: %s\n",
                    nHeight, EncodeNameForMessage (name));

          CNameTxUndo opUndo;
          opUndo.fromOldState (name, view);
          undo.vnameundo.push_back (opUndo);

          CNameData data;
          data.fromScript (nHeight, COutPoint (tx.GetHash (), i), op);
          view.SetName (name, data, false);
        }
    }
}

bool
ExpireNames (unsigned nHeight, CCoinsViewCache& view, CBlockUndo& undo,
             std::set<valtype>& names)
{
  names.clear ();

  /* The genesis block contains no name expirations.  */
  if (nHeight == 0)
    return true;

  /* Otherwise, find out at which update heights names have expired
     since the last block.  If the expiration depth changes, this could
     be multiple heights at once.  */

  const Consensus::Params& params = Params ().GetConsensus ();
  const unsigned expDepthOld = params.rules->NameExpirationDepth (nHeight - 1);
  const unsigned expDepthNow = params.rules->NameExpirationDepth (nHeight);

  if (expDepthNow > nHeight)
    return true;

  /* Both are inclusive!  The last expireTo was nHeight - 1 - expDepthOld,
     now we start at this value + 1.  */
  const unsigned expireFrom = nHeight - expDepthOld;
  const unsigned expireTo = nHeight - expDepthNow;

  /* It is possible that expireFrom = expireTo + 1, in case that the
     expiration period is raised together with the block height.  In this
     case, no names expire in the current step.  This case means that
     the absolute expiration height "n - expirationDepth(n)" is
     flat -- which is fine.  */
  assert (expireFrom <= expireTo + 1);

  /* Find all names that expire at those depths.  Note that GetNamesForHeight
     clears the output set, to we union all sets here.  */
  for (unsigned h = expireFrom; h <= expireTo; ++h)
    {
      std::set<valtype> newNames;
      view.GetNamesForHeight (h, newNames);
      names.insert (newNames.begin (), newNames.end ());
    }

  /* Expire all those names.  */
  for (std::set<valtype>::const_iterator i = names.begin ();
       i != names.end (); ++i)
    {
      const std::string nameStr = EncodeNameForMessage (*i);

      CNameData data;
      if (!view.GetName (*i, data))
        {
          LogError ("%s : name %s not found in the database",
                    __func__, nameStr);
          return false;
        }
      if (!data.isExpired (nHeight))
        {
          LogError ("%s : name %s is not actually expired",
                    __func__, nameStr);
          return false;
        }

      /* Special rule:  When d/postmortem expires (the name used by
         libcoin in the name-stealing demonstration), it's coin
         is already spent.  Ignore.  */
      if (nHeight == 175868
            && EncodeName (*i, NameEncoding::ASCII) == "d/postmortem")
        continue;

      const COutPoint& out = data.getUpdateOutpoint ();
      const auto coin = view.GetCoin (out);
      if (!coin)
        {
          LogError ("%s : name coin for %s is not available",
                    __func__, nameStr);
          return false;
        }
      const CNameScript nameOp(coin->out.scriptPubKey);
      if (!nameOp.isNameOp () || !nameOp.isAnyUpdate ()
          || nameOp.getOpName () != *i)
        {
          LogError ("%s : name coin to be expired is wrong script", __func__);
          return false;
        }

      if (!view.SpendCoin (out))
        {
          LogError ("%s : spending name coin failed", __func__);
          return false;
        }
      undo.vexpired.push_back (*coin);
    }

  return true;
}

bool
UnexpireNames (unsigned nHeight, CBlockUndo& undo, CCoinsViewCache& view,
               std::set<valtype>& names)
{
  names.clear ();

  /* The genesis block contains no name expirations.  */
  if (nHeight == 0)
    return true;

  std::vector<Coin>::reverse_iterator i;
  for (i = undo.vexpired.rbegin (); i != undo.vexpired.rend (); ++i)
    {
      const CNameScript nameOp(i->out.scriptPubKey);
      if (!nameOp.isNameOp () || !nameOp.isAnyUpdate ())
        {
          LogError ("%s : wrong script to be unexpired", __func__);
          return false;
        }

      const valtype& name = nameOp.getOpName ();
      if (names.count (name) > 0)
        {
          LogError ("%s : name %s unexpired twice",
                    __func__, EncodeNameForMessage (name));
          return false;
        }
      names.insert (name);

      CNameData data;
      if (!view.GetName (nameOp.getOpName (), data))
        {
          LogError ("%s : no data for name '%s' to be unexpired",
                    __func__, EncodeNameForMessage (name));
          return false;
        }
      if (!data.isExpired (nHeight) || data.isExpired (nHeight - 1))
        {
          LogError ("%s : name '%s' to be unexpired is not expired in the DB"
                    " or it was already expired before the current height",
                    __func__, EncodeNameForMessage (name));
          return false;
        }

      if (ApplyTxInUndo (std::move(*i), view,
                         data.getUpdateOutpoint ()) != DISCONNECT_OK)
        {
          LogError ("%s : failed to undo name coin spending", __func__);
          return false;
        }
    }

  return true;
}

void
CheckNameDB (Chainstate& chainState, bool disconnect)
{
  const int option
    = gArgs.GetIntArg ("-checknamedb", Params ().DefaultCheckNameDB ());

  if (option == -1)
    return;

  assert (option >= 0);
  if (option != 0)
    {
      if (disconnect || chainState.m_chain.Height () % option != 0)
        return;
    }

  auto& coinsTip = chainState.CoinsTip ();
  coinsTip.Flush ();
  const bool ok = coinsTip.ValidateNameDB (chainState, [] () {});

  /* The DB is inconsistent (mismatch between UTXO set and names DB) between
     (roughly) blocks 139,000 and 180,000.  This is caused by libcoin's
     "name stealing" bug.  For instance, d/postmortem is removed from
     the UTXO set shortly after registration (when it is used to steal
     names), but it remains in the name DB until it expires.  */
  if (!ok)
    {
      const unsigned nHeight = chainState.m_chain.Height ();
      LogDebug (BCLog::NAMES, "%s : name database is inconsistent", __func__);
      if (nHeight >= 139000 && nHeight <= 180000)
        LogDebug (BCLog::NAMES, "This is expected due to 'name stealing'.");
      else
        assert (false);
    }
}
