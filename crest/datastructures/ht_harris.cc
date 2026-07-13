#include "ht_harris.h"

namespace ht_harris
{
    // HarrisLinkedList private methods
    inline bool dict::HarrisLinkedList::is_marked(void *ptr)
    {
        return (reinterpret_cast<uintptr_t>(ptr) & 0x1);
    }

    inline HarrisNode<void *, void *> *dict::HarrisLinkedList::get_unmarked(void *ptr)
    {
        return reinterpret_cast<HarrisNode<void *, void *> *>(
            reinterpret_cast<uintptr_t>(ptr) & ~0x1);
    }

    inline HarrisNode<void *, void *> *dict::HarrisLinkedList::get_marked(HarrisNode<void *, void *> *ptr)
    {
        return reinterpret_cast<HarrisNode<void *, void *> *>(
            reinterpret_cast<uintptr_t>(ptr) | 0x1);
    }

    inline int dict::HarrisLinkedList::compare_keys(void *key1, void *key2, size_t key_size)
    {
        return memcmp(key1, key2, key_size);
    }

    // HarrisLinkedList constructor and destructor
    dict::HarrisLinkedList::HarrisLinkedList()
    {
        tail = new HarrisNode<void *, void *>();
        tail->key = reinterpret_cast<void *>(UINTPTR_MAX);
        tail->value = nullptr;

        head = new HarrisNode<void *, void *>();
        head->key = reinterpret_cast<void *>(0);
        head->value = nullptr;
        head->next.store(tail, std::memory_order_relaxed);
    }

    dict::HarrisLinkedList::~HarrisLinkedList()
    {
        HarrisNode<void *, void *> *curr = head;
        while (curr)
        {
            HarrisNode<void *, void *> *next = curr->next.load();
            delete curr;
            curr = next;
        }
    }

