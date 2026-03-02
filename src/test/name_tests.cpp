// Copyright (c) 2014-2026 Daniel Kraft
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <base58.h>
#include <coins.h>
#include <consensus/validation.h>
#include <key_io.h>
#include <names/encoding.h>
#include <names/main.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <primitives/transaction.h>
#include <script/names.h>
#include <txdb.h>
#include <undo.h>
#include <validation.h>

#include <test/util/common.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <list>
#include <memory>
#include <stdexcept>

#include <stdint.h>

/* No space between BOOST_FIXTURE_TEST_SUITE and '(', so that extraction of
   the test-suite name works with grep as done in the Makefile.  */
BOOST_FIXTURE_TEST_SUITE(name_tests, TestingSetup)

namespace
{

// From script_p2sh_tests
bool IsStandardTx(const CTransaction& tx, std::string& reason)
{
    return IsStandardTx(tx, std::nullopt, DEFAULT_PERMIT_BAREMULTISIG, CFeeRate{DUST_RELAY_TX_FEE}, reason);
}

/**
 * Utility function that returns a sample address script to use in the tests.
 * @return A script that represents a simple address.
 */
CScript
getTestAddress ()
{
  const CTxDestination dest
    = DecodeDestination ("N5e1vXUUL3KfhPyVjQZSes1qQ7eyarDbUU");
  BOOST_CHECK (IsValidDestination (dest));

  return GetScriptForDestination (dest);
}

/**
 * Wrapper around CheckNameTransaction to make it callable with a
 * CMutableTransaction.  This avoids tons of explicit conversions that
 * would otherwise be needed below for the tests.
 */
bool
CheckNameTransaction (const CMutableTransaction& mtx, const unsigned nHeight,
                      const CCoinsView& view,
                      TxValidationState& state, const script_verify_flags flags)
{
  const CTransaction tx(mtx);
  return CheckNameTransaction (tx, nHeight, view, state, flags);
}

} // anonymous namespace

/* ************************************************************************** */

BOOST_AUTO_TEST_CASE (name_scripts)
{
  const CScript addr = getTestAddress ();
  const CNameScript opNone(addr);
  BOOST_CHECK (!opNone.isNameOp ());
  BOOST_CHECK (opNone.getAddress () == addr);

  const valtype name = DecodeName ("my-cool-name", NameEncoding::ASCII);
  const valtype value = DecodeName ("42!", NameEncoding::ASCII);

  const valtype rand(20, 'x');
  valtype toHash(rand);
  toHash.insert (toHash.end (), name.begin (), name.end ());
  const uint160 hash = Hash160 (toHash);

  CScript script;
  script = CNameScript::buildNameNew (addr, name, rand);
  const CNameScript opNew(script);
  BOOST_CHECK (opNew.isNameOp ());
  BOOST_CHECK (opNew.getAddress () == addr);
  BOOST_CHECK (!opNew.isAnyUpdate ());
  BOOST_CHECK (opNew.getNameOp () == OP_NAME_NEW);
  BOOST_CHECK (uint160 (opNew.getOpHash ()) == hash);

  script = CNameScript::buildNameFirstupdate (addr, name, value, rand);
  const CNameScript opFirstupdate(script);
  BOOST_CHECK (opFirstupdate.isNameOp ());
  BOOST_CHECK (opFirstupdate.getAddress () == addr);
  BOOST_CHECK (opFirstupdate.isAnyUpdate ());
  BOOST_CHECK (opFirstupdate.getNameOp () == OP_NAME_FIRSTUPDATE);
  BOOST_CHECK (opFirstupdate.getOpName () == name);
  BOOST_CHECK (opFirstupdate.getOpValue () == value);
  BOOST_CHECK (opFirstupdate.getOpRand () == rand);

  script = CNameScript::buildNameUpdate (addr, name, value);
  const CNameScript opUpdate(script);
  BOOST_CHECK (opUpdate.isNameOp ());
  BOOST_CHECK (opUpdate.getAddress () == addr);
  BOOST_CHECK (opUpdate.isAnyUpdate ());
  BOOST_CHECK (opUpdate.getNameOp () == OP_NAME_UPDATE);
  BOOST_CHECK (opUpdate.getOpName () == name);
  BOOST_CHECK (opUpdate.getOpValue () == value);
}

/* ************************************************************************** */

