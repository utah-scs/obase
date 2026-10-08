#include "ObjectCollector.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include "spdlog/spdlog.h"

std::once_flag ObjectCollector::initFlag;
std::atomic<bool> ObjectCollector::isInitialized{false};
std::unique_ptr<ObjectCollector> ObjectCollector::instance; // Define static instance

const uint8_t ObjectCollector::NEW_HEAP;
const uint8_t ObjectCollector::HOT_HEAP;
const uint8_t ObjectCollector::COLD_HEAP;

// Global variable for the arena allocator to be used by GuideVoid::destroy()
Sama *g_sama = nullptr;

ObjectCollector::ObjectCollector(uint64_t intervalDuration, uint8_t inactiveWindowThreshold,
                               dict *dictionary, Sama *allocator, std::atomic<uint8_t> *ObaseRTMode)
    : intervalDuration(intervalDuration), inactiveWindowThreshold(inactiveWindowThreshold),
      dictionary(dictionary), sama(allocator), ObaseRTMode(ObaseRTMode),
      targetPromotionRate(0.01),                               // Paper default: 1% per minute
      backoffActive(false),                                    // Start not in backoff mode
      multiplicationFactor(2),                                 // Double W each time
      incrementSize(1),                                        // Increase by 1 when above target
      decrementSize(1),                                        // Decrease by 1 when below target
      W_min(1),                                                // Minimum threshold
      // W_max must stay <= CONSECUTIVE_INACTIVE_WINS_MAX (63): the CIW field
      // saturates there, so a larger threshold silently disables demotion.
      W_max(32),                                               // Maximum threshold (paper: 1 <= Ct <= 32)
      initialIntervalCount(0),                                 // Start counting from 0
      initialIntervals(inactiveWindowThreshold),               // Wait for initial threshold intervals
                                                               //   initialIntervals(12),                                    // Wait for initial threshold intervals // meta
      currentPromotionRate(std::numeric_limits<double>::max()) // Start with max value
{
    spdlog::info("Object Collector: scan interval={}s, target promotion rate={:.2f}% per minute, "
                 "Ct initial={}, step=+{}/-{}, bounds=[{}, {}]",
                 intervalDuration, targetPromotionRate * 100, inactiveWindowThreshold,
                 incrementSize, decrementSize, W_min, W_max);
}

/*
 * Start a new migrator thread and clear all access bits
 */
ObjectCollector *ObjectCollector::initializeIfNeeded(
    uint64_t intervalDuration, uint8_t inactiveWindowThreshold,
    dict *dictionary, Sama *allocator, std::atomic<uint8_t> *ObaseRTMode)
{
    if (!isInitialized)
    {
        std::call_once(initFlag, [&]()
                       {
            instance = std::unique_ptr<ObjectCollector>(new ObjectCollector(
                intervalDuration,
                inactiveWindowThreshold,
                dictionary,
                allocator,
                ObaseRTMode
            ));
            instance->clearAllAccessBits();
            instance->migratorThread = std::thread(
                &ObjectCollector::migratorThreadFunction,
                instance.get()
            );
            isInitialized = true; });
    }
    return instance.get();
}

ObjectCollector::~ObjectCollector()
{
    if (migratorThread.joinable())
    {
        migratorThread.join();
    }
}

void ObjectCollector::clearAllAccessBits()
{
    uint64_t index = 1;
    while (soda_.FindNextSetBit(&index))
    {
        uintptr_t address = reinterpret_cast<uintptr_t>(global_addr_start) + (index * 8);
        clearAccessBit(reinterpret_cast<uintptr_t *>(address));
        index++;
    }
}