    // HarrisLinkedList public methods
    bool dict::HarrisLinkedList::insert(void *key, size_t key_size, void *value, size_t value_size)
    {
        HarrisNode<void *, void *> *newNode = nullptr;
        HarrisNode<void *, void *> *pred, *curr;

        while (true)
        {
            // Search for insertion point
            if (search(key, key_size, &pred, &curr))
            {
                // Key exists - handle update without a lock
                void *new_value = jem_malloc(value_size);
                if (!new_value)
                {
                    // Handle allocation failure
                    if (newNode)
                    {
                        newNode->key.destroy();
                        newNode->value.destroy();
                        delete newNode;
                    }
                    return false;
                }

                memcpy(new_value, value, value_size);

                // Atomic update loop
                uintptr_t old_ptr_val, new_ptr_val;
                MemType oldMemType;
                void *old_value_addr;

                do
                {
                    // Load current value atomically
                    old_ptr_val = __atomic_load_n(&curr->value.ptr, __ATOMIC_ACQUIRE);

                    // Extract old address and memory type
                    old_value_addr = reinterpret_cast<void *>(old_ptr_val & ADDRESS_MASK);
                    uint8_t heap_id = (old_ptr_val & HEAP_ID_MASK) >> HEAP_ID_SHIFT;

                    // Determine memory type based on heap ID
                    if (heap_id == 0)
                    {
                        oldMemType = MemType::NEW_HEAP;
                    }
                    else if (heap_id == 1)
                    {
                        oldMemType = MemType::DRAM_2MB_THP;
                    }
                    else if (heap_id == 2)
                    {
                        oldMemType = MemType::DRAM;
                    }
                    else
                    {
                        oldMemType = MemType::NEW_HEAP; // Default
                    }

                    // Create new pointer value with:
                    // 1. New address but preserve metadata
                    // 2. Set Heap ID to 0 (START heap)
                    // 3. Set access bit and clear migration lock
                    new_ptr_val = (old_ptr_val & ~ADDRESS_MASK) | reinterpret_cast<uintptr_t>(new_value);
                    new_ptr_val = (new_ptr_val & ~HEAP_ID_MASK) | (0UL << HEAP_ID_SHIFT);
                    new_ptr_val |= ACCESS_BIT_MASK;
                    new_ptr_val &= ~MIGRATION_LOCK_MASK;

                } while (!__atomic_compare_exchange_n(
                    &curr->value.ptr,
                    &old_ptr_val,
                    new_ptr_val,
                    false, // strong compare-exchange
                    __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE));

                // Successfully updated - free old value
                g_sama->free(old_value_addr, oldMemType);

                // Set the appropriate bit in soda_
                soda_.Set1((((uint64_t)&curr->value - (uint64_t)global_addr_start) / 8));

                // Clean up new node if it was allocated
                if (newNode)
                {
                    newNode->key.destroy();
                    newNode->value.destroy();
                    delete newNode;
                }

                return true;
            }

            // Key doesn't exist - handle insertion (this part remains unchanged)
            if (!newNode)
            {
                newNode = new HarrisNode<void *, void *>();
                newNode->key = jem_malloc(key_size);
                newNode->value = jem_malloc(value_size);
                memcpy(newNode->key, key, key_size);
                memcpy(newNode->value, value, value_size);
            }

            newNode->next.store(curr, std::memory_order_relaxed);

            // Try to link new node
            HarrisNode<void *, void *> *expected = curr;
            if (pred->next.compare_exchange_strong(
                    expected, newNode,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                return true;
            }
        }
    }

    void *dict::HarrisLinkedList::remove(void *key, size_t key_size)
    {
        HarrisNode<void *, void *> *pred, *curr;
        void *value = nullptr;

        do
        {
            if (!search(key, key_size, &pred, &curr))
            {
                return nullptr; // Key not found
            }

            HarrisNode<void *, void *> *succ = curr->next.load(std::memory_order_relaxed);
            HarrisNode<void *, void *> *markedSucc = get_marked(succ);

            // Try to mark node for deletion
            HarrisNode<void *, void *> *expected = succ;
            if (curr->next.compare_exchange_strong(
                    expected, markedSucc,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                value = curr->value;
                break;
            }
        } while (true);

        // Physical removal
        HarrisNode<void *, void *> *unmarked_next =
            get_unmarked(curr->next.load(std::memory_order_relaxed));
        HarrisNode<void *, void *> *expected = curr;
        if (pred->next.compare_exchange_strong(
                expected, unmarked_next,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            curr->key.destroy();
            curr->value.destroy();
            delete curr;
        }

        return value;
    }

    void *dict::HarrisLinkedList::search(void *key, size_t key_size)
    {
        HarrisNode<void *, void *> *curr;
        search(key, key_size, nullptr, &curr);
        if (curr != tail && compare_keys(curr->key, key, key_size) == 0)
        {
            return curr->value;
        }
        return nullptr;
    }

    bool dict::HarrisLinkedList::search(void *key, size_t key_size,
                                        HarrisNode<void *, void *> **pred_ptr,
                                        HarrisNode<void *, void *> **curr_ptr)
    {
        HarrisNode<void *, void *> *pred, *curr, *succ;

    retry:
        pred = head;
        curr = pred->next.load(std::memory_order_acquire);

        while (true)
        {
            succ = curr->next.load(std::memory_order_acquire);

            // Remove marked nodes
            while (is_marked(succ))
            {
                HarrisNode<void *, void *> *unmarked = get_unmarked(succ);
                HarrisNode<void *, void *> *expected = curr;
                if (!pred->next.compare_exchange_strong(
                        expected, unmarked,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    goto retry;
                }
                curr = unmarked;
                succ = curr->next.load(std::memory_order_acquire);
            }

            if (curr == tail)
            {
                if (pred_ptr)
                    *pred_ptr = pred;
                if (curr_ptr)
                    *curr_ptr = curr;
                return false;
            }

            int cmp = compare_keys(curr->key, key, key_size);

            if (cmp >= 0)
            {
                if (pred_ptr)
                    *pred_ptr = pred;
                if (curr_ptr)
                    *curr_ptr = curr;
                return (cmp == 0);
            }

            pred = curr;
            curr = succ;
        }
    }

    // dict class methods
    size_t dict::hash(void *key, size_t key_size)
    {
        unsigned long hash_val = 5381;
        const unsigned char *data = static_cast<const unsigned char *>(key);
        for (size_t i = 0; i < key_size; ++i)
        {
            hash_val = ((hash_val << 5) + hash_val) + data[i];
        }
        return hash_val % bucket_count;
    }

    dict::HarrisLinkedList *dict::get_or_create_bucket(size_t idx)
    {
        dict::HarrisLinkedList *bucket = buckets[idx].load(std::memory_order_acquire);

        if (!bucket)
        {
            dict::HarrisLinkedList *new_bucket = new dict::HarrisLinkedList();
            if (buckets[idx].compare_exchange_strong(
                    bucket,
                    new_bucket,
                    std::memory_order_release,
                    std::memory_order_acquire))
            {
                used_buckets.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                delete new_bucket;
            }
            bucket = buckets[idx].load(std::memory_order_acquire);
        }
        return bucket;
    }

    dict::dict(size_t initial_capacity)
        : bucket_count(initial_capacity),
          size(0),
          used_buckets(0)
    {
        buckets = new std::atomic<dict::HarrisLinkedList *>[bucket_count];
        for (size_t i = 0; i < bucket_count; ++i)
        {
            buckets[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    dict::~dict()
    {
        for (size_t i = 0; i < bucket_count; ++i)
        {
            dict::HarrisLinkedList *bucket = buckets[i].load(std::memory_order_relaxed);
            if (bucket)
            {
                delete bucket;
            }
        }
        delete[] buckets;
    }

    bool dict::insert(void *key, size_t key_size, void *value, size_t value_size)
    {
        size_t idx = hash(key, key_size);
        dict::HarrisLinkedList *bucket = get_or_create_bucket(idx);
        bool result = bucket->insert(key, key_size, value, value_size);
        if (result)
            size.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    void *dict::remove(void *key, size_t key_size)
    {
        size_t idx = hash(key, key_size);
        dict::HarrisLinkedList *bucket = buckets[idx].load(std::memory_order_acquire);
        if (!bucket)
            return nullptr;

        void *result = bucket->remove(key, key_size);
        if (result)
            size.fetch_sub(1, std::memory_order_relaxed);
        return result;
    }

    void *dict::search(void *key, size_t key_size)
    {
        size_t idx = hash(key, key_size);
        dict::HarrisLinkedList *bucket = buckets[idx].load(std::memory_order_acquire);
        if (!bucket)
            return nullptr;
        return bucket->search(key, key_size);
    }

    size_t dict::get_size()
    {
        return size.load(std::memory_order_relaxed);
    }

    size_t dict::get_used_buckets()
    {
        return used_buckets.load(std::memory_order_relaxed);
    }


    /*
     * GET-path helper: copy the value out while still inside the public
     * data-structure operation. search() returns a raw pointer whose object
     * can be migrated (and freed) as soon as the operation's TAG scope ends,
     * so callers must never dereference it after return.
     */
    int dict::searchCopy(void *key, size_t key_size, std::string &out)
    {
        void *v = search(key, key_size);
        if (v == nullptr)
        {
            return 0;
        }
        out.assign(static_cast<const char *>(v));
        return 1;
    }

} // namespace ht_harris