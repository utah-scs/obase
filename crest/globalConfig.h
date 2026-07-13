#ifndef GLOBAL_CONFIG_H
#define GLOBAL_CONFIG_H
#include <unistd.h>
#include <stdint.h>
#include <atomic>
#include <thread>
#include "spdlog/spdlog.h"
#include "soda.h"

typedef struct
{
    void *heap_start;
    void *stack_start;
    uint64_t stack_to_heap_distance;
} MemoryInfo;

// Get heap start and and stack end address
inline MemoryInfo get_mem_info();

// Declare global variables as extern
extern MemoryInfo mi;
extern void *global_addr_start;
extern SodaBitmap soda_;
extern const uintptr_t ADDRESS_MASK;

// Global scope guard state
enum class ScopeGuardState
{
    INACTIVE, // Guides not added to TLS set. Threads are registered with the runtime
    PREPARE,  // Incremeent generation and wait until all threads see it
    ACTIVE    // All threads have seen the new generation so can begin migration
};

// Thread activity tracking
struct ThreadActivity
{
    std::atomic<int> active_public_funcs{0};
    std::atomic<uint64_t> last_observed_generation{0};
};

extern std::atomic<ScopeGuardState> g_scope_guard_state;
extern std::atomic<uint64_t> g_migration_generation;
extern thread_local uint64_t tl_observed_generation;

// Max number of thread slots
constexpr size_t MAX_THREAD_SLOTS = 64;
extern ThreadActivity g_thread_activity[MAX_THREAD_SLOTS];
extern std::atomic<size_t> g_thread_slot_counter;

/*
 * Assign each thread a UNIQUE slot in the Thread Activity Index. Hashing the
 * thread id (the old scheme) collides with high probability (birthday bound:
 * ~66% for 12 threads over 64 slots); a collision lets a newly-entered thread
 * overwrite the slot's observed generation and make the OC's convergence
 * check pass while an older-generation thread is still mid-operation.
 * Sequential assignment is collision-free for the first MAX_THREAD_SLOTS
 * threads that touch guides (workers + OC), far above the server's thread
 * count. If the counter ever wraps, we log and fall back to sharing.
 */
inline size_t getThreadSlot()
{
    static thread_local size_t slot = []
    {
        size_t s = g_thread_slot_counter.fetch_add(1, std::memory_order_relaxed);
        if (s >= MAX_THREAD_SLOTS)
        {
            spdlog::error("Thread slot counter exceeded {}; slots are now shared and "
                          "migration convergence may be unsafe",
                          MAX_THREAD_SLOTS);
            s = s % MAX_THREAD_SLOTS;
        }
        return s;
    }();
    return slot;
}

#endif // GLOBAL_CONFIG_H