void ObjectCollector::signalMigrationBegin()
{
    // Increment the generation counter to mark a new migration cycle
    uint64_t new_gen = g_migration_generation.fetch_add(1, std::memory_order_release) + 1;

    // Enter PREPARE state
    g_scope_guard_state.store(ScopeGuardState::PREPARE, std::memory_order_release);
    spdlog::info("Migration preparation phase started - generation {}", new_gen);

    // Wait for active threads to observe the new generation
    while (true)
    {
        bool all_observed = true;
        int active_threads = 0;
        std::vector<size_t> waiting_for_slots;

        // Scan all thread slots
        for (size_t i = 0; i < MAX_THREAD_SLOTS; i++)
        {
            int active = g_thread_activity[i].active_public_funcs.load(std::memory_order_acquire);

            if (active > 0)
            {
                active_threads++;
                uint64_t observed = g_thread_activity[i].last_observed_generation.load(std::memory_order_acquire);

                if (observed < new_gen)
                {
                    all_observed = false;
                    waiting_for_slots.push_back(i);
                }
            }
        }

        if (all_observed)
        {
            spdlog::info("All {} active threads have observed preparation phase", active_threads);
            break;
        }

        spdlog::debug("Waiting for {} threads to observe preparation phase", waiting_for_slots.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Enter ACTIVE state
    g_scope_guard_state.store(ScopeGuardState::ACTIVE, std::memory_order_release);
    spdlog::info("Migration active phase started - reference counting activated");
}

void ObjectCollector::signalMigrationEnd()
{
    g_scope_guard_state.store(ScopeGuardState::INACTIVE, std::memory_order_release);
    spdlog::info("Migration phase ended - reference counting deactivated");
}

void ObjectCollector::migratorThreadFunction()
{
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(intervalDuration));

        if (ObaseRTMode->load(std::memory_order_acquire) >= 1)
        {
            updateObjectTrackingWithStats();

            // Promote/demote objects if migration is enabled and we've completed the initialization period
            if (ObaseRTMode->load(std::memory_order_acquire) == 2 && initialIntervalCount >= initialIntervals)
            {
                signalMigrationBegin();

                // TODO:  Only demote if promotion rate is below target
                // That way the throughput is not degraded suddenly due 
                // excessive paging out objects
                demoteColdObjects();

                promoteHotObjects();

                signalMigrationEnd();
            }
        }
    }
}

double ObjectCollector::calculatePromotionRate(std::unordered_map<uint8_t, HeapStats> &heapStats)
{
    constexpr uint64_t PAGE_SIZE_4K = 4096;            // 4 KiB
    constexpr uint64_t PAGE_SIZE_2M = 2 * 1024 * 1024; // 2 MiB

    // Calculate total bytes of cold memory accessed
    uint64_t coldMemoryAccessed = heapStats.at(COLD_HEAP).accessed_pages.size() * PAGE_SIZE_4K;

    // Calculate total working set size in bytes
    uint64_t totalWorkingSetBytes = 0;
    for (const auto &[heapId, stats] : heapStats)
    {
        uint64_t pageSize = (heapId == HOT_HEAP) ? PAGE_SIZE_2M : PAGE_SIZE_4K;
        totalWorkingSetBytes += stats.accessed_pages.size() * pageSize;
    }

    // If no working set, return 0
    if (totalWorkingSetBytes == 0)
        return 0.0;

    // Convert from interval duration to per-minute rate
    double perMinuteMultiplier = 60.0 / intervalDuration;

    spdlog::debug("Cold memory accessed: {} bytes, total working set: {} bytes",
                  coldMemoryAccessed, totalWorkingSetBytes);

    // Calculate promotion rate as percentage of working set per minute
    return (static_cast<double>(coldMemoryAccessed) / totalWorkingSetBytes) * perMinuteMultiplier;
}

// Additive-increase/additive-decrease (AIAD) controller for the cold
// threshold: raise Ct while the promotion rate is above target, lower it
// gently when below.
void ObjectCollector::adjustInactiveWindowThreshold(double promotionRate)
{
    spdlog::info("Current promotion rate: {:.2f}%, target: {:.2f}%, threshold: {}",
                 promotionRate * 100, targetPromotionRate * 100, inactiveWindowThreshold);

    if (promotionRate > targetPromotionRate)
    {
        uint16_t old_threshold = inactiveWindowThreshold;
        inactiveWindowThreshold = std::min<uint16_t>(W_max, inactiveWindowThreshold + incrementSize);
        backoffActive = true;

        spdlog::info("Promotion rate above target: Increasing threshold from {} to {}",
                     old_threshold, inactiveWindowThreshold);
    }
    // Otherwise, gently decrease threshold when below target
    else
    {
        uint16_t old_threshold = inactiveWindowThreshold;
        inactiveWindowThreshold = std::max<uint16_t>(W_min, inactiveWindowThreshold - decrementSize);
        backoffActive = false;

        if (old_threshold != inactiveWindowThreshold)
        {
            spdlog::info("Promotion rate below target: Decreasing threshold from {} to {}",
                         old_threshold, inactiveWindowThreshold);
        }
    }
}

