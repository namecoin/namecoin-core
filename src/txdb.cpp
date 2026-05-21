// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txdb.h>

#include <coins.h>
#include <dbwrapper.h>
#include <logging/timer.h>
#include <names/encoding.h>
#include <primitives/transaction.h>
#include <random.h>
#include <serialize.h>
#include <script/names.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/log.h>
#include <util/vector.h>
#include <validation.h>

#include <cassert>
#include <cstdlib>
#include <iterator>
#include <utility>

static constexpr uint8_t DB_COIN{'C'};

static constexpr uint8_t DB_NAME{'n'};
static constexpr uint8_t DB_NAME_HISTORY{'h'};
static constexpr uint8_t DB_NAME_EXPIRY{'x'};
// History-tail expire index: keyed by (height, name), value is empty.
// Records the height of each individual entry archived into
// DB_NAME_HISTORY (i.e. the *previous* live height at the moment of an
// update), not the name's current live height.  Entries are added by
// SetName when an old CNameData gets pushed onto the history stack,
// removed by SetName-undo when the corresponding push is reversed, and
// consumed by CCoinsViewCache::PruneExpiredHistory.  Append-only with
// respect to renewals: a name updated many times has many distinct
// keys here, one per archived entry.
static constexpr uint8_t DB_NAME_HISTORY_EXPIRY{'y'};

static constexpr uint8_t DB_BEST_BLOCK{'B'};
static constexpr uint8_t DB_HEAD_BLOCKS{'H'};
// Keys used in previous version that might still be found in the DB:
static constexpr uint8_t DB_COINS{'c'};

// Threshold for warning when writing this many dirty cache entries to disk.
static constexpr size_t WARN_FLUSH_COINS_COUNT{10'000'000};

bool CCoinsViewDB::NeedsUpgrade()
{
    std::unique_ptr<CDBIterator> cursor{m_db->NewIterator()};
    // DB_COINS was deprecated in v0.15.0, commit
    // 1088b02f0ccd7358d2b7076bb9e122d59d502d02
    cursor->Seek(std::make_pair(DB_COINS, uint256{}));
    std::pair<uint8_t, uint256> key;
    return cursor->Valid() && cursor->GetKey(key) && key.first == DB_COINS;
}

namespace {

struct CoinEntry {
    COutPoint* outpoint;
    uint8_t key{DB_COIN};
    explicit CoinEntry(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)) {}

    SERIALIZE_METHODS(CoinEntry, obj) { READWRITE(obj.key, obj.outpoint->hash, VARINT(obj.outpoint->n)); }
};

} // namespace

CCoinsViewDB::CCoinsViewDB(DBParams db_params, CoinsViewOptions options) :
    m_db_params{std::move(db_params)},
    m_options{std::move(options)},
    m_db{std::make_unique<CDBWrapper>(m_db_params)} { }

void CCoinsViewDB::ResizeCache(size_t new_cache_size)
{
    // We can't do this operation with an in-memory DB since we'll lose all the coins upon
    // reset.
    if (!m_db_params.memory_only) {
        // Have to do a reset first to get the original `m_db` state to release its
        // filesystem lock.
        m_db.reset();
        m_db_params.cache_bytes = new_cache_size;
        m_db_params.wipe_data = false;
        m_db = std::make_unique<CDBWrapper>(m_db_params);
    }
}

std::optional<Coin> CCoinsViewDB::GetCoin(const COutPoint& outpoint) const
{
    if (Coin coin; m_db->Read(CoinEntry(&outpoint), coin)) {
        Assert(!coin.IsSpent()); // The UTXO database should never contain spent coins
        return coin;
    }
    return std::nullopt;
}

std::optional<Coin> CCoinsViewDB::PeekCoin(const COutPoint& outpoint) const
{
    return GetCoin(outpoint);
}

bool CCoinsViewDB::HaveCoin(const COutPoint& outpoint) const
{
    return m_db->Exists(CoinEntry(&outpoint));
}

