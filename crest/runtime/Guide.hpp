#ifndef OBASE_GUIDE_HPP
#define OBASE_GUIDE_HPP

#include <iostream>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <bitset>
#include <set>
#include <mutex>
#include <jemalloc/jemalloc.h>
#include "spdlog/spdlog.h"

#if CREST_RAW_POINTERS
// Load-then-read benchmark mode: real C++ pointers, no guide metadata or hooks.
template <typename T>
using Guide = T *;
#else
#include "Sama.h"
#include "globalConfig.h"

extern Sama *g_sama;

/*
 64-bit ptr value:

|   63  |  62  | 61 - 60 | 59 - 54 | 53 - 48 | 47 - 0 |
|-------|------|---------|---------|---------|--------|
| MLOCK | ACC  | HEAP_ID |   ATC   |  INACT  | ADDR   |
*/
// Masks and shifts for metadata fields
static constexpr uintptr_t MIGRATION_LOCK_MASK = 0x8000000000000000; // Bit 63
static constexpr int MIGRATION_LOCK_SHIFT = 63;

static constexpr uintptr_t ACCESS_BIT_MASK = 0x4000000000000000; // Bit 62
static constexpr int ACCESS_BIT_SHIFT = 62;

static constexpr uintptr_t HEAP_ID_MASK = 0x3000000000000000; // Bits 61-60
static constexpr int HEAP_ID_SHIFT = 60;

static constexpr uintptr_t ACTIVE_THREAD_COUNT_MASK = 0x0FC0000000000000; // Bits 59-54 (6 bits)
static constexpr int ACTIVE_THREAD_COUNT_SHIFT = 54;
static constexpr uintptr_t ACTIVE_THREAD_COUNT_MAX = 63; // 6-bit field

static constexpr uintptr_t CONSECUTIVE_INACTIVE_WINS_MASK = 0x003F000000000000; // Bits 53-48 (6 bits)
static constexpr int CONSECUTIVE_INACTIVE_WINS_SHIFT = 48;
static constexpr uintptr_t CONSECUTIVE_INACTIVE_WINS_MAX = 63; // 6-bit field

template <typename T>
class Guide
{
private:
    uintptr_t ptr;

public:
    Guide();
    Guide(T *p);
    Guide(const Guide<T> &other);
    /*
     * The four operations Pass 3 instruments (assignment, conversion,
     * deref, arrow) must stay out-of-line in the emitted IR: the pass
     * matches calls to mangled Guide methods, and the instrumentation
     * pipeline runs on IR that clang -O2 has already optimized. If these
     * were inlined at emission time there would be no call left to match
     * and the code would silently lose ATC/TAG tracking. Guide<void>
     * gets this for free (its definitions live in GuideVoid.cc); the
     * generic template's definitions are in this header, so they need
     * noinline explicitly.
     */
    Guide<T> &operator=(T *p) __attribute__((noinline));
    operator T *() __attribute__((noinline));
    T &operator*() __attribute__((noinline));
    T *operator->() __attribute__((noinline));
    void debugPrint() const;
    ~Guide();
    inline uintptr_t touchAndGetAddr();
    inline void decrementATC();
    inline uint8_t getATC();

    inline void setAccessBit();
    inline void clearAccessBit();
    inline bool getAccessBit() const;
    inline void setHeapId(uint8_t heapId);
    inline uint8_t getHeapId() const;
    inline MemType getMemType();
    inline void setMigrationLock();
    inline void clearMigrationLock();
    inline bool getMigrationLock() const;
    inline void incrementConsecutiveInactiveWins();
    inline void resetConsecutiveInactiveWins();
    inline uint8_t getConsecutiveInactiveWins() const;

    inline uintptr_t getCanonicalAddress() const
    {
        return ptr & ADDRESS_MASK;
    }

    inline uint16_t getMetaData() const
    {
        return (ptr >> 48) & 0xFFFF;
    }

    inline void clearMetaData()
    {
        ptr &= ADDRESS_MASK;
    }

    static void decrementATC(void *ptr)
    {
        if (ptr)
        {
            reinterpret_cast<Guide<T> *>(ptr)->decrementATC();
        }
    }
    inline void incrementATC()
    {
        uintptr_t old_val, new_val;
        do
        {
            old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
            uintptr_t refCount = (old_val & ACTIVE_THREAD_COUNT_MASK) >> ACTIVE_THREAD_COUNT_SHIFT;
            if (refCount >= ACTIVE_THREAD_COUNT_MAX)
            {
                // Saturate: more concurrent threads on one guide than the field
                // can count. The object is pinned by the other 63 anyway.
                spdlog::error("ATC saturated for guide {:p}", (void *)&ptr);
                return;
            }
            new_val = old_val + (1UL << ACTIVE_THREAD_COUNT_SHIFT);
        } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    }
};

