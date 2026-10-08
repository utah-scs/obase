#ifndef OBASE_OBJECT_COLLECTOR_H
#define OBASE_OBJECT_COLLECTOR_H

#include <iostream>
#include <thread>
#include <atomic>
#include "unordered_set"

// Data structure selected at build time: make all DS=<name> (see dsconfig.h)
#include "dsconfig.h"

#include "Sama.h"
#include "Guide.hpp"

class ObjectCollector
{
private:
    // Exponential backoff parameters
    double targetPromotionRate;    // Target promotion rate (P)
    double currentPromotionRate;   // Track the most recent promotion rate
    bool backoffActive;            // Whether we're in backoff mode
    uint16_t multiplicationFactor; // Factor to multiply W by (typically 2)
    uint16_t incrementSize;        // How much to increase W by when R > P
    uint16_t decrementSize;        // How much to decrease W by when R <= P
    uint16_t W_min, W_max;         // Bounds for inactiveWindowThreshold

    // Migration pacing: cap objects moved per promote/demote round. An
    // unpaced round can double-buffer gigabytes (new copies live before the
    // freed originals are purged back to the OS) and OOM the box; deferred
    // objects simply migrate in later rounds.
    static constexpr uint64_t maxMigrationsPerRound = 2500000;

    // Purge freed pages back to the OS every N migrations within a round,
    // capping the round's transient RSS growth at ~N x object size instead
    // of the full round (freed source objects otherwise stay resident until
    // jemalloc's decay timer fires).
    static constexpr uint64_t purgeInterval = 500000;
    // Track initialization period
    uint16_t initialIntervalCount; // Count of intervals that have passed
    uint16_t initialIntervals;     // Number of intervals to wait before adjusting

    struct HeapStats
    {
        uint64_t total_bytes = 0;                    // Total bytes of all objects
        uint64_t total_objects = 0;                  // Total number of objects
        uint64_t total_accesses = 0;                 // Number of accesses
        uint64_t accessed_bytes = 0;                 // Bytes from accessed objects only
        std::unordered_set<uint64_t> accessed_pages; // Pages touched by accessed objects
        std::unordered_set<uint64_t> total_pages;    // All pages used by objects in this heap
    };

    // Add these function declarations
    double calculatePromotionRate(std::unordered_map<uint8_t, struct HeapStats> &heapStats);
    void adjustInactiveWindowThreshold(double promotionRate);

    static const uint8_t NEW_HEAP = 0;
    static const uint8_t HOT_HEAP = 1;
    static const uint8_t COLD_HEAP = 2;

    uint64_t intervalDuration;
    uint8_t inactiveWindowThreshold;
    dict *dictionary;
    Sama *sama;
    std::atomic<uint8_t> *ObaseRTMode;
    std::thread migratorThread;
    static std::once_flag initFlag;
    static std::atomic<bool> isInitialized;
    static std::unique_ptr<ObjectCollector> instance;

    void migratorThreadFunction();
    void updateObjectTrackingWithStats();
    void promoteHotObjects();
    void demoteColdObjects();
    bool migrateObject(uintptr_t *ptrAddress, uint8_t targetHeap, uint64_t &bytesMoved);
    void clearAllAccessBits();

    static bool getAccessBit(const uintptr_t *ptr)
    {
        return (__atomic_load_n(ptr, __ATOMIC_RELAXED) & ACCESS_BIT_MASK) != 0;
    }

