// Copyright (c) 2014-2026 Daniel Kraft
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <base58.h>
#include <coins.h>
#include <key_io.h>
#include <names/encoding.h>
#include <names/mempool.h>
#include <primitives/transaction.h>
#include <script/names.h>
#include <txmempool.h>
#include <validation.h>
#include <validationinterface.h>

#include <test/util/setup_common.h>
#include <test/util/txmempool.h>

#include <boost/test/unit_test.hpp>

/* No space between BOOST_FIXTURE_TEST_SUITE and '(', so that extraction of
   the test-suite name works with grep as done in the Makefile.  */
BOOST_AUTO_TEST_SUITE(name_mempool_tests)

namespace
{

class NameMempoolTestSetup : public TestingSetup
{

private:

  const TestMemPoolEntryHelper memPoolHelper;

public:

  CTxMemPool& mempool;

  CScript ADDR;
  CScript OTHER_ADDR;

  const LockPoints lp;

  NameMempoolTestSetup ()
    : mempool(*Assert(m_node.mempool))
  {
    ADDR = CScript () << OP_TRUE;
    OTHER_ADDR = CScript () << OP_TRUE << OP_RETURN;

    cs_main.lock();
    mempool.cs.lock();
  }

  ~NameMempoolTestSetup ()
  {
    mempool.cs.unlock();
    cs_main.unlock();
  }

  /**
   * Returns a valtype name based on the given string.
   */
  static valtype
  Name (const std::string& str)
  {
    return DecodeName (str, NameEncoding::ASCII);
  }

  /**
   * Returns the hash bytes for a preregistername.
   */
  static valtype
  NewHash (const std::string& nm, const char rand)
  {
    const valtype nameVal = Name (nm);
    const valtype randVal(20, rand);

    valtype toHash(randVal);
    toHash.insert (toHash.end (), nameVal.begin (), nameVal.end ());
    const uint160 hash = Hash160 (toHash);

    return valtype (hash.begin (), hash.end ());
  }

  /**
   * Builds a name new script for the given test name and rand based
   * on a character (just to allow different rand's).
   */
  static CScript
  NewScript (const CScript& addr, const std::string& nm, const char rand)
  {
    const valtype randVal(20, rand);

    return CNameScript::buildNameNew (addr, Name (nm), randVal);
  }

  /**
   * Builds a registername script for the given name and rand value.
   * The value we update to is just a fixed one.
   */
  static CScript
  FirstScript (const CScript& addr, const std::string& nm, const char rand)
  {
    const valtype randVal(20, rand);
    const valtype value = DecodeName ("firstupdate value", NameEncoding::ASCII);

    return CNameScript::buildNameFirstupdate (addr, Name (nm), value, randVal);
  }

  /**
   * Builds a updatename script based on the given name and value.
   */
  static CScript
  UpdateScript (const CScript& addr, const std::string& nm,
                const std::string& val)
  {
    const valtype value = DecodeName (val, NameEncoding::ASCII);

    return CNameScript::buildNameUpdate (addr, Name (nm), value);
  }

  /**
   * Builds a transaction spending to a name-output script.  The transaction
   * is not valid, but it is "valid enough" for testing the name mempool
   * rules with it.
   */
  static CTransaction
  Tx (const CScript& out)
  {
    CMutableTransaction mtx;
    mtx.SetNamecoin ();
    mtx.vout.push_back (CTxOut (COIN, out));

    return CTransaction (mtx);
  }

  /**
   * Builds a mempool entry for the given transaction.
   */
  CTxMemPoolEntry
  Entry (const CTransaction& tx)
  {
    CTransaction txCopy = tx;
    return memPoolHelper.FromTx (MakeTransactionRef (std::move (txCopy)));
  }

};

} // anonymous namespace

/* ************************************************************************** */

BOOST_FIXTURE_TEST_CASE (invalid_tx, NameMempoolTestSetup)
{
  /* Invalid transactions should not crash / assert fail the mempool check.  */

  CMutableTransaction mtx;
  mtx.SetNamecoin ();
  mempool.checkNameOps (CTransaction (mtx));

  mtx.vout.push_back (CTxOut (COIN, NewScript (ADDR, "foo", 'a')));
  mtx.vout.push_back (CTxOut (COIN, NewScript (ADDR, "bar", 'b')));
  mtx.vout.push_back (CTxOut (COIN, FirstScript (ADDR, "foo", 'a')));
  mtx.vout.push_back (CTxOut (COIN, FirstScript (ADDR, "bar", 'b')));
  mtx.vout.push_back (CTxOut (COIN, UpdateScript (ADDR, "foo", "x")));
  mtx.vout.push_back (CTxOut (COIN, UpdateScript (ADDR, "bar", "y")));
  mempool.checkNameOps (CTransaction (mtx));
}

