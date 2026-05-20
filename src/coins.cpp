// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>

#include <consensus/consensus.h>
#include <random.h>
#include <uint256.h>
#include <util/log.h>
#include <util/trace.h>

TRACEPOINT_SEMAPHORE(utxocache, add);
TRACEPOINT_SEMAPHORE(utxocache, spent);
TRACEPOINT_SEMAPHORE(utxocache, uncache);

CoinsViewEmpty& CoinsViewEmpty::Get()
{
    static CoinsViewEmpty instance;
    return instance;
}

std::optional<Coin> CCoinsViewCache::PeekCoin(const COutPoint& outpoint) const
{
    if (auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
        return it->second.coin.IsSpent() ? std::nullopt : std::optional{it->second.coin};
    }
    return base->PeekCoin(outpoint);
}

CCoinsViewCache::CCoinsViewCache(CCoinsView* in_base, bool deterministic) :
    CCoinsViewBacked(in_base), m_deterministic(deterministic),
    cacheCoins(0, SaltedOutpointHasher(/*deterministic=*/deterministic), CCoinsMap::key_equal{}, &m_cache_coins_memory_resource)
{
    m_sentinel.second.SelfRef(m_sentinel);
}

size_t CCoinsViewCache::DynamicMemoryUsage() const {
    return memusage::DynamicUsage(cacheCoins) + cachedCoinsUsage;
}

std::optional<Coin> CCoinsViewCache::FetchCoinFromBase(const COutPoint& outpoint) const
{
    return base->GetCoin(outpoint);
}

CCoinsMap::iterator CCoinsViewCache::FetchCoin(const COutPoint &outpoint) const {
    const auto [ret, inserted] = cacheCoins.try_emplace(outpoint);
    if (inserted) {
        if (auto coin{FetchCoinFromBase(outpoint)}) {
            ret->second.coin = std::move(*coin);
            cachedCoinsUsage += ret->second.coin.DynamicMemoryUsage();
            Assert(!ret->second.coin.IsSpent());
        } else {
            cacheCoins.erase(ret);
            return cacheCoins.end();
        }
    }
    return ret;
}

std::optional<Coin> CCoinsViewCache::GetCoin(const COutPoint& outpoint) const
{
    if (auto it{FetchCoin(outpoint)}; it != cacheCoins.end() && !it->second.coin.IsSpent()) return it->second.coin;
    return std::nullopt;
}

void CCoinsViewCache::AddCoin(const COutPoint &outpoint, Coin&& coin, bool possible_overwrite) {
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    CCoinsMap::iterator it;
    bool inserted;
    std::tie(it, inserted) = cacheCoins.emplace(std::piecewise_construct, std::forward_as_tuple(outpoint), std::tuple<>());
    bool fresh = false;
    if (!possible_overwrite) {
        if (!it->second.coin.IsSpent()) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // If the coin exists in this cache as a spent coin and is DIRTY, then
        // its spentness hasn't been flushed to the parent cache. We're
        // re-adding the coin to this cache now but we can't mark it as FRESH.
        // If we mark it FRESH and then spend it before the cache is flushed
        // we would remove it from this cache and would never flush spentness
        // to the parent cache.
        //
        // Re-adding a spent coin can happen in the case of a re-org (the coin
        // is 'spent' when the block adding it is disconnected and then
        // re-added when it is also added in a newly connected block).
        //
        // If the coin doesn't exist in the current cache, or is spent but not
        // DIRTY, then it can be marked FRESH.
        fresh = !it->second.IsDirty();
    }
    if (!inserted) {
        Assume(TrySub(m_dirty_count, it->second.IsDirty()));
        Assume(TrySub(cachedCoinsUsage, it->second.coin.DynamicMemoryUsage()));
    }
    it->second.coin = std::move(coin);
    CCoinsCacheEntry::SetDirty(*it, m_sentinel);
    ++m_dirty_count;
    if (fresh) CCoinsCacheEntry::SetFresh(*it, m_sentinel);
    cachedCoinsUsage += it->second.coin.DynamicMemoryUsage();
    TRACEPOINT(utxocache, add,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
}

void CCoinsViewCache::EmplaceCoinInternalDANGER(COutPoint&& outpoint, Coin&& coin) {
    const auto mem_usage{coin.DynamicMemoryUsage()};
    auto [it, inserted] = cacheCoins.try_emplace(std::move(outpoint), std::move(coin));
    if (inserted) {
        CCoinsCacheEntry::SetDirty(*it, m_sentinel);
        ++m_dirty_count;
        cachedCoinsUsage += mem_usage;
    }
}

void AddCoins(CCoinsViewCache& cache, const CTransaction &tx, int nHeight, bool check_for_overwrite) {
    bool fCoinbase = tx.IsCoinBase();
    const Txid& txid = tx.GetHash();
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        bool overwrite = check_for_overwrite ? cache.HaveCoin(COutPoint(txid, i)) : fCoinbase;
        // Coinbase transactions can always be overwritten, in order to correctly
        // deal with the pre-BIP30 occurrences of duplicate coinbase transactions.
        cache.AddCoin(COutPoint(txid, i), Coin(tx.vout[i], nHeight, fCoinbase), overwrite);
    }
}

