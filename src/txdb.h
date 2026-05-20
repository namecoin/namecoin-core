// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <coins.h>
#include <dbwrapper.h>
#include <kernel/caches.h>
#include <kernel/cs_main.h>
#include <sync.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class COutPoint;
class uint256;

//! User-controlled performance and debug options.
struct CoinsViewOptions {
    //! Maximum database write batch size in bytes.
    size_t batch_write_bytes{DEFAULT_DB_CACHE_BATCH};
    //! If non-zero, randomly exit when the database is flushed with (1/ratio) probability.
    int simulate_crash_ratio{0};
};

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB final : public CCoinsView
{
protected:
    DBParams m_db_params;
    CoinsViewOptions m_options;
    std::unique_ptr<CDBWrapper> m_db;
public:
    explicit CCoinsViewDB(DBParams db_params, CoinsViewOptions options);

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    bool GetName(const valtype &name, CNameData &data) const override;
    bool GetNameHistory(const valtype &name, CNameHistory &data) const override;
    bool GetNamesForHeight(unsigned nHeight, std::set<valtype>& data) const override;
    CNameIterator* IterateNames() const override;
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash, const CNameCache& names) override;
    std::unique_ptr<CCoinsViewCursor> Cursor() const override;
    bool ValidateNameDB(const Chainstate& chainState, const std::function<void()>& interruption_point) const override;

    //! Whether an unsupported database format is used.
    bool NeedsUpgrade();

    //! Write the current name-database format version into the chainstate
    //! if it is not already present and the database has no name rows yet.
    //! Called once at startup, after wiping if applicable, so that a fresh
    //! or reindexed database is unambiguously stamped as belonging to the
    //! current layout.  No-op if the version key is already present or if
    //! name data exists without a matching version (that case is the
    //! responsibility of NeedsUpgrade).
    void MaybeStampNameDbVersion();

    size_t EstimateSize() const override;

    //! Erase the name-database format version marker.  Used only by the
    //! functional test that exercises the legacy-format detection path;
    //! exposed here so the test can simulate a database written by an
    //! older release that never wrote the version key.
    void EraseNameDbVersionForTesting();

    //! Dynamically alter the underlying leveldb cache size.
    void ResizeCache(size_t new_cache_size) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
};

#endif // BITCOIN_TXDB_H
