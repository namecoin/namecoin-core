// Copyright (c) 2019-2022 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_NAMEHASH_H
#define BITCOIN_INDEX_NAMEHASH_H

#include <index/base.h>
#include <script/script.h>
#include <uint256.h>

#include <memory>
#include <cstdint>

/** Default value for the -namehashindex argument.  */
static constexpr bool DEFAULT_NAMEHASHINDEX = false;

/** Maximum size of the DB cache for the name-hash index.  */
static constexpr uint64_t MAX_NAMEHASH_CACHE = 1024_GiB;

/**
 * This keeps an index of SHA-256d hashes of names to the corresponding preimage
 * (the names themselves).  This allows "unhashing" known names, so that we
 * can implement lookup of names in name_show and other commands by hash.
 *
 * What this enables is querying an untrusted node about the existence of
 * a name, without the node learning what the name is if it does not exist yet.
 *
 * Note that this is "append only".  When rewinding a block that first
 * mentions a name, we do not attempt to remove that name again from the index.
 * There's not really a point in doing so.
 */
class NameHashIndex : public BaseIndex
{

private:

  class DB;

  const std::unique_ptr<DB> db;

  bool AllowPrune() const override { return false; }

protected:

    bool CustomAppend (const interfaces::BlockInfo& block) override;

    BaseIndex::DB& GetDB () const override;

public:

    /**
     * Constructs the index, which becomes available to be queried.
     */
    explicit NameHashIndex (std::unique_ptr<interfaces::Chain> chain,
                            size_t cache_size, bool memory, bool wipe);

    ~NameHashIndex ();

    /**
     * Looks up a name by hash.  Returns false if the preimage cannot
     * be found (because the name has not been indexed yet).
     */
    bool FindNamePreimage (const uint256& hash, valtype& name) const;

};

/** The global name-hash index.  May be null.  */
extern std::unique_ptr<NameHashIndex> g_name_hash_index;

#endif // BITCOIN_INDEX_NAMEHASH_H