BOOST_FIXTURE_TEST_CASE (empty_mempool, NameMempoolTestSetup)
{
  /* While the mempool is empty (we do not add any transactions in this test),
     all should be fine without respect to conflicts among the transactions.  */

  BOOST_CHECK (!mempool.registersName (Name ("foo")));
  BOOST_CHECK (!mempool.updatesName (Name ("foo")));

  BOOST_CHECK (mempool.checkNameOps (Tx (NewScript (ADDR, "foo", 'a'))));
  BOOST_CHECK (mempool.checkNameOps (Tx (NewScript (ADDR, "foo", 'b'))));
  BOOST_CHECK (mempool.checkNameOps (Tx (NewScript (OTHER_ADDR, "foo", 'a'))));

  BOOST_CHECK (mempool.checkNameOps (Tx (FirstScript (ADDR, "foo", 'a'))));
  BOOST_CHECK (mempool.checkNameOps (Tx (FirstScript (ADDR, "foo", 'b'))));

  BOOST_CHECK (mempool.checkNameOps (Tx (UpdateScript (ADDR, "foo", "x"))));
  BOOST_CHECK (mempool.checkNameOps (Tx (UpdateScript (ADDR, "foo", "y"))));
}

BOOST_FIXTURE_TEST_CASE (pendingChainLength_lastNameOutput,
                         NameMempoolTestSetup)
{
  const auto txNew = Tx (NewScript (ADDR, "new", 'a'));
  const auto txReg = Tx (FirstScript (ADDR, "reg", 'b'));
  const auto txUpd = Tx (UpdateScript (ADDR, "upd", "x"));

  TryAddToMempool (mempool, Entry (txNew));
  TryAddToMempool (mempool, Entry (txReg));
  TryAddToMempool (mempool, Entry (txUpd));

  /* For testing chained name updates, we have to build a "real" chain of
     transactions with matching inputs and outputs.  */

  CMutableTransaction mtx;
  mtx.SetNamecoin ();
  mtx.vout.push_back (CTxOut (COIN, FirstScript (ADDR, "chain", 'a')));
  mtx.vout.push_back (CTxOut (COIN, ADDR));
  mtx.vout.push_back (CTxOut (COIN, OTHER_ADDR));
  const CTransaction chain1(mtx);
  TryAddToMempool (mempool, Entry (chain1));

  mtx.vout.clear ();
  mtx.vout.push_back (CTxOut (COIN, ADDR));
  mtx.vout.push_back (CTxOut (COIN, UpdateScript (ADDR, "chain", "x")));
  mtx.vin.push_back (CTxIn (COutPoint (chain1.GetHash (), 0)));
  const CTransaction chain2(mtx);
  TryAddToMempool (mempool, Entry (chain2));

  mtx.vout.clear ();
  mtx.vout.push_back (CTxOut (COIN, OTHER_ADDR));
  mtx.vout.push_back (CTxOut (COIN, UpdateScript (ADDR, "chain", "y")));
  mtx.vin.push_back (CTxIn (COutPoint (chain2.GetHash (), 0)));
  mtx.vin.push_back (CTxIn (COutPoint (chain1.GetHash (), 1)));
  const CTransaction chain3(mtx);
  TryAddToMempool (mempool, Entry (chain3));

  CMutableTransaction mtxCurrency;
  mtxCurrency.vin.push_back (CTxIn (COutPoint (chain1.GetHash (), 2)));
  mtxCurrency.vin.push_back (CTxIn (COutPoint (chain3.GetHash (), 0)));
  TryAddToMempool (mempool, Entry (CTransaction (mtxCurrency)));

  BOOST_CHECK (mempool.lastNameOutput (Name ("new")).IsNull ());
  BOOST_CHECK (mempool.lastNameOutput (Name ("reg"))
                  == COutPoint (txReg.GetHash (), 0));
  BOOST_CHECK (mempool.lastNameOutput (Name ("upd"))
                  == COutPoint (txUpd.GetHash (), 0));
  BOOST_CHECK (mempool.lastNameOutput (Name ("chain"))
                  == COutPoint (chain3.GetHash (), 1));

  BOOST_CHECK_EQUAL (mempool.pendingNameChainLength (Name ("new")), 0);
  BOOST_CHECK_EQUAL (mempool.pendingNameChainLength (Name ("reg")), 1);
  BOOST_CHECK_EQUAL (mempool.pendingNameChainLength (Name ("upd")), 1);
  BOOST_CHECK_EQUAL (mempool.pendingNameChainLength (Name ("chain")), 3);
}

