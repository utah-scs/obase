#pragma once

#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include "/home/vin/jemalloc/include/jemalloc/jemalloc.h"

class Sama
{
public:
    enum class MemoryType
    {
        NEW_HEAP,           // Where all objects start (jemalloc default arenas)
        DRAM,               // mmap backed by 4KB pages
        DRAM_2MB_THP,       // mmap backed by 2MB THP, using MADV_COLLAPSE
        DRAM_2MB_HUGETLBFS, // mmap backed by 2MB huge pages allocated by hugetlbfs
        DRAM_1GB_HUGETLBFS, // mmap backed by 1GB huge pages allocated by hugetlbfs
        SSD,
        CXL
    };

    // Allow direct construction
    explicit Sama(std::vector<MemoryType> types);
    ~Sama();

    // Delete copy operations
    Sama(const Sama &) = delete;
    Sama &operator=(const Sama &) = delete;

    void *malloc(size_t size, MemoryType type);
    void free(void *ptr, MemoryType type);

    // Usable size of any jemalloc-owned allocation (jem_sallocx), regardless
    // of arena. The Object Collector sizes migration copies with this.
    static size_t GetAllocatedSize(void *ptr);

    void madviseDontNeed(MemoryType type);
    void madvisePageOut(MemoryType type);
    void madviseCold(MemoryType type);
    void madviseUsedRange(MemoryType type, int advice, const char *name);
    void madviseTryVMCollapse(MemoryType type);
    void madviseTryVMHugePage(MemoryType type);
    size_t getActualMemoryUsage();
    void purgeUnused();

    static constexpr size_t Alignment = 1 * 1024 * 1024; // 1MB

private:
    struct ArenaInfo
    {
        int mallocx_flags;
        extent_hooks_t *hooks;
        uintptr_t base_pointer;
        uintptr_t base_end_pointer;
        std::atomic<uintptr_t> pre_alloc;
        // Highest address ever handed out. Ranges above this are pristine
        // mmap pages (zero on first touch), so zero-requests need no memset.
        std::atomic<uintptr_t> high_water{0};
        std::atomic<size_t> current_size = 0;
        int file_fd; // Only for SSD
        MemoryType type = MemoryType::NEW_HEAP;

        // Default constructor
        ArenaInfo() = default;

        // Copy constructor - deleted since we have atomics
        ArenaInfo(const ArenaInfo &) = delete;

        // Move constructor
        ArenaInfo(ArenaInfo &&other) noexcept
            : mallocx_flags(other.mallocx_flags), hooks(other.hooks),
              base_pointer(other.base_pointer),
              base_end_pointer(other.base_end_pointer),
              pre_alloc(other.pre_alloc.load(std::memory_order_relaxed)),
              high_water(other.high_water.load(std::memory_order_relaxed)),
              current_size(other.current_size.load(std::memory_order_relaxed)),
              file_fd(other.file_fd), type(other.type)
        {
            other.hooks = nullptr;
            other.file_fd = -1;
        }

        // Copy assignment - deleted since we have atomics
        ArenaInfo &operator=(const ArenaInfo &) = delete;

        // Move assignment
        ArenaInfo &operator=(ArenaInfo &&other) noexcept
        {
            if (this != &other)
            {
                mallocx_flags = other.mallocx_flags;
                hooks = other.hooks;
                base_pointer = other.base_pointer;
                base_end_pointer = other.base_end_pointer;
                pre_alloc.store(other.pre_alloc.load(std::memory_order_relaxed), std::memory_order_relaxed);
                high_water.store(other.high_water.load(std::memory_order_relaxed), std::memory_order_relaxed);
                current_size.store(other.current_size.load(std::memory_order_relaxed), std::memory_order_relaxed);
                file_fd = other.file_fd;
                type = other.type;

                other.hooks = nullptr;
                other.file_fd = -1;
            }
            return *this;
        }

        // Lock-free allocation
        uintptr_t allocate(size_t size, size_t alignment) noexcept
        {
            uintptr_t current = pre_alloc.load(std::memory_order_relaxed);
            uintptr_t next;

            do
            {
                uintptr_t aligned = (current + alignment - 1) & ~(alignment - 1);
                next = aligned + size;
                if (next > base_end_pointer)
                    return 0;
            } while (!pre_alloc.compare_exchange_weak(current, next,
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed));

            return (current + alignment - 1) & ~(alignment - 1);
        }
    };

    static Sama *instance; // Singleton instance

    std::unordered_map<unsigned int, ArenaInfo> arenas;
    extent_hooks_t *default_hooks = nullptr;

    // Management functions
    void setupArena(MemoryType type);
    int doDestroyArena(unsigned arena_index);
    int doResetArena(unsigned arena_index);
    void destroyAllArenas();
    void resetAllArenas();
    void ConfigureArenas();
    void CleanupJemalloc();
    int LookUp(void *ptr);

    // Hooks
    static void *extent_alloc_hook_dram(extent_hooks_t *extent_hooks, void *new_addr,
                                        size_t size, size_t alignment, bool *zero,
                                        bool *commit, unsigned arena_ind);
    static bool extent_split_hook_dram(extent_hooks_t *extent_hooks, void *addr,
                                       size_t size, size_t size_a, size_t size_b,
                                       bool committed, unsigned arena_ind);
    static bool extent_dalloc_hook_dram(extent_hooks_t *extent_hooks, void *addr,
                                        size_t size, bool committed, unsigned arena_ind);
    static void *extent_alloc_hook_ssd(extent_hooks_t *extent_hooks, void *new_addr,
                                       size_t size, size_t alignment, bool *zero,
                                       bool *commit, unsigned arena_ind);
    static bool extent_split_hook_ssd(extent_hooks_t *extent_hooks, void *addr,
                                      size_t size, size_t size_a, size_t size_b,
                                      bool committed, unsigned arena_ind);
    static bool extent_dalloc_hook_ssd(extent_hooks_t *extent_hooks, void *addr,
                                       size_t size, bool committed, unsigned arena_ind);
    static void *extent_alloc_hook_cxl(extent_hooks_t *extent_hooks, void *new_addr,
                                       size_t size, size_t alignment, bool *zero,
                                       bool *commit, unsigned arena_ind);

    uint64_t ThreadTotalAllocatedBytes();
    uint64_t ThreadTotalDeallocatedBytes();
};

using MemType = Sama::MemoryType;