// Template implementation

template <typename T>
Guide<T>::Guide() : ptr(0) {}

// Constructor with pointer
template <typename T>
Guide<T>::Guide(T *p) : ptr(reinterpret_cast<uintptr_t>(p))
{
    if (p != nullptr)
    {
        soda_.Set1((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
        touchAndGetAddr();
    }
}

// Copy constructor
template <typename T>
Guide<T>::Guide(const Guide<T> &other) : ptr(other.ptr)
{
    if (ptr != 0)
    {
        soda_.Set1((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
        touchAndGetAddr();
    }
}

/*
 * Assignment from a raw pointer. Mirrors Guide<void>::operator=(void*):
 * atomic address swap with NO transient SODA Set0 (a destroy-then-reassign
 * window lets concurrent readers skip their ATC decrement and ratchet the
 * count), and the heap id resets to NEW -- raw pointers assigned by data
 * structure code always come from user allocation; only the Object
 * Collector places objects in HOT/COLD, and it updates guides by direct
 * word CAS, never through this operator.
 */
template <typename T>
Guide<T> &Guide<T>::operator=(T *p)
{
    if (p != nullptr)
    {
        uintptr_t old_val, new_val;
        do
        {
            old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
            new_val = (old_val & ~(ADDRESS_MASK | HEAP_ID_MASK)) | reinterpret_cast<uintptr_t>(p);
        } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
        soda_.Set1((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
        touchAndGetAddr();
    }
    else
    {
        soda_.Set0((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
        clearMigrationLock();
    }
    return *this;
}

// Destructor
template <typename T>
Guide<T>::~Guide()
{
    if (ptr != 0)
    {
        soda_.Set0((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
    }
}

/*
 * The dereference RMW. In ONE atomic update: set the access bit and clear the
 * migration lock. This is the safety keystone of ODM: the OC's commit CAS
 * expects the guide word it observed with MLOCK set, so any concurrent
 * dereference must change the word (clearing MLOCK) to veto the migration.
 * Doing this as two separate operations (or a non-atomic clear) allows a
 * dereference's stale store to overwrite a committed migration -- UAF.
 *
 * Fast path: if the access bit is already set and MLOCK is clear, the word
 * needs no update and we skip the write entirely (no cache-line dirtying for
 * hot objects). This is safe: while MLOCK is clear the OC has not begun
 * copying this object, and in-flight operations are covered by ATC.
 *
 * Returns the canonical address from the exact word we validated/published,
 * so callers never re-read the guide non-atomically.
 */
template <typename T>
uintptr_t Guide<T>::touchAndGetAddr()
{
    uintptr_t old_val = __atomic_load_n(&ptr, __ATOMIC_ACQUIRE);
    while (true)
    {
        if ((old_val & ACCESS_BIT_MASK) && !(old_val & MIGRATION_LOCK_MASK))
        {
            return old_val & ADDRESS_MASK; // fast path: no write needed
        }
        uintptr_t new_val = (old_val | ACCESS_BIT_MASK) & ~MIGRATION_LOCK_MASK;
        if (__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            return new_val & ADDRESS_MASK;
        }
        // old_val reloaded by the failed CAS; retry
    }
}

template <typename T>
Guide<T>::operator T *()
{
    return reinterpret_cast<T *>(touchAndGetAddr());
}

template <typename T>
T &Guide<T>::operator*()
{
    return *reinterpret_cast<T *>(touchAndGetAddr());
}

template <typename T>
T *Guide<T>::operator->()
{
    return reinterpret_cast<T *>(touchAndGetAddr());
}

template <typename T>
void Guide<T>::debugPrint() const
{
    uintptr_t storedPtr = ptr;
    bool accessBit = getAccessBit();
    bool migrationLock = getMigrationLock();
    uint8_t heapId = getHeapId();
    uint8_t InactiveWins = getConsecutiveInactiveWins();
    uint8_t activeRefCount = getATC();
    std::cout << "Address: " << (void *)(storedPtr & ADDRESS_MASK) << " "
              << "AccessBit: " << accessBit << " "
              << "MigrationLock: " << migrationLock << " "
              << "HeapId: " << heapId << " "
              << "InactiveWins: " << InactiveWins << " "
              << "ActiveRefCount: " << activeRefCount << std::endl;
}

template <typename T>
void Guide<T>::setAccessBit()
{
    // Skip the write entirely when the bit is already set -- avoids
    // cache-line dirtying for hot objects. (The dereference path uses
    // touchAndGetAddr(), which additionally clears the migration lock in
    // the same RMW; this helper is for metadata-only callers.)
    uintptr_t current = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
    if (current & ACCESS_BIT_MASK)
    {
        return;
    }
    __atomic_fetch_or(&ptr, ACCESS_BIT_MASK, __ATOMIC_RELEASE);
}

template <typename T>
void Guide<T>::clearAccessBit()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        new_val = old_val & ~ACCESS_BIT_MASK;
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

template <typename T>
bool Guide<T>::getAccessBit() const
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & ACCESS_BIT_MASK) != 0;
}

template <typename T>
void Guide<T>::setMigrationLock()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        new_val = old_val | MIGRATION_LOCK_MASK;
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

template <typename T>
void Guide<T>::clearMigrationLock()
{
    // Must be an atomic RMW: a plain load/and/store can overwrite a guide
    // the OC concurrently published, resurrecting a freed address.
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        if (!(old_val & MIGRATION_LOCK_MASK))
        {
            return;
        }
        new_val = old_val & ~MIGRATION_LOCK_MASK;
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

template <typename T>
bool Guide<T>::getMigrationLock() const
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & MIGRATION_LOCK_MASK) != 0;
}

/*
00 - start heap
01 - hot heap
10 - cold heap
*/
template <typename T>
void Guide<T>::setHeapId(uint8_t id)
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        new_val = (old_val & ~HEAP_ID_MASK) | (static_cast<uintptr_t>(id) << HEAP_ID_SHIFT);
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

template <typename T>
uint8_t Guide<T>::getHeapId() const
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & HEAP_ID_MASK) >> HEAP_ID_SHIFT;
}

template <typename T>
MemType Guide<T>::getMemType()
{
    uint8_t heapId = (__atomic_load_n(&ptr, __ATOMIC_ACQUIRE) & HEAP_ID_MASK) >> HEAP_ID_SHIFT;
    if (heapId == 0)
    {
        return MemType::NEW_HEAP;
    }
    else if (heapId == 1)
    {
        // return MemType::DRAM_2MB_HUGETLBFS;
        return MemType::DRAM_2MB_THP;
    }
    else if (heapId == 2)
    {
        return MemType::DRAM;
    }
    return MemType::NEW_HEAP;
}

template <typename T>
void Guide<T>::incrementConsecutiveInactiveWins()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        uint8_t inactiveWindows = (old_val & CONSECUTIVE_INACTIVE_WINS_MASK) >> CONSECUTIVE_INACTIVE_WINS_SHIFT;
        if (inactiveWindows < CONSECUTIVE_INACTIVE_WINS_MAX)
        {
            inactiveWindows++;
            new_val = (old_val & ~CONSECUTIVE_INACTIVE_WINS_MASK) | (static_cast<uintptr_t>(inactiveWindows) << CONSECUTIVE_INACTIVE_WINS_SHIFT);
        }
        else
        {
            new_val = old_val; // No change if already at max
            break;
        }
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

template <typename T>
void Guide<T>::resetConsecutiveInactiveWins()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        new_val = old_val & ~CONSECUTIVE_INACTIVE_WINS_MASK;
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

template <typename T>
uint8_t Guide<T>::getConsecutiveInactiveWins() const
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & CONSECUTIVE_INACTIVE_WINS_MASK) >> CONSECUTIVE_INACTIVE_WINS_SHIFT;
}

template <typename T>
void Guide<T>::decrementATC()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        uint16_t refCount = (old_val & ACTIVE_THREAD_COUNT_MASK) >> ACTIVE_THREAD_COUNT_SHIFT;
        if (refCount == 0)
            break;
        new_val = old_val - (1UL << ACTIVE_THREAD_COUNT_SHIFT);
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

template <typename T>
uint8_t Guide<T>::getATC()
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & ACTIVE_THREAD_COUNT_MASK) >> ACTIVE_THREAD_COUNT_SHIFT;
}

// Specialization for Guide<void>
template <>
class Guide<void>
{
public:
    uintptr_t ptr;
    Guide();
    Guide(void *p);
    Guide(const Guide<void> &other);
    Guide<void> &operator=(void *p);
    Guide<void> &operator=(const Guide<void> &other);
    void operator delete(void *p);
    operator void *();
    void *operator->();
    void debugPrint() const;
    uintptr_t touchAndGetAddr();
    void decrementATC();
    inline uint8_t getATC() const;

    inline void setAccessBit();
    inline void clearAccessBit();
    inline bool getAccessBit() const;

    inline void setHeapId(uint8_t id)
    {
        uintptr_t old_val, new_val;
        do
        {
            old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
            new_val = (old_val & ~HEAP_ID_MASK) | (static_cast<uintptr_t>(id) << HEAP_ID_SHIFT);
        } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }

    inline void setMigrationLock();
    inline void clearMigrationLock();
    inline bool getMigrationLock() const;
    inline void incrementConsecutiveInactiveWins();
    inline void resetConsecutiveInactiveWins();
    inline uint8_t getConsecutiveInactiveWins() const;

    inline uint8_t getHeapId() const
    {
        return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & HEAP_ID_MASK) >> HEAP_ID_SHIFT;
    }

    inline MemType getMemType()
    {
        uint8_t heapId = (__atomic_load_n(&ptr, __ATOMIC_ACQUIRE) & HEAP_ID_MASK) >> HEAP_ID_SHIFT;
        if (heapId == 0)
        {
            return MemType::NEW_HEAP;
        }
        else if (heapId == 1)
        {
            // HOT HEAP
            // return MemType::DRAM_2MB_HUGETLBFS;
            return MemType::DRAM_2MB_THP;
        }
        else if (heapId == 2)
        {
            // COLD HEAP
            return MemType::DRAM;
        }
        return MemType::NEW_HEAP;
    }

    inline uintptr_t getCanonicalAddress() const
    {
        return ptr & ADDRESS_MASK;
    }

    inline uint16_t getMetaData() const
    {
        return (ptr >> 48) & 0xFFFF;
    }

    inline void clearMetaData()
    {
        ptr &= ADDRESS_MASK;
    }

    static void decrementATC(void *ptr)
    {
        if (ptr)
        {
            reinterpret_cast<Guide<void> *>(ptr)->decrementATC();
        }
    }
    inline void incrementATC()
    {
        uintptr_t old_val, new_val;
        do
        {
            old_val = __atomic_load_n(&ptr, __ATOMIC_ACQUIRE);
            uintptr_t refCount = (old_val & ACTIVE_THREAD_COUNT_MASK) >> ACTIVE_THREAD_COUNT_SHIFT;
            if (refCount >= ACTIVE_THREAD_COUNT_MAX)
            {
                // Saturate: more concurrent threads on one guide than the field
                // can count. The object is pinned by the other 63 anyway.
                spdlog::error("ATC saturated for guide {:p}", (void *)&ptr);
                return;
            }
            new_val = old_val + (1UL << ACTIVE_THREAD_COUNT_SHIFT);
        } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

        // Add a full memory barrier to ensure the increment is visible to all threads
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    void destroy();
};

class BaseDeltaPtrSet
{
private:
    static constexpr size_t GROUP_SIZE = 16;  // adjustable, fits cache-line nicely
    static constexpr uint8_t ALIGN_SHIFT = 3; // 8-byte alignment

    struct Group
    {
        uintptr_t base;
        uint32_t deltas[GROUP_SIZE]; // 32-bit deltas for wider range
        // Each group to cover a much larger memory range (±17GB)
        uint8_t count;

        Group(uintptr_t ptr) : base(ptr), count(0)
        {
            // Pre-insert the base pointer
            deltas[0] = 0;
            count = 1;
        }
    };

    std::vector<Group> groups;

    // Avoid potentially expensive reserve/resize operations
    void ensureCapacity()
    {
        if (groups.capacity() == groups.size())
        {
            groups.reserve(groups.size() * 2 + 4); // Grow geometrically
        }
    }

public:
    BaseDeltaPtrSet()
    {
        // Reserve initial capacity to avoid early reallocations
        groups.reserve(64);
    }

    // if ptr is already in the set, returns false
    // otherwise, inserts ptr and returns true
    __attribute__((always_inline)) bool insert(void *ptr)
    {

        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);

        // Single-pass iteration through groups
        for (auto &group : groups)
        {
            // Check if ptr ≥ group.base (required for consistent delta calculation)
            if (p >= group.base)
            {
                const uintptr_t rawDelta = (p - group.base) >> ALIGN_SHIFT;
                if (rawDelta > UINT32_MAX)
                    continue; // out of this group's range; a truncated delta would alias
                const uint32_t delta = static_cast<uint32_t>(rawDelta);

                // First check if ptr already exists in this group
                for (uint8_t i = 0; i < group.count; ++i)
                {
                    if (group.deltas[i] == delta)
                        return false; // Already exists
                }

                // If group has space, add the pointer
                if (group.count < GROUP_SIZE)
                {
                    group.deltas[group.count++] = delta;
                    return true;
                }
            }
        }

        // No suitable group found, create new group
        ensureCapacity();
        groups.emplace_back(p);
        return true; // We pre-insert the pointer in the Group constructor
    }

    __attribute__((always_inline)) bool contains(void *ptr) const
    {
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);

        for (const auto &group : groups)
        {
            // Only consider pointers ≥ base
            if (p >= group.base)
            {
                const uintptr_t rawDelta = (p - group.base) >> ALIGN_SHIFT;
                if (rawDelta > UINT32_MAX)
                    continue; // out of this group's range
                const uint32_t delta = static_cast<uint32_t>(rawDelta);

                // Unrolled loop for small counts to improve branch prediction
                switch (group.count)
                {
                // Handle counts in reverse order for better branch prediction
                // Most groups will be full or near-full
                default: // Fallthrough for larger counts
                    for (uint8_t i = 0; i < group.count; ++i)
                    {
                        if (group.deltas[i] == delta)
                            return true;
                    }
                    break;
                case 4:
                    if (group.deltas[3] == delta)
                        return true; // Fallthrough
                case 3:
                    if (group.deltas[2] == delta)
                        return true; // Fallthrough
                case 2:
                    if (group.deltas[1] == delta)
                        return true; // Fallthrough
                case 1:
                    if (group.deltas[0] == delta)
                        return true;
                    break;
                case 0:
                    break; // Empty group (should never happen)
                }
            }
        }
        return false;
    }

    template <typename Func>
    __attribute__((always_inline)) void traverse(Func &&func) const
    {
        for (const auto &group : groups)
        {
            for (uint8_t i = 0; i < group.count; ++i)
            {
                const void *ptr = reinterpret_cast<void *>(
                    group.base + (static_cast<uintptr_t>(group.deltas[i]) << ALIGN_SHIFT));
                func(const_cast<void *>(ptr)); // cast away const for callback
            }
        }
    }

    __attribute__((always_inline)) void clear()
    {
        // Clear without deallocating memory
        groups.clear();
    }
};

