#include "Sama.h"
#include <sys/mman.h>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <linux/mman.h>
#include "spdlog/spdlog.h"

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define MIN_FILE_SIZE (33554432ULL)                                      // 32 MB initial file size
#define MAX_FILE_SIZE (4ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)     // 256 GB maximum file size
#define INITIAL_MMAP_SIZE (1ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL) // 1 TB initial mmap size

Sama *Sama::instance = nullptr;

Sama::Sama(std::vector<MemoryType> types)
{
    instance = this;
    for (const auto &type : types)
    {
        setupArena(type);
    }
    ConfigureArenas();
}

Sama::~Sama()
{
    destroyAllArenas();
    CleanupJemalloc();
}

void Sama::setupArena(MemoryType type)
{
    ArenaInfo info;
    extent_hooks_t *hooks = new extent_hooks_t();
    memset(hooks, 0, sizeof(extent_hooks_t));
    int vm_flags;

    // Get the default hooks
    auto default_hooks_size = sizeof(default_hooks);
    if (jem_mallctl("arena.0.extent_hooks", (void *)&default_hooks, &default_hooks_size, NULL, 0))
    {
        throw std::runtime_error("mallctl() failed for arena.0.extent_hooks");
    }

    switch (type)
    {
    case MemoryType::DRAM:
        hooks->alloc = &extent_alloc_hook_dram;
        hooks->split = &extent_split_hook_dram;
        hooks->dalloc = &extent_dalloc_hook_dram;
        vm_flags = MAP_PRIVATE | MAP_ANONYMOUS;
        info.base_pointer = reinterpret_cast<uintptr_t>(mmap(NULL, INITIAL_MMAP_SIZE,
                                                             PROT_READ | PROT_WRITE, vm_flags, -1, 0));
        if (reinterpret_cast<void *>(info.base_pointer) == MAP_FAILED)
        {
            throw std::runtime_error("Could not map memory for arena");
        }
        info.base_end_pointer = info.base_pointer + INITIAL_MMAP_SIZE;
        info.current_size.store(0, std::memory_order_release);
        info.pre_alloc.store(info.base_pointer, std::memory_order_release);
        spdlog::info("DRAM Base pointer: {}", reinterpret_cast<void *>(info.base_pointer));
        break;
    case MemoryType::DRAM_2MB_THP:
        hooks->alloc = &extent_alloc_hook_dram;
        hooks->split = &extent_split_hook_dram;
        hooks->dalloc = &extent_dalloc_hook_dram;
        vm_flags = MAP_PRIVATE | MAP_ANONYMOUS;
        info.base_pointer = reinterpret_cast<uintptr_t>(mmap(NULL, INITIAL_MMAP_SIZE,
                                                             PROT_READ | PROT_WRITE, vm_flags, -1, 0));
        if (reinterpret_cast<void *>(info.base_pointer) == MAP_FAILED)
        {
            throw std::runtime_error("Could not map memory for arena");
        }
        info.base_end_pointer = info.base_pointer + INITIAL_MMAP_SIZE;
        info.current_size.store(0, std::memory_order_release);
        info.pre_alloc.store(info.base_pointer, std::memory_order_release);

        spdlog::info("DRAM_2MB_THP Base pointer: {}", reinterpret_cast<void *>(info.base_pointer));
        break;
    case MemoryType::DRAM_2MB_HUGETLBFS:
        hooks->alloc = &extent_alloc_hook_dram;
        hooks->split = &extent_split_hook_dram;
        hooks->dalloc = &extent_dalloc_hook_dram;
        vm_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB | MAP_NORESERVE;
        info.base_pointer = reinterpret_cast<uintptr_t>(mmap(NULL, INITIAL_MMAP_SIZE,
                                                             PROT_READ | PROT_WRITE, vm_flags, -1, 0));
        if (reinterpret_cast<void *>(info.base_pointer) == MAP_FAILED)
        {
            throw std::runtime_error("Could not map memory for arena");
        }
        info.base_end_pointer = info.base_pointer + INITIAL_MMAP_SIZE;
        info.current_size.store(INITIAL_MMAP_SIZE, std::memory_order_release);
        info.pre_alloc.store(info.base_pointer, std::memory_order_release);
        spdlog::info("DRAM_2M Base pointer: {}", reinterpret_cast<void *>(info.base_pointer));
        break;
    case MemoryType::DRAM_1GB_HUGETLBFS:
        hooks->alloc = &extent_alloc_hook_dram;
        hooks->split = &extent_split_hook_dram;
        hooks->dalloc = &extent_dalloc_hook_dram;
        vm_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB | MAP_NORESERVE;
        info.base_pointer = reinterpret_cast<uintptr_t>(mmap(NULL, INITIAL_MMAP_SIZE,
                                                             PROT_READ | PROT_WRITE, vm_flags, -1, 0));
        if (reinterpret_cast<void *>(info.base_pointer) == MAP_FAILED)
        {
            throw std::runtime_error("Could not map memory for arena");
        }
        info.base_end_pointer = info.base_pointer + INITIAL_MMAP_SIZE;
        info.current_size = 0;
        info.pre_alloc = info.base_pointer;
        spdlog::info("DRAM_1G Base pointer: {}", reinterpret_cast<void *>(info.base_pointer));
        break;
    case MemoryType::SSD:
        hooks->alloc = &extent_alloc_hook_ssd;
        hooks->dalloc = &extent_dalloc_hook_ssd;
        hooks->split = &extent_split_hook_ssd;
        info.file_fd = open("ColdObjectFile", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (info.file_fd == -1)
        {
            throw std::runtime_error("Could not open SSD file");
        }
        ftruncate(info.file_fd, MIN_FILE_SIZE);
        vm_flags = MAP_SHARED;
        info.base_pointer = reinterpret_cast<uintptr_t>(mmap(NULL, INITIAL_MMAP_SIZE, PROT_READ | PROT_WRITE,
                                                             vm_flags, info.file_fd, 0));
        if (reinterpret_cast<void *>(info.base_pointer) == MAP_FAILED)
        {
            throw std::runtime_error("Could not map memory for arena");
        }
        info.base_end_pointer = info.base_pointer + MIN_FILE_SIZE;
        info.current_size = MIN_FILE_SIZE;
        info.pre_alloc = info.base_pointer;
        spdlog::info("SSD Base pointer: {}", reinterpret_cast<void *>(info.base_pointer));
        break;
    case MemoryType::CXL:
        // TODO: Implement CXL setup
        break;
    }

    // Nothing above base_pointer has been handed out yet
    info.high_water.store(info.base_pointer, std::memory_order_relaxed);


    unsigned int arena_index;
    size_t sz = sizeof(arena_index);
    jem_mallctl("arenas.create", &arena_index, &sz, nullptr, 0);
    if (arena_index == -1)
    {
        throw std::runtime_error("Failed to create new arena of type: " +
                                 std::to_string(static_cast<int>(type)));
    }
    std::ostringstream hooks_key;
    hooks_key << "arena." << std::to_string(arena_index) << ".extent_hooks";
    size_t len = sizeof(hooks);
    printf("Hooks key is: %s\n", hooks_key.str().c_str());
    if (auto ret = jem_mallctl(hooks_key.str().c_str(), nullptr, nullptr, &hooks,
                               len))
    {
        throw std::runtime_error("Failed to set extent_hooks for arena");
    }
    arenas.emplace(arena_index, std::move(info));
    arenas[arena_index].hooks = hooks;
    arenas[arena_index].type = type;
    arenas[arena_index].mallocx_flags = MALLOCX_ARENA(arena_index) | MALLOCX_TCACHE_NONE;
    printf("Created new arena of type: %d with arena_index: %d\n", static_cast<int>(type), arena_index);
}

void *Sama::malloc(size_t size, MemoryType type)
{
    for (auto &[arena_index, info] : arenas)
    {
        if (info.type == type)
        {
            return jem_mallocx(size, info.mallocx_flags);
        }
    }
    throw std::runtime_error("Arena type not initialized");
}

void Sama::free(void *ptr, MemoryType type)
{
    if (type == MemoryType::NEW_HEAP)
    {
        jem_free(ptr);
        return;
    }

    for (auto &[arena_index, info] : arenas)
    {
        if (info.type == type)
        {
            jem_dallocx(ptr, info.mallocx_flags);
            return;
        }
    }
    spdlog::error("Freeing pointer {} in unkown arena type {}", fmt::ptr(ptr), static_cast<int>(type));
}

void *Sama::extent_alloc_hook_dram(extent_hooks_t *extent_hooks, void *new_addr,
                                             size_t size, size_t alignment, bool *zero,
                                             bool *commit, unsigned arena_ind)
{

    if (!instance)
        return nullptr;

    // std::shared_lock lock(instance->arenas_rwlock); ///////
    auto it = instance->arenas.find(arena_ind);
    if (it == instance->arenas.end())
        return nullptr;

    auto &info = it->second;

    // Bump-allocate with a CAS loop: jemalloc can invoke extent hooks from
    // multiple threads, and a plain load/store here would hand out
    // overlapping extents.
    uintptr_t current = info.pre_alloc.load(std::memory_order_relaxed);
    uintptr_t ret, next;
    do
    {
        ret = (current + alignment - 1) & ~(alignment - 1);
        next = ret + size;
        if (next > info.base_end_pointer)
        {
            return nullptr;
        }
    } while (!info.pre_alloc.compare_exchange_weak(current, next,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed));

    info.current_size.fetch_add(size, std::memory_order_relaxed);

    // Track the pristine frontier and satisfy zero-requests without memset
    // when the range has never been handed out: fresh anonymous mmap pages
    // are zero on first touch. memset-ing a whole extent here faults in
    // every page immediately -- with jemalloc's exponentially growing extent
    // sizes that turned a ~1GB migration round into a multi-GB RSS spike
    // (observed OOM). Only ranges below the high-water mark (re-issued after
    // an end-rewind in the dalloc hook) can hold stale data and need zeroing.
    uintptr_t hw = info.high_water.load(std::memory_order_acquire);
    bool pristine = (ret >= hw);
    while (next > hw &&
           !info.high_water.compare_exchange_weak(hw, next,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))
    {
    }

    if (*zero && !pristine)
    {
        memset(reinterpret_cast<void *>(ret), 0, size);
    }
    *commit = true;

    return reinterpret_cast<void *>(ret);
}

bool Sama::extent_dalloc_hook_dram(extent_hooks_t *extent_hooks, void *addr,
                                             size_t size, bool committed, unsigned arena_ind)
{
    if (!instance)
        return true;

    // std::shared_lock lock(instance->arenas_rwlock); ///////
    auto it = instance->arenas.find(arena_ind);
    if (it == instance->arenas.end())
        return true;

    auto &info = it->second;

    // Always update current_size to reflect the deallocation
    info.current_size.fetch_sub(size, std::memory_order_relaxed);

    // If this was the last allocation, rewind pre_alloc (CAS: only if nothing
    // was allocated past it in the meantime)
    const uintptr_t dealloc_end = reinterpret_cast<uintptr_t>(addr) + size;
    uintptr_t expected = dealloc_end;
    info.pre_alloc.compare_exchange_strong(expected, dealloc_end - size,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed);
    return instance->default_hooks->dalloc(instance->default_hooks, addr, size, committed, 0);
}

bool Sama::extent_split_hook_dram(extent_hooks_t *extent_hooks, void *addr, size_t size, size_t size_a, size_t size_b, bool committed, unsigned arena_ind)
{
    return instance->default_hooks->split(instance->default_hooks, addr, size, size_a, size_b, committed, 0);
}

void *Sama::extent_alloc_hook_ssd(extent_hooks_t *extent_hooks, void *new_addr,
                                            size_t size, size_t alignment, bool *zero, bool *commit, unsigned arena_ind)
{
    if (!instance)
        return nullptr;

    ArenaInfo &info = instance->arenas[arena_ind];
    uintptr_t ret = (info.pre_alloc + alignment - 1) & ~(alignment - 1);
    size_t offset = ret - info.base_pointer;

    printf("extent_alloc_hook_ssd: %lu %lu %lu %p\n", size, alignment, offset, (void *)ret);

    if (offset + size > info.current_size)
    {
        size_t new_size = info.current_size;
        while (offset + size > new_size)
        {
            new_size *= 2;
        }

        if (new_size > MAX_FILE_SIZE)
        {
            throw std::runtime_error("New file size: " + std::to_string(new_size) + " exceeds maximum limit of " + std::to_string(MAX_FILE_SIZE));
        }

        if (ftruncate(info.file_fd, new_size) == -1)
        {

            return nullptr;
        }

        info.current_size = new_size;
        info.base_end_pointer = info.base_pointer + new_size;
    }

    info.pre_alloc = ret + size;

    if (*zero)
    {
        memset(reinterpret_cast<void *>(ret), 0, size);
    }

    *commit = true;
    return reinterpret_cast<void *>(ret);
}

bool Sama::extent_split_hook_ssd(extent_hooks_t *extent_hooks, void *addr, size_t size, size_t size_a, size_t size_b, bool committed, unsigned arena_ind)
{
    return instance->default_hooks->split(instance->default_hooks, addr, size, size_a, size_b, committed, 0);
}

void *Sama::extent_alloc_hook_cxl(extent_hooks_t *extent_hooks, void *new_addr,
                                            size_t size, size_t alignment, bool *zero, bool *commit, unsigned arena_ind)
{
    // TODO: Implement CXL allocation
    return nullptr;
}

bool Sama::extent_dalloc_hook_ssd(extent_hooks_t *extent_hooks, void *addr,
                                            size_t size, bool committed, unsigned arena_ind)
{
    if (!instance)
        return true;

    ArenaInfo &info = instance->arenas[arena_ind];
    off_t offset = reinterpret_cast<char *>(addr) - reinterpret_cast<char *>(info.base_pointer);

    if (fallocate(info.file_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, size) == -1)
    {
        return true;
    }

    return false;
}

// Apply an madvise hint to the used portion of each arena of the given type.
// Only advises [base, pre_alloc) rounded up to a page: advising the whole 1TB
// reservation makes the kernel walk vast empty ranges. Errors are logged, not
// thrown -- these run on the migrator thread, where an uncaught exception
// would terminate the server.
void Sama::madviseUsedRange(MemoryType type, int advice, const char *name)
{
    constexpr size_t PAGE = 4096;
    for (auto &[arena_index, info] : arenas)
    {
        if (info.type == type)
        {
            uintptr_t pre = info.pre_alloc.load(std::memory_order_acquire);
            if (pre <= info.base_pointer)
            {
                continue;
            }
            size_t used_size = (pre - info.base_pointer + PAGE - 1) & ~(PAGE - 1);

            if (madvise(reinterpret_cast<void *>(info.base_pointer), used_size, advice) == -1)
            {
                spdlog::error("madvise({}) failed for arena {} ({} bytes): {}",
                              name, arena_index, used_size, strerror(errno));
            }
        }
    }
}

// This call is destructive, use with caution
void Sama::madviseDontNeed(MemoryType type)
{
    madviseUsedRange(type, MADV_DONTNEED, "MADV_DONTNEED");
}

void Sama::madvisePageOut(MemoryType type)
{
    madviseUsedRange(type, MADV_PAGEOUT, "MADV_PAGEOUT");
}

void Sama::madviseCold(MemoryType type)
{
    madviseUsedRange(type, MADV_COLD, "MADV_COLD");
}

// MemType::DRAM_2MB_THP is the only ideal candidate for this.
// Be cautious about the advises the mmap might have previously received.
void Sama::madviseTryVMCollapse(MemoryType type)
{
    constexpr size_t TWO_MB = 2 * 1024 * 1024;

    for (auto &[arena_index, info] : arenas)
    {
        if (info.type == type)
        {
            // Get the current pre_alloc position to determine actual used size
            uintptr_t current_pre_alloc = info.pre_alloc.load(std::memory_order_acquire);
            if (current_pre_alloc < info.base_pointer)
            {
                spdlog::error("Arena {}: Invalid pre_alloc value: pre_alloc ({}) < base_pointer ({})",
                              arena_index,
                              reinterpret_cast<void *>(current_pre_alloc),
                              reinterpret_cast<void *>(info.base_pointer));
                continue; // Skip this arena
            }
            size_t used_size = current_pre_alloc - info.base_pointer;

            // If no memory is used, skip this arena
            if (used_size == 0)
            {
                spdlog::debug("Arena {} has no used memory, skipping", arena_index);
                continue;
            }

            // Round down base address to nearest 2MB boundary
            uintptr_t aligned_addr = (info.base_pointer & ~(TWO_MB - 1));

            // Ensure the aligned address isn't before our mapped region
            if (aligned_addr < info.base_pointer)
            {
                aligned_addr += TWO_MB;
            }

            // Calculate the extra bytes needed at start due to alignment
            size_t start_padding = 0;
            if (aligned_addr < info.base_pointer)
            {
                start_padding = info.base_pointer - aligned_addr;
            }
            else
            {
                // If we somehow got an aligned address beyond our base, use the base
                aligned_addr = info.base_pointer;
            }

            // Round up size to nearest 2MB, including any padding needed for alignment
            size_t padded_size = used_size + start_padding;
            size_t aligned_size = (padded_size + TWO_MB - 1) & ~(TWO_MB - 1);

            // Log the alignment calculations
            spdlog::debug("Arena {}: Original addr: {}, Aligned addr: {}, Used size: {}, Aligned size: {}, Start padding: {}",
                          arena_index,
                          fmt::ptr(reinterpret_cast<void *>(info.base_pointer)),
                          fmt::ptr(reinterpret_cast<void *>(aligned_addr)),
                          used_size,
                          aligned_size,
                          start_padding);

            // Skip if we don't have enough space to collapse
            if (aligned_size < TWO_MB)
            {
                spdlog::debug("Arena {} has insufficient size for 2MB pages, skipping", arena_index);
                continue;
            }

            // First populate the pages to ensure they're in memory
            if (madvise(reinterpret_cast<void *>(aligned_addr), aligned_size, MADV_POPULATE_WRITE) == -1)
            {
                spdlog::error("MADV_POPULATE_WRITE failed for arena {} (type: {}) with size: {}. Error: {}",
                              arena_index,
                              static_cast<int>(type),
                              aligned_size,
                              strerror(errno));
                continue; // log, don't throw: may run on the migrator thread
            }

            spdlog::debug("MADV_POPULATE_WRITE succeeded for arena {}", arena_index);
            sleep(2); // Sleep for 2 seconds to ensure the pages are populated and kernel is not busy

            // Then try to collapse the pages
            if (madvise(reinterpret_cast<void *>(aligned_addr), aligned_size, MADV_COLLAPSE) == -1)
            {
                spdlog::error("MADV_COLLAPSE failed for arena {} (type: {}) with size: {}. Error: {}",
                              arena_index,
                              static_cast<int>(type),
                              aligned_size,
                              strerror(errno));
                continue; // log, don't throw: may run on the migrator thread
            }

            spdlog::info("Successfully collapsed {} bytes to 2MB pages in arena {}",
                         aligned_size,
                         arena_index);
        }
    }
}

void Sama::madviseTryVMHugePage(MemoryType type)
{
    for (auto &arena : arenas)
    {
        if (arena.second.type == type)
        {
            size_t used = arena.second.pre_alloc - arena.second.base_pointer;
            // used must be a multiple of 2MB
            used = (used / (2 * 1024 * 1024)) * (2 * 1024 * 1024);
            if (used > 0)
            {
                if (madvise(reinterpret_cast<void *>(arena.second.base_pointer), used, MADV_HUGEPAGE) == -1)
                {
                    // Log, don't throw: this runs on the migrator thread
                    spdlog::error("madvise(MADV_HUGEPAGE) failed ({} bytes): {}",
                                  used, strerror(errno));
                }
            }
        }
    }
}

void Sama::destroyAllArenas()
{
    for (auto &[arena_index, info] : arenas)
    {
        doDestroyArena(arena_index);
        if (info.type == MemoryType::SSD)
            close(info.file_fd);
        munmap(reinterpret_cast<void *>(info.base_pointer), info.current_size.load());
        delete info.hooks;
    }
    arenas.clear();
}

int Sama::doDestroyArena(unsigned arena_index)
{
    size_t mib[3];
    size_t miblen = sizeof(mib) / sizeof(size_t);

    std::ostringstream cmd;

    cmd << "arena." << arena_index << ".destroy";
    if (jem_mallctlnametomib(cmd.str().c_str(), mib, &miblen))
    {
        return -1;
    }

    mib[1] = arena_index;
    if (jem_mallctlbymib(mib, miblen, NULL, NULL, NULL, 0))
    {
        return -1;
    }

    return 0;
}

// Discard all of the arena's extant allocations
int Sama::doResetArena(unsigned arena_index)
{
    size_t mib[3];
    size_t miblen = sizeof(mib) / sizeof(size_t);

    std::ostringstream cmd;
    cmd << "arena." << arena_index << ".reset";

    if (jem_mallctlnametomib(cmd.str().c_str(), mib, &miblen))
    {
        return -1;
    }

    mib[1] = arena_index;
    if (jem_mallctlbymib(mib, miblen, NULL, NULL, NULL, 0))
    {
        return -1;
    }

    return 0;
}

void Sama::resetAllArenas()
{
    for (auto &[arena_index, info] : arenas)
    {
        doResetArena(arena_index);
        info.pre_alloc = info.base_pointer;
    }
}

// utility functions

int Sama::LookUp(void *ptr)
{
    unsigned arena_index = -1;
    size_t sz = sizeof(unsigned);
    if (auto ret = jem_mallctl("arenas.lookup", &arena_index, &sz, &ptr,
                               sizeof(ptr)))
    {
        return -1;
    }
    return arena_index;
}

//  get the current allocation size
size_t Sama::GetAllocatedSize(void *pointer)
{
    return jem_sallocx(pointer, 0);
}

// get the total allocated bytes for the current thread
uint64_t Sama::ThreadTotalAllocatedBytes()
{
    uint64_t allocated;
    size_t sz = sizeof(allocated);
    if (jem_mallctl("thread.allocated", reinterpret_cast<void *>(&allocated), &sz, NULL, 0) != 0)
    {
        return 0; // Return 0 if the call fails
    }
    return allocated;
}

// get the total deallocated bytes for the current thread
uint64_t Sama::ThreadTotalDeallocatedBytes()
{
    uint64_t deallocated;
    size_t sz = sizeof(deallocated);
    if (jem_mallctl("thread.deallocated", reinterpret_cast<void *>(&deallocated), &sz, NULL, 0) != 0)
    {
        return 0; // Return 0 if the call fails
    }
    return deallocated;
}

void Sama::ConfigureArenas()
{
    // Enable jemalloc's background thread so decay-based purging runs off
    // the application threads.
    bool opt_background_thread = true;
    size_t sz = sizeof(opt_background_thread);
    if (jem_mallctl("background_thread", NULL, NULL, &opt_background_thread, sz) != 0)
    {
        std::cerr << "Failed to enable background thread" << std::endl;
    }

    // Fast decay on ALL arenas, not just arena 0. Migrated START objects are
    // jem_free()d into whichever default arena the worker threads map to;
    // with the default 10s dirty decay those pages stay resident for a full
    // scan sample and the migration round shows up as an RSS spike.
    // (arena.<MALLCTL_ARENAS_ALL>.dirty_decay_ms is not writable in this
    // jemalloc build, so set the template for future arenas plus each
    // existing arena individually.)
    ssize_t decay_time = 1000; // ms
    if (jem_mallctl("arenas.dirty_decay_ms", NULL, NULL, &decay_time, sizeof(decay_time)))
    {
        fprintf(stderr, "Failed to set default dirty_decay_ms\n");
    }
    if (jem_mallctl("arenas.muzzy_decay_ms", NULL, NULL, &decay_time, sizeof(decay_time)))
    {
        fprintf(stderr, "Failed to set default muzzy_decay_ms\n");
    }

    unsigned narenas = 0;
    size_t nsz = sizeof(narenas);
    if (jem_mallctl("arenas.narenas", &narenas, &nsz, NULL, 0) == 0)
    {
        char cmd[64];
        for (unsigned i = 0; i < narenas; i++)
        {
            // Uninitialized arena slots return an error; that's fine, they
            // will inherit the template values above when created.
            snprintf(cmd, sizeof(cmd), "arena.%u.dirty_decay_ms", i);
            jem_mallctl(cmd, NULL, NULL, &decay_time, sizeof(decay_time));
            snprintf(cmd, sizeof(cmd), "arena.%u.muzzy_decay_ms", i);
            jem_mallctl(cmd, NULL, NULL, &decay_time, sizeof(decay_time));
        }
    }
}

// Force-return all unused (dirty) pages in every arena to the OS now rather
// than waiting for decay. Used by the Object Collector to cap the transient
// RSS growth of a migration round: freed source objects otherwise stay
// resident until the decay timer fires.
void Sama::purgeUnused()
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "arena.%u.purge", MALLCTL_ARENAS_ALL);
    if (jem_mallctl(cmd, NULL, NULL, NULL, 0))
    {
        spdlog::error("arena.{}.purge failed", MALLCTL_ARENAS_ALL);
    }
}

void Sama::CleanupJemalloc()
{
    unsigned narenas;
    size_t sz = sizeof(narenas);
    if (jem_mallctl("arenas.narenas", &narenas, &sz, NULL, 0) == 0)
    {
        for (unsigned i = 0; i < narenas; i++)
        {
            char command[32];
            snprintf(command, sizeof(command), "arena.%u.purge", i);
            jem_mallctl(command, NULL, NULL, NULL, 0);
        }
    }
}