BOOST_AUTO_TEST_CASE (name_database)
{
  const valtype name1 = DecodeName ("db-test-name-1", NameEncoding::ASCII);
  const valtype name2 = DecodeName ("db-test-name-2", NameEncoding::ASCII);
  const valtype value = DecodeName ("my-value", NameEncoding::ASCII);
  const CScript addr = getTestAddress ();

  /* Choose two height values.  To verify that serialisation of the
     expiration entries works as it should, make sure that the values
     are ordered wrongly when serialised in little-endian.  */
  const unsigned height1 = 0x00ff;
  const unsigned height2 = 0x0142;

  CNameData dataHeight1, dataHeight2, data2;
  CScript updateScript = CNameScript::buildNameUpdate (addr, name1, value);
  const CNameScript nameOp(updateScript);
  dataHeight1.fromScript (height1, COutPoint (Txid (), 0), nameOp);
  dataHeight2.fromScript (height2, COutPoint (Txid (), 0), nameOp);

  std::set<valtype> setExpected, setRet;

  LOCK (cs_main);
  CCoinsViewCache& view = m_node.chainman->ActiveChainstate ().CoinsTip ();

  setExpected.clear ();
  setRet.clear ();
  setRet.insert (name1);
  BOOST_CHECK (view.GetNamesForHeight (height2, setRet));
  BOOST_CHECK (setRet == setExpected);

  BOOST_CHECK (!view.GetName (name1, data2));
  view.SetName (name1, dataHeight2, false);
  BOOST_CHECK (view.GetName (name1, data2));
  BOOST_CHECK (dataHeight2 == data2);

  BOOST_CHECK (view.GetNamesForHeight (height1, setRet));
  BOOST_CHECK (setRet == setExpected);
  setExpected.insert (name1);
  BOOST_CHECK (view.GetNamesForHeight (height2, setRet));
  BOOST_CHECK (setRet == setExpected);

  view.Flush ();
  BOOST_CHECK (view.GetName (name1, data2));
  BOOST_CHECK (dataHeight2 == data2);

  view.SetName (name2, dataHeight2, false);
  BOOST_CHECK (view.GetNamesForHeight (height2, setRet));
  setExpected.insert (name2);
  BOOST_CHECK (setRet == setExpected);

  view.DeleteName (name1);
  BOOST_CHECK (!view.GetName (name1, data2));
  view.Flush ();
  BOOST_CHECK (!view.GetName (name1, data2));

  BOOST_CHECK (view.GetNamesForHeight (height2, setRet));
  setExpected.erase (name1);
  BOOST_CHECK (setRet == setExpected);

  view.SetName (name2, dataHeight1, false);
  view.Flush ();
  view.SetName (name1, dataHeight1, false);

  BOOST_CHECK (view.GetNamesForHeight (height2, setRet));
  setExpected.clear ();
  BOOST_CHECK (setRet == setExpected);
  BOOST_CHECK (view.GetNamesForHeight (height1, setRet));
  setExpected.insert (name1);
  setExpected.insert (name2);
  BOOST_CHECK (setRet == setExpected);
}

/* ************************************************************************** */

/**
 * Define a class that can be used as "dummy" base name database.  It allows
 * iteration over its content, but always returns an empty range for that.
 * This is necessary to define a "purely cached" view, since the iteration
 * over CCoinsViewCache always calls through to the base iteration.
 */
class CDummyIterationView : public CCoinsView
{

private:

  /**
   * "Fake" name iterator returned.
   */
  class Iterator : public CNameIterator
  {
  public:

    void
    seek (const valtype& start)
    {}

    bool
    next (valtype& name, CNameData& data)
    {
      return false;
    }

  };

public:

  CNameIterator*
  IterateNames () const
  {
    return new Iterator ();
  }

};

/**
 * Helper class for testing name iteration.  It allows performing changes
 * to the name list, and mirrors them to a purely cached view, a name DB
 * and a name DB with cache that is flushed from time to time.  It compares
 * all of them to each other.
 */
class NameIterationTester
{

private:

  /** Type used for ordered lists of entries.  */
  typedef std::list<std::pair<valtype, CNameData> > EntryList;

  /** Name database view.  */
  CCoinsViewDB& db;
  /** Cached view based off the database.  */
  CCoinsViewCache hybrid;

  /** Dummy base view without real content.  */
  CDummyIterationView dummy;
  /**
   * Cache view based off the dummy.  This allows to test iteration
   * based solely on the cache.
   */
  CCoinsViewCache cache;

  /** Keep track of what the name set should look like as comparison.  */
  CNameCache::EntryMap data;

  /**
   * Keep an internal counter to build unique and changing CNameData
   * objects for testing purposes.  The counter value will be used
   * as the name's height in the data.
   */
  unsigned counter;

  /**
   * Verify consistency of the given view with the expected data.
   * @param view The view to check against data.
   */
  void verify (const CCoinsView& view) const;

  /**
   * Get a new CNameData object for testing purposes.  This also
   * increments the counter, so that each returned value is unique.
   * @return A new CNameData object.
   */
  CNameData getNextData ();

  /**
   * Iterate all names in a view and return the result as an ordered
   * list in the way they appeared.
   * @param view The view to iterate over.
   * @param start The start name.
   * @return The resulting entry list.
   */
  static EntryList getNamesFromView (const CCoinsView& view,
                                     const valtype& start);

  /**
   * Return all names that are produced by the given iterator.
   * @param iter The iterator to use.
   * @return The resulting entry list.
   */
  static EntryList getNamesFromIterator (CNameIterator& iter);

public:

  /**
   * Construct the tester with the given database view to use.
   * @param base The database coins view to use.
   */
  explicit NameIterationTester (CCoinsViewDB& base);

  /**
   * Verify consistency of all views.  This also flushes the hybrid cache
   * between verifying it and the base db view.
   */
  void verify ();

