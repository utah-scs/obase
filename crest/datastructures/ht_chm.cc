#include "ht_chm.h"

namespace ht_chm
{
    size_t dict::segment_index(const void *key_data, size_t key_size)
    {
        return hash_fn(key_data, key_size) & segment_mask;
    }

    HashSegment *dict::create_segment(size_t capacity)
    {
        HashSegment *seg = (HashSegment *)aligned_alloc(CACHE_LINE_SIZE, sizeof(HashSegment));
        INIT_LOCK(&seg->lock);
        seg->capacity = capacity;
        seg->size = 0;
        seg->threshold = capacity * load_factor;
        seg->table = (HashNode **)jem_calloc(capacity, sizeof(HashNode *));
        return seg;
    }

    void dict::rehash_segment(size_t seg_idx)
    {
        HashSegment *seg = segments[seg_idx];
        // Lock is already held by the insert function

        size_t new_cap = seg->capacity << 1;
        HashNode **new_table = (HashNode **)jem_calloc(new_cap, sizeof(HashNode *));

        // Transfer nodes to the new table
        for (size_t i = 0; i < seg->capacity; i++)
        {
            HashNode *node = seg->table[i];
            while (node)
            {
                HashNode *next = node->next;
                size_t new_idx = hash_fn(node->key, node->key_size) & (new_cap - 1);
                node->next = new_table[new_idx];
                new_table[new_idx] = node;
                node = next;
            }
        }

        // Replace the old table and update metadata
        jem_free(seg->table);
        seg->table = new_table;
        seg->capacity = new_cap;
        seg->threshold = new_cap * load_factor;
    }

    dict::dict(
        size_t (*hash_function)(const void *, size_t),
        int (*compare_function)(const void *, size_t, const void *, size_t),
        size_t initial_cap,
        float lf) : hash_fn(hash_function), key_compare(compare_function), load_factor(lf)
    {
        num_segments = 1;
        while (num_segments < DEFAULT_NUM_SEGMENTS)
            num_segments <<= 1;
        segment_mask = num_segments - 1;

        segments = (HashSegment **)jem_malloc(num_segments * sizeof(HashSegment *));
        size_t seg_cap = (initial_cap + num_segments - 1) / num_segments;
        seg_cap = 1 << (int)ceil(log2(seg_cap));

        for (size_t i = 0; i < num_segments; i++)
        {
            segments[i] = create_segment(seg_cap);
        }
    }

    dict::~dict()
    {
        for (size_t i = 0; i < num_segments; i++)
        {
            HashSegment *seg = segments[i];
            for (size_t b = 0; b < seg->capacity; b++)
            {
                HashNode *node = seg->table[b];
                while (node)
                {
                    HashNode *next = node->next;
#if CREST_RAW_POINTERS
                    jem_free(node->key);
                    jem_free(node->value);
#else
                    node->key.destroy();
                    node->value.destroy();
#endif
                    jem_free(node);
                    node = next;
                }
            }
            DESTROY_LOCK(&seg->lock);
            jem_free(seg->table);
            jem_free(seg);
        }
        jem_free(segments);
    }

    size_t dict::string_hash(const void *key_data, size_t key_size)
    {
        // FNV-1a hash
        const unsigned char *data = static_cast<const unsigned char *>(key_data);
        size_t hash = 14695981039346656037ULL; // FNV offset basis
        for (size_t i = 0; i < key_size; ++i)
        {
            hash ^= data[i];
            hash *= 1099511628211ULL; // FNV prime
        }
        return hash;
    }

    int dict::string_compare(const void *key1_data, size_t key1_size,
                             const void *key2_data, size_t key2_size)
    {
        if (key1_size != key2_size)
            return -1;
        return memcmp(key1_data, key2_data, key1_size);
    }