BOOST_FIXTURE_TEST_CASE (preregistername, NameMempoolTestSetup)
{
  const auto tx1 = Tx (NewScript (ADDR, "foo", 'a'));
  const auto tx1p = Tx (NewScript (OTHER_ADDR, "foo", 'a'));
  const auto tx2 = Tx (NewScript (ADDR, "foo", 'b'));

  const auto e1 = Entry (tx1);
  const auto e2 = Entry (tx2);
  BOOST_CHECK (e1.isNameNew () && e2.isNameNew ());
  BOOST_CHECK (e1.getNameNewHash () == NewHash ("foo", 'a'));
  BOOST_CHECK (e2.getNameNewHash () == NewHash ("foo", 'b'));

  TryAddToMempool (mempool, e1);
  TryAddToMempool (mempool, e2);
  BOOST_CHECK (mempool.checkNameOps (tx1));
  BOOST_CHECK (mempool.checkNameOps (tx2));
  BOOST_CHECK (!mempool.checkNameOps (tx1p));

  mempool.removeRecursive (tx1, MemPoolRemovalReason::EXPIRY);
  mempool.removeRecursive (tx2, MemPoolRemovalReason::EXPIRY);
  BOOST_CHECK (mempool.checkNameOps (tx1));
  BOOST_CHECK (mempool.checkNameOps (tx2));
  BOOST_CHECK (!mempool.checkNameOps (tx1p));
}

BOOST_FIXTURE_TEST_CASE (registername, NameMempoolTestSetup)
{
  const auto tx1 = Tx (FirstScript (ADDR, "foo", 'a'));
  const auto tx2 = Tx (FirstScript (ADDR, "foo", 'b'));

  const auto e = Entry (tx1);
  BOOST_CHECK (e.isNameRegistration () && !e.isNameUpdate ());
  BOOST_CHECK (e.getName () == Name ("foo"));

  TryAddToMempool (mempool, e);
  BOOST_CHECK (mempool.registersName (Name ("foo")));
  BOOST_CHECK (!mempool.updatesName (Name ("foo")));
  BOOST_CHECK (!mempool.checkNameOps (tx2));

  mempool.removeRecursive (tx1, MemPoolRemovalReason::EXPIRY);
  BOOST_CHECK (!mempool.registersName (Name ("foo")));
  BOOST_CHECK (mempool.checkNameOps (tx1));
  BOOST_CHECK (mempool.checkNameOps (tx2));
}

BOOST_FIXTURE_TEST_CASE (updatename, NameMempoolTestSetup)
{
  const auto tx1 = Tx (UpdateScript (ADDR, "foo", "x"));
  const auto tx2 = Tx (UpdateScript (ADDR, "foo", "y"));
  const auto tx3 = Tx (UpdateScript (ADDR, "bar", "z"));

  const auto e1 = Entry (tx1);
  const auto e2 = Entry (tx2);
  const auto e3 = Entry (tx3);
  BOOST_CHECK (!e1.isNameRegistration () && e1.isNameUpdate ());
  BOOST_CHECK (e1.getName () == Name ("foo"));

  TryAddToMempool (mempool, e1);
  TryAddToMempool (mempool, e2);
  TryAddToMempool (mempool, e3);
  BOOST_CHECK (!mempool.registersName (Name ("foo")));
  BOOST_CHECK (mempool.updatesName (Name ("foo")));
  BOOST_CHECK (mempool.updatesName (Name ("bar")));

  mempool.removeRecursive (tx2, MemPoolRemovalReason::EXPIRY);
  BOOST_CHECK (mempool.updatesName (Name ("foo")));
  BOOST_CHECK (mempool.updatesName (Name ("bar")));

  mempool.removeRecursive (tx1, MemPoolRemovalReason::EXPIRY);
  BOOST_CHECK (!mempool.updatesName (Name ("foo")));
  BOOST_CHECK (mempool.updatesName (Name ("bar")));

  mempool.removeRecursive (tx3, MemPoolRemovalReason::EXPIRY);
  BOOST_CHECK (!mempool.updatesName (Name ("foo")));
  BOOST_CHECK (!mempool.updatesName (Name ("bar")));
}