class ActiveScopeGuard
{
private:
    // Track nesting level for the thread
    static thread_local int publicFuncNestingLevel;
    // Each thread has only one actual set of pointers
    static thread_local BaseDeltaPtrSet *ptrs;
    // Remember which level this guard belongs to
    int myLevel;

public:
    __attribute__((always_inline)) ActiveScopeGuard()
    {
        // Initialize if needed
        if (ptrs == nullptr)
        {
            ptrs = new BaseDeltaPtrSet;
            publicFuncNestingLevel = 0;
        }

        // Record our nesting level
        myLevel = publicFuncNestingLevel++;

        // If this is a top-level function (level 0), update thread activity
        if (myLevel == 0)
        {
            size_t slot = getThreadSlot();
            g_thread_activity[slot].active_public_funcs.fetch_add(1, std::memory_order_release);

            // Check if there's a new migration generation to observe
            uint64_t current_gen = g_migration_generation.load(std::memory_order_acquire);
            if (current_gen > tl_observed_generation)
            {
                tl_observed_generation = current_gen;
                g_thread_activity[slot].last_observed_generation.store(current_gen, std::memory_order_release);
            }
        }
    }

    __attribute__((always_inline)) bool add(void *ptr)
    {
        // All levels share the same pointer set
        return ptrs->insert(ptr);
    }

