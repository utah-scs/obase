#include "globalConfig.h"

MemoryInfo get_mem_info()
{
    MemoryInfo info = {0};
    void *stack_dummy = NULL;
    FILE *maps;
    char line[256];
    void *heap_end = NULL;
    void *stack_start = NULL;

    maps = fopen("/proc/self/maps", "r");
    if (maps == NULL)
    {
        perror("Failed to open /proc/self/maps");
        return info; // Return with zero-initialized struct if failed to open
    }

    // Iterate over the lines in the maps file
    while (fgets(line, sizeof(line), maps))
    {
        if (strstr(line, "[heap]"))
        {
            sscanf(line, "%p-%p", &info.heap_start, &heap_end); // Get heap range
        }
        if (strstr(line, "[stack]"))
        {
            sscanf(line, "%p", &stack_start); // Get start of stack
        }
    }
    fclose(maps);

    // Stack dummy variable to capture the current stack address (approximate)
    stack_dummy = &stack_dummy;
    info.stack_start = stack_dummy;
    // Compute the distance if both heap and stack starts are found
    if (info.heap_start && stack_start)
    {
        // If stack is above heap (typical in many systems)
        if (stack_start > info.heap_start)
        {
            info.stack_to_heap_distance = (uint64_t)stack_start - (uint64_t)info.heap_start;
        }
        else
        {
            // If stack somehow begins below heap (unusual, but for completeness)
            info.stack_to_heap_distance = (uint64_t)info.heap_start - (uint64_t)stack_start;
        }
    }

    // Output for debugging (can be removed or commented out in production)
    spdlog::info("Heap start: {}, Heap end: {}\n", info.heap_start, heap_end);
    spdlog::info("Stack start: {}, Stack dummy address: {}\n", stack_start, stack_dummy);
    spdlog::info("Stack to heap distance: {} bytes\n", info.stack_to_heap_distance / 64);

    return info;
}

MemoryInfo mi = get_mem_info();
void *global_addr_start = mi.heap_start;
SodaBitmap soda_(mi.stack_to_heap_distance / 8); // divide by 8 because the pointers are 8 byte aligned.
const uintptr_t ADDRESS_MASK = ~(0xFFFFULL << 48);

// For tracking the state of the scope guard (ACTICE/INACTIVE/PREPARE)
std::atomic<ScopeGuardState> g_scope_guard_state{ScopeGuardState::INACTIVE};

// Migration generation - incremented each migration cycle
std::atomic<uint64_t> g_migration_generation{0};

// Thread-local storage for tracking which generation a thread has observed
thread_local uint64_t tl_observed_generation = 0;

// Fixed-size array of thread activity records
ThreadActivity g_thread_activity[MAX_THREAD_SLOTS];

// Monotonic counter handing out unique TAI slots (see getThreadSlot)
std::atomic<size_t> g_thread_slot_counter{0};