BOOST_FIXTURE_TEST_CASE (mempool_sanity_check, NameMempoolTestSetup)
{
  TryAddToMempool (mempool, Entry (Tx (NewScript (ADDR, "new", 'a'))));
  TryAddToMempool (mempool, Entry (Tx (NewScript (ADDR, "new", 'b'))));

  TryAddToMempool (mempool, Entry (Tx (FirstScript (ADDR, "reg", 'a'))));
  TryAddToMempool (mempool, Entry (Tx (UpdateScript (ADDR, "reg", "n"))));

  TryAddToMempool (mempool, Entry (Tx (UpdateScript (ADDR, "upd", "x"))));
  TryAddToMempool (mempool, Entry (Tx (UpdateScript (ADDR, "upd", "y"))));

  LOCK (cs_main);
  auto& chainState = m_node.chainman->ActiveChainstate ();
  auto& view = chainState.CoinsTip ();

  const CNameScript nameOp(UpdateScript (ADDR, "upd", "o"));
  CNameData data;
  data.fromScript (100, COutPoint (Txid (), 0), nameOp);
  view.SetName (Name ("upd"), data, false);

  mempool.checkNames (view, 10);
}

namespace
{

/**
 * Helper class that listens to TransactionRemovedFromMempool and records
 * the txid's of all transactions removed.
 */
class NameConflictTracker : public CValidationInterface
{

private:

  /** The node context, used to register the interface.  */
  node::NodeContext& m_node;

  /** The txids that have been removed according to our callback.  */
  std::vector<Txid> txids;

public:

  NameConflictTracker (node::NodeContext& node)
    : m_node(node)
  {
    m_node.validation_signals->RegisterValidationInterface (this);
  }

  ~NameConflictTracker ()
  {
    m_node.validation_signals->UnregisterValidationInterface (this);
  }

  void
  TransactionRemovedFromMempool (const CTransactionRef& ptxn,
                                 const MemPoolRemovalReason reason,
                                 const uint64_t sequence) override
  {
    txids.push_back (ptxn->GetHash ());
  }

  /**
   * Expects the given list of txids to be removed.  Waits for all outstanding
   * callbacks to be processed as needed.
   */
  void
  ExpectTxids (const std::vector<Txid>& expected) const
  {
    while (m_node.validation_signals->CallbacksPending () > 0)
      UninterruptibleSleep (std::chrono::milliseconds (10));
    BOOST_CHECK (txids == expected);
  }

};

} // anonymous namespace

BOOST_FIXTURE_TEST_CASE (registration_conflicts, NameMempoolTestSetup)
{
  const auto tx1 = Tx (FirstScript (ADDR, "foo", 'a'));
  const auto tx2 = Tx (FirstScript (ADDR, "foo", 'b'));
  const auto e = Entry (tx1);

  TryAddToMempool (mempool, e);
  BOOST_CHECK (mempool.registersName (Name ("foo")));
  BOOST_CHECK (!mempool.checkNameOps (tx2));

  NameConflictTracker tracker(m_node);
  mempool.removeConflicts (tx2);
  tracker.ExpectTxids ({tx1.GetHash ()});

  BOOST_CHECK (!mempool.registersName (Name ("foo")));
  BOOST_CHECK (mempool.checkNameOps (tx1));
  BOOST_CHECK (mempool.checkNameOps (tx2));
  BOOST_CHECK (mempool.mapTx.empty ());
}

BOOST_FIXTURE_TEST_CASE (expire_conflicts, NameMempoolTestSetup)
{
  const auto tx1 = Tx (UpdateScript (ADDR, "foo", "x"));
  const auto tx2 = Tx (UpdateScript (ADDR, "foo", "y"));
  const auto e1 = Entry (tx1);
  const auto e2 = Entry (tx2);

  TryAddToMempool (mempool, e1);
  TryAddToMempool (mempool, e2);
  BOOST_CHECK (mempool.updatesName (Name ("foo")));

  NameConflictTracker tracker(m_node);
  mempool.removeExpireConflicts ({Name ("foo")});
  tracker.ExpectTxids ({tx1.GetHash (), tx2.GetHash ()});

  BOOST_CHECK (!mempool.updatesName (Name ("foo")));
  BOOST_CHECK (mempool.mapTx.empty ());
}

