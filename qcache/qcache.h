#ifndef __QCACHE_H
#define __QCACHE_H

#include <iostream>
#include <iomanip>

#include <vector>

#include <stdint.h>
#include <pthread.h>
#include <limits.h>

#include <stdlib.h>

#if 1
#define ASSERT(b) do { if (!(b)) { \
  std::cerr << "Error: Failed assertion at " << __FILE__ << ':' << std::dec \
            << __LINE__ << '\n'; \
  abort(); \
} } while(0)

#else
#define ASSERT(b) do {} while(0)
#endif

namespace Qcache {
  typedef unsigned timestamp_t;
  #define TIMESTAMP_MAX UINT_MAX

  typedef pthread_spinlock_t spinlock_t;
  #define spinlock_init(s) do { pthread_spin_init((s), 0); } while (0)
  #define spin_lock(s) do { pthread_spin_lock((s)); } while (0)
  #define spin_unlock(s) do { pthread_spin_unlock((s)); } while (0)
  typedef uint64_t addr_t;

  // Things to throw when we're angry.
  struct InvalidAccess {};

  // Every level in the memory hierarchy is one of these.
  class MemSysDev {
   public:
    virtual ~MemSysDev() {}

    virtual void access(addr_t addr, bool wr) = 0;
    virtual void invalidate(addr_t addr) = 0;
  };

  class MemSysDevSet {
   public:
    virtual ~MemSysDevSet() {}
    virtual MemSysDev &getMemSysDev(size_t i)=0;
  };

  // Place one of these at any level in the hierarchy to get a read/write trace
  // at that level.
  class Tracer : public MemSysDev {
   public:
    Tracer(std::ostream &tf) : tracefile(tf) {}

    void access(addr_t addr, bool wr) {
      tracefile << std::dec << addr << (wr?" W\n":" R\n");
    }

    void invalidate(addr_t addr) { throw InvalidAccess(); }
   private:
    std::ostream &tracefile;
  };

  // Caches, private or shared, of any dimension
  template
    <typename CPROT_T, int WAYS, int L2SETS, int L2LINESZ, bool SHARED=false>
  class Cache : public MemSysDev
  {
   public:
    Cache(MemSysDev &ll, const char *n = "Unnamed") :
      peers(NULL), lowerLevel(&ll), id(0), name(n), accesses(0), misses(0)
    {
      initArrays();
    }

    Cache(std::vector<Cache> &peers, MemSysDev &ll, int id,
        const char *n = "Unnamed") :
      peers(&peers), lowerLevel(&ll), id(id), name(n), accesses(0), misses(0)
    {
      initArrays();
    }

    ~Cache() {
      if (accesses == 0) return;
      std::cout << name << ", " << id << ", " << accesses << ", " << misses 
                << '\n';
    }

    void access(addr_t addr, bool wr) {
      ++accesses;
      if (SHARED) spin_lock(&accessLock);

      addr_t stateMask((1<<L2LINESZ)-1), tag(addr>>L2LINESZ),
             set(tag%(1<<L2SETS));
      
      spin_lock(&setLocks[set]);
      addr_t idx;
      for (idx = set*WAYS; idx < (set+1)*WAYS; ++idx) {
        if ((tagarray[idx]>>L2LINESZ)==tag && (tagarray[idx]&stateMask)) {
          updateRepl(set, idx);
          spin_unlock(&setLocks[set]);
          goto finish;
        }
      }
      ++misses;

      // Miss. TODO: Tell the coherence protocol here was a read miss and see
      //             if we get the line from a peer cache.
        
      // TODO: Record miss.

      lowerLevel->access(tag<<L2LINESZ, wr);
      idx = findVictim(set);
      updateRepl(set, idx); // MRU insertion policy.
      tagarray[idx] = (tag<<L2LINESZ)|0x01; // TODO: Init state TBD by CP
      
      spin_unlock(&setLocks[set]); // TODO: move to after finish?
        

    finish:
      if (SHARED) spin_unlock(&accessLock);
    }

