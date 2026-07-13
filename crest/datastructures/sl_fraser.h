#ifndef _SKIPLIST_FRASER_H_
#define _SKIPLIST_FRASER_H_

#include <stdlib.h>
#include <string>
#include <vector>
#include <utility>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "Guide.hpp"

#define MAXLEVEL 32
// #define MAXLEVEL 64

// Function to get a random number in a range
inline unsigned int rand_range(unsigned int n)
{
    return rand() % n;
}

// Helper macros for likely/unlikely branch prediction
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

// Atomic operations and pointer marking utilities
// Set the least significant bit of a pointer to mark it as logically deleted
inline uintptr_t set_mark(uintptr_t ptr)
{
    return ptr | 0x1;
}

// Clear the mark bit from a pointer
inline uintptr_t unset_mark(uintptr_t ptr)
{
    return ptr & ~0x1;
}

// Check if a pointer is marked
inline int is_marked(uintptr_t ptr)
{
    return (int)(ptr & 0x1);
}

// Atomic compare-and-swap with memory barrier
#define ATOMIC_CAS_MB(addr, old_val, new_val) \
    __sync_bool_compare_and_swap((addr), (old_val), (new_val))

// Atomic fetch-and-increment with full memory barrier
#define ATOMIC_FETCH_AND_INC_FULL(addr) \
    __sync_fetch_and_add((addr), 1)

// Memory barrier
#define MEM_BARRIER __sync_synchronize()

// Skip list node structure using void* for key and value
struct sl_node
{
    Guide<void> key;
    Guide<void> val;
    uint32_t toplevel;                // Highest level of the node
    uint32_t deleted;                 // Flag for logical deletion
    struct sl_node *volatile next[1]; // Array of next pointers at each level - MUST BE VOLATILE

    sl_node(void *key = nullptr, void *val = nullptr, int toplevel = 0, int deleted = 0);
    ~sl_node();
};

namespace sl_fraser
{
    class dict
    {
    private:
        struct sl_node *head;
        unsigned int levelmax;
        size_t size_pad_32;

        // Get random level for new nodes
        inline int get_rand_level()
        {
            int level = 1;
            for (int i = 0; i < levelmax - 1; i++)
            {
                if (rand_range(101) < 50)
                {
                    level++;
                }
                else
                {
                    break;
                }
            }
            // 1 <= level <= levelmax
            return level;
        }

        // Core search function that handles concurrent modifications
        void fraser_search(void *key, struct sl_node **left_list,
                           struct sl_node **right_list);

        struct sl_node *allocate_node_unlinked(void *key, size_t key_len, void *val, size_t val_len,
                                               size_t size_pad_32, int toplevel, int padding);

        struct sl_node *allocate_node(void *key, size_t key_len,
                                      void *val, size_t val_len,
                                      struct sl_node *next,
                                      int levelmax, size_t size_pad_32,
                                      int toplevel, int padding);

        void node_delete(struct sl_node *node, int free_key, int free_val);

        // Calculate the size needed for a node with a given number of levels
        inline size_t node_size(int levels)
        {
            return sizeof(struct sl_node) + (levels - 1) * sizeof(struct sl_node *);
        }

        void mark_node_ptrs(struct sl_node *node);

    public:
        // Constructor and destructor
        dict(unsigned int level_max = MAXLEVEL);
        ~dict();

        // Core operations
        int length();
        void *search(void *key, size_t key_len);
        // GET-path helper: copies the value out while still inside the
        // public operation's TAG scope (raw pointers from search() must not
        // escape the operation; migration may free them afterwards).
        int searchCopy(void *key, size_t key_len, std::string &out);
        int insert(void *key, size_t key_len, void *val, size_t val_len);
        void *remove(void *key, size_t key_len);

        // Range scan: copies up to n key/value pairs starting at the first
        // key >= start_key, in key order, inside the operation's TAG scope.
        int scanCopy(void *start_key, size_t key_len, int n,
                     std::vector<std::pair<std::string, std::string>> &out);
    };

} // namespace sl_fraser

#endif // _SKIPLIST_FRASER_H_