bool CCoinsViewCache::SpendCoin(const COutPoint &outpoint, Coin* moveout) {
    CCoinsMap::iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) return false;
    Assume(TrySub(m_dirty_count, it->second.IsDirty()));
    Assume(TrySub(cachedCoinsUsage, it->second.coin.DynamicMemoryUsage()));
    TRACEPOINT(utxocache, spent,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
    if (moveout) {
        *moveout = std::move(it->second.coin);
    }
    if (it->second.IsFresh()) {
        cacheCoins.erase(it);
    } else {
        CCoinsCacheEntry::SetDirty(*it, m_sentinel);
        ++m_dirty_count;
        it->second.coin.Clear();
    }
    return true;
}

static const Coin coinEmpty;

const Coin& CCoinsViewCache::AccessCoin(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) {
        return coinEmpty;
    } else {
        return it->second.coin;
    }
}

bool CCoinsViewCache::HaveCoin(const COutPoint& outpoint) const
{
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    return (it != cacheCoins.end() && !it->second.coin.IsSpent());
}

bool CCoinsViewCache::HaveCoinInCache(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = cacheCoins.find(outpoint);
    return (it != cacheCoins.end() && !it->second.coin.IsSpent());
}

uint256 CCoinsViewCache::GetBestBlock() const {
    if (m_block_hash.IsNull())
        m_block_hash = base->GetBestBlock();
    return m_block_hash;
}

void CCoinsViewCache::SetBestBlock(const uint256& in_block_hash)
{
    m_block_hash = in_block_hash;
}

bool CCoinsViewCache::GetName(const valtype &name, CNameData& data) const {
    if (cacheNames.isDeleted(name))
        return false;
    if (cacheNames.get(name, data))
        return true;

    /* Note: This does not attempt to cache name queries.  The cache
       only keeps track of changes!  */

    return base->GetName(name, data);
}

bool CCoinsViewCache::GetNameHistory(const valtype &name, CNameHistory& data) const {
    if (cacheNames.getHistory(name, data))
        return true;

    /* Note: This does not attempt to cache backend queries.  The cache
       only keeps track of changes!  */

    return base->GetNameHistory(name, data);
}

bool CCoinsViewCache::GetNamesForHeight(unsigned nHeight, std::set<valtype>& names) const {
    /* Query the base view first, and then apply the cached changes (if
       there are any).  */

    if (!base->GetNamesForHeight(nHeight, names))
        return false;

    cacheNames.updateNamesForHeight(nHeight, names);
    return true;
}

CNameIterator* CCoinsViewCache::IterateNames() const {
    return cacheNames.iterateNames(base->IterateNames());
}

/* undo is set if the change is due to disconnecting blocks / going back in
   time.  The ordinary case (!undo) means that we update the name normally,
   going forward in time.  This is important for keeping track of the
   name history.  */