/**
 * Update object tracking data structures with page utilization statistics
 */
void ObjectCollector::updateObjectTrackingWithStats()
{
    uint64_t index = 1;
    constexpr uint64_t PAGE_SIZE_4K = 4096;            // 4 KiB
    constexpr uint64_t PAGE_SIZE_2M = 2 * 1024 * 1024; // 2 MiB

    std::unordered_map<uint8_t, HeapStats> heap_stats;

    heap_stats[NEW_HEAP] = HeapStats();
    heap_stats[HOT_HEAP] = HeapStats();
    heap_stats[COLD_HEAP] = HeapStats();

    while (soda_.FindNextSetBit(&index))
    {
        // Re-check the bit right before touching the slot: the guide may have
        // been destroyed (its bit cleared) since FindNextSetBit observed it.
        // This narrows -- but does not fully close -- the window in which the
        // OC can read a freed guide slot; closing it needs reclamation that
        // quiesces against the OC (see notes in the repo discussion).
        if (!soda_.Get(index))
        {
            index++;
            continue;
        }

        uintptr_t address = reinterpret_cast<uintptr_t>(global_addr_start) + (index * 8);
        uintptr_t *ptrAddress = reinterpret_cast<uintptr_t *>(address);
        uint8_t heapId = getHeapId(ptrAddress);

        // Get object address without touching content
        uintptr_t originalPtr = __atomic_load_n(ptrAddress, __ATOMIC_RELAXED);
        uintptr_t objAddr = originalPtr & ADDRESS_MASK;

        // Skip NULL pointer objects
        if (objAddr == 0 || objAddr == 0xffffffffffffULL)
        {
            index++;
            continue;
        }

        // Skip slots with a corrupt/garbage heap id (e.g., a stale slot whose
        // memory was reused); indexing heap_stats/heap_names with it would be UB.
        if (heapId > COLD_HEAP)
        {
            index++;
            continue;
        }

        // Count all objects without accessing their content
        heap_stats[heapId].total_objects++;

        // Add this object's page to total_pages without accessing content
        uint64_t page_size = (heapId == HOT_HEAP) ? PAGE_SIZE_2M : PAGE_SIZE_4K;
        uint64_t page = objAddr / page_size;
        heap_stats[heapId].total_pages.insert(page);

        if (getAccessBit(ptrAddress))
        {
            resetConsecutiveInactiveWins(ptrAddress);
            clearAccessBit(ptrAddress);

            // Allocator-reported size; only looked up for accessed objects
            size_t objSize = Sama::GetAllocatedSize(reinterpret_cast<void *>(objAddr));
            // Update access statistics
            heap_stats[heapId].total_accesses++;
            heap_stats[heapId].accessed_bytes += objSize;
            heap_stats[heapId].total_bytes += objSize;

            // Calculate all pages for accessed objects
            uint64_t start_page = objAddr / page_size;
            uint64_t end_page = (objAddr + objSize - 1) / page_size;

            // Add to accessed pages
            for (uint64_t p = start_page; p <= end_page; p++)
            {
                heap_stats[heapId].accessed_pages.insert(p);
            }
        }
        else
        {
            incrementConsecutiveInactiveWins(ptrAddress);
        }

        spdlog::trace("PTR:{} ADDR:{}, ATC:{}, INACT:{}, HEAP_ID:{}, MLOCK:{}, ACC:{}",
                      fmt::ptr(reinterpret_cast<void *>(ptrAddress)),
                      fmt::ptr(reinterpret_cast<void *>(objAddr)),
                      getATC(ptrAddress),
                      getConsecutiveInactiveWins(ptrAddress),
                      heapId,
                      getMigrationLock(ptrAddress),
                      getAccessBit(ptrAddress));

        index++;
    }

    // Calculate and log detailed statistics for each heap
    uint64_t total_accessed_bytes = 0;
    uint64_t total_accessed_page_space = 0;
    uint64_t total_objects = 0;
    uint64_t total_accesses = 0;
    uint64_t total_all_page_space = 0;

    const char *heap_names[] = {"START", "HOT", "COLD"};

    for (const auto &[heap_id, stats] : heap_stats)
    {
        uint64_t page_size = (heap_id == HOT_HEAP) ? PAGE_SIZE_2M : PAGE_SIZE_4K;
        uint64_t heap_accessed_page_space = stats.accessed_pages.size() * page_size;
        uint64_t heap_total_page_space = stats.total_pages.size() * page_size;

        // Calculate utilization based on accessed bytes and accessed pages
        double heap_utilization = stats.accessed_pages.empty() ? 0.0 : static_cast<double>(stats.accessed_bytes) / heap_accessed_page_space;

        // Update totals
        total_accessed_bytes += stats.accessed_bytes;
        total_accessed_page_space += heap_accessed_page_space;
        total_all_page_space += heap_total_page_space;
        total_objects += stats.total_objects;
        total_accesses += stats.total_accesses;

        spdlog::info("{} Heap Statistics: "
                     "Utilization: {:.2f}% "
                     "Total Size: {} "
                     "Accessed Size: {} "
                     "Objects: {} "
                     "Accesses: {} "
                     "Accessed Pages: {} "
                     "Total Pages: {} "
                     "Page Size: {}",
                     heap_names[heap_id],
                     heap_utilization * 100,
                     stats.total_bytes,
                     stats.accessed_bytes,
                     stats.total_objects,
                     stats.total_accesses,
                     stats.accessed_pages.size(),
                     stats.total_pages.size(),
                     (heap_id == HOT_HEAP) ? "2MiB" : "4KiB");
    }

    // Calculate overall utilization based on accessed bytes only
    double overall_utilization = total_accessed_page_space > 0 ? static_cast<double>(total_accessed_bytes) / total_accessed_page_space : 0.0;

    spdlog::info("Overall Statistics: "
                 "Page Utilization: {:.2f}% "
                 "Total Accessed: {} "
                 "Total Objects: {} "
                 "Total Accesses: {} "
                 "Accessed Page Space: {} "
                 "Total Page Space: {}",
                 overall_utilization * 100,
                 total_accessed_bytes,
                 total_objects,
                 total_accesses,
                 total_accessed_page_space,
                 total_all_page_space);

    // // Increment initialization counter
    initialIntervalCount++;

    // Only calculate promotion rate and adjust threshold after initialization period
    if (initialIntervalCount >= initialIntervals + 1)
    {
        // Calculate promotion rate
        currentPromotionRate = calculatePromotionRate(heap_stats);

        // Log current promotion rate
        spdlog::info("Current promotion rate: {:.2f}%, target: {:.2f}%",
                     currentPromotionRate * 100, targetPromotionRate * 100);

        // Adjust threshold based on promotion rate
        adjustInactiveWindowThreshold(currentPromotionRate);
    }
    else
    {
        spdlog::info("Initialization period {}/{}: not calculating promotion rate yet",
                     initialIntervalCount, initialIntervals);
        // Keep promotion rate at -1 during initialization
        currentPromotionRate = std::numeric_limits<double>::max();
    }
}

