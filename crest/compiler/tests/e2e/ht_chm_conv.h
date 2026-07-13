/*
 * Pass 2 fixture: ht_chm (segmented concurrent hash table, lock striping)
 * de-converted to plain C++. Identical to datastructures/ht_chm.h except:
 * the guide fields are raw annotated pointers, guide-API calls in the .cc
 * are their plain equivalents (read/assign/jem_free), and the global-scope
 * types live inside the namespace so the fixture links alongside the real
 * structure. guide-converter regenerates the guided form; the e2e harness
 * proves the result migration-correct.
 *
 * [HashSegment]                 [HashNode]                [Key Data]      [Value Data]
 * +-------------+               +------------+            +----------+    +------------+
 * | lock        |               | key        | ---------->| testKeyyy|    | testValueee|
 * | capacity=16 |               | value      | ---------->| (9 bytes)|    | (11 bytes) |
 * | table       | ----> [0] --> | next=null  |            +----------+    +------------+
 * +-------------+       [1]     +------------+
 *              ...      [15]
 *
 * Segmented concurrent hashtable: the table is divided into segments
 * (default 128), each with its own lock, so threads on different segments
 * never contend. A segment doubles its capacity when size >= threshold.
 */

#ifndef CONCURRENT_HASHMAP_CONV_H
#define CONCURRENT_HASHMAP_CONV_H

#include <stdlib.h>
#include <string>
#include <vector>
#include <utility>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>

#include "GuideAnnotations.h"

#define INIT_LOCK(lock) pthread_mutex_init(lock, NULL)
#define DESTROY_LOCK(lock) pthread_mutex_destroy(lock)
#define LOCK(lock) pthread_mutex_lock(lock)
#define UNLOCK(lock) pthread_mutex_unlock(lock)

namespace ht_chm_conv
{
    constexpr size_t CACHE_LINE_SIZE = 64;
    constexpr float DEFAULT_LOAD_FACTOR = 0.75f;
    constexpr size_t DEFAULT_NUM_SEGMENTS = 128;

    struct alignas(CACHE_LINE_SIZE) HashNode
    {
        OBASE_GUIDED void *key;
        size_t key_size;
        OBASE_GUIDED void *value;
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
        // operation (raw pointers from search() must not escape it).
        int searchCopy(const void *key_data, size_t key_size, std::string &out);

        // Range scan: hash tables are unordered; scans are unsupported.
        int scanCopy(const void *, size_t, int,
                     std::vector<std::pair<std::string, std::string>> &)
        {
            return -1;
        }
    };
}

#endif // CONCURRENT_HASHMAP_CONV_H
