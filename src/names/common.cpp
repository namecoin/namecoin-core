// Copyright (c) 2014-2017 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <names/common.h>

#include <script/names.h>

bool fNameHistory = false;
unsigned nPruneNameHistory = 0;

/* ************************************************************************** */
/* CNameData.  */

void
CNameData::fromScript (unsigned h, const COutPoint& out,
                       const CNameScript& script)
{
  assert (script.isAnyUpdate ());
  value = script.getOpValue ();
  nHeight = h;
  prevout = out;
  addr = script.getAddress ();
}

/* ************************************************************************** */
/* CNameIterator.  */

CNameIterator::~CNameIterator ()
{
  /* Nothing to be done here.  This may be overwritten by
     subclasses if they need a destructor.  */
}

/* ************************************************************************** */
/* CNameCacheNameIterator.  */

class CCacheNameIterator : public CNameIterator
{

private:

  /** Reference to cache object that is used.  */
  const CNameCache& cache;

  /** Base iterator to combine with the cache.  */
  CNameIterator* base;

  /** Whether or not the base iterator has more entries.  */
  bool baseHasMore;
  /** "Next" name of the base iterator.  */
  valtype baseName;
  /** "Next" data of the base iterator.  */
  CNameData baseData;

  /** Iterator of the cache's entries.  */
  CNameCache::EntryMap::const_iterator cacheIter;

  /* Call the base iterator's next() routine to fill in the internal
     "cache" for the next entry.  This already skips entries that are
     marked as deleted in the cache.  */
  void advanceBaseIterator ();

public:

  /**
   * Construct the iterator.  This takes ownership of the base iterator.
   * @param c The cache object to use.
   * @param b The base iterator.
   */
  CCacheNameIterator (const CNameCache& c, CNameIterator* b);

  /* Destruct, this deletes also the base iterator.  */
  ~CCacheNameIterator ();

  /* Implement iterator methods.  */
  void seek (const valtype& name) override;
  bool next (valtype& name, CNameData& data) override;

};

CCacheNameIterator::CCacheNameIterator (const CNameCache& c, CNameIterator* b)
  : cache(c), base(b)
{
  /* Add a seek-to-start to ensure that everything is consistent.  This call
     may be superfluous if we seek to another position afterwards anyway,
     but it should also not hurt too much.  */
  seek (valtype ());
}

CCacheNameIterator::~CCacheNameIterator ()
{
  delete base;
}

void
CCacheNameIterator::advanceBaseIterator ()
{
  assert (baseHasMore);
  do
    baseHasMore = base->next (baseName, baseData);
  while (baseHasMore && cache.isDeleted (baseName));
}

void
CCacheNameIterator::seek (const valtype& start)
{
  cacheIter = cache.entries.lower_bound (start);
  base->seek (start);

  baseHasMore = true;
  advanceBaseIterator ();
}

bool
CCacheNameIterator::next (valtype& name, CNameData& data)
{
  /* Exit early if no more data is available in either the cache
     nor the base iterator.  */
  if (!baseHasMore && cacheIter == cache.entries.end ())
    return false;

  /* Determine which source to use for the next.  */
  bool useBase;
  if (!baseHasMore)
    useBase = false;
  else if (cacheIter == cache.entries.end ())
    useBase = true;
  else
    {
      /* A special case is when both iterators are equal.  In this case,
         we want to use the cached version.  We also have to advance
         the base iterator.  */
      if (baseName == cacheIter->first)
        advanceBaseIterator ();

      /* Due to advancing the base iterator above, it may happen that
         no more base entries are present.  Handle this gracefully.  */
      if (!baseHasMore)
        useBase = false;
      else
        {
          assert (baseName != cacheIter->first);

          CNameCache::NameComparator cmp;
          useBase = cmp (baseName, cacheIter->first);
        }
    }

  /* Use the correct source now and advance it.  */
  if (useBase)
    {
      name = baseName;
      data = baseData;
      advanceBaseIterator ();
    }
  else
    {
      name = cacheIter->first;
      data = cacheIter->second;
      ++cacheIter;
    }

  return true;
}

/* ************************************************************************** */
/* CNameCache.  */

bool
CNameCache::get (const valtype& name, CNameData& data) const
{
  const EntryMap::const_iterator i = entries.find (name);
  if (i == entries.end ())
    return false;

  data = i->second;
  return true;
}

