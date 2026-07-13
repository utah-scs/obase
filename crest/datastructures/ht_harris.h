/*
 * Concurrent Harris Hashtable
 * ---------------------------
 * A concurrent hash table implementation using Harris linked lists.
 *
 * This implementation is based on the paper "A Pragmatic Implementation of
 * Non-blocking Linked-Lists" by Timothy L. Harris.
 *
 * The hash table uses a fixed number of buckets, each containing a Harris
 * linked list. Each linked list node contains a key, a value, and a pointer to
 * the next node. The linked list is sorted by key, and supports insert, remove,
 * and search operations.
 *
 * Similar to redis the buckets are lazy initialized.
 *
 * Lock-free algorithm
 *
 */

#ifndef CONCURRENT_HARRIS_HASHTABLE_H
#define CONCURRENT_HARRIS_HASHTABLE_H

#include <cstdlib>
#include <string>
#include <vector>
#include <utility>
#include <cstring>
#include <limits>
#include <atomic>
#include <memory>

#include "Guide.hpp"

#define CACHE_LINE_SIZE 64
#define ALIGNED(N) __attribute__((aligned(N)))
#define DICT_HT_INITIAL_SIZE 67108864 // 67,108,864 buckets (2^26)

// Harris linked list node structure
template <typename K, typename V>
struct ALIGNED(CACHE_LINE_SIZE) HarrisNode
{
    Guide<void> key;
    Guide<void> value;
    std::atomic<HarrisNode<K, V> *> next;

    // HarrisNode(K k = nullptr, V v = nullptr, HarrisNode<K, V> *n = nullptr)
};

namespace ht_harris
{
    class dict
    {
    private:
        // Harris linked list implementation
        class HarrisLinkedList
        {
        private:
            HarrisNode<void *, void *> *head;
            HarrisNode<void *, void *> *tail;

            inline bool is_marked(void *ptr);
            inline HarrisNode<void *, void *> *get_unmarked(void *ptr);
            inline HarrisNode<void *, void *> *get_marked(HarrisNode<void *, void *> *ptr);
            inline int compare_keys(void *key1, void *key2, size_t key_size);
            bool search(void *key, size_t key_size,
                        HarrisNode<void *, void *> **pred_ptr,
                        HarrisNode<void *, void *> **curr_ptr);

        public:
            HarrisLinkedList();
            ~HarrisLinkedList();
            bool insert(void *key, size_t key_size, void *value, size_t value_size);
            void *remove(void *key, size_t key_size);
            void *search(void *key, size_t key_size);
        };

        // Hash table components
        std::atomic<HarrisLinkedList *> *buckets;
        size_t bucket_count;
        std::atomic<size_t> size;
        std::atomic<size_t> used_buckets;

        // Helper functions
        size_t hash(void *key, size_t key_size);
        HarrisLinkedList *get_or_create_bucket(size_t idx);

    public:
        dict(size_t initial_capacity = DICT_HT_INITIAL_SIZE);
        ~dict();
        bool insert(void *key, size_t key_size, void *value, size_t value_size);
        void *remove(void *key, size_t key_size);
        void *search(void *key, size_t key_size);
        // GET-path helper: copies the value out while still inside the
        // public operation's TAG scope (raw pointers from search() must not
        // escape the operation; migration may free them afterwards).
        int searchCopy(void *key, size_t key_size, std::string &out);

        // Range scan: hash tables are unordered; scans are unsupported.
        int scanCopy(const void *, size_t, int,
                     std::vector<std::pair<std::string, std::string>> &)
        {
            return -1;
        }
        size_t get_size();
        size_t get_used_buckets();
    };
}

#endif // CONCURRENT_HARRIS_HASHTABLE_H