void CCoinsViewCache::SetName(const valtype &name, const CNameData& data, bool undo) {
    CNameData oldData;
    if (GetName(name, oldData))
    {
        cacheNames.removeExpireIndex(name, oldData.getHeight());

        /* Update the name history.  If we are undoing, we expect that
           the top history item matches the data being set now.  If we
           are not undoing, push the overwritten data onto the history stack.
           Note that we only have to do this if the name already existed
           in the database.  Otherwise, no special action is required
           for the name history.  */
        if (fNameHistory)
        {
            CNameHistory history;
            if (!GetNameHistory(name, history))
            {
                /* Ensure that the history stack is indeed (still) empty
                   and was not modified by the failing GetNameHistory call.  */
                assert(history.empty());
            }

            if (undo)
            {
                /* Normally we expect history to be non-empty and its top
                   to match `data`.  However, with -prunenamehistory the
                   stack may have been dropped earlier; in that case the
                   undo is intentionally lossy and we just leave the
                   history empty.  */
                if (!history.empty ())
                    history.pop(data);
            }
            else
                history.push(oldData);

            cacheNames.setHistory(name, history);
        }
    } else
        assert (!undo);

    cacheNames.set(name, data);
    cacheNames.addExpireIndex(name, data.getHeight());
}

void CCoinsViewCache::DeleteName(const valtype &name) {
    CNameData oldData;
    if (GetName(name, oldData))
        cacheNames.removeExpireIndex(name, oldData.getHeight());
    else
        assert(false);

    if (fNameHistory)
    {
        /* When deleting a name, the history should already be clean.  */
        CNameHistory history;
        assert (!GetNameHistory(name, history) || history.empty());
    }

    cacheNames.remove(name);
}

void CCoinsViewCache::PruneExpiredHistory(unsigned nHeight,
                                          unsigned expirationDepth,
                                          unsigned pruneDepth)
{
    /* This is a node-local optimisation.  It must never run when name
       history is disabled (the cache invariants assume that history is
       only ever touched under fNameHistory), and it is a no-op when
       pruning is disabled by configuration.  */
    if (!fNameHistory || pruneDepth == 0)
        return;

    /* Operate on the names whose grace period ends exactly at this
       block, i.e. whose latest update height is
       nHeight - expirationDepth - pruneDepth.  Skip if that height is
       negative (chain too short).  */
    if (nHeight < expirationDepth + pruneDepth)
        return;
    const unsigned targetHeight = nHeight - expirationDepth - pruneDepth;

    std::set<valtype> candidates;
    if (!GetNamesForHeight (targetHeight, candidates))
        return;

    for (const valtype& name : candidates)
    {
        CNameData liveData;
        if (!GetName (name, liveData))
            continue;

        /* If the entry was re-registered after targetHeight, the expire
           index still points to the old height until the next compact;
           skip those.  We also skip names that are not (yet) expired,
           which can happen if expirationDepth has changed.  */
        if (liveData.getHeight () != targetHeight)
            continue;
        if (!liveData.isExpired (nHeight))
            continue;

        CNameHistory history;
        if (!GetNameHistory (name, history))
            continue;
        if (history.empty ())
            continue;

        cacheNames.setHistory (name, CNameHistory ());
    }
}