uint256 CCoinsViewDB::GetBestBlock() const {
    uint256 hashBestChain;
    if (!m_db->Read(DB_BEST_BLOCK, hashBestChain))
        return uint256();
    return hashBestChain;
}

std::vector<uint256> CCoinsViewDB::GetHeadBlocks() const {
    std::vector<uint256> vhashHeadBlocks;
    if (!m_db->Read(DB_HEAD_BLOCKS, vhashHeadBlocks)) {
        return std::vector<uint256>();
    }
    return vhashHeadBlocks;
}

bool CCoinsViewDB::GetName(const valtype &name, CNameData& data) const {
    return m_db->Read(std::make_pair(DB_NAME, name), data);
}

bool CCoinsViewDB::GetNameHistory(const valtype &name, CNameHistory& data) const {
    assert (fNameHistory);
    return m_db->Read(std::make_pair(DB_NAME_HISTORY, name), data);
}

bool CCoinsViewDB::GetNamesForHeight(unsigned nHeight, std::set<valtype>& names) const {
    names.clear();

    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    std::unique_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(m_db.get())->NewIterator());

    const CNameCache::ExpireEntry seekEntry(nHeight, valtype ());
    pcursor->Seek(std::make_pair(DB_NAME_EXPIRY, seekEntry));

    for (; pcursor->Valid(); pcursor->Next())
    {
        std::pair<uint8_t, CNameCache::ExpireEntry> key;
        if (!pcursor->GetKey(key) || key.first != DB_NAME_EXPIRY)
            break;
        const CNameCache::ExpireEntry& entry = key.second;

        assert (entry.nHeight >= nHeight);
        if (entry.nHeight > nHeight)
          break;

        const valtype& name = entry.name;
        if (names.count(name) > 0) {
            LogError ("%s : duplicate name %s in expire index",
                      __func__, EncodeNameForMessage(name));
            return false;
        }
        names.insert(name);
    }

    return true;
}

bool CCoinsViewDB::GetHistoryNamesForHeight(unsigned nHeight, std::set<valtype>& names) const {
    names.clear();

    /* History-tail index lookup, mirroring GetNamesForHeight but against
       DB_NAME_HISTORY_EXPIRY.  Unlike that index, this one is
       append-only per (height, name) pair (one update per name per
       block), so duplicate detection collapses to "key already in the
       set", which would indicate DB corruption.  */
    std::unique_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(m_db.get())->NewIterator());

    const CNameCache::ExpireEntry seekEntry(nHeight, valtype ());
    pcursor->Seek(std::make_pair(DB_NAME_HISTORY_EXPIRY, seekEntry));

    for (; pcursor->Valid(); pcursor->Next())
    {
        std::pair<uint8_t, CNameCache::ExpireEntry> key;
        if (!pcursor->GetKey(key) || key.first != DB_NAME_HISTORY_EXPIRY)
            break;
        const CNameCache::ExpireEntry& entry = key.second;

        assert (entry.nHeight >= nHeight);
        if (entry.nHeight > nHeight)
          break;

        const valtype& name = entry.name;
        if (names.count(name) > 0) {
            LogError ("%s : duplicate name %s in history-expire index",
                      __func__, EncodeNameForMessage(name));
            return false;
        }
        names.insert(name);
    }

    return true;
}

class CDbNameIterator : public CNameIterator
{

private:

    /** The backing LevelDB iterator.  */
    std::unique_ptr<CDBIterator> iter;

public:

    /**
     * Construct a new name iterator for the database.
     * @param db The database to create the iterator for.
     */
    CDbNameIterator(const CDBWrapper& db);

    /* Implement iterator methods.  */
    void seek (const valtype& start) override;
    bool next (valtype& name, CNameData& data) override;

};

CDbNameIterator::CDbNameIterator(const CDBWrapper& db)
    : iter(const_cast<CDBWrapper*>(&db)->NewIterator())
{
    seek(valtype());
}

