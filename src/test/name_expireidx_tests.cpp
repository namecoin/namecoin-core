// Copyright (c) 2014-2026 Daniel Kraft
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <key_io.h>
#include <names/common.h>
#include <names/encoding.h>
#include <primitives/transaction.h>
#include <script/names.h>
#include <script/script.h>
#include <streams.h>
#include <txdb.h>
#include <uint256.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <set>
#include <string>
#include <vector>

/* No space between BOOST_FIXTURE_TEST_SUITE and '(', so that extraction of
   the test-suite name works with grep as done in the Makefile.  */
BOOST_FIXTURE_TEST_SUITE(name_expireidx_tests, TestingSetup)

namespace
{

/**
 * Decode a plain ASCII string into a valtype name.
 */
valtype
N (const std::string& str)
{
  return DecodeName (str, NameEncoding::ASCII);
}

/**
 * Build a CNameData with a given creation height by routing through a
 * real CNameScript so that the resulting object survives all the usual
 * invariants enforced by SetName / CCoinsViewCache.
 */
CNameData
DummyData (const std::string& name_str, unsigned nHeight)
{
  const CTxDestination dest
    = DecodeDestination ("N5e1vXUUL3KfhPyVjQZSes1qQ7eyarDbUU");
  const CScript addr = GetScriptForDestination (dest);
  const valtype name = DecodeName (name_str, NameEncoding::ASCII);
  const valtype value = DecodeName ("dummy", NameEncoding::ASCII);
  const CScript script = CNameScript::buildNameUpdate (addr, name, value);
  const CNameScript nameOp (script);

  CNameData data;
  data.fromScript (nHeight, COutPoint (Txid (), 0), nameOp);
  return data;
}

/**
 * Flush a CCoinsViewCache to its base.  Uses a deterministic dummy block
 * hash derived from a salt so that successive flushes don't trip the
 * "best block" invariants in BatchWrite.
 */
void
FlushWithHash (CCoinsViewCache& view, uint8_t salt)
{
  uint256 hash;
  *hash.begin () = salt;
  view.SetBestBlock (hash);
  view.Flush ();
}

} // anonymous namespace

/* ************************************************************************** */

/**
 * Add several names that all expire at the same height, flush to the
 * backing database, and verify that GetNamesForHeight returns exactly
 * the set we inserted.
 */
BOOST_AUTO_TEST_CASE (round_trip_single_height)
{
  LOCK (cs_main);
  CCoinsViewDB& db = m_node.chainman->ActiveChainstate ().CoinsDB ();
  CCoinsViewCache cache (&db);

  const unsigned height = 200000;
  const std::vector<std::string> name_strs{"aaa", "bbb", "ccc", "ddd", "eee"};
  std::vector<valtype> names;
  for (const std::string& s : name_strs)
    names.push_back (N (s));

  for (const std::string& s : name_strs)
    cache.SetName (N (s), DummyData (s, height), false);

  FlushWithHash (cache, 0x01);

  std::set<valtype> got;
  BOOST_CHECK (db.GetNamesForHeight (height, got));
  BOOST_CHECK (got == std::set<valtype> (names.begin (), names.end ()));

  /* An unrelated height must be empty.  */
  BOOST_CHECK (db.GetNamesForHeight (height + 1, got));
  BOOST_CHECK (got.empty ());
}

/* ************************************************************************** */

/**
 * Apply a mixed add + remove batch at the same height in a single
 * cache flush, then verify the resulting on-disk set.
 */
BOOST_AUTO_TEST_CASE (mixed_add_remove_one_flush)
{
  LOCK (cs_main);
  CCoinsViewDB& db = m_node.chainman->ActiveChainstate ().CoinsDB ();
  CCoinsViewCache cache (&db);

  const unsigned height = 250000;

  /* Seed the backing DB with three names.  */
  cache.SetName (N ("keep-1"), DummyData ("keep-1", height), false);
  cache.SetName (N ("drop-1"), DummyData ("drop-1", height), false);
  cache.SetName (N ("drop-2"), DummyData ("drop-2", height), false);
  FlushWithHash (cache, 0x10);

  std::set<valtype> got;
  BOOST_CHECK (db.GetNamesForHeight (height, got));
  BOOST_CHECK_EQUAL (got.size (), 3);

  /* Now in a fresh cache, drop two and add two more.  */
  CCoinsViewCache cache2 (&db);
  cache2.DeleteName (N ("drop-1"));
  cache2.DeleteName (N ("drop-2"));
  cache2.SetName (N ("add-1"), DummyData ("add-1", height), false);
  cache2.SetName (N ("add-2"), DummyData ("add-2", height), false);
  FlushWithHash (cache2, 0x11);

  BOOST_CHECK (db.GetNamesForHeight (height, got));
  std::set<valtype> expected{N ("keep-1"), N ("add-1"), N ("add-2")};
  BOOST_CHECK (got == expected);
}

/* ************************************************************************** */

/**
 * Removing every name at a height must erase the on-disk row entirely
 * (i.e. GetNamesForHeight reports an empty set, with no stale row left
 * behind).
 */