void
CNameCache::set (const valtype& name, const CNameData& data)
{
  const std::set<valtype>::iterator di = deleted.find (name);
  if (di != deleted.end ())
    deleted.erase (di);

  const EntryMap::iterator ei = entries.find (name);
  if (ei != entries.end ())
    ei->second = data;
  else
    entries.insert (std::make_pair (name, data));
}

void
CNameCache::remove (const valtype& name)
{
  const EntryMap::iterator ei = entries.find (name);
  if (ei != entries.end ())
    entries.erase (ei);

  deleted.insert (name);
}

CNameIterator*
CNameCache::iterateNames (CNameIterator* base) const
{
  return new CCacheNameIterator (*this, base);
}

bool
CNameCache::getHistory (const valtype& name, CNameHistory& res) const
{
  assert (fNameHistory);

  const std::map<valtype, CNameHistory>::const_iterator i = history.find (name);
  if (i == history.end ())
    return false;

  res = i->second;
  return true;
}

void
CNameCache::setHistory (const valtype& name, const CNameHistory& data)
{
  assert (fNameHistory);

  const std::map<valtype, CNameHistory>::iterator ei = history.find (name);
  if (ei != history.end ())
    ei->second = data;
  else
    history.insert (std::make_pair (name, data));
}

void
CNameCache::updateNamesForHeight (unsigned nHeight,
                                  std::set<valtype>& names) const
{
  /* Seek in the map of cached entries to the first one corresponding
     to our height.  */

  const ExpireEntry seekEntry(nHeight, valtype ());
  std::map<ExpireEntry, bool>::const_iterator it;

  for (it = expireIndex.lower_bound (seekEntry); it != expireIndex.end (); ++it)
    {
      const ExpireEntry& cur = it->first;
      assert (cur.nHeight >= nHeight);
      if (cur.nHeight > nHeight)
        break;

      if (it->second)
        names.insert (cur.name);
      else
        names.erase (cur.name);
    }
}

void
CNameCache::addExpireIndex (const valtype& name, unsigned height)
{
  const ExpireEntry entry(height, name);
  expireIndex[entry] = true;
}

void
CNameCache::removeExpireIndex (const valtype& name, unsigned height)
{
  const ExpireEntry entry(height, name);
  expireIndex[entry] = false;
}

void
CNameCache::updateHistoryNamesForHeight (unsigned nHeight,
                                         std::set<valtype>& names) const
{
  const ExpireEntry seekEntry(nHeight, valtype ());
  std::map<ExpireEntry, bool>::const_iterator it;

  for (it = historyExpireIndex.lower_bound (seekEntry);
       it != historyExpireIndex.end (); ++it)
    {
      const ExpireEntry& cur = it->first;
      assert (cur.nHeight >= nHeight);
      if (cur.nHeight > nHeight)
        break;

      if (it->second)
        names.insert (cur.name);
      else
        names.erase (cur.name);
    }
}

void
CNameCache::addHistoryExpireIndex (const valtype& name, unsigned height)
{
  const ExpireEntry entry(height, name);
  historyExpireIndex[entry] = true;
}

void
CNameCache::removeHistoryExpireIndex (const valtype& name, unsigned height)
{
  const ExpireEntry entry(height, name);
  historyExpireIndex[entry] = false;
}

void
CNameCache::apply (const CNameCache& cache)
{
  for (EntryMap::const_iterator i = cache.entries.begin ();
       i != cache.entries.end (); ++i)
    set (i->first, i->second);

  for (std::set<valtype>::const_iterator i = cache.deleted.begin ();
       i != cache.deleted.end (); ++i)
    remove (*i);

  for (std::map<valtype, CNameHistory>::const_iterator i
        = cache.history.begin (); i != cache.history.end (); ++i)
    setHistory (i->first, i->second);

  for (std::map<ExpireEntry, bool>::const_iterator i
        = cache.expireIndex.begin (); i != cache.expireIndex.end (); ++i)
    expireIndex[i->first] = i->second;

  for (std::map<ExpireEntry, bool>::const_iterator i
        = cache.historyExpireIndex.begin ();
       i != cache.historyExpireIndex.end (); ++i)
    historyExpireIndex[i->first] = i->second;
}