    void invalidate(addr_t addr) {
      if (SHARED) spin_lock(&accessLock);

      addr_t stateMask((1<<L2LINESZ)-1), tag(addr>>L2LINESZ),
             set(tag%(1<<L2SETS));

      spin_lock(&setLocks[set]);
      addr_t idx;
      for (idx = set*WAYS; idx < (set+1)*WAYS; ++idx) {
        if ((tagarray[idx]>>L2LINESZ)==tag) tagarray[idx] = 0;
      }
      spin_unlock(&setLocks[set]);

      if (SHARED) spin_unlock(&accessLock);
    }
   private:
    std::vector<Cache> *peers;
    MemSysDev *lowerLevel;
    const char *name;
    int id;   

    uint64_t tagarray[(size_t)WAYS<<L2SETS];
    timestamp_t tsarray[(size_t)WAYS<<L2SETS];
    timestamp_t tsmax[1l<<L2SETS];
    spinlock_t setLocks[1l<<L2SETS];

    spinlock_t accessLock; // One at a time in shared LLC

    uint64_t accesses, misses;

    void updateRepl(addr_t set, addr_t idx) {
      // TODO: Handle timestamp overflow.
      ASSERT(tsmax[set] != TIMESTAMP_MAX);
      tsarray[idx] = ++tsmax[set];
    }

    addr_t findVictim(addr_t set) {
      size_t i = set*WAYS, maxIdx = i;
      timestamp_t maxTs = tsarray[i];
      for (i = set*WAYS + 1; i < (set+1)*WAYS; ++i) {
        if (!(tagarray[i] & ((1<<L2LINESZ)-1))) return i;
        if (tsarray[i] > maxTs) { maxIdx = i; maxTs = tsarray[i]; }
      }
      return maxIdx;
    }

    void initArrays() {
      if (SHARED) spinlock_init(&accessLock);

      for (size_t i = 0; i < (size_t)WAYS<<L2SETS; ++i) {
        tsarray[i] = tagarray[i] = 0;
      }

      for (size_t i = 0; i < (size_t)(1<<L2SETS); ++i) {
        tsmax[i] = 0;
        spinlock_init(&setLocks[i]);
      }
    }
  };

  // Group of caches at the same level. Sets up the CachePeerDir and the
  // coherence protocol.
  template
    <typename CPROT_T, int WAYS, int L2SETS, int L2LINESZ>
    class CacheGrp : public MemSysDevSet
  {
   public:
    CacheGrp(int n, MemSysDev &ll, const char *name = "Unnamed") {
      for (int i = 0; i < n; ++i)
        caches.push_back(CACHE(caches, ll, i, name));
    }

    CacheGrp(int n, MemSysDevSet &ll, const char *name = "Unnamed") {
      for (int i = 0; i < n; ++i)
        caches.push_back(CACHE(caches, ll.getMemSysDev(i), i, name));
    }

    Cache<CPROT_T, WAYS, L2SETS, L2LINESZ> &getCache(size_t i) {
      return caches[i];
    }

    MemSysDev &getMemSysDev(size_t i) { return getCache(i); }

   private:
    typedef Cache<CPROT_T, WAYS, L2SETS, L2LINESZ> CACHE;

    std::vector<CACHE> caches;
  };

  // A coherence protocol for levels below L1. Takes no action to maintain
  // coherence.
  class CPNull {
  public:
    void lockAddr(addr_t addr)   {}
    void unlockAddr(addr_t addr) {}
    void addAddr(addr_t addr, int id) {}
    void remAddr(addr_t addr, int id) {}
    void hitAddr(int id, addr_t addr, uint64_t *line, bool wr) {}
    void missAddr(int id, addr_t addr, bool wr) {}
    void evAddr(int id, addr_t addr) {}
  };

  // Directory MOESI coherence protocol.
  class CPDirMoesi {
    void lockAddr(addr_t addr)   {}
    void unlockAddr(addr_t addr) {}
    void addAddr(addr_t addr, int id) {}
    void remAddr(addr_t addr, int id) {}
    void hitAddr(int id, addr_t addr, uint64_t *line, bool wr) {}
    void missAddr(int id, addr_t addr, bool wr) {}
    void evAddr(int id, addr_t addr) {}
  };
};

#endif