    static void clearAccessBit(uintptr_t *ptr)
    {
        uintptr_t oldVal, newVal;
        do
        {
            oldVal = __atomic_load_n(ptr, __ATOMIC_RELAXED);
            newVal = oldVal & ~ACCESS_BIT_MASK;
        } while (!__atomic_compare_exchange_n(ptr, &oldVal, newVal, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }

    static uint8_t getHeapId(const uintptr_t *ptr)
    {
        return (__atomic_load_n(ptr, __ATOMIC_RELAXED) & HEAP_ID_MASK) >> HEAP_ID_SHIFT;
    }

    static void setHeapId(uintptr_t *ptr, uint8_t heapId)
    {
        uintptr_t oldVal, newVal;
        do
        {
            oldVal = __atomic_load_n(ptr, __ATOMIC_RELAXED);
            newVal = (oldVal & ~HEAP_ID_MASK) | (static_cast<uintptr_t>(heapId) << HEAP_ID_SHIFT);
        } while (!__atomic_compare_exchange_n(ptr, &oldVal, newVal, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }

    static uint8_t getConsecutiveInactiveWins(const uintptr_t *ptr)
    {
        return (__atomic_load_n(ptr, __ATOMIC_RELAXED) & CONSECUTIVE_INACTIVE_WINS_MASK) >> CONSECUTIVE_INACTIVE_WINS_SHIFT;
    }

    static void incrementConsecutiveInactiveWins(uintptr_t *ptr)
    {
        uintptr_t oldVal, newVal;
        do
        {
            oldVal = __atomic_load_n(ptr, __ATOMIC_RELAXED);
            uint8_t wins = (oldVal & CONSECUTIVE_INACTIVE_WINS_MASK) >> CONSECUTIVE_INACTIVE_WINS_SHIFT;
            if (wins < CONSECUTIVE_INACTIVE_WINS_MAX)
            {
                newVal = (oldVal & ~CONSECUTIVE_INACTIVE_WINS_MASK) |
                         (static_cast<uintptr_t>(wins + 1) << CONSECUTIVE_INACTIVE_WINS_SHIFT);
            }
            else
            {
                newVal = oldVal;
            }
        } while (!__atomic_compare_exchange_n(ptr, &oldVal, newVal, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }

    static void resetConsecutiveInactiveWins(uintptr_t *ptr)
    {
        uintptr_t oldVal, newVal;
        do
        {
            oldVal = __atomic_load_n(ptr, __ATOMIC_RELAXED);
            newVal = oldVal & ~CONSECUTIVE_INACTIVE_WINS_MASK;
        } while (!__atomic_compare_exchange_n(ptr, &oldVal, newVal, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }

    static uint8_t getATC(const uintptr_t *ptr)
    {
        return (__atomic_load_n(ptr, __ATOMIC_RELAXED) & ACTIVE_THREAD_COUNT_MASK) >> ACTIVE_THREAD_COUNT_SHIFT;
    }

    static bool getMigrationLock(const uintptr_t *ptr)
    {
        return (__atomic_load_n(ptr, __ATOMIC_RELAXED) & MIGRATION_LOCK_MASK) != 0;
    }

    static void setMigrationLock(uintptr_t *ptr)
    {
        uintptr_t oldVal, newVal;
        do
        {
            oldVal = __atomic_load_n(ptr, __ATOMIC_RELAXED);
            newVal = oldVal | MIGRATION_LOCK_MASK;
        } while (!__atomic_compare_exchange_n(ptr, &oldVal, newVal, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }

    static void clearMigrationLock(uintptr_t *ptr)
    {
        uintptr_t oldVal, newVal;
        do
        {
            oldVal = __atomic_load_n(ptr, __ATOMIC_RELAXED);
            newVal = oldVal & ~MIGRATION_LOCK_MASK;
        } while (!__atomic_compare_exchange_n(ptr, &oldVal, newVal, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }

public:
    ObjectCollector(uint64_t intervalDuration, uint8_t inactiveWindowThreshold,
                   dict *dictionary, Sama *allocator, std::atomic<uint8_t> *ObaseRTMode);
    ~ObjectCollector();

    // Static method to initialize the thread
    static ObjectCollector *initializeIfNeeded(
        uint64_t intervalDuration, uint8_t inactiveWindowThreshold,
        dict *dictionary, Sama *allocator, std::atomic<uint8_t> *ObaseRTMode);

    // Method to enable reference count tracking in scope guards
    static void signalMigrationBegin();
    // Method to disable reference count tracking in scope guards
    static void signalMigrationEnd();
};

#endif // OBASE_OBJECT_COLLECTOR_H