void CDbNameIterator::seek(const valtype& start) {
    iter->Seek(std::make_pair(DB_NAME, start));
}

bool CDbNameIterator::next(valtype& name, CNameData& data) {
    if (!iter->Valid())
        return false;

    std::pair<uint8_t, valtype> key;
    if (!iter->GetKey(key) || key.first != DB_NAME)
        return false;
    name = key.second;

    if (!iter->GetValue(data)) {
        LogError ("%s : failed to read data from iterator", __func__);
        return false;
    }

    iter->Next ();
    return true;
}

CNameIterator* CCoinsViewDB::IterateNames() const {
    return new CDbNameIterator(*m_db);
}

void CCoinsViewDB::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash, const CNameCache& names)
{
    CDBBatch batch(*m_db);
    size_t count = 0;
    const size_t dirty_count{cursor.GetDirtyCount()};
    assert(!block_hash.IsNull());

    uint256 old_tip = GetBestBlock();
    if (old_tip.IsNull()) {
        // We may be in the middle of replaying.
        std::vector<uint256> old_heads = GetHeadBlocks();
        if (old_heads.size() == 2) {
            if (old_heads[0] != block_hash) {
                LogError("The coins database detected an inconsistent state, likely due to a previous crash or shutdown. You will need to restart bitcoind with the -reindex-chainstate or -reindex configuration option.\n");
            }
            assert(old_heads[0] == block_hash);
            old_tip = old_heads[1];
        }
    }

    if (dirty_count > WARN_FLUSH_COINS_COUNT) LogWarning("Flushing large (%d entries) UTXO set to disk, it may take several minutes", dirty_count);
    LOG_TIME_MILLIS_WITH_CATEGORY(strprintf("write coins cache to disk (%d out of %d cached coins)",
        dirty_count, cursor.GetTotalCount()), BCLog::BENCH);

    // In the first batch, mark the database as being in the middle of a
    // transition from old_tip to block_hash.
    // A vector is used for future extensibility, as we may want to support
    // interrupting after partial writes from multiple independent reorgs.
    batch.Erase(DB_BEST_BLOCK);
    batch.Write(DB_HEAD_BLOCKS, Vector(block_hash, old_tip));

    for (auto it{cursor.Begin()}; it != cursor.End();) {
        if (it->second.IsDirty()) {
            CoinEntry entry(&it->first);
            if (it->second.coin.IsSpent()) {
                batch.Erase(entry);
            } else {
                batch.Write(entry, it->second.coin);
            }
        }
        count++;
        it = cursor.NextAndMaybeErase(*it);
        if (batch.ApproximateSize() > m_options.batch_write_bytes) {
            LogDebug(BCLog::COINDB, "Writing partial batch of %.2f MiB\n", batch.ApproximateSize() / double(1_MiB));

            m_db->WriteBatch(batch);
            batch.Clear();
            if (m_options.simulate_crash_ratio) {
                static FastRandomContext rng;
                if (rng.randrange(m_options.simulate_crash_ratio) == 0) {
                    LogError("Simulating a crash. Goodbye.");
                    _Exit(0);
                }
            }
        }
    }

    names.writeBatch(batch);

    // In the last batch, mark the database as consistent with block_hash again.
    batch.Erase(DB_HEAD_BLOCKS);
    batch.Write(DB_BEST_BLOCK, block_hash);

    LogDebug(BCLog::COINDB, "Writing final batch of %.2f MiB\n", batch.ApproximateSize() / double(1_MiB));
    m_db->WriteBatch(batch);
    LogDebug(BCLog::COINDB, "Committed %u changed transaction outputs (out of %u) to coin database...", (unsigned int)dirty_count, (unsigned int)count);
}

