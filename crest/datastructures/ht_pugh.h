/*
Pugh uses fine grained locking per linked list node to allow for concurrent access.
*/

#ifndef _HASHTABLE_PUGH_H_
#define _HASHTABLE_PUGH_H_

#include <stdio.h>
#include <string>
#include <vector>
#include <utility>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>

#include "Guide.hpp"

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

typedef pthread_mutex_t ptlock_t;
#define INIT_LOCK(lock) pthread_mutex_init((pthread_mutex_t *)lock, NULL)
#define DESTROY_LOCK(lock) pthread_mutex_destroy((pthread_mutex_t *)lock)
#define LOCK(lock) pthread_mutex_lock((pthread_mutex_t *)lock)
#define TRYLOCK(lock) pthread_mutex_trylock((pthread_mutex_t *)lock)
#define UNLOCK(lock) pthread_mutex_unlock((pthread_mutex_t *)lock)

#define CACHE_LINE_SIZE 64
#define PUGH_RO_FAIL RO_FAIL
// #define DEFAULT_HASHTABLE_SIZE 65536
#define DEFAULT_HASHTABLE_SIZE 67108864

// Node structure with void* key/value
struct ll_node
{
    Guide<void> key; // Pointer to key
    Guide<void> val; // Pointer to value
    size_t key_size;   // Size of the key for memcmp/memcpy
    size_t val_size;   // Size of the value for memcpy
    struct ll_node *next;
    ptlock_t lock;

    ll_node(void *k = nullptr, void *v = nullptr, size_t ks = 0, size_t vs = 0, ll_node *n = nullptr);
    ~ll_node();
};

class LinkedListPugh
{
private:
    ll_node *head;

    // Helper to create new node with proper memory management
    ll_node *create_node(const void *key, size_t key_size,
                         const void *val, size_t val_size,
                         ll_node *next);

    void delete_node(ll_node *node);

    int compare_keys(const void *key1, size_t size1, const void *key2, size_t size2);

    // Weak right search (no locking)
    ll_node *search_weak_right(const void *key, size_t key_size);

    // Weak left search (no locking)
    ll_node *search_weak_left(const void *key, size_t key_size);

    // Strong search with locking
    ll_node *search_strong(const void *key, size_t key_size,
                           ll_node **right);

    // Strong conditional search
    ll_node *search_strong_cond(const void *key, size_t key_size,
                                ll_node **right, int equal);

public:
    LinkedListPugh();
    ~LinkedListPugh();

    // Search operation
    void *search(const void *key, size_t key_size);

    // Insert operation
    int insert(const void *key, size_t key_size,
               const void *val, size_t val_size);

    // Remove operation
    void *remove(const void *key, size_t key_size);

    // Get list length
    int length();
};

namespace ht_pugh
{
    class dict
    {
    private:
        size_t maxhtlength;
        size_t hash_mask; // Used for bitwise AND operation
        std::atomic<LinkedListPugh *> *buckets;

        // Hash function for void* keys
        size_t hash_function(const void *key, size_t key_size);

    public:
        dict(size_t size = DEFAULT_HASHTABLE_SIZE);
        ~dict();

        void *search(const void *key, size_t key_size);
        // GET-path helper: copies the value out while still inside the
        // public operation's TAG scope (raw pointers from search() must not
        // escape the operation; migration may free them afterwards).
        int searchCopy(const void *key, size_t key_size, std::string &out);

        // Range scan: hash tables are unordered; scans are unsupported.
        int scanCopy(const void *, size_t, int,
                     std::vector<std::pair<std::string, std::string>> &)
        {
            return -1;
        }
        int insert(const void *key, size_t key_size, const void *value, size_t value_size);
        void *remove(const void *key, size_t key_size);
        size_t length();
    };
}

#endif // _HASHTABLE_PUGH_H_