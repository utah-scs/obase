#include "ht_conv.h"
#include <cstring>
#include <jemalloc/jemalloc.h>

namespace ht_conv
{
    static const size_t HT_CONV_BUCKETS = 1 << 20;

    dict::dict() : bucket_count(HT_CONV_BUCKETS), count(0)
    {
        buckets = new Node *[bucket_count]();
    }

    size_t dict::hash(const void *key, size_t key_len) const
    {
        // FNV-1a
        const unsigned char *p = (const unsigned char *)key;
        size_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < key_len; i++)
        {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

    Node *dict::find(size_t idx, const void *key, size_t key_len)
    {
        for (Node *n = buckets[idx]; n != nullptr; n = n->next)
        {
            if (n->key_len == key_len && memcmp(n->key, key, key_len) == 0)
            {
                return n;
            }
        }
        return nullptr;
    }

    int dict::insert(void *key, size_t key_len, void *val, size_t val_len)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        size_t idx = hash(key, key_len) % bucket_count;

        Node *n = find(idx, key, key_len);
        if (n != nullptr)
        {
            // Update: read the old value, publish the replacement, then
            // free the old object.
            void *old = n->val;
            n->val = jem_malloc(val_len);
            memcpy(n->val, val, val_len);
            n->val_len = val_len;
            jem_free(old);
            return 1;
        }

        n = new Node();
        n->key = jem_malloc(key_len);
        memcpy(n->key, key, key_len);
        n->key_len = key_len;
        n->val = jem_malloc(val_len);
        memcpy(n->val, val, val_len);
        n->val_len = val_len;
        n->next = buckets[idx];
        buckets[idx] = n;
        count++;
        return 1;
    }

    void *dict::search(void *key, size_t key_len)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        Node *n = find(hash(key, key_len) % bucket_count, key, key_len);
        if (n == nullptr)
        {
            return nullptr;
        }
        return n->val;
    }

    void *dict::remove(void *key, size_t key_len)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        size_t idx = hash(key, key_len) % bucket_count;
        Node *prev = nullptr;
        for (Node *n = buckets[idx]; n != nullptr; prev = n, n = n->next)
        {
            if (n->key_len == key_len && memcmp(n->key, key, key_len) == 0)
            {
                if (prev != nullptr)
                {
                    prev->next = n->next;
                }
                else
                {
                    buckets[idx] = n->next;
                }
                void *v = n->val;
                jem_free(n->key);
                delete n;
                count--;
                return v;
            }
        }
        return nullptr;
    }

    int dict::searchCopy(void *key, size_t key_len, std::string &out)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        Node *n = find(hash(key, key_len) % bucket_count, key, key_len);
        if (n == nullptr)
        {
            return 0;
        }
        out.assign((const char *)n->val, n->val_len - 1);
        return 1;
    }
}
