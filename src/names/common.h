// Copyright (c) 2014-2020 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef H_BITCOIN_NAMES_COMMON
#define H_BITCOIN_NAMES_COMMON

#include <compat/endian.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>

#include <map>
#include <set>

class CNameScript;
class CDBBatch;

/** Whether or not name history is enabled.  */
extern bool fNameHistory;

/**
 * Number of blocks of "grace" past the consensus expiration depth before
 * a name_history entry is dropped from DB_NAME_HISTORY.  Applied uniformly
 * to live, expired, and renewed names: any history entry whose own update
 * height is more than (expirationDepth + nPruneNameHistory) blocks deep
 * is eligible to be trimmed.  Zero disables pruning entirely.  This is a
 * node-local policy option (-prunenamehistory) and has no consensus
 * impact.
 */
extern unsigned nPruneNameHistory;

/* ************************************************************************** */
/* CNameData.  */

/**
 * Information stored for a name in the database.
 */
class CNameData
{

private:

  /** The name's value.  */
  valtype value;

  /** The transaction's height.  Used for expiry.  */
  unsigned nHeight;

  /** The name's last update outpoint.  */
  COutPoint prevout;

  /**
   * The name's address (as script).  This is kept here also, because
   * that information is useful to extract on demand (e. g., in name_show).
   */
  CScript addr;

public:

  SERIALIZE_METHODS (CNameData, obj)
  {
    READWRITE (obj.value, obj.nHeight, obj.prevout, obj.addr);
  }

  /* Compare for equality.  */
  friend inline bool
  operator== (const CNameData& a, const CNameData& b)
  {
    return a.value == b.value && a.nHeight == b.nHeight
            && a.prevout == b.prevout && a.addr == b.addr;
  }
  friend inline bool
  operator!= (const CNameData& a, const CNameData& b)
  {
    return !(a == b);
  }

  /**
   * Get the height.
   * @return The name's update height.
   */
  inline unsigned
  getHeight () const
  {
    return nHeight;
  }

  /**
   * Get the value.
   * @return The name's value.
   */
  inline const valtype&
  getValue () const
  {
    return value;
  }

  /**
   * Get the name's update outpoint.
   * @return The update outpoint.
   */
  inline const COutPoint&
  getUpdateOutpoint () const
  {
    return prevout;
  }

  /**
   * Get the address.
   * @return The name's address.
   */
  inline const CScript&
  getAddress () const
  {
    return addr;
  }

  /**
   * Check if the name is expired at the given height.
   * @param h The height at which to check.
   * @return True iff the name is expired at height h.
   */
  bool isExpired (unsigned h) const;

  /**
   * Set from a name update operation.
   * @param h The height (not available from script).
   * @param out The update outpoint.
   * @param script The name script.  Should be a name (first) update.
   */
  void fromScript (unsigned h, const COutPoint& out, const CNameScript& script);

};

/* ************************************************************************** */
/* CNameHistory.  */

/**
 * Keep track of a name's history.  This is a stack of old CNameData
 * objects that have been obsoleted.
 */
class CNameHistory
{

private:

  /** The actual data.  */
  std::vector<CNameData> data;

public:

  SERIALIZE_METHODS (CNameHistory, obj)
  {
    READWRITE (obj.data);
  }

  /**
   * Check if the stack is empty.  This is used to decide when to fully
   * delete an entry in the database.
   * @return True iff the data stack is empty.
   */
  inline bool
  empty () const
  {
    return data.empty ();
  }

  /**
   * Access the data in a read-only way.
   * @return The data stack.
   */
  inline const std::vector<CNameData>&
  getData () const
  {
    return data;
  }

  /**
   * Push a new entry onto the data stack.  The new entry's height should
   * be at least as high as the stack top entry's.  If not, fail.
   * @param entry The new entry to push onto the stack.
   */
  inline void
  push (const CNameData& entry)
  {
    assert (data.empty () || data.back ().getHeight () <= entry.getHeight ());
    data.push_back (entry);
  }

  /**
   * Pop the top entry off the stack.  This is used when undoing name
   * changes.  The name's new value is passed as argument and should
   * match the removed entry.  If not, fail.
   * @param entry The name's value after undoing.
   */
  inline void
  pop (const CNameData& entry)
  {
    assert (!data.empty () && data.back () == entry);
    data.pop_back ();
  }

