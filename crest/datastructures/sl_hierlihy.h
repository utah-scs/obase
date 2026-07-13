#ifndef _SKIPLIST_H_
#define _SKIPLIST_H_

#include <stdlib.h>
#include <string>
#include <vector>
#include <utility>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include "Guide.hpp"

// Lock definitions
#define LOCK(lock) pthread_mutex_lock((pthread_mutex_t *)(lock))
#define UNLOCK(lock) pthread_mutex_unlock((pthread_mutex_t *)(lock))
#define INIT_LOCK(lock) pthread_mutex_init((pthread_mutex_t *)(lock), NULL)
#define DESTROY_LOCK(lock) pthread_mutex_destroy((pthread_mutex_t *)(lock))

// Global lock macros
#define GL_LOCK(lock) pthread_mutex_lock(lock)
#define GL_UNLOCK(lock) pthread_mutex_unlock(lock)
#define GL_INIT_LOCK(lock) pthread_mutex_init(lock, NULL)

// Memory barrier
#define MEM_BARRIER __sync_synchronize()

// unlikely macro (for performance optimization)
#define unlikely(x) __builtin_expect(!!(x), 0)

// Pause for backoff
#define PAUSE __asm__ __volatile__("pause")

// Constants
#define MAX_BACKOFF 131071
// #define HERLIHY_MAX_MAX_LEVEL 64
#define HERLIHY_MAX_MAX_LEVEL 32

// Skip list node structure
struct sl_marked
{
    Guide<void> key;
    Guide<void> val;
    uint32_t toplevel;
    volatile uint32_t marked;
    volatile uint32_t fullylinked;
    volatile uint32_t padding;
    pthread_mutex_t lock;
    struct sl_marked *volatile next[1];

    sl_marked(void *key = nullptr, void *val = nullptr, int toplevel = 0);
    ~sl_marked();
};

// Lock getter macro
#define ND_GET_LOCK(node) (&(node)->lock)

namespace sl_hierlihy
{
    class dict
    {
    public:
        /**
         * @brief Construct a new skiplist dictionary
         *
         * @param level_max Maximum level for the skiplist (default is HERLIHY_MAX_MAX_LEVEL)
         */
        dict(unsigned int level_max = HERLIHY_MAX_MAX_LEVEL);

        /**
         * @brief Destroy the skiplist dictionary and free all allocated memory
         */
        ~dict();

        /**
         * @brief Search for a key in the skiplist
         *
         * @param key Pointer to the key to search for
         * @param key_len Length of the key in bytes
         * @return void* Pointer to the value if found, NULL otherwise
         */
        void *search(void *key, size_t key_len);
        // GET-path helper: copies the value out while still inside the
        // public operation's TAG scope (raw pointers from search() must not
        // escape the operation; migration may free them afterwards).
        int searchCopy(void *key, size_t key_len, std::string &out);

        /**
         * @brief Insert a key-value pair into the skiplist
         *
         * @param key Pointer to the key
         * @param key_len Length of the key in bytes
         * @param val Pointer to the value
         * @param val_len Length of the value in bytes
         * @return int 1 if inserted, 0 if updated existing key
         */
        int insert(void *key, size_t key_len, void *val, size_t val_len);

        /**
         * @brief Remove a key-value pair from the skiplist
         *
         * @param key Pointer to the key to remove
         * @param key_len Length of the key in bytes
         * @return void* Pointer to the removed value if found, NULL otherwise
         */
        void *remove(void *key, size_t key_len);

        // Range scan: copies up to n key/value pairs starting at the first
        // key >= start_key, in key order, inside the operation's TAG scope.
        int scanCopy(void *start_key, size_t key_len, int n,
                     std::vector<std::pair<std::string, std::string>> &out);

        /**
         * @brief Get the number of elements in the skiplist
         *
         * @return int Number of elements
         */
        int length();

    private:
        sl_marked *head;          // Head of the skiplist
        pthread_mutex_t *lock;    // Global lock for the skiplist
        unsigned int levelmax;    // Maximum level of the skiplist
        unsigned int size_pad_32; // Size of node with padding to 32-byte boundary

        /**
         * @brief Get a random level for a new node
         *
         * @return int Random level between 1 and levelmax
         */
        int get_rand_level();

        /**
         * @brief Check if a node can be deleted
         *
         * @param node Node to check
         * @param found Level where the node was found
         * @return int 1 if node can be deleted, 0 otherwise
         */
        int ok_to_delete(sl_marked *node, int found);

        /**
         * @brief Search for a key in the skiplist, filling arrays of predecessors and successors
         *
         * @param key Key to search for
         * @param preds Array to fill with predecessor nodes
         * @param succs Array to fill with successor nodes
         * @param fast Whether to stop early when the key is found
         * @return int Level where the key was found, or -1 if not found
         */
        int optimistic_search(void *key, sl_marked **preds,
                              sl_marked **succs, int fast);

        /**
         * @brief Search for a key in the skiplist, returning the node if found
         *
         * @param key Key to search for
         * @return volatile sl_marked* Node containing the key, or NULL if not found
         */
        sl_marked *optimistic_left_search(void *key);

        /**
         * @brief Unlock all locked levels
         *
         * @param nodes Array of nodes to unlock
         * @param highestlevel Highest level to unlock
         */
        void unlock_levels(sl_marked **nodes, int highestlevel);

        // Function declarations for node operations
        sl_marked *allocate_sl_marked_unlinked(void *key, size_t key_len, void *val, size_t val_len,
                                               unsigned int size_pad_32, int toplevel, int transactional);

        sl_marked *allocate_sl_marked(void *key, size_t key_len, void *val, size_t val_len,
                                      sl_marked *next, int levelmax,
                                      unsigned int size_pad_32, int toplevel, int transactional);

        void delete_sl_marked(sl_marked *node);

        // Utility function declarations
        int rand_range(int n);
        void nop_rep(int n);
    };
}

#endif // _SKIPLIST_H_