void CCoinsViewCache::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& in_block_hash, const CNameCache& names)
{
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) { // TODO a cursor can only contain dirty entries
            continue;
        }
        auto [itUs, inserted]{cacheCoins.try_emplace(it->first)};
        if (inserted) {
            if (it->second.IsFresh() && it->second.coin.IsSpent()) {
                cacheCoins.erase(itUs); // TODO fresh coins should have been removed at spend
            } else {
                // The parent cache does not have an entry, while the child cache does.
                // Move the data up and mark it as dirty.
                CCoinsCacheEntry& entry{itUs->second};
                assert(entry.coin.DynamicMemoryUsage() == 0);
                if (cursor.WillErase(*it)) {
                    // Since this entry will be erased,
                    // we can move the coin into us instead of copying it
                    entry.coin = std::move(it->second.coin);
                } else {
                    entry.coin = it->second.coin;
                }
                CCoinsCacheEntry::SetDirty(*itUs, m_sentinel);
                ++m_dirty_count;
                cachedCoinsUsage += entry.coin.DynamicMemoryUsage();
                // We can mark it FRESH in the parent if it was FRESH in the child
                // Otherwise it might have just been flushed from the parent's cache
                // and already exist in the grandparent
                if (it->second.IsFresh()) CCoinsCacheEntry::SetFresh(*itUs, m_sentinel);
            }
        } else {
            // Found the entry in the parent cache
            if (it->second.IsFresh() && !itUs->second.coin.IsSpent()) {
                // The coin was marked FRESH in the child cache, but the coin
                // exists in the parent cache. If this ever happens, it means
                // the FRESH flag was misapplied and there is a logic error in
                // the calling code.
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }

            if (itUs->second.IsFresh() && it->second.coin.IsSpent()) {
                // The grandparent cache does not have an entry, and the coin
                // has been spent. We can just delete it from the parent cache.
                Assume(TrySub(m_dirty_count, itUs->second.IsDirty()));
                Assume(TrySub(cachedCoinsUsage, itUs->second.coin.DynamicMemoryUsage()));
                cacheCoins.erase(itUs);
            } else {
                // A normal modification.
                Assume(TrySub(cachedCoinsUsage, itUs->second.coin.DynamicMemoryUsage()));
                if (cursor.WillErase(*it)) {
                    // Since this entry will be erased,
                    // we can move the coin into us instead of copying it
                    itUs->second.coin = std::move(it->second.coin);
                } else {
                    itUs->second.coin = it->second.coin;
                }
                cachedCoinsUsage += itUs->second.coin.DynamicMemoryUsage();
                if (!itUs->second.IsDirty()) {
                    CCoinsCacheEntry::SetDirty(*itUs, m_sentinel);
                    ++m_dirty_count;
                }
                // NOTE: It isn't safe to mark the coin as FRESH in the parent
                // cache. If it already existed and was spent in the parent
                // cache then marking it FRESH would prevent that spentness
                // from being flushed to the grandparent.
            }
        }
    }
    SetBestBlock(in_block_hash);
    cacheNames.apply(names);
}

void CCoinsViewCache::Flush(bool reallocate_cache)
{
    /* This function is called when validating the name mempool, and BatchWrite
       actually fails if hashBlock is not set.  Thus we have to make sure here
       that it is a valid no-op when nothing is cached.  */
    if (m_block_hash.IsNull() && cacheCoins.empty() && cacheNames.empty())
        return;

    auto cursor{CoinsViewCacheCursor(m_dirty_count, m_sentinel, cacheCoins, /*will_erase=*/true)};
    base->BatchWrite(cursor, m_block_hash, cacheNames);
    Assume(m_dirty_count == 0);
    cacheCoins.clear();
    if (reallocate_cache) {
        ReallocateCache();
    }
    cachedCoinsUsage = 0;
}

void CCoinsViewCache::Sync()
{
    auto cursor{CoinsViewCacheCursor(m_dirty_count, m_sentinel, cacheCoins, /*will_erase=*/false)};
    base->BatchWrite(cursor, m_block_hash, cacheNames);
    Assume(m_dirty_count == 0);
    if (m_sentinel.second.Next() != &m_sentinel) {
        /* BatchWrite must clear flags of all entries */
        throw std::logic_error("Not all unspent flagged entries were cleared");
    }
}

void CCoinsViewCache::Reset() noexcept
{
    cacheCoins.clear();
    cachedCoinsUsage = 0;
    m_dirty_count = 0;
    SetBestBlock(uint256::ZERO);
    cacheNames.clear();
}

void CCoinsViewCache::Uncache(const COutPoint& hash)
{
    CCoinsMap::iterator it = cacheCoins.find(hash);
    if (it != cacheCoins.end() && !it->second.IsDirty()) {
        Assume(TrySub(cachedCoinsUsage, it->second.coin.DynamicMemoryUsage()));
        TRACEPOINT(utxocache, uncache,
               hash.hash.data(),
               (uint32_t)hash.n,
               (uint32_t)it->second.coin.nHeight,
               (int64_t)it->second.coin.out.nValue,
               (bool)it->second.coin.IsCoinBase());
        cacheCoins.erase(it);
    }
}