  /**
   * Add a new name with created dummy data.
   * @param n The name to add.
   */
  void add (const std::string& n);

  /**
   * Update the name with new dummy data.
   * @param n The name to update.
   */
  void update (const std::string& n);

  /**
   * Delete the name.
   * @param n The name to delete.
   */
  void remove (const std::string& n);

};

NameIterationTester::NameIterationTester (CCoinsViewDB& base)
  : db(base), hybrid(&db), dummy(), cache(&dummy), data(), counter(100)
{
  // Nothing else to do.
}

CNameData
NameIterationTester::getNextData ()
{
  const CScript addr = getTestAddress ();
  const valtype name = DecodeName ("dummy", NameEncoding::ASCII);
  const valtype value = DecodeName ("abc", NameEncoding::ASCII);
  const CScript updateScript = CNameScript::buildNameUpdate (addr, name, value);
  const CNameScript nameOp(updateScript);

  CNameData res;
  res.fromScript (++counter, COutPoint (Txid (), 0), nameOp);

  return res;
}

void
NameIterationTester::verify (const CCoinsView& view) const
{
  /* Try out everything with all names as "start".  This thoroughly checks
     that also the start implementation is correct.  It also checks using
     a single iterator and seeking vs using a fresh iterator.  */

  valtype start;
  EntryList remaining(data.begin (), data.end ());

  /* Seek the iterator to the end first for "maximum confusion".  This ensures
     that seeking to valtype() works.  */
  std::unique_ptr<CNameIterator> iter(view.IterateNames ());
  const valtype end = DecodeName ("zzzzzzzzzzzzzzzz", NameEncoding::ASCII);
  {
    valtype name;
    CNameData nameData;

    iter->seek (end);
    BOOST_CHECK (!iter->next (name, nameData));
  }

  while (true)
    {
      EntryList got = getNamesFromView (view, start);
      BOOST_CHECK (got == remaining);

      iter->seek (start);
      got = getNamesFromIterator (*iter);
      BOOST_CHECK (got == remaining);

      if (remaining.empty ())
        break;

      if (start == remaining.front ().first)
        remaining.pop_front ();

      if (remaining.empty ())
        start = end;
      else
        start = remaining.front ().first;
    }
}

void
NameIterationTester::verify ()
{
  verify (hybrid);

  /* Flush calls BatchWrite internally, and for that to work, we need to have
     a non-zero block hash.  Just set the block hash based on our counter.  */
  uint256 dummyBlockHash;
  *reinterpret_cast<unsigned*> (dummyBlockHash.begin ()) = counter;
  hybrid.SetBestBlock (dummyBlockHash);

  hybrid.Flush ();
  verify (db);
  verify (cache);
}

NameIterationTester::EntryList
NameIterationTester::getNamesFromView (const CCoinsView& view,
                                       const valtype& start)
{
  std::unique_ptr<CNameIterator> iter(view.IterateNames ());
  iter->seek (start);

  return getNamesFromIterator (*iter);
}

NameIterationTester::EntryList
NameIterationTester::getNamesFromIterator (CNameIterator& iter)
{
  EntryList res;

  valtype name;
  CNameData data;
  while (iter.next (name, data))
    res.push_back (std::make_pair (name, data));

  return res;
}

void
NameIterationTester::add (const std::string& n)
{
  const valtype& name = DecodeName (n, NameEncoding::ASCII);
  const CNameData testData = getNextData ();

  assert (data.count (name) == 0);
  data[name] = testData;
  hybrid.SetName (name, testData, false);
  cache.SetName (name, testData, false);
  verify ();
}

void
NameIterationTester::update (const std::string& n)
{
  const valtype& name = DecodeName (n, NameEncoding::ASCII);
  const CNameData testData = getNextData ();

  assert (data.count (name) == 1);
  data[name] = testData;
  hybrid.SetName (name, testData, false);
  cache.SetName (name, testData, false);
  verify ();
}

void
NameIterationTester::remove (const std::string& n)
{
  const valtype& name = DecodeName (n, NameEncoding::ASCII);

  assert (data.count (name) == 1);
  data.erase (name);
  hybrid.DeleteName (name);
  cache.DeleteName (name);
  verify ();
}

BOOST_AUTO_TEST_CASE (name_iteration)
{
  LOCK (cs_main);
  NameIterationTester tester(m_node.chainman->ActiveChainstate ().CoinsDB ());

  tester.verify ();

  tester.add ("");
  tester.add ("a");
  tester.add ("aa");
  tester.add ("b");
  
  tester.remove ("aa");
  tester.remove ("b");
  tester.add ("b");
  tester.add ("aa");
  tester.remove ("b");
  tester.remove ("aa");

  tester.update ("");
  tester.add ("aa");
  tester.add ("b");
  tester.update ("b");
  tester.update ("aa");
}

/* ************************************************************************** */