void ObjectCollector::promoteHotObjects()
{
    uint64_t promoted = 0;
    uint64_t promotedBytes = 0;
    uint64_t index = 1;
    while (soda_.FindNextSetBit(&index))
    {
        if (!soda_.Get(index)) // re-check: slot may have been freed since the scan
        {
            index++;
            continue;
        }
        uintptr_t address = reinterpret_cast<uintptr_t>(global_addr_start) + (index * 8);
        uintptr_t *ptrAddress = reinterpret_cast<uintptr_t *>(address);

        if (getConsecutiveInactiveWins(ptrAddress) == 0 && getHeapId(ptrAddress) != HOT_HEAP && (*ptrAddress & ADDRESS_MASK) != 0xffffffffffffULL)
        {
            if (migrateObject(ptrAddress, HOT_HEAP, promotedBytes))
            {
                promoted++;
                spdlog::trace("Promoted object to hot heap: {}", fmt::ptr(reinterpret_cast<void *>(ptrAddress)));
                if (promoted % purgeInterval == 0)
                {
                    sama->purgeUnused();
                }
                if (promoted >= maxMigrationsPerRound)
                {
                    spdlog::info("Promotion cap ({}) reached; remaining objects deferred to next round", maxMigrationsPerRound);
                    break;
                }
            }
        }

        index++;
    }
    // Enable this when using DRAM_2MB_THP pool and if the madvise HUGE_PAGES
    // is not set during creation of DRAM_2MB_THP. If using the DRAM_2MB_HUGETLBFS
    // pool, then need to call the below madvise.
    if (promoted > 0)
    {
        // A) Syncronous call to collapse 4K pages to 2MB THP
        // sama->madviseTryVMCollapse(MemType::DRAM_2MB_THP);
        // B) Asynchronous call to promote 2MB pages to THP -- scanner thread will
        // promote the pages in the background
        sama->madviseTryVMHugePage(MemType::DRAM_2MB_THP);
        sama->purgeUnused();
    }
    spdlog::debug("Promoted {} objects to hot heap. bytes_promoted={}", promoted, promotedBytes);
}