BOOST_AUTO_TEST_CASE (empty_set_erases_row)
{
  LOCK (cs_main);
  CCoinsViewDB& db = m_node.chainman->ActiveChainstate ().CoinsDB ();
  CCoinsViewCache cache (&db);

  const unsigned height = 300000;

  cache.SetName (N ("only-1"), DummyData ("only-1", height), false);
  cache.SetName (N ("only-2"), DummyData ("only-2", height), false);
  FlushWithHash (cache, 0x20);

  std::set<valtype> got;
  BOOST_CHECK (db.GetNamesForHeight (height, got));
  BOOST_CHECK_EQUAL (got.size (), 2);

  CCoinsViewCache cache2 (&db);
  cache2.DeleteName (N ("only-1"));
  cache2.DeleteName (N ("only-2"));
  FlushWithHash (cache2, 0x21);

  BOOST_CHECK (db.GetNamesForHeight (height, got));
  BOOST_CHECK (got.empty ());

  /* Re-adding a single name at the same height must work cleanly,
     proving the row was actually erased and not just left with a
     stale value.  */
  CCoinsViewCache cache3 (&db);
  cache3.SetName (N ("revived"), DummyData ("revived", height), false);
  FlushWithHash (cache3, 0x22);

  BOOST_CHECK (db.GetNamesForHeight (height, got));
  std::set<valtype> expected{N ("revived")};
  BOOST_CHECK (got == expected);
}

/* ************************************************************************** */

/**
 * Verify that the packed expire-index key serialises height in
 * big-endian byte order, so that LevelDB key ordering matches numeric
 * height ordering even across pairs that disagree in the low byte.
 *
 * This guards against silently switching back to host byte order, which
 * would scramble the iterator-based ValidateNameDB path.
 */
BOOST_AUTO_TEST_CASE (height_key_big_endian_ordering)
{
  DataStream ssLow{};
  DataStream ssHigh{};

  const CNameCache::ExpireKey keyLow (0x000000ff);
  const CNameCache::ExpireKey keyHigh (0x00000142);

  ssLow << keyLow;
  ssHigh << keyHigh;

  std::vector<uint8_t> bytesLow;
  std::vector<uint8_t> bytesHigh;
  for (const auto b : ssLow)
    bytesLow.push_back (static_cast<uint8_t> (b));
  for (const auto b : ssHigh)
    bytesHigh.push_back (static_cast<uint8_t> (b));

  /* Big-endian means the low height encodes as 00 00 00 ff and the
     larger one as 00 00 01 42, which compare correctly as byte
     strings.  Little-endian would have produced ff 00 00 00 vs
     42 01 00 00 and ordered them the wrong way round.  */
  BOOST_CHECK_EQUAL (bytesLow.size (), 4);
  BOOST_CHECK_EQUAL (bytesHigh.size (), 4);
  BOOST_CHECK (bytesLow < bytesHigh);

  BOOST_CHECK_EQUAL (bytesLow[0], 0x00);
  BOOST_CHECK_EQUAL (bytesLow[3], 0xff);
  BOOST_CHECK_EQUAL (bytesHigh[2], 0x01);
  BOOST_CHECK_EQUAL (bytesHigh[3], 0x42);

  /* Round-trip through Unserialize.  */
  DataStream ssRoundTrip{};
  ssRoundTrip << keyLow;
  CNameCache::ExpireKey decoded;
  ssRoundTrip >> decoded;
  BOOST_CHECK_EQUAL (decoded.nHeight, static_cast<uint32_t> (0x000000ff));
}

/* ************************************************************************** */

/**
 * Exercise the writeBatch read-modify-write path with two distinct
 * heights touched in the same flush, including the case where one
 * height is created from empty and the other is partly cleared from
 * an existing row.
 */
BOOST_AUTO_TEST_CASE (two_heights_one_flush)
{
  LOCK (cs_main);
  CCoinsViewDB& db = m_node.chainman->ActiveChainstate ().CoinsDB ();
  CCoinsViewCache cache (&db);

  const unsigned h1 = 410000;
  const unsigned h2 = 410500;

  /* Pre-populate h1 with two entries.  */
  cache.SetName (N ("h1-old-1"), DummyData ("h1-old-1", h1), false);
  cache.SetName (N ("h1-old-2"), DummyData ("h1-old-2", h1), false);
  FlushWithHash (cache, 0x30);

  /* Now in one flush: remove one entry at h1, add one at h1, and
     populate a fresh h2.  */
  CCoinsViewCache cache2 (&db);
  cache2.DeleteName (N ("h1-old-1"));
  cache2.SetName (N ("h1-new"), DummyData ("h1-new", h1), false);
  cache2.SetName (N ("h2-a"), DummyData ("h2-a", h2), false);
  cache2.SetName (N ("h2-b"), DummyData ("h2-b", h2), false);
  FlushWithHash (cache2, 0x31);

  std::set<valtype> got1, got2;
  BOOST_CHECK (db.GetNamesForHeight (h1, got1));
  BOOST_CHECK (db.GetNamesForHeight (h2, got2));

  std::set<valtype> exp1{N ("h1-old-2"), N ("h1-new")};
  std::set<valtype> exp2{N ("h2-a"), N ("h2-b")};
  BOOST_CHECK (got1 == exp1);
  BOOST_CHECK (got2 == exp2);
}

/* ************************************************************************** */

BOOST_AUTO_TEST_SUITE_END ()