    __attribute__((always_inline)) ~ActiveScopeGuard()
    {
        if (ptrs == nullptr)
            return;
        // Decrement nesting level as we exit
        --publicFuncNestingLevel;

        // Process reference counting if needed
        if (myLevel == 0)
        {
            // Always decrement references for tracked pointers, regardless of stat
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            ptrs->traverse([](void *ptr)
                           {
                if (soda_.Get(((uint64_t)ptr - (uint64_t)global_addr_start) / 8))
                {
                    reinterpret_cast<Guide<void> *>(ptr)->decrementATC();
                    __atomic_thread_fence(__ATOMIC_SEQ_CST);
                } });

            ptrs->clear();
            __atomic_thread_fence(__ATOMIC_SEQ_CST);

            // Update thread activity as we exit top-level function
            size_t slot = getThreadSlot();
            g_thread_activity[slot].active_public_funcs.fetch_sub(1, std::memory_order_release);
        }
    }

    // Static method to access thread-local storage
    __attribute__((always_inline)) static BaseDeltaPtrSet *getThreadLocalSet()
    {
        if (ptrs == nullptr)
        {
            ptrs = new BaseDeltaPtrSet;
            publicFuncNestingLevel = 0;
        }
        return ptrs;
    }
};

#endif // CREST_RAW_POINTERS
#endif // OBASE_GUIDE_HPP