void ObjectCollector::demoteColdObjects()
{
    uint64_t demoted = 0;
    uint64_t demotedBytes = 0;
    uint64_t index = 1;
    while (soda_.FindNextSetBit(&index))
    {
        if (!soda_.Get(index)) // re-check: slot may have been freed since the scan
        {
            index++;
            continue;
        }
        uintptr_t address = reinterpret_cast<uintptr_t>(global_addr_start) + (index * 8);
        uintptr_t *ptrAddress = reinterpret_cast<uintptr_t *>(address);

        if (getConsecutiveInactiveWins(ptrAddress) >= inactiveWindowThreshold && getHeapId(ptrAddress) != COLD_HEAP && (*ptrAddress & ADDRESS_MASK) != 0xffffffffffffULL)
        {
            if (migrateObject(ptrAddress, COLD_HEAP, demotedBytes))
            {
                demoted++;
                spdlog::trace("Demoted object to cold heap: {}", fmt::ptr(reinterpret_cast<void *>(ptrAddress)));
                if (demoted % purgeInterval == 0)
                {
                    sama->purgeUnused();
                }
                if (demoted >= maxMigrationsPerRound)
                {
                    spdlog::info("Demotion cap ({}) reached; remaining objects deferred to next round", maxMigrationsPerRound);
                    break;
                }
            }
        }

        index++;
    }

    if (currentPromotionRate <= targetPromotionRate)
    {
        // Promotion rate is under control (or equal to target) - safe to page out
        sama->madvisePageOut(MemType::DRAM);
        spdlog::info("Target promotion rate reached: Paging out cold heap memory (rate: {:.2f}% <= target: {:.2f}%)",
                     currentPromotionRate * 100, targetPromotionRate * 100);
    }
    else
    {
        if ((demoted > 0))
        {
            // Promotion rate is above target or in initialization - just mark as cold
            sama->madviseCold(MemType::DRAM);
            spdlog::info("Target promotion rate not reached: Marking heap as MADV_COLD without paging out (rate: {:.2f}%)",
                         currentPromotionRate * 100);
        }
    }
    if (demoted > 0)
    {
        sama->purgeUnused();
    }
    spdlog::debug("Demoted {} objects to cold heap. bytes_demoted={}", demoted, demotedBytes);
}