/**
 * Construct a dummy tx that provides the given script as input
 * for further tests in the given CCoinsView.  The txid is returned
 * to refer to it.  The "index" is always 0.  The output's
 * value is always set to 1000 COIN, since it doesn't matter for
 * the tests we are interested in.
 * @param scr The script that should be provided as output.
 * @param nHeight The height of the coin.
 * @param view Add it to this view.
 * @return The out point for the newly added coin.
 */
static COutPoint
addTestCoin (const CScript& scr, unsigned nHeight, CCoinsViewCache& view)
{
  const CTxOut txout(1000 * COIN, scr);

  CMutableTransaction mtx;
  mtx.vout.push_back (txout);
  const CTransaction tx(mtx);

  Coin coin(txout, nHeight, false);
  const COutPoint outp(tx.GetHash (), 0);
  view.AddCoin (outp, std::move (coin), false);

  return outp;
}

BOOST_AUTO_TEST_CASE (name_tx_verification)
{
  const valtype name1 = DecodeName ("test-name-1", NameEncoding::ASCII);
  const valtype name2 = DecodeName ("test-name-2", NameEncoding::ASCII);
  const valtype value = DecodeName ("my-value", NameEncoding::ASCII);

  const valtype tooLongName(256, 'x');
  const valtype tooLongValue(1024, 'x');

  const CScript addr = getTestAddress ();

  const valtype rand(20, 'x');

  /* We use a basic coin view as standard situation for all the tests.
     Set it up with some basic input coins.  */

  CCoinsView dummyView;
  CCoinsViewCache view(&dummyView);

  const CScript scrNew = CNameScript::buildNameNew (addr, name1, rand);
  const CScript scrFirst = CNameScript::buildNameFirstupdate (addr, name1,
                                                              value, rand);
  const CScript scrUpdate = CNameScript::buildNameUpdate (addr, name1, value);

  const COutPoint inCoin = addTestCoin (addr, 1, view);
  const COutPoint inNew = addTestCoin (scrNew, 100000, view);
  const COutPoint inFirst = addTestCoin (scrFirst, 100000, view);
  const COutPoint inUpdate = addTestCoin (scrUpdate, 100000, view);

  CNameData data1;
  data1.fromScript (100000, inFirst, CNameScript (scrFirst));
  view.SetName (name1, data1, false);

  /* ****************************************************** */
  /* Try out the Namecoin / non-Namecoin tx version check.  */

  TxValidationState state;
  CMutableTransaction mtx;
  CScript scr;
  std::string reason;

  mtx.vin.push_back (CTxIn (inCoin));
  mtx.vout.push_back (CTxOut (COIN, addr));
  const CTransaction baseTx(mtx);

  /* Non-name tx should be non-Namecoin version.  */
  BOOST_CHECK (CheckNameTransaction (baseTx, 200000, view, state, 0));
  mtx.SetNamecoin ();
  BOOST_CHECK (!CheckNameTransaction (mtx, 200000, view, state, 0));

  /* Name tx should be Namecoin version.  */
  mtx = CMutableTransaction (baseTx);
  mtx.vin.push_back (CTxIn (inNew));
  BOOST_CHECK (!CheckNameTransaction (mtx, 200000, view, state, 0));
  mtx.SetNamecoin ();
  mtx.vin.push_back (CTxIn (inUpdate));
  BOOST_CHECK (!CheckNameTransaction (mtx, 200000, view, state, 0));

  /* Duplicate name outs are not allowed.  */
  mtx = CMutableTransaction (baseTx);
  mtx.vout.push_back (CTxOut (COIN, scrNew));
  BOOST_CHECK (!CheckNameTransaction (mtx, 200000, view, state, 0));
  mtx.SetNamecoin ();
  BOOST_CHECK (CheckNameTransaction (mtx, 200000, view, state, 0));
  mtx.vout.push_back (CTxOut (COIN, scrNew));
  BOOST_CHECK (!CheckNameTransaction (mtx, 200000, view, state, 0));

  /* ************************** */
  /* Test NAME_NEW validation.  */

  /* Basic verification of NAME_NEW.  */
  mtx = CMutableTransaction (baseTx);
  mtx.SetNamecoin ();
  mtx.vout.push_back (CTxOut (COIN, scrNew));
  BOOST_CHECK (CheckNameTransaction (mtx, 200000, view, state, 0));
  mtx.vin.push_back (CTxIn (inNew));
  BOOST_CHECK (!CheckNameTransaction (mtx, 200000, view, state, 0));
  BOOST_CHECK (IsStandardTx (CTransaction (mtx), reason));

  /* Greedy names.  */
  mtx.vin.clear ();
  mtx.vout[1].nValue = COIN / 100;
  BOOST_CHECK (CheckNameTransaction (mtx, 212500, view, state, 0));
  mtx.vout[1].nValue = COIN / 100 - 1;
  BOOST_CHECK (CheckNameTransaction (mtx, 212499, view, state, 0));
  BOOST_CHECK (!CheckNameTransaction (mtx, 212500, view, state, 0));

  /* ***************************** */
  /* Test NAME_UPDATE validation.  */

  /* Construct two versions of the coin view.  Since CheckNameTransaction
     verifies the name update against the name database, we have to ensure
     that it fits to the current test.  One version has the NAME_FIRSTUPDATE
     as previous state of the name, and one has the NAME_UPDATE.  */
  CCoinsViewCache viewFirst(&view);
  CCoinsViewCache viewUpd(&view);
  data1.fromScript (100000, inUpdate, CNameScript (scrUpdate));
  viewUpd.SetName (name1, data1, false);

  /* Check update of UPDATE output, plus expiry.  */
  mtx = CMutableTransaction (baseTx);
  mtx.SetNamecoin ();
  mtx.vout.push_back (CTxOut (COIN, scrUpdate));
  BOOST_CHECK (!CheckNameTransaction (mtx, 135999, viewUpd, state, 0));
  mtx.vin.push_back (CTxIn (inUpdate));
  BOOST_CHECK (CheckNameTransaction (mtx, 135999, viewUpd, state, 0));
  BOOST_CHECK (!CheckNameTransaction (mtx, 136000, viewUpd, state, 0));
  BOOST_CHECK (IsStandardTx (CTransaction (mtx), reason));

  /* Check update of FIRSTUPDATE output, plus expiry.  */
  mtx.vin.clear ();
  mtx.vin.push_back (CTxIn (inFirst));
  BOOST_CHECK (CheckNameTransaction (mtx, 135999, viewFirst, state, 0));
  BOOST_CHECK (!CheckNameTransaction (mtx, 136000, viewFirst, state, 0));

  /* No check for greedy names, since the test names are expired
     already at the greedy-name fork height.  Should not matter
     too much, though, as the checks are there for NAME_NEW
     and NAME_FIRSTUPDATE.  */

  /* Value length limits.  */
  mtx = CMutableTransaction (baseTx);
  mtx.SetNamecoin ();
  mtx.vin.push_back (CTxIn (inUpdate));
  scr = CNameScript::buildNameUpdate (addr, name1, tooLongValue);
  mtx.vout.push_back (CTxOut (COIN, scr));
  BOOST_CHECK (!CheckNameTransaction (mtx, 110000, viewUpd, state, 0));
  
  /* Name mismatch to prev out.  */
  mtx.vout.clear ();
  scr = CNameScript::buildNameUpdate (addr, name2, value);
  mtx.vout.push_back (CTxOut (COIN, scr));
  BOOST_CHECK (!CheckNameTransaction (mtx, 110000, viewUpd, state, 0));

  /* Previous NAME_NEW is not allowed!  */
  mtx = CMutableTransaction (baseTx);
  mtx.SetNamecoin ();
  mtx.vout.push_back (CTxOut (COIN, scrUpdate));
  mtx.vin.push_back (CTxIn (inNew));
  CCoinsViewCache viewNew(&view);
  viewNew.DeleteName (name1);
  BOOST_CHECK (!CheckNameTransaction (mtx, 110000, viewNew, state, 0));

  /* ********************************** */
  /* Test NAME_FIRSTUPDATE validation.  */

  CCoinsViewCache viewClean(&view);
  viewClean.DeleteName (name1);

  /* Basic valid transaction.  */
  mtx = CMutableTransaction (baseTx);
  mtx.SetNamecoin ();
  mtx.vout.push_back (CTxOut (COIN, scrFirst));
  BOOST_CHECK (!CheckNameTransaction (mtx, 100012, viewClean, state, 0));
  mtx.vin.push_back (CTxIn (inNew));
  BOOST_CHECK (CheckNameTransaction (mtx, 100012, viewClean, state, 0));
  BOOST_CHECK (IsStandardTx (CTransaction (mtx), reason));

  /* Maturity of prev out, acceptable for mempool.  */
  BOOST_CHECK (!CheckNameTransaction (mtx, 100011, viewClean, state, 0));
  BOOST_CHECK (CheckNameTransaction (mtx, 100011, viewClean, state,
                                     SCRIPT_VERIFY_NAMES_MEMPOOL));

  /* Expiry and re-registration of a name.  */
  BOOST_CHECK (!CheckNameTransaction (mtx, 135999, view, state, 0));
  BOOST_CHECK (CheckNameTransaction (mtx, 136000, view, state, 0));

  /* "Greedy" names.  */
  mtx.vout[1].nValue = COIN / 100;
  BOOST_CHECK (CheckNameTransaction (mtx, 212500, viewClean, state, 0));
  mtx.vout[1].nValue = COIN / 100 - 1;
  BOOST_CHECK (CheckNameTransaction (mtx, 212499, viewClean, state, 0));
  BOOST_CHECK (!CheckNameTransaction (mtx, 212500, viewClean, state, 0));

  /* Rand mismatch (wrong name activated).  */
  mtx.vout.clear ();
  scr = CNameScript::buildNameFirstupdate (addr, name2, value, rand);
  BOOST_CHECK (!CheckNameTransaction (mtx, 100012, viewClean, state, 0));

  /* Non-NAME_NEW prev output.  */
  mtx = CMutableTransaction (baseTx);
  mtx.SetNamecoin ();
  mtx.vout.push_back (CTxOut (COIN, scrFirst));
  mtx.vin.push_back (CTxIn (inUpdate));
  BOOST_CHECK (!CheckNameTransaction (mtx, 100012, viewClean, state, 0));
  mtx.vin.clear ();
  mtx.vin.push_back (CTxIn (inFirst));
  BOOST_CHECK (!CheckNameTransaction (mtx, 100012, viewClean, state, 0));
}

