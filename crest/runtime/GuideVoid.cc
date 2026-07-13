#include "Guide.hpp"

// Specialization implementation

Guide<void>::Guide() : ptr(0) {}

// Constructor with pointer
Guide<void>::Guide(void *p) : ptr(reinterpret_cast<uintptr_t>(p))
{
    if (p != nullptr)
    {
        soda_.Set1((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
        touchAndGetAddr();
    }
}

// Copy constructor
Guide<void>::Guide(const Guide<void> &other) : ptr(other.ptr)
{
    if (ptr != 0)
    {
        soda_.Set1((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
        touchAndGetAddr();
    }
}

/*
 * Assignment from a raw pointer: atomic address swap that preserves the
 * tracking metadata (ATC, access bit, CIW) but resets the heap id to NEW.
 * Raw pointers assigned by data structure code always come from user
 * allocation (jem_malloc -> NEW heap); only the Object Collector places
 * objects in HOT/COLD, and it updates guides by direct word CAS, never
 * through this operator. Preserving the old heap id here would leave a
 * stale HOT/COLD id on the guide after a value replacement, and a later
 * migration would free the new object against the wrong arena.
 */
Guide<void> &Guide<void>::operator=(void *p)
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

Guide<void> &Guide<void>::operator=(const Guide<void> &other)
{
    if (this != &other)
    {
        // Old pointer cleanup if needed
        if (ptr != 0)
        {
            soda_.Set0((((uint64_t)&other.ptr - (uint64_t)global_addr_start) / 8));
            soda_.Set0((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
        }

        // Full transfer of metadata and address
        __atomic_store_n(&ptr, other.ptr, __ATOMIC_RELAXED);

        // Register new pointer
        if (ptr != 0)
        {
            soda_.Set1((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
            touchAndGetAddr();
        }
    }
    return *this;
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
uintptr_t Guide<void>::touchAndGetAddr()
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

Guide<void>::operator void *()
{
    return reinterpret_cast<void *>(touchAndGetAddr());
}

void *Guide<void>::operator->()
{
    return reinterpret_cast<void *>(touchAndGetAddr());
}

void Guide<void>::debugPrint() const
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

void Guide<void>::setAccessBit()
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

void Guide<void>::clearAccessBit()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        new_val = old_val & ~ACCESS_BIT_MASK;
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

bool Guide<void>::getAccessBit() const
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & ACCESS_BIT_MASK) != 0;
}

void Guide<void>::setMigrationLock()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        new_val = old_val | MIGRATION_LOCK_MASK;
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

void Guide<void>::clearMigrationLock()
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

bool Guide<void>::getMigrationLock() const
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & MIGRATION_LOCK_MASK) != 0;
}

void Guide<void>::incrementConsecutiveInactiveWins()
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

void Guide<void>::resetConsecutiveInactiveWins()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_RELAXED);
        new_val = old_val & ~CONSECUTIVE_INACTIVE_WINS_MASK;
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

uint8_t Guide<void>::getConsecutiveInactiveWins() const
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & CONSECUTIVE_INACTIVE_WINS_MASK) >> CONSECUTIVE_INACTIVE_WINS_SHIFT;
}

void Guide<void>::decrementATC()
{
    uintptr_t old_val, new_val;
    do
    {
        old_val = __atomic_load_n(&ptr, __ATOMIC_ACQUIRE);
        uintptr_t refCount = (old_val & ACTIVE_THREAD_COUNT_MASK) >> ACTIVE_THREAD_COUNT_SHIFT;
        if (refCount == 0)
        {
            // Nothing to decrement (increment may have been skipped while
            // the state was INACTIVE, or the count saturated).
            break;
        }
        new_val = old_val - (1UL << ACTIVE_THREAD_COUNT_SHIFT);
    } while (!__atomic_compare_exchange_n(&ptr, &old_val, new_val, true,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    // Add a full memory barrier to ensure the decrement is visible to all threads
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

uint8_t Guide<void>::getATC() const
{
    return (__atomic_load_n(&ptr, __ATOMIC_RELAXED) & ACTIVE_THREAD_COUNT_MASK) >> ACTIVE_THREAD_COUNT_SHIFT;
}

void Guide<void>::destroy()
{
    if (ptr != 0)
    {
        soda_.Set0((((uint64_t)&ptr - (uint64_t)global_addr_start) / 8));
    }

    g_sama->free(reinterpret_cast<void *>(getCanonicalAddress()), getMemType());
}

// thread_local storage of scope guards
thread_local BaseDeltaPtrSet *ActiveScopeGuard::ptrs = nullptr;
thread_local int ActiveScopeGuard::publicFuncNestingLevel = 0;