size_t CCoinsViewDB::EstimateSize() const
{
    return m_db->EstimateSize(DB_COIN, uint8_t(DB_COIN + 1));
}

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor: public CCoinsViewCursor
{
public:
    // Prefer using CCoinsViewDB::Cursor() since we want to perform some
    // cache warmup on instantiation.
    CCoinsViewDBCursor(CDBIterator* pcursorIn, const uint256& in_block_hash):
        CCoinsViewCursor(in_block_hash), pcursor(pcursorIn) {}
    ~CCoinsViewDBCursor() = default;

    bool GetKey(COutPoint &key) const override;
    bool GetValue(Coin &coin) const override;

    bool Valid() const override;
    void Next() override;

private:
    std::unique_ptr<CDBIterator> pcursor;
    std::pair<char, COutPoint> keyTmp;

    friend class CCoinsViewDB;
};

std::unique_ptr<CCoinsViewCursor> CCoinsViewDB::Cursor() const
{
    auto i = std::make_unique<CCoinsViewDBCursor>(
        const_cast<CDBWrapper&>(*m_db).NewIterator(), GetBestBlock());
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(DB_COIN);
    // Cache key of first record
    if (i->pcursor->Valid()) {
        CoinEntry entry(&i->keyTmp.second);
        i->pcursor->GetKey(entry);
        i->keyTmp.first = entry.key;
    } else {
        i->keyTmp.first = 0; // Make sure Valid() and GetKey() return false
    }
    return i;
}