BOOST_AUTO_TEST_CASE (name_firstupdate_salt_length)
{
  const valtype name = DecodeName ("x/test-name", NameEncoding::ASCII);
  const valtype value = DecodeName ("{}", NameEncoding::ASCII);
  const CScript addr = getTestAddress ();

  const valtype shortRand(19, 'x');
  const valtype correctRand(20, 'x');
  const valtype longRand(21, 'x');

  CCoinsView dummyView;

  /* Builds and checks a firstupdate transaction for our test name and
     the given rand value and verify flags.  */
  const auto checkRand = [&] (const valtype& rand,
                              const script_verify_flags flags)
    {
      CCoinsViewCache view(&dummyView);

      const CScript scrNew = CNameScript::buildNameNew (addr, name, rand);
      const CScript scrFirst = CNameScript::buildNameFirstupdate (addr, name,
                                                                  value, rand);

      const COutPoint inCoin = addTestCoin (addr, 1, view);
      const COutPoint inNew = addTestCoin (scrNew, 100'000, view);

      CMutableTransaction mtx;
      mtx.vin.emplace_back (inCoin);
      mtx.vin.emplace_back (inNew);
      mtx.vout.emplace_back (COIN, scrFirst);
      mtx.SetNamecoin ();

      TxValidationState state;
      return CheckNameTransaction (mtx, 100'100, view, state, flags);
    };

  BOOST_CHECK (checkRand (shortRand, 0));
  BOOST_CHECK (!checkRand (shortRand, SCRIPT_VERIFY_NAMES_LONG_SALT));

  BOOST_CHECK (checkRand (correctRand, 0));
  BOOST_CHECK (checkRand (correctRand, SCRIPT_VERIFY_NAMES_LONG_SALT));

  BOOST_CHECK (!checkRand (longRand, 0));
  BOOST_CHECK (!checkRand (longRand, SCRIPT_VERIFY_NAMES_LONG_SALT));
}

/* ************************************************************************** */

BOOST_AUTO_TEST_CASE (name_updates_undo)
{
  /* Enable name history to test this on the go.  */
  fNameHistory = true;

  const valtype name = DecodeName ("db-test-name", NameEncoding::ASCII);
  const valtype value1 = DecodeName ("old-value", NameEncoding::ASCII);
  const valtype value2 = DecodeName ("new-value", NameEncoding::ASCII);
  const CScript addr = getTestAddress ();

  CCoinsView dummyView;
  CCoinsViewCache view(&dummyView);
  CBlockUndo undo;
  CNameData data;
  CNameHistory history;

  const valtype rand(20, 'x');

  const CScript scrNew = CNameScript::buildNameNew (addr, name, rand);
  const CScript scrFirst = CNameScript::buildNameFirstupdate (addr, name,
                                                              value1, rand);
  const CScript scrUpdate = CNameScript::buildNameUpdate (addr, name, value2);

  /* The constructed tx needs not be valid.  We only test
     ApplyNameTransaction and not validation.  */

  CMutableTransaction mtx;
  mtx.SetNamecoin ();
  mtx.vout.push_back (CTxOut (COIN, scrNew));
  ApplyNameTransaction (CTransaction (mtx), 100, view, undo);
  BOOST_CHECK (!view.GetName (name, data));
  BOOST_CHECK (undo.vnameundo.empty ());
  BOOST_CHECK (!view.GetNameHistory (name, history));

  mtx.vout.clear ();
  mtx.vout.push_back (CTxOut (COIN, scrFirst));
  ApplyNameTransaction (CTransaction (mtx), 200, view, undo);
  BOOST_CHECK (view.GetName (name, data));
  BOOST_CHECK (data.getHeight () == 200);
  BOOST_CHECK (data.getValue () == value1);
  BOOST_CHECK (data.getAddress () == addr);
  BOOST_CHECK (!view.GetNameHistory (name, history));
  BOOST_CHECK (undo.vnameundo.size () == 1);
  const CNameData firstData = data;

  mtx.vout.clear ();
  mtx.vout.push_back (CTxOut (COIN, scrUpdate));
  ApplyNameTransaction (CTransaction (mtx), 300, view, undo);
  BOOST_CHECK (view.GetName (name, data));
  BOOST_CHECK (data.getHeight () == 300);
  BOOST_CHECK (data.getValue () == value2);
  BOOST_CHECK (data.getAddress () == addr);
  BOOST_CHECK (view.GetNameHistory (name, history));
  BOOST_CHECK (history.getData ().size () == 1);
  BOOST_CHECK (history.getData ().back () == firstData);
  BOOST_CHECK (undo.vnameundo.size () == 2);

  undo.vnameundo.back ().apply (view);
  BOOST_CHECK (view.GetName (name, data));
  BOOST_CHECK (data.getHeight () == 200);
  BOOST_CHECK (data.getValue () == value1);
  BOOST_CHECK (data.getAddress () == addr);
  BOOST_CHECK (!view.GetNameHistory (name, history) || history.empty ());
  undo.vnameundo.pop_back ();

  undo.vnameundo.back ().apply (view);
  BOOST_CHECK (!view.GetName (name, data));
  BOOST_CHECK (!view.GetNameHistory (name, history) || history.empty ());
  undo.vnameundo.pop_back ();
  BOOST_CHECK (undo.vnameundo.empty ());
}

/* ************************************************************************** */

BOOST_AUTO_TEST_CASE (name_expire_utxo)
{
  const valtype name1 = DecodeName ("test-name-1", NameEncoding::ASCII);
  const valtype name2 = DecodeName ("test-name-2", NameEncoding::ASCII);
  const valtype value = DecodeName ("value", NameEncoding::ASCII);
  const CScript addr = getTestAddress ();
  
  const CScript upd1 = CNameScript::buildNameUpdate (addr, name1, value);
  const CScript upd2 = CNameScript::buildNameUpdate (addr, name2, value);

  const CNameScript op1(upd1);
  const CNameScript op2(upd2);

  /* Use a "real" backing view, since GetNamesForHeight calls through
     to the base in any case.  */
  LOCK (cs_main);
  CCoinsViewCache view(&m_node.chainman->ActiveChainstate ().CoinsTip ());

  const COutPoint coinId1 = addTestCoin (upd1, 100000, view);
  const COutPoint coinId2 = addTestCoin (upd2, 100010, view);

  CNameData data;
  data.fromScript (100000, coinId1, op1);
  view.SetName (name1, data, false);
  BOOST_CHECK (!data.isExpired (135999) && data.isExpired (136000));
  data.fromScript (100010, coinId2, op2);
  view.SetName (name2, data, false);
  BOOST_CHECK (!data.isExpired (136009) && data.isExpired (136010));

  std::set<valtype> setExpired;
  BOOST_CHECK (view.GetNamesForHeight (100000, setExpired));
  BOOST_CHECK (setExpired.size () == 1 && *setExpired.begin () == name1);
  BOOST_CHECK (view.GetNamesForHeight (100010, setExpired));
  BOOST_CHECK (setExpired.size () == 1 && *setExpired.begin () == name2);

  const auto coin1 = view.GetCoin (coinId1);
  const auto coin2 = view.GetCoin (coinId2);
  BOOST_CHECK (coin1);
  BOOST_CHECK (coin2);

  CBlockUndo undo1, undo2;

  /* None of the two names should be expired.  */
  BOOST_CHECK (ExpireNames (135999, view, undo1, setExpired));
  BOOST_CHECK (undo1.vexpired.empty ());
  BOOST_CHECK (setExpired.empty ());
  auto coin = view.GetCoin (coinId1);
  BOOST_CHECK (coin);
  BOOST_CHECK (*coin == *coin1);
  coin = view.GetCoin (coinId2);
  BOOST_CHECK (coin);
  BOOST_CHECK (*coin == *coin2);

  /* The first name expires.  */
  BOOST_CHECK (ExpireNames (136000, view, undo1, setExpired));
  BOOST_CHECK (undo1.vexpired.size () == 1);
  BOOST_CHECK (undo1.vexpired[0] == coin1);
  BOOST_CHECK (setExpired.size () == 1 && *setExpired.begin () == name1);
  BOOST_CHECK (!view.GetCoin (coinId1));
  coin = view.GetCoin (coinId2);
  BOOST_CHECK (coin);
  BOOST_CHECK (*coin == *coin2);

  /* Also the second name expires.  */
  BOOST_CHECK (ExpireNames (136010, view, undo2, setExpired));
  BOOST_CHECK (undo2.vexpired.size () == 1);
  BOOST_CHECK (undo2.vexpired[0] == coin2);
  BOOST_CHECK (setExpired.size () == 1 && *setExpired.begin () == name2);
  BOOST_CHECK (!view.GetCoin (coinId1));
  BOOST_CHECK (!view.GetCoin (coinId2));

  /* Undo the second expiration.  */
  BOOST_CHECK (UnexpireNames (136010, undo2, view, setExpired));
  BOOST_CHECK (setExpired.size () == 1 && *setExpired.begin () == name2);
  BOOST_CHECK (!view.GetCoin (coinId1));
  coin = view.GetCoin (coinId2);
  BOOST_CHECK (coin);
  BOOST_CHECK (*coin == *coin2);

  /* Undoing at the wrong height should fail.  */
  BOOST_CHECK (!UnexpireNames (136001, undo1, view, setExpired));
  BOOST_CHECK (!UnexpireNames (135999, undo1, view, setExpired));

  /* Undo the first expiration.  */
  BOOST_CHECK (UnexpireNames (136000, undo1, view, setExpired));
  BOOST_CHECK (setExpired.size () == 1 && *setExpired.begin () == name1);
  coin = view.GetCoin (coinId1);
  BOOST_CHECK (coin);
  BOOST_CHECK (*coin == *coin1);
  coin = view.GetCoin (coinId2);
  BOOST_CHECK (coin);
  BOOST_CHECK (*coin == *coin2);
}

/* ************************************************************************** */

BOOST_AUTO_TEST_CASE (encoding_to_from_string)
{
  for (const std::string& encStr : {"ascii", "utf8", "hex"})
    BOOST_CHECK_EQUAL (EncodingToString (EncodingFromString (encStr)), encStr);

  BOOST_CHECK_EQUAL (EncodingToString (NameEncoding::ASCII), "ascii");
  BOOST_CHECK_EQUAL (EncodingToString (NameEncoding::UTF8), "utf8");
  BOOST_CHECK_EQUAL (EncodingToString (NameEncoding::HEX), "hex");

  BOOST_CHECK_THROW (EncodingFromString ("invalid"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE (encoding_args)
{
  BOOST_CHECK (ConfiguredNameEncoding () == DEFAULT_NAME_ENCODING);
  BOOST_CHECK (ConfiguredValueEncoding () == DEFAULT_VALUE_ENCODING);

  gArgs.ForceSetArg ("-nameencoding", "utf8");
  BOOST_CHECK (ConfiguredNameEncoding () == NameEncoding::UTF8);
  BOOST_CHECK (ConfiguredValueEncoding () == DEFAULT_VALUE_ENCODING);

  gArgs.ForceSetArg ("-valueencoding", "hex");
  BOOST_CHECK (ConfiguredNameEncoding () == NameEncoding::UTF8);
  BOOST_CHECK (ConfiguredValueEncoding () == NameEncoding::HEX);

  gArgs.ForceSetArg ("-nameencoding", "invalid");
  BOOST_CHECK (ConfiguredNameEncoding () == DEFAULT_NAME_ENCODING);
  BOOST_CHECK (ConfiguredValueEncoding () == NameEncoding::HEX);
}

namespace
{

class EncodingTestSetup : public TestingSetup
{
public:

  NameEncoding encoding;

  void
  ValidRoundtrip (const std::string& str, const valtype& data) const
  {
    BOOST_CHECK_EQUAL (EncodeName (data, encoding), str);
    BOOST_CHECK (DecodeName (str, encoding) == data);
  }

  void
  InvalidString (const std::string& str) const
  {
    BOOST_CHECK_THROW (DecodeName (str, encoding), InvalidNameString);
  }

  void
  InvalidData (const valtype& data) const
  {
    BOOST_CHECK_THROW (EncodeName (data, encoding), InvalidNameString);
  }

};

} // anonymous namespace

BOOST_FIXTURE_TEST_CASE (encoding_ascii, EncodingTestSetup)
{
  encoding = NameEncoding::ASCII;

  ValidRoundtrip (" abc42\x7f", {0x20, 'a', 'b', 'c', '4', '2', 0x7f});
  ValidRoundtrip ("", {});

  InvalidString ("a\tx");
  InvalidString ("a\x80x");
  InvalidString (std::string ({'a', 0, 'x'}));
  InvalidString (reinterpret_cast<const char*> (u8"ä"));

  InvalidData ({'a', 0, 'x'});
  InvalidData ({'a', 0x19, 'x'});
  InvalidData ({'a', 0x80, 'x'});
}

BOOST_FIXTURE_TEST_CASE (encoding_utf8, EncodingTestSetup)
{
  encoding = NameEncoding::UTF8;

  valtype expected({0x20, 'a', 'b', 'c', '\t', '4', '2', 0x00, 0x7f});
  const std::string utf8Str(reinterpret_cast<const char*> (u8"äöü"));
  BOOST_CHECK_EQUAL (utf8Str.size (), 6);
  expected.insert (expected.end (), utf8Str.begin (), utf8Str.end ());
  ValidRoundtrip (" abc\t42" + std::string ({0})
                    + reinterpret_cast<const char*> (u8"\x7fäöü"),
                  expected);
  ValidRoundtrip ("", {});

  InvalidString ("a\x80x");
  InvalidData ({'a', 0x80, 'x'});
}

BOOST_FIXTURE_TEST_CASE (encoding_hex, EncodingTestSetup)
{
  encoding = NameEncoding::HEX;

  ValidRoundtrip ("0065ff", {0x00, 0x65, 0xff});
  ValidRoundtrip ("", {});
  BOOST_CHECK (DecodeName ("aaBBcCDd", encoding)
                == valtype ({0xaa, 0xbb, 0xcc, 0xdd}));

  InvalidString ("aaa");
  InvalidString ("zz");
}

BOOST_AUTO_TEST_CASE (encode_name_for_message)
{
  BOOST_CHECK_EQUAL (
      EncodeNameForMessage (DecodeName ("d/abc", NameEncoding::ASCII)),
      "'d/abc'");
  BOOST_CHECK_EQUAL (
      EncodeNameForMessage (DecodeName ("00ff", NameEncoding::HEX)),
      "0x00ff");
}

/* ************************************************************************** */

BOOST_AUTO_TEST_SUITE_END ()