BOOST_FIXTURE_TEST_CASE (expire_conflicts_chain, NameMempoolTestSetup)
{
  /* Build a real chain of two updates.  The saved txid set is iterated in
     hash order, so try different values until the parent is visited before
     its descendant; then removeRecursive removes both at once.  */

  const CTransaction base = Tx (FirstScript (ADDR, "chain", 'a'));

  bool found = false;
  for (unsigned i = 0; !found; ++i)
    {
      CMutableTransaction mtx;
      mtx.SetNamecoin ();
      mtx.vin.push_back (CTxIn (COutPoint (base.GetHash (), 0)));
      mtx.vout.push_back (CTxOut (
          COIN, UpdateScript (ADDR, "chain", "first " + std::to_string (i))));
      const CTransaction parent (mtx);

      mtx.vin.clear ();
      mtx.vout.clear ();
      mtx.vin.push_back (CTxIn (COutPoint (parent.GetHash (), 0)));
      mtx.vout.push_back (
          CTxOut (COIN, UpdateScript (ADDR, "chain", "second")));
      const CTransaction child (mtx);

      if (parent.GetHash () < child.GetHash ())
        {
          found = true;
          TryAddToMempool (mempool, Entry (parent));
          TryAddToMempool (mempool, Entry (child));
        }
    }

  BOOST_CHECK_EQUAL (mempool.pendingNameChainLength (Name ("chain")), 2);

  mempool.removeExpireConflicts ({Name ("chain")});

  BOOST_CHECK (!mempool.updatesName (Name ("chain")));
  BOOST_CHECK (mempool.mapTx.empty ());
}

BOOST_FIXTURE_TEST_CASE (unexpire_conflicts, NameMempoolTestSetup)
{
  const auto tx = Tx (FirstScript (ADDR, "foo", 'a'));
  const auto e = Entry (tx);

  TryAddToMempool (mempool, e);
  BOOST_CHECK (mempool.registersName (Name ("foo")));

  NameConflictTracker tracker(m_node);
  mempool.removeUnexpireConflicts ({Name ("foo")});
  tracker.ExpectTxids ({tx.GetHash ()});

  BOOST_CHECK (!mempool.registersName (Name ("foo")));
  BOOST_CHECK (mempool.mapTx.empty ());
}

BOOST_FIXTURE_TEST_CASE (package_conflicts, NameMempoolTestSetup)
{
  const auto reg1 = Tx (FirstScript (ADDR, "foo", 'a'));
  const auto reg2 = Tx (FirstScript (ADDR, "foo", 'b'));
  const auto reg3 = Tx (FirstScript (ADDR, "bar", 'a'));

  CNameMemPool::PackageChecker regChecker;
  BOOST_CHECK (regChecker.checkTx (reg1));
  BOOST_CHECK (!regChecker.checkTx (reg2));

  CNameMemPool::PackageChecker distinctRegChecker;
  BOOST_CHECK (distinctRegChecker.checkTx (reg1));
  BOOST_CHECK (distinctRegChecker.checkTx (reg3));

  const auto new1 = Tx (NewScript (ADDR, "foo", 'a'));
  const auto new2 = Tx (NewScript (OTHER_ADDR, "foo", 'a'));
  const auto new3 = Tx (NewScript (ADDR, "foo", 'b'));

  CNameMemPool::PackageChecker newChecker;
  BOOST_CHECK (newChecker.checkTx (new1));
  BOOST_CHECK (!newChecker.checkTx (new2));

  CNameMemPool::PackageChecker distinctNewChecker;
  BOOST_CHECK (distinctNewChecker.checkTx (new1));
  BOOST_CHECK (distinctNewChecker.checkTx (new3));

  const auto upd1 = Tx (UpdateScript (ADDR, "foo", "x"));
  const auto upd2 = Tx (UpdateScript (ADDR, "foo", "y"));

  CNameMemPool::PackageChecker updChecker;
  BOOST_CHECK (updChecker.checkTx (upd1));
  BOOST_CHECK (updChecker.checkTx (upd2));
}

/* ************************************************************************** */

BOOST_AUTO_TEST_SUITE_END ()
