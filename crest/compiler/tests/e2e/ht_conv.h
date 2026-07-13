/*
 * Pass 2 end-to-end fixture: a chained hash table written as plain C++
 * with raw pointers. The key/value payloads are annotated with
 * OBASE_GUIDED; guide-converter turns them into guides and the result is
 * built through the full OBASE pipeline and run against the migration
 * integrity harness. This file is the "before" source -- it must never
 * contain Guide types by hand.
 */
#ifndef HT_CONV_H
#define HT_CONV_H

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>
#include <utility>
#include "GuideAnnotations.h"

namespace ht_conv
{
    struct Node
    {
        OBASE_GUIDED void *key;
        OBASE_GUIDED void *val;
        size_t key_len;
        size_t val_len;
        Node *next; // routing metadata: not annotated
    };

    // Coarse-lock chained hash table (same concurrency model as sl_seq).
    class dict
    {
    public:
        dict();
        int insert(void *key, size_t key_len, void *val, size_t val_len);
        void *search(void *key, size_t key_len);
        void *remove(void *key, size_t key_len);
        int searchCopy(void *key, size_t key_len, std::string &out);

        // Range scan: hash tables are unordered; scans are unsupported.
        int scanCopy(const void *, size_t, int,
                     std::vector<std::pair<std::string, std::string>> &)
        {
            return -1;
        }

    private:
        Node **buckets;
        size_t bucket_count;
        size_t count;
        std::mutex op_mutex;

        size_t hash(const void *key, size_t key_len) const;
        Node *find(size_t idx, const void *key, size_t key_len);
    };
}

#endif // HT_CONV_H
