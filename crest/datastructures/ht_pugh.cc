#include "ht_pugh.h"

// Special sentinel values
static const char *MIN_KEY = "";
static const char *MAX_KEY = "~~~"; // Using high ASCII value string

// ll_node implementation
ll_node::ll_node(void *k, void *v, size_t ks, size_t vs, ll_node *n)
    : key(k), val(v), key_size(ks), val_size(vs), next(n) {}

ll_node::~ll_node()
{
    key.destroy();
    val.destroy();
    DESTROY_LOCK(&lock);
}

// LinkedListPugh implementation
ll_node *LinkedListPugh::create_node(const void *key, size_t key_size,
                                     const void *val, size_t val_size,
                                     ll_node *next)
{
    // Allocate and copy key
    void *key_copy = jem_malloc(key_size);
    if (!key_copy)
    {
        perror("jem_malloc @ create_node key");
        exit(1);
    }
    memcpy(key_copy, key, key_size);
    // Allocate and copy value
    void *val_copy = jem_malloc(val_size);
    if (!val_copy)
    {
        free(key_copy);
        perror("jem_malloc @ create_node val");
        exit(1);
    }
    memcpy(val_copy, val, val_size);

    ll_node *new_node = new ll_node(key_copy, val_copy, key_size, val_size, next);
    if (!new_node)
    {
        perror("new @ create_node");
        exit(1);
    }

    // Initialize lock after node is fully constructed
    INIT_LOCK(&new_node->lock);

    return new_node;
}

void LinkedListPugh::delete_node(ll_node *node)
{
    if (node)
    {
        delete node; // Will call destructor which handles cleanup
    }
}

int LinkedListPugh::compare_keys(const void *key1, size_t size1, const void *key2, size_t size2)
{
    // Handle sentinel nodes by string comparison
    const char *str1 = static_cast<const char *>(key1);
    const char *str2 = static_cast<const char *>(key2);
    // Sentinel node checks
    if (strcmp(str1, MIN_KEY) == 0)
        return -1;
    if (strcmp(str2, MIN_KEY) == 0)
        return 1;
    if (strcmp(str1, MAX_KEY) == 0)
        return 1;
    if (strcmp(str2, MAX_KEY) == 0)
        return -1;

    // Regular key comparison
    int cmp = strcmp(str1, str2);
    if (cmp != 0)
        return cmp;

    // If strings are equal by strcmp, use size as tiebreaker
    // Note: size includes null terminator
    if (size1 < size2)
        return -1;
    if (size1 > size2)
        return 1;
    return 0;
}

ll_node *LinkedListPugh::search_weak_right(const void *key, size_t key_size)
{
    ll_node *succ = head->next;
    while (compare_keys(succ->key, succ->key_size, key, key_size) < 0)
    {
        succ = succ->next;
    }
    return succ;
}

ll_node *LinkedListPugh::search_weak_left(const void *key, size_t key_size)
{
    ll_node *pred = head;
    ll_node *succ = pred->next;

    while (compare_keys(succ->key, succ->key_size, key, key_size) < 0)
    {
        pred = succ;
        succ = succ->next;
    }
    return pred;
}

ll_node *LinkedListPugh::search_strong(const void *key, size_t key_size,
                                       ll_node **right)
{
    ll_node *pred = search_weak_left(key, key_size);
    if (!pred)
        return NULL; // Safety check
    LOCK(&pred->lock);
    ll_node *succ = pred->next;

    while (unlikely(compare_keys(succ->key, succ->key_size, key, key_size) < 0))
    {
        UNLOCK(&pred->lock);
        pred = succ;
        LOCK(&pred->lock);
        succ = pred->next;
    }
    *right = succ;
    return pred;
}

ll_node *LinkedListPugh::search_strong_cond(const void *key, size_t key_size,
                                            ll_node **right, int equal)
{
    ll_node *pred = search_weak_left(key, key_size);
    ll_node *succ = pred->next;

    if ((compare_keys(succ->key, succ->key_size, key, key_size) == 0) == equal)
    {
        return NULL;
    }

    LOCK(&pred->lock);
    succ = pred->next;

    while (unlikely(compare_keys(succ->key, succ->key_size, key, key_size) < 0))
    {
        UNLOCK(&pred->lock);
        pred = succ;
        LOCK(&pred->lock);
        succ = pred->next;
    }

    *right = succ;
    return pred;
}

LinkedListPugh::LinkedListPugh()
{
    // Create sentinel nodes with min/max values
    ll_node *max = create_node(MAX_KEY, strlen(MAX_KEY) + 1,
                               "", 1, NULL);
    head = create_node(MIN_KEY, strlen(MIN_KEY) + 1,
                       "", 1, max);
}

LinkedListPugh::~LinkedListPugh()
{
    ll_node *curr = head;
    while (curr)
    {
        ll_node *next = curr->next;
        delete_node(curr);
        curr = next;
    }
}

void *LinkedListPugh::search(const void *key, size_t key_size)
{
    ll_node *right = search_weak_right(key, key_size);
    if (compare_keys(right->key, right->key_size, key, key_size) == 0)
    {
        return right->val;
    }
    return NULL;
}