unsigned int CCoinsViewCache::GetCacheSize() const {
    // Do not take name operations into account here.
    return cacheCoins.size();
}

bool CCoinsViewCache::HaveInputs(const CTransaction& tx) const
{
    if (!tx.IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            if (!HaveCoin(tx.vin[i].prevout)) {
                return false;
            }
        }
    }
    return true;
}

void CCoinsViewCache::ReallocateCache()
{
    // Cache should be empty when we're calling this.
    assert(cacheCoins.size() == 0);
    cacheCoins.~CCoinsMap();
    m_cache_coins_memory_resource.~CCoinsMapMemoryResource();
    ::new (&m_cache_coins_memory_resource) CCoinsMapMemoryResource{};
    ::new (&cacheCoins) CCoinsMap{0, SaltedOutpointHasher{/*deterministic=*/m_deterministic}, CCoinsMap::key_equal{}, &m_cache_coins_memory_resource};
}

void CCoinsViewCache::SanityCheck() const
{
    size_t recomputed_usage = 0;
    size_t count_dirty = 0;
    for (const auto& [_, entry] : cacheCoins) {
        if (entry.coin.IsSpent()) {
            assert(entry.IsDirty() && !entry.IsFresh()); // A spent coin must be dirty and cannot be fresh
        } else {
            assert(entry.IsDirty() || !entry.IsFresh()); // An unspent coin must not be fresh if not dirty
        }

        // Recompute cachedCoinsUsage.
        recomputed_usage += entry.coin.DynamicMemoryUsage();

        // Count the number of entries we expect in the linked list.
        if (entry.IsDirty()) ++count_dirty;
    }
    // Iterate over the linked list of flagged entries.
    size_t count_linked = 0;
    for (auto it = m_sentinel.second.Next(); it != &m_sentinel; it = it->second.Next()) {
        // Verify linked list integrity.
        assert(it->second.Next()->second.Prev() == it);
        assert(it->second.Prev()->second.Next() == it);
        // Verify they are actually flagged.
        assert(it->second.IsDirty());
        // Count the number of entries actually in the list.
        ++count_linked;
    }
    assert(count_dirty == count_linked && count_dirty == m_dirty_count);
    assert(recomputed_usage == cachedCoinsUsage);
}

static const uint64_t MIN_TRANSACTION_OUTPUT_WEIGHT{WITNESS_SCALE_FACTOR * ::GetSerializeSize(CTxOut())};
static const uint64_t MAX_OUTPUTS_PER_BLOCK{MAX_BLOCK_WEIGHT / MIN_TRANSACTION_OUTPUT_WEIGHT};

const Coin& AccessByTxid(const CCoinsViewCache& view, const Txid& txid)
{
    COutPoint iter(txid, 0);
    while (iter.n < MAX_OUTPUTS_PER_BLOCK) {
        const Coin& alternate = view.AccessCoin(iter);
        if (!alternate.IsSpent()) return alternate;
        ++iter.n;
    }
    return coinEmpty;
}

template <typename ReturnType, typename Func>
static ReturnType ExecuteBackedWrapper(Func func, const std::vector<std::function<void()>>& err_callbacks)
{
    try {
        return func();
    } catch(const std::runtime_error& e) {
        for (const auto& f : err_callbacks) {
            f();
        }
        LogError("Error reading from database: %s\n", e.what());
        // Starting the shutdown sequence and returning false to the caller would be
        // interpreted as 'entry not found' (as opposed to unable to read data), and
        // could lead to invalid interpretation. Just exit immediately, as we can't
        // continue anyway, and all writes should be atomic.
        std::abort();
    }
}

std::optional<Coin> CCoinsViewErrorCatcher::GetCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<std::optional<Coin>>([&]() { return CCoinsViewBacked::GetCoin(outpoint); }, m_err_callbacks);
}

bool CCoinsViewErrorCatcher::HaveCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<bool>([&]() { return CCoinsViewBacked::HaveCoin(outpoint); }, m_err_callbacks);
}

std::optional<Coin> CCoinsViewErrorCatcher::PeekCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<std::optional<Coin>>([&]() { return CCoinsViewBacked::PeekCoin(outpoint); }, m_err_callbacks);
}