bool CCoinsViewDBCursor::GetKey(COutPoint &key) const
{
    // Return cached key
    if (keyTmp.first == DB_COIN) {
        key = keyTmp.second;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::GetValue(Coin &coin) const
{
    return pcursor->GetValue(coin);
}

bool CCoinsViewDBCursor::Valid() const
{
    return keyTmp.first == DB_COIN;
}

void CCoinsViewDBCursor::Next()
{
    pcursor->Next();
    CoinEntry entry(&keyTmp.second);
    if (!pcursor->Valid() || !pcursor->GetKey(entry)) {
        keyTmp.first = 0; // Invalidate cached key after last record so that Valid() and GetKey() return false
    } else {
        keyTmp.first = entry.key;
    }
}

bool CCoinsViewDB::ValidateNameDB(const Chainstate& chainState, const std::function<void()>& interruption_point) const
{
    const uint256 blockHash = GetBestBlock();
    int nHeight;
    if (blockHash.IsNull())
        nHeight = 0;
    else
        nHeight = chainState.m_blockman.m_block_index.find(blockHash)->second.nHeight;

    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    std::unique_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(m_db.get())->NewIterator());
    pcursor->SeekToFirst();

    /* Loop over the total database and read interesting
       things to memory.  We later use that to check
       everything against each other.  */

    std::map<valtype, unsigned> nameHeightsIndex;
    std::map<valtype, unsigned> nameHeightsData;
    std::set<valtype> namesInDB;
    std::set<valtype> namesInUTXO;
    std::set<valtype> namesWithHistory;
    /* Per-name set of heights present in DB_NAME_HISTORY values, used
       to validate the DB_NAME_HISTORY_EXPIRY index.  */
    std::map<valtype, std::set<unsigned>> historyEntryHeights;
    /* Per-name set of (height, name) keys in DB_NAME_HISTORY_EXPIRY.  */
    std::map<valtype, std::set<unsigned>> historyIndexHeights;

    for (; pcursor->Valid(); pcursor->Next())
    {
        interruption_point();
        uint8_t chType;
        if (!pcursor->GetKey(chType))
            continue;

        switch (chType)
        {
        case DB_COIN:
        {
            Coin coin;
            if (!pcursor->GetValue(coin)) {
                LogError ("%s : failed to read coin", __func__);
                return false;
            }

            if (!coin.out.IsNull())
            {
                const CNameScript nameOp(coin.out.scriptPubKey);
                if (nameOp.isNameOp() && nameOp.isAnyUpdate())
                {
                    const valtype& name = nameOp.getOpName();
                    if (namesInUTXO.count(name) > 0) {
                        LogError ("%s : name %s duplicated in UTXO set",
                                  __func__, EncodeNameForMessage(name));
                        return false;
                    }
                    namesInUTXO.insert(nameOp.getOpName());
                }
            }
            break;
        }

        case DB_NAME:
        {
            std::pair<uint8_t, valtype> key;
            if (!pcursor->GetKey(key) || key.first != DB_NAME) {
                LogError ("%s : failed to read DB_NAME key", __func__);
                return false;
            }
            const valtype& name = key.second;

            CNameData data;
            if (!pcursor->GetValue(data)) {
                LogError ("%s : failed to read name value", __func__);
                return false;
            }

            if (nameHeightsData.count(name) > 0) {
                LogError ("%s : name %s duplicated in name index",
                          __func__, EncodeNameForMessage(name));
                return false;
            }
            nameHeightsData.insert(std::make_pair(name, data.getHeight()));
            
            /* Expiration is checked at height+1, because that matches
               how the UTXO set is cleared in ExpireNames.  */
            assert(namesInDB.count(name) == 0);
            if (!data.isExpired(nHeight + 1))
                namesInDB.insert(name);
            break;
        }

        case DB_NAME_HISTORY:
        {
            std::pair<uint8_t, valtype> key;
            if (!pcursor->GetKey(key) || key.first != DB_NAME_HISTORY) {
                LogError ("%s : failed to read DB_NAME_HISTORY key", __func__);
                return false;
            }
            const valtype& name = key.second;

            if (namesWithHistory.count(name) > 0) {
                LogError ("%s : name %s has duplicate history",
                          __func__, EncodeNameForMessage(name));
                return false;
            }
            namesWithHistory.insert(name);

            /* Read the history payload too, so we can cross-check the
               new DB_NAME_HISTORY_EXPIRY index below.  */
            CNameHistory hist;
            if (!pcursor->GetValue(hist)) {
                LogError ("%s : failed to read DB_NAME_HISTORY value", __func__);
                return false;
            }
            for (const CNameData& entry : hist.getData ())
                historyEntryHeights[name].insert (entry.getHeight ());
            break;
        }

        case DB_NAME_EXPIRY:
        {
            std::pair<uint8_t, CNameCache::ExpireEntry> key;
            if (!pcursor->GetKey(key) || key.first != DB_NAME_EXPIRY) {
                LogError ("%s : failed to read DB_NAME_EXPIRY key", __func__);
                return false;
            }
            const CNameCache::ExpireEntry& entry = key.second;
            const valtype& name = entry.name;

            if (nameHeightsIndex.count(name) > 0) {
                LogError ("%s : name %s duplicated in expire idnex",
                          __func__, EncodeNameForMessage(name));
                return false;
            }

            nameHeightsIndex.insert(std::make_pair(name, entry.nHeight));
            break;
        }

        case DB_NAME_HISTORY_EXPIRY:
        {
            std::pair<uint8_t, CNameCache::ExpireEntry> key;
            if (!pcursor->GetKey(key)
                || key.first != DB_NAME_HISTORY_EXPIRY) {
                LogError ("%s : failed to read DB_NAME_HISTORY_EXPIRY key",
                          __func__);
                return false;
            }
            const CNameCache::ExpireEntry& entry = key.second;
            auto [it, inserted]
              = historyIndexHeights[entry.name].insert (entry.nHeight);
            if (!inserted) {
                LogError ("%s : duplicate key in history-expire index"
                          " for name %s at height %u",
                          __func__,
                          EncodeNameForMessage(entry.name), entry.nHeight);
                return false;
            }
            break;
        }

        default:
            break;
        }
    }

    /* Now verify the collected data.  */

    assert (nameHeightsData.size() >= namesInDB.size());

    if (nameHeightsIndex != nameHeightsData) {
        LogError ("%s : name height data mismatch", __func__);
        return false;
    }

    for (const auto& name : namesInDB)
        if (namesInUTXO.count(name) == 0) {
            LogError ("%s : name '%s' in DB but not UTXO set",
                      __func__, EncodeNameForMessage(name));
            return false;
        }
    for (const auto& name : namesInUTXO)
        if (namesInDB.count(name) == 0) {
            LogError ("%s : name '%s' in UTXO set but not DB",
                      __func__, EncodeNameForMessage(name));
            return false;
        }

    if (fNameHistory)
    {
        for (const auto& name : namesWithHistory)
            if (nameHeightsData.count(name) == 0) {
                LogError ("%s : history entry for name '%s' not in main DB",
                          __func__, EncodeNameForMessage(name));
                return false;
            }

        /* History-tail index validation.  Every key in the index must
           reference a real DB_NAME_HISTORY entry at that height (otherwise
           we have a dangling index entry that would cause the trim driver
           to visit a name with no matching history row -- harmless but
           wrong).  We do NOT require the converse (every history entry
           must be indexed): pre-existing DB_NAME_HISTORY rows from before
           the history-tail index shipped will not have index entries until
           a reindex.  -prunenamehistory documents this catch-up gap.  */
        for (const auto& [name, heights] : historyIndexHeights)
        {
            const auto it = historyEntryHeights.find (name);
            if (it == historyEntryHeights.end ()) {
                LogError ("%s : history-expire index references name '%s'"
                          " with no DB_NAME_HISTORY row",
                          __func__, EncodeNameForMessage(name));
                return false;
            }
            for (const unsigned h : heights)
                if (it->second.count (h) == 0) {
                    LogError ("%s : history-expire index references"
                              " name '%s' at height %u, but no matching"
                              " history entry exists",
                              __func__, EncodeNameForMessage(name), h);
                    return false;
                }
        }
    } else {
        if (!namesWithHistory.empty ()) {
            LogError ("%s : name_history entries in DB, but"
                      " -namehistory not set", __func__);
            return false;
        }
        if (!historyIndexHeights.empty ()) {
            LogError ("%s : history-expire index entries in DB, but"
                      " -namehistory not set", __func__);
            return false;
        }
    }

    LogInfo("Checked name database, %u unexpired names, %u total.\n",
            namesInDB.size(), nameHeightsData.size());
    LogInfo("Names with history: %u\n", namesWithHistory.size());

    return true;
}

void
CNameCache::writeBatch (CDBBatch& batch) const
{
  for (EntryMap::const_iterator i = entries.begin ();
       i != entries.end (); ++i)
    batch.Write (std::make_pair (DB_NAME, i->first), i->second);

  for (std::set<valtype>::const_iterator i = deleted.begin ();
       i != deleted.end (); ++i)
    batch.Erase (std::make_pair (DB_NAME, *i));

  assert (fNameHistory || history.empty ());
  for (std::map<valtype, CNameHistory>::const_iterator i = history.begin ();
       i != history.end (); ++i)
    if (i->second.empty ())
      batch.Erase (std::make_pair (DB_NAME_HISTORY, i->first));
    else
      batch.Write (std::make_pair (DB_NAME_HISTORY, i->first), i->second);

  for (std::map<ExpireEntry, bool>::const_iterator i = expireIndex.begin ();
       i != expireIndex.end (); ++i)
    if (i->second)
      batch.Write (std::make_pair (DB_NAME_EXPIRY, i->first));
    else
      batch.Erase (std::make_pair (DB_NAME_EXPIRY, i->first));

  assert (fNameHistory || historyExpireIndex.empty ());
  for (std::map<ExpireEntry, bool>::const_iterator i = historyExpireIndex.begin ();
       i != historyExpireIndex.end (); ++i)
    if (i->second)
      batch.Write (std::make_pair (DB_NAME_HISTORY_EXPIRY, i->first));
    else
      batch.Erase (std::make_pair (DB_NAME_HISTORY_EXPIRY, i->first));
}