// Follows the Asycnhronous Lock-Free Migration protocol (ODM)
bool ObjectCollector::migrateObject(uintptr_t *ptrAddress, uint8_t targetHeap, uint64_t &bytesMoved)
{
    // Step 1 & 2: Read original guide and check for active references.
    // All eligibility checks are made on this single snapshot; the two CAS
    // steps below only succeed if the guide still matches it.
    uintptr_t originalPtr = __atomic_load_n(ptrAddress, __ATOMIC_ACQUIRE);
    if ((originalPtr & ACTIVE_THREAD_COUNT_MASK) != 0 || (originalPtr & MIGRATION_LOCK_MASK))
    {
        return false;
    }

    uintptr_t objAddr = originalPtr & ADDRESS_MASK;
    if (objAddr == 0 || objAddr == 0xffffffffffffULL)
    {
        return false;
    }

    MemType originMemType;
    switch ((originalPtr & HEAP_ID_MASK) >> HEAP_ID_SHIFT)
    {
    case HOT_HEAP:
        // originMemType = MemType::DRAM_2MB_HUGETLBFS;
        originMemType = MemType::DRAM_2MB_THP;
        break;
    case COLD_HEAP:
        originMemType = MemType::DRAM;
        break;
    case NEW_HEAP:
        originMemType = MemType::NEW_HEAP;
        break;
    default:
        // Corrupt or stale heap id: freeing into the wrong arena would
        // corrupt the allocator. Skip this object.
        spdlog::error("migrateObject: invalid heap id in guide {:p}", (void *)ptrAddress);
        return false;
    }

    MemType targetMemType;
    switch (targetHeap)
    {
    case HOT_HEAP:
        // targetMemType = MemType::DRAM_2MB_HUGETLBFS;
        targetMemType = MemType::DRAM_2MB_THP;
        break;
    case COLD_HEAP:
        targetMemType = MemType::DRAM;
        break;
    default:
        return false;
    }

    // Step 3: First CAS -- set the migration lock on the exact word we
    // validated. If the guide changed since the snapshot (a dereference, an
    // ATC increment), abort instead of locking an unvalidated word.
    uintptr_t lockedPtr = originalPtr | MIGRATION_LOCK_MASK;
    if (!__atomic_compare_exchange_n(ptrAddress, &originalPtr, lockedPtr, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    {
        return false; // guide changed since validation
    }

    // Step 4: Allocate and copy. Size comes from the allocator (usable size
    // of the source allocation), not from the payload -- the OC makes no
    // assumptions about object contents, and the copy stays in the same
    // jemalloc size class across migrations.
    void *oldAddr = reinterpret_cast<void *>(objAddr);
    size_t objSize = Sama::GetAllocatedSize(oldAddr);

    void *newAddr = sama->malloc(objSize, targetMemType);
    if (newAddr == nullptr)
    {
        clearMigrationLock(ptrAddress);
        spdlog::error("Failed to allocate {} bytes in heap {}", objSize, targetHeap);
        return false;
    }

    memcpy(newAddr, oldAddr, objSize);

    // Step 5: Second CAS -- publish the new guide (new address, new heap id,
    // lock cleared) only if the word is still exactly the locked snapshot.
    // Any concurrent dereference cleared MLOCK and makes this fail.
    uintptr_t newPtr = (lockedPtr & ~ADDRESS_MASK) | reinterpret_cast<uintptr_t>(newAddr);
    newPtr = (newPtr & ~HEAP_ID_MASK) | (static_cast<uintptr_t>(targetHeap) << HEAP_ID_SHIFT);
    newPtr &= ~MIGRATION_LOCK_MASK; // Clear migration lock

    uintptr_t expected = lockedPtr;
    if (__atomic_compare_exchange_n(ptrAddress, &expected, newPtr, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
    {
        // Step 6a: Migration successful
        sama->free(oldAddr, originMemType);
        bytesMoved += objSize;
        return true;
    }
    else
    {
        // Step 6b: Migration failed, guide changed (concurrent access vetoed)
        sama->free(newAddr, targetMemType);
        clearMigrationLock(ptrAddress);
        return false;
    }
}
