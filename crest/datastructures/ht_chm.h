/*
[HashSegment]                 [HashNode]                [Key Data]      [Value Data]
+-------------+               +------------+            +----------+    +------------+
| lock        |               | key        | ---------->| testKeyyy|    | testValueee|
| capacity=16 |               | value      | ---------->| (9 bytes)|    | (11 bytes) |
| table       | ----> [0] --> | next=null  |            +----------+    +------------+
+-------------+       [1]     +------------+
             ...      [15]

* This is a segmented concurrent hashtable implementation
* The hashtable is divided into multiple segments (default 128 segments) to reduce contention
* Each segment has its own lock and operates independently, meaning 128 threads can access the hashtable concurrently
* When a segment's size >= threshold, a new segment with double the capacity is created.
* Nodes from the old segment are redistributed to the new segment's buckets using the updated capacity for index calculation.
* The old segment is replaced atomically, and its memory is jem_freed after the switch.
* The design uses lock striping pattern for better concurrency
*/

#ifndef CONCURRENT_HASHMAP_H
#define CONCURRENT_HASHMAP_H

#include <stdlib.h>
#include <string>
#include <vector>
#include <utility>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>

#include "Guide.hpp"

constexpr size_t CACHE_LINE_SIZE = 64;
constexpr float DEFAULT_LOAD_FACTOR = 0.75f;
constexpr size_t DEFAULT_NUM_SEGMENTS = 128;

struct HashNode;
struct HashSegment;

#define INIT_LOCK(lock) pthread_mutex_init(lock, NULL)
#define DESTROY_LOCK(lock) pthread_mutex_destroy(lock)
#define LOCK(lock) pthread_mutex_lock(lock)
#define UNLOCK(lock) pthread_mutex_unlock(lock)

struct alignas(CACHE_LINE_SIZE) HashNode
{
    Guide<void> key;
    size_t key_size;
    Guide<void> value;
    size_t value_size;
    HashNode *next;
};

struct alignas(CACHE_LINE_SIZE) HashSegment
{
    pthread_mutex_t lock;
    size_t capacity;
    size_t size;
    size_t threshold;
    HashNode **table;
};

namespace ht_chm
{
    class dict
    {
    private:
        size_t num_segments;
        size_t segment_mask;
        float load_factor;
        HashSegment **segments;

        size_t (*hash_fn)(const void *, size_t);
        int (*key_compare)(const void *, size_t, const void *, size_t);

        size_t segment_index(const void *key_data, size_t key_size);
        HashSegment *create_segment(size_t capacity);
        void rehash_segment(size_t seg_idx);

    public:
        dict(
            size_t (*hash_function)(const void *, size_t) = dict::string_hash,
            int (*compare_function)(const void *, size_t, const void *, size_t) = dict::string_compare,
            // size_t initial_cap = 4096,
            // size_t initial_cap = 1048576,
            size_t initial_cap = 67108864,
            float lf = DEFAULT_LOAD_FACTOR);

        ~dict();

        static size_t string_hash(const void *key_data, size_t key_size);
        static int string_compare(const void *key1_data, size_t key1_size,
                                  const void *key2_data, size_t key2_size);

        int insert(const void *key_data, size_t key_size,
                   const void *value_data, size_t value_size);

        void *remove(const void *key_data, size_t key_size);
        void *search(const void *key_data, size_t key_size);
        // GET-path helper: copies the value out while still inside the
        // public operation's TAG scope (raw pointers from search() must not
        // escape the operation; migration may free them afterwards).
        int searchCopy(const void *key_data, size_t key_size, std::string &out);

        // Range scan: hash tables are unordered; scans are unsupported.
        int scanCopy(const void *, size_t, int,
                     std::vector<std::pair<std::string, std::string>> &)
        {
            return -1;
        }
    };
}

#endif // CONCURRENT_HASHMAP_H