int LinkedListPugh::insert(const void *key, size_t key_size,
                           const void *val, size_t val_size)
{
    int result = 1;
    ll_node *right;

#if PUGH_RO_FAIL == 1
    ll_node *left = search_strong_cond(key, key_size, &right, 1);
    if (!left)
    {
        return 0;
    }
#else
    ll_node *left = search_strong(key, key_size, &right);
#endif

    if (compare_keys(right->key, right->key_size, key, key_size) == 0)
    {
        // Key exists - update the value
        LOCK(&right->lock);

        // Swap in the new value, then free the old one. Never destroy()
        // a live slot before reassigning: the transient SODA Set0 makes
        // concurrent readers skip their ATC decrement (permanent leak).
        void *old_val = static_cast<void *>(right->val); // pins via ATC
        MemType old_type = right->val.getMemType();
        right->val = jem_malloc(val_size);
        right->val.setHeapId(0);
        if (!right->val)
        {
            perror("jem_malloc @ insert update");
            exit(1);
        }
        memcpy(right->val, val, val_size);
        right->val_size = val_size;
        g_sama->free(old_val, old_type);

        UNLOCK(&right->lock);
        result = 1; // Indicate update was successful
    }
    else
    {
        // New key - create new node
        ll_node *n = create_node(key, key_size,
                                 val, val_size, right);
        left->next = n;
    }

    UNLOCK(&left->lock);
    return result;
}

void *LinkedListPugh::remove(const void *key, size_t key_size)
{
    void *result = NULL;
    ll_node *right;

#if PUGH_RO_FAIL == 1
    ll_node *left = search_strong_cond(key, key_size, &right, 0);
    if (!left)
    {
        return NULL;
    }
#else
    ll_node *left = search_strong(key, key_size, &right);
#endif

    if (compare_keys(right->key, right->key_size, key, key_size) == 0)
    {
        LOCK(&right->lock);
        result = jem_malloc(right->val_size);
        if (!result)
        {
            perror("jem_malloc @ remove");
            exit(1);
        }
        memcpy(result, right->val, right->val_size);

        left->next = right->next;
        right->next = left; // For helping garbage collection

        UNLOCK(&right->lock);
        delete_node(right);
    }

    UNLOCK(&left->lock);
    return result;
}

int LinkedListPugh::length()
{
    int count = 0;
    ll_node *tmp = head->next;
    while (tmp->next)
    {
        count++;
        tmp = tmp->next;
    }
    return count;
}

// ht_pugh::dict implementation
namespace ht_pugh
{
    size_t dict::hash_function(const void *key, size_t key_size)
    {
        // Basic but effective hash function for binary data
        const unsigned char *bytes = static_cast<const unsigned char *>(key);
        size_t hash = 5381; // djb2 hash initial value

        for (size_t i = 0; i < key_size; i++)
        {
            hash = ((hash << 5) + hash) + bytes[i]; // hash * 33 + c
        }

        return hash & hash_mask;
    }

    dict::dict(size_t size)
    {
        // Ensure size is power of 2 for efficient modulo operation
        maxhtlength = 1;
        while (maxhtlength < size)
        {
            maxhtlength <<= 1;
        }
        hash_mask = maxhtlength - 1;

        // Initialize buckets
        buckets = new std::atomic<LinkedListPugh *>[maxhtlength];
        for (size_t i = 0; i < maxhtlength; i++)
        {
            buckets[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    dict::~dict()
    {
        for (size_t i = 0; i < maxhtlength; i++)
        {
            LinkedListPugh *bucket = buckets[i].load(std::memory_order_relaxed);
            if (bucket)
            {
                delete bucket;
            }
        }
        delete[] buckets;
    }

    void *dict::search(const void *key, size_t key_size)
    {
        size_t bucket_idx = hash_function(key, key_size);
        LinkedListPugh *bucket = buckets[bucket_idx].load(std::memory_order_acquire);
        if (!bucket)
        {
            return nullptr;
        }
        return bucket->search(key, key_size);
    }

    int dict::insert(const void *key, size_t key_size, const void *value, size_t value_size)
    {
        size_t bucket_idx = hash_function(key, key_size);
        std::atomic<LinkedListPugh *> &bucket_ptr = buckets[bucket_idx];
        LinkedListPugh *bucket = bucket_ptr.load(std::memory_order_acquire);

        if (!bucket)
        {
            LinkedListPugh *new_bucket = new LinkedListPugh();
            LinkedListPugh *expected = nullptr;
            if (bucket_ptr.compare_exchange_strong(expected, new_bucket,
                                                   std::memory_order_release,
                                                   std::memory_order_acquire))
            {
                bucket = new_bucket;
            }
            else
            {
                delete new_bucket;
                bucket = bucket_ptr.load(std::memory_order_acquire);
            }
        }

        return bucket->insert(key, key_size, value, value_size);
    }

    void *dict::remove(const void *key, size_t key_size)
    {
        size_t bucket_idx = hash_function(key, key_size);
        std::atomic<LinkedListPugh *> &bucket_ptr = buckets[bucket_idx];
        LinkedListPugh *bucket = bucket_ptr.load(std::memory_order_acquire);

        if (!bucket)
        {
            return nullptr;
        }

        return bucket->remove(key, key_size);
    }

    size_t dict::length()
    {
        size_t count = 0;
        for (size_t i = 0; i < maxhtlength; i++)
        {
            LinkedListPugh *bucket = buckets[i].load(std::memory_order_relaxed);
            if (bucket)
            {
                count += bucket->length();
            }
        }
        return count;
    }

    /*
     * GET-path helper: copy the value out while still inside the public
     * data-structure operation. search() returns a raw pointer whose object
     * can be migrated (and freed) as soon as the operation's TAG scope ends,
     * so callers must never dereference it after return.
     */
    int dict::searchCopy(const void *key, size_t key_size, std::string &out)
    {
        void *v = search(key, key_size);
        if (v == nullptr)
        {
            return 0;
        }
        out.assign(static_cast<const char *>(v));
        return 1;
    }

}