  /**
   * Drop the leading (oldest) entries whose own height is <= the given
   * cutoff height, keeping the remaining tail of the vector.  Entries
   * are kept height-ordered by the push() precondition, so the cut is
   * a single contiguous prefix.  Used by the optional node-local
   * history-pruning policy.
   * @param cutoffHeight  Highest height to drop (entries with
   *                      nHeight <= cutoffHeight are removed).
   * @return Number of entries removed.
   */
  inline size_t
  trimBelowOrEqual (unsigned cutoffHeight)
  {
    auto it = data.begin ();
    while (it != data.end () && it->getHeight () <= cutoffHeight)
      ++it;
    const size_t removed = static_cast<size_t> (it - data.begin ());
    data.erase (data.begin (), it);
    return removed;
  }

};

/* ************************************************************************** */
/* CNameIterator.  */

/**
 * Interface for iterators over the name database.
 */
class CNameIterator
{

public:

  // Virtual destructor in case subclasses need them.
  virtual ~CNameIterator ();

  /**
   * Seek to a given lower bound.
   */
  virtual void seek (const valtype& name) = 0;

  /**
   * Get the next name.  Returns false if no more names are available.
   * @return True if successful, false if no more names.
   */
  virtual bool next (valtype& name, CNameData& data) = 0;

};

/* ************************************************************************** */
/* CNameCache.  */

/**
 * Cache / record of updates to the name database.  In addition to
 * new names (or updates to them), this also keeps track of deleted names
 * (when rolling back changes).
 */
class CNameCache
{

private:

  /**
   * Special comparator class for names that compares by length first.
   * This is used to sort the cache entry map in the same way as the
   * database is sorted.
   */
  class NameComparator
  {
  public:
    inline bool
    operator() (const valtype& a, const valtype& b) const
    {
      if (a.size () != b.size ())
        return a.size () < b.size ();

      return a < b;
    }
  };

public:

  /**
   * Type for expire-index entries.  We have to make sure that
   * it is serialised in such a way that ordering is done correctly
   * by height.  This is not true if we use a std::pair, since then
   * the height is serialised as byte-array with little-endian order,
   * which does not correspond to the ordering by actual value.
   */
  class ExpireEntry
  {
  public:

    unsigned nHeight;
    valtype name;

    inline ExpireEntry ()
      : nHeight(0), name()
    {}

    inline ExpireEntry (unsigned h, const valtype& n)
      : nHeight(h), name(n)
    {}

    /* Default copy and assignment.  */

    template<typename Stream>
      inline void
      Serialize (Stream& s) const
    {
      /* Flip the byte order of nHeight to big endian.  */
      const uint32_t nHeightFlipped = htobe32_internal (nHeight);

      ::Serialize (s, nHeightFlipped);
      ::Serialize (s, name);
    }

    template<typename Stream>
      inline void
      Unserialize (Stream& s)
    {
      uint32_t nHeightFlipped;

      ::Unserialize (s, nHeightFlipped);
      ::Unserialize (s, name);

      /* Unflip the byte order.  */
      nHeight = be32toh_internal (nHeightFlipped);
    }

    friend inline bool
    operator== (const ExpireEntry& a, const ExpireEntry& b)
    {
      return a.nHeight == b.nHeight && a.name == b.name;
    }

    friend inline bool
    operator!= (const ExpireEntry& a, const ExpireEntry& b)
    {
      return !(a == b);
    }

    friend inline bool
    operator< (const ExpireEntry& a, const ExpireEntry& b)
    {
      if (a.nHeight != b.nHeight)
        return a.nHeight < b.nHeight;

      return a.name < b.name;
    }

  };

  /**
   * Type of name entry map.  This is public because it is also used
   * by the unit tests.
   */
  typedef std::map<valtype, CNameData, NameComparator> EntryMap;

private:

  /** New or updated names.  */
  EntryMap entries;
  /** Deleted names.  */
  std::set<valtype> deleted;

  /**
   * New or updated history stacks.  If they are empty, the corresponding
   * database entry is deleted instead.
   */
  std::map<valtype, CNameHistory> history;

  /**
   * Changes to be performed to the expire index.  The entry is mapped
   * to either "true" (meaning to add it) or "false" (delete).
   */
  std::map<ExpireEntry, bool> expireIndex;

  /**
   * Changes to be performed to the history-expire index.  Keyed by
   * the height at which an individual DB_NAME_HISTORY entry was written
   * (not by the name's current live height) and append-only on a per
   * (height, name) basis: every time a CNameHistory push happens we
   * record an add here, and undo / trim drop the matching key.  The
   * map value mirrors expireIndex: true = add, false = erase.
   */
  std::map<ExpireEntry, bool> historyExpireIndex;

  friend class CCacheNameIterator;

public:

  inline void
  clear ()
  {
    entries.clear ();
    deleted.clear ();
    history.clear ();
    expireIndex.clear ();
    historyExpireIndex.clear ();
  }

  /**
   * Check if the cache is "clean" (no cached changes).  This also
   * performs internal checks and fails with an assertion if the
   * internal state is inconsistent.
   * @return True iff no changes are cached.
   */
  inline bool
  empty () const
  {
    if (entries.empty () && deleted.empty ())
      {
        assert (history.empty () && expireIndex.empty ()
                && historyExpireIndex.empty ());
        return true;
      }

    return false;
  }

  /* See if the given name is marked as deleted.  */
  inline bool
  isDeleted (const valtype& name) const
  {
    return (deleted.count (name) > 0);
  }

  /* Try to get a name's associated data.  This looks only
     in entries, and doesn't care about deleted data.  */
  bool get (const valtype& name, CNameData& data) const;

  /* Insert (or update) a name.  If it is marked as "deleted", this also
     removes the "deleted" mark.  */
  void set (const valtype& name, const CNameData& data);

  /* Delete a name.  If it is in the "entries" set also, remove it there.  */
  void remove (const valtype& name);

  /* Return a name iterator that combines a "base" iterator with the changes
     made to it according to the cache.  The base iterator is taken
     ownership of.  */
  CNameIterator* iterateNames (CNameIterator* base) const;

  /**
   * Query for an history entry.
   * @param name The name to look up.
   * @param res Put the resulting history entry here.
   * @return True iff the name was found in the cache.
   */
  bool getHistory (const valtype& name, CNameHistory& res) const;

  /**
   * Set a name history entry.
   * @param name The name to modify.
   * @param data The new history entry.
   */
  void setHistory (const valtype& name, const CNameHistory& data);

  /* Query the cached changes to the expire index.  In particular,
     for a given height and a given set of names that were indexed to
     this update height, apply possible changes to the set that
     are represented by the cached expire index changes.  */
  void updateNamesForHeight (unsigned nHeight, std::set<valtype>& names) const;

  /* Add an expire-index entry.  */
  void addExpireIndex (const valtype& name, unsigned height);

  /* Remove an expire-index entry.  */
  void removeExpireIndex (const valtype& name, unsigned height);

  /**
   * For a given target height, return the set of names that have a
   * DB_NAME_HISTORY entry pinned at that height.  Combines cached
   * deltas (historyExpireIndex) with the passed-in base set, which the
   * caller has typically obtained from the DB layer.  Behaviour mirrors
   * updateNamesForHeight.
   * @param nHeight  The height to query.
   * @param names    In/out: set of names; entries added or removed in
   *                 place according to the cached deltas.
   */
  void updateHistoryNamesForHeight (unsigned nHeight,
                                    std::set<valtype>& names) const;

  /* Add a history-expire-index entry.  Called from SetName when a
     CNameHistory push happens.  height is the height of the *pushed*
     CNameData (i.e. the old data being archived), not the new live
     height.  */
  void addHistoryExpireIndex (const valtype& name, unsigned height);

  /* Remove a history-expire-index entry.  Called from the SetName undo
     path when a CNameHistory pop happens, and from the trim driver
     when it erases an index key after consuming it.  */
  void removeHistoryExpireIndex (const valtype& name, unsigned height);

  /* Apply all the changes in the passed-in record on top of this one.  */
  void apply (const CNameCache& cache);

  /* Write all cached changes to a database batch update object.  */
  void writeBatch (CDBBatch& batch) const;

};

#endif // H_BITCOIN_NAMES_COMMON