    int dict::insert(const void *key_data, size_t key_size,
                     const void *value_data, size_t value_size)
    {
        size_t seg_idx = segment_index(key_data, key_size);
        HashSegment *seg = segments[seg_idx];
        LOCK(&seg->lock);

        size_t bucket_idx = hash_fn(key_data, key_size) & (seg->capacity - 1);
        HashNode **bucket = &seg->table[bucket_idx];
        HashNode *curr = *bucket;

        while (curr)
        {
            if (key_compare(curr->key, curr->key_size, key_data, key_size) == 0)
            {
                // Update existing value: swap then free (never destroy()
                // a live slot first -- transient SODA Set0 leaks ATC)
                void *old_val = static_cast<void *>(curr->value); // pins via ATC
#if !CREST_RAW_POINTERS
                MemType old_type = curr->value.getMemType();
#endif
                curr->value = jem_malloc(value_size);
#if !CREST_RAW_POINTERS
                curr->value.setHeapId(0); // set heap id to START heap
#endif
                curr->value_size = value_size;
                memcpy(curr->value, value_data, value_size);
#if CREST_RAW_POINTERS
                jem_free(old_val);
#else
                g_sama->free(old_val, old_type);
#endif
                UNLOCK(&seg->lock);
                return 1;
            }
            curr = curr->next;
        }

        // Insert new node
        // HashNode *new_node = (HashNode *)aligned_alloc(CACHE_LINE_SIZE, sizeof(HashNode));
        HashNode *new_node = (HashNode *)jem_calloc(1, sizeof(HashNode));
        new_node->key = jem_malloc(key_size);
        new_node->key_size = key_size;
        memcpy(new_node->key, key_data, key_size);
        new_node->value = jem_malloc(value_size);
        new_node->value_size = value_size;
        memcpy(new_node->value, value_data, value_size);
        new_node->next = *bucket;
        *bucket = new_node;

        if (++seg->size >= seg->threshold)
        {
            rehash_segment(seg_idx);
            UNLOCK(&seg->lock);
        }
        else
        {
            UNLOCK(&seg->lock);
        }

        // inerted key
        return 1;
    }

    void *dict::remove(const void *key_data, size_t key_size)
    {
        size_t seg_idx = segment_index(key_data, key_size);
        HashSegment *seg = segments[seg_idx];
        LOCK(&seg->lock);

        size_t bucket_idx = hash_fn(key_data, key_size) & (seg->capacity - 1);
        HashNode **bucket = &seg->table[bucket_idx];
        HashNode *curr = *bucket, *prev = nullptr;

        while (curr)
        {
            if (key_compare(curr->key, curr->key_size, key_data, key_size) == 0)
            {
                if (prev)
                    prev->next = curr->next;
                else
                    *bucket = curr->next;

                void *value_copy = jem_malloc(curr->value_size);
                memcpy(value_copy, curr->value, curr->value_size);

#if CREST_RAW_POINTERS
                jem_free(curr->key);
                jem_free(curr->value);
#else
                curr->key.destroy();
                curr->value.destroy();
#endif
                jem_free(curr);
                seg->size--;

                UNLOCK(&seg->lock);
                return value_copy;
            }
            prev = curr;
            curr = curr->next;
        }

        UNLOCK(&seg->lock);
        return nullptr;
    }

    void *dict::search(const void *key_data, size_t key_size)
    {
        size_t seg_idx = segment_index(key_data, key_size);
        HashSegment *seg = segments[seg_idx];
        LOCK(&seg->lock);

        size_t bucket_idx = hash_fn(key_data, key_size) & (seg->capacity - 1);
        HashNode *curr = seg->table[bucket_idx];

        while (curr)
        {
            if (key_compare(curr->key, curr->key_size, key_data, key_size) == 0)
            {
                // void *value_copy = jem_malloc(curr->value_size);
                // memcpy(value_copy, curr->value, curr->value_size);
                // UNLOCK(&seg->lock);

                // don't need to copy value, just return reference
                UNLOCK(&seg->lock);
                return curr->value;
            }
            curr = curr->next;
        }

        UNLOCK(&seg->lock);
        return nullptr;
    }

    /*
     * GET-path helper: copy the value out while still inside the public
     * data-structure operation. search() returns a raw pointer whose object
     * can be migrated (and freed) as soon as the operation's TAG scope ends,
     * so callers must never dereference it after return.
     */
    int dict::searchCopy(const void *key_data, size_t key_size, std::string &out)
    {
        void *v = search(key_data, key_size);
        if (v == nullptr)
        {
            return 0;
        }
        out.assign(static_cast<const char *>(v));
        return 1;
    }

}
