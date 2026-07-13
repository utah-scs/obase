#include "sl_fraser.h"

sl_node::sl_node(void *key, void *val, int toplevel, int deleted)
    : key(key), val(val), toplevel(toplevel), deleted(deleted) {}

namespace sl_fraser
{

    // Allocate a new node without linking it
    struct sl_node *dict::allocate_node_unlinked(void *key, size_t key_len,
                                                 void *val, size_t val_len,
                                                 size_t size_pad_32, int toplevel, int padding)
    {
        void *mem = nullptr;

        if (padding)
        {
            mem = jem_calloc(1, size_pad_32);
        }
        else
        {
            mem = jem_calloc(1, node_size(toplevel));
        }

        if (NULL == mem)
        {
            perror("jem_malloc @ allocate_node_unlinked");
            exit(1);
        }

        void *key_ptr = jem_malloc(key_len);
        void *val_ptr = jem_malloc(val_len);

        memcpy(key_ptr, key, key_len);
        memcpy(val_ptr, val, val_len);


        sl_node *node = new (mem) sl_node(key_ptr, val_ptr, toplevel);
        if (NULL == node)
        {
            perror("jem_malloc @ allocate_node_unlinked");
            exit(1);
        }

        if (!node->key || !node->val)
        {
            jem_free(node->key);
            jem_free(node->val);
            spdlog::error("jem_malloc @ insert");
            return 0; // Memory allocation failed
        }

        // Set node properties
        node->toplevel = toplevel;

        // Initialize next pointers to NULL
        for (int i = 0; i < toplevel; i++)
        {
            node->next[i] = NULL;
        }
        node->deleted = 0;

        return node;
    }

    // Allocate and initialize a node with next pointers
    struct sl_node *dict::allocate_node(void *key, size_t key_len,
                                        void *val, size_t val_len,
                                        struct sl_node *next,
                                        int levelmax, size_t size_pad_32,
                                        int toplevel, int padding)
    {
        struct sl_node *node = allocate_node_unlinked(key, key_len, val, val_len,
                                                      size_pad_32, toplevel, padding);

        for (int i = 0; i < toplevel; i++)
        {
            node->next[i] = next;
        }

        return node;
    }

    // Delete a node and its key/value if needed
    void dict::node_delete(struct sl_node *node, int free_key, int free_val)
    {
        if (free_key && node->key)
            node->key.destroy();
        if (free_val && node->val)
            node->val.destroy();
        jem_free((void *)node);
    }

    // Mark all next pointers in a node as logically deleted
    void dict::mark_node_ptrs(struct sl_node *node)
    {
        struct sl_node *node_next;

        for (int i = node->toplevel - 1; i >= 0; i--)
        {
            do
            {
                node_next = node->next[i];
                if (is_marked((uintptr_t)node_next))
                {
                    break;
                }
            } while (!ATOMIC_CAS_MB(&node->next[i],
                                    node_next,
                                    (struct sl_node *)set_mark((uintptr_t)node_next)));
        }
    }

    // Core search function implementation
    void dict::fraser_search(void *key,
                             struct sl_node **left_list,
                             struct sl_node **right_list)
    {
        struct sl_node *left, *left_next, *right, *right_next;
    retry:
        left = head;
        for (int i = levelmax - 1; i >= 0; i--)
        {
            left_next = left->next[i];
            if (unlikely(is_marked((uintptr_t)left_next)))
            {
                goto retry;
            }

            for (right = left_next;; right = right_next)
            {
                // Get next pointer and handle marked pointers
                right_next = right->next[i];
                while (unlikely(is_marked((uintptr_t)right_next)))
                {
                    right = (struct sl_node *)unset_mark((uintptr_t)right_next);
                    right_next = right->next[i];
                }

                // Check if we've found the position for the key
                if (right->next[0] == NULL ||
                    strcmp(static_cast<char *>(static_cast<void *>(right->key)), (char *)key) >= 0)
                {
                    break;
                }

                left = right;
                left_next = right_next;
            }

            // Try to physically remove marked nodes
            if (left_next != right)
            {
                if (!ATOMIC_CAS_MB(&left->next[i], left_next, right))
                {
                    goto retry;
                }
            }

            // Store nodes in the output arrays if provided
            if (left_list != NULL)
            {
                left_list[i] = left;
            }
            if (right_list != NULL)
            {
                right_list[i] = right;
            }
        }
    }

    // Constructor
    dict::dict(unsigned int level_max)
    {
        levelmax = level_max;

        // Calculate padded size for nodes
        size_pad_32 = sizeof(struct sl_node) + levelmax * sizeof(struct sl_node *);
        while (size_pad_32 & 31)
        {
            size_pad_32++;
        }

        // Create sentinel nodes (min and max values)
        char *min_key = (char *)jem_malloc(1); // Empty string for min key
        char *max_key = (char *)jem_malloc(2); // Max character for max key

        char *min_val = (char *)jem_malloc(1); // Empty string for value
        char *max_val = (char *)jem_malloc(1); // Empty string for value

        if (!min_key || !max_key)
        {
            if (min_key)
                jem_free(min_key);
            if (max_key)
                jem_free(max_key);
            perror("jem_malloc @ constructor");
            exit(1);
        }

        min_key[0] = '\0';   // Empty string
        max_key[0] = '\xff'; // Max character
        max_key[1] = '\0';

        min_val[0] = '\0'; // Empty string
        max_val[0] = '\0'; // Empty string

        struct sl_node *max = allocate_node(max_key, 2, max_val, 1, NULL,
                                            levelmax, size_pad_32, levelmax, 1);
        struct sl_node *min = allocate_node(min_key, 1, min_val, 1, max,
                                            levelmax, size_pad_32, levelmax, 1);

        head = min;
    }

    // Destructor
    dict::~dict()
    {
        struct sl_node *node, *next;

        node = head;
        while (node != NULL)
        {
            next = (struct sl_node *)unset_mark((uintptr_t)node->next[0]);
            node_delete(node, 1, 1); // Free both key and value
            node = next;
        }
    }

    // Get the number of elements in the list
    int dict::length()
    {
        int count = 0;
        struct sl_node *node;

        node = (struct sl_node *)unset_mark((uintptr_t)head->next[0]);

        // Count all nodes except the sentinel nodes (head and tail)
        while (node->next[0] != NULL)
        {
            if (!is_marked((uintptr_t)node->next[0]))
            {
                count++;
            }
            node = (struct sl_node *)unset_mark((uintptr_t)node->next[0]);
        }

        return count;
    }

    // Search for a key and return pointer to its value
    void *dict::search(void *key, size_t key_len)
    {
        struct sl_node *succs[MAXLEVEL];
        void *result = NULL;

        fraser_search(key, NULL, succs);

        if (succs[0]->key != NULL &&
            strcmp(static_cast<char *>(static_cast<void *>(succs[0]->key)), (char *)key) == 0 &&
            !succs[0]->deleted)
        {
            result = succs[0]->val;
        }

        return result;
    }

    // Insert or update a key-value pair
    int dict::insert(void *key, size_t key_len, void *val, size_t val_len)
    {
        struct sl_node *preds[MAXLEVEL], *succs[MAXLEVEL];
        struct sl_node *new_node, *pred, *succ, *new_next;
        int result = 0;
        char *val_copy = NULL;

    retry:
        fraser_search(key, preds, succs);

        // Check if key already exists
        if (succs[0]->key != NULL &&
            strcmp(static_cast<char *>(static_cast<void *>(succs[0]->key)), (char *)key) == 0)
        {
            if (succs[0]->deleted)
            {
                // Key exists but is marked for deletion - retry the operation
                mark_node_ptrs(succs[0]);
                goto retry;
            }

            uintptr_t new_ptr_val;

            do
            {
                // Key exists and is not deleted - update the value
                val_copy = (char *)jem_malloc(val_len);
                if (!val_copy)
                {
                    perror("jem_malloc @ insert\n");
                }

                memcpy(val_copy, val, val_len);
                // Load current value atomically
                uintptr_t current_ptr_val = succs[0]->val.ptr;

                // Create new pointer value - ONLY CALCULATE THIS ONCE
                new_ptr_val = (current_ptr_val & ~ADDRESS_MASK) | reinterpret_cast<uintptr_t>(val_copy);
                new_ptr_val = (new_ptr_val & ~HEAP_ID_MASK) | (0UL << HEAP_ID_SHIFT);
                new_ptr_val |= ACCESS_BIT_MASK;
                new_ptr_val &= ~MIGRATION_LOCK_MASK;

                if (__atomic_compare_exchange_n(
                        &succs[0]->val.ptr,
                        &current_ptr_val, // This will be updated if CAS fails
                        new_ptr_val,
                        false,
                        __ATOMIC_ACQ_REL,
                        __ATOMIC_ACQUIRE))
                {
                    soda_.Set1((((uint64_t)&(succs[0]->val.ptr) - (uint64_t)global_addr_start) / 8));

                    MEM_BARRIER;

                    // CAS succeeded - ONLY NOW extract address/type from the value we replaced
                    void *old_addr = reinterpret_cast<void *>(current_ptr_val & ADDRESS_MASK);
                    uint8_t heap_id = (current_ptr_val & HEAP_ID_MASK) >> HEAP_ID_SHIFT;

                    // Determine memory type
                    MemType old_mem_type;
                    if (heap_id == 0)
                    {
                        old_mem_type = MemType::NEW_HEAP;
                    }
                    else if (heap_id == 1)
                    {
                        old_mem_type = MemType::DRAM_2MB_THP;
                    }
                    else if (heap_id == 2)
                    {
                        old_mem_type = MemType::DRAM;
                    }
                    else
                    {
                        old_mem_type = MemType::NEW_HEAP;
                    }
                    g_sama->free(old_addr, old_mem_type);
                    break;
                }

                // CAS failed, current_ptr_val has been updated automatically
                // We'll retry the loop with the updated value
                // free val_copy
                if (val_copy)
                    jem_free(val_copy);

            } while (true);

            result = 1; // Return 1 to indicate an update
            goto end;
        }

        // Key doesn't exist, create a new node
        new_node = allocate_node_unlinked(key, key_len, val, val_len,
                                          size_pad_32, get_rand_level(), 0);

        // Link the new node at all its levels
        for (int i = 0; i < new_node->toplevel; i++)
        {
            new_node->next[i] = succs[i];
        }

        // Ensure all writes to the node are visible before linking it
        MEM_BARRIER;

        // Try to link at level 0 - this is the critical insertion point
        if (!ATOMIC_CAS_MB(&preds[0]->next[0], succs[0], new_node))
        {
            // Free the node we just created since insertion failed
            node_delete(new_node, 1, 1);
            goto retry;
        }

        // Try to link at higher levels
        for (int i = 1; i < new_node->toplevel; i++)
        {
            while (1)
            {
                pred = preds[i];
                succ = succs[i];
                new_next = new_node->next[i];

                // Check if node has been marked for deletion
                if (is_marked((uintptr_t)new_next))
                {
                    goto success; // Node is being deleted, abandon higher-level linking
                }

                // Try to update the next pointer if needed
                if ((new_next != succ) &&
                    (!ATOMIC_CAS_MB(&new_node->next[i],
                                    (struct sl_node *)unset_mark((uintptr_t)new_next),
                                    succ)))
                {
                    break;
                }

                // Handle the case where successor has the same key
                if (succ->key != NULL &&
                    strcmp(static_cast<char *>(static_cast<void *>(succ->key)),
                           (char *)key) == 0)
                {
                    succ = (struct sl_node *)unset_mark((uintptr_t)succ->next[0]);
                }

                // Try to link the new node at this level
                if (ATOMIC_CAS_MB(&pred->next[i], succ, new_node))
                {
                    break;
                }

                // Linking failed, search again to get updated positions
                fraser_search(key, preds, succs);
            }
        }

    success:
        result = 1; // Insertion succeeded
    end:
        return result;
    }

    // Remove a key-value pair and return pointer to the value
    void *dict::remove(void *key, size_t key_len)
    {
        struct sl_node *succs[MAXLEVEL];
        void *result = NULL;

        fraser_search(key, NULL, succs);

        // Check if key exists and is not already deleted
        if (succs[0]->key == NULL ||
            strcmp(static_cast<char *>(static_cast<void *>(succs[0]->key)), (char *)key) != 0 ||
            succs[0]->deleted)
        {
            goto end;
        }

        // Attempt to mark the node as logically deleted
        if (ATOMIC_FETCH_AND_INC_FULL(&succs[0]->deleted) == 0)
        {
            // We successfully marked it as deleted
            // Now mark all next pointers to help physical removal
            mark_node_ptrs(succs[0]);

            // Save the value before physical removal
            if (succs[0]->val)
            {
                // Need to know the value length - we can't rely on strlen
                // This should ideally be passed in or stored with the value
                // Assuming string for now
                size_t val_size = strlen(static_cast<char *>(static_cast<void *>(succs[0]->val))) + 1;
                char *val_copy = (char *)jem_malloc(val_size);
                if (val_copy)
                {
                    memcpy(val_copy, succs[0]->val, val_size);
                    result = val_copy;
                }
                else
                {
                    perror("jem_malloc @ remove\n");
                }
            }

            // Start physical removal of the node by another search
            fraser_search(key, NULL, NULL);
        }

    end:
        return result;
    }


        /*
     * Range scan: locate the first node >= start_key via fraser_search,
     * then walk level 0 (stripping deletion marks from next pointers) and
     * copy up to n live key/value pairs inside this operation's TAG scope.
     */
    int dict::scanCopy(void *start_key, size_t key_len, int n,
                       std::vector<std::pair<std::string, std::string>> &out)
    {
        struct sl_node *succs[MAXLEVEL];
        fraser_search(start_key, NULL, succs);

        struct sl_node *node = succs[0];
        int copied = 0;
        while (node != NULL && copied < n)
        {
            if (node->key == NULL)
            {
                break; // tail sentinel
            }
            if (!node->deleted)
            {
                out.emplace_back(static_cast<char *>(static_cast<void *>(node->key)),
                                 static_cast<char *>(static_cast<void *>(node->val)));
                copied++;
            }
            node = (struct sl_node *)unset_mark((uintptr_t)node->next[0]);
        }
        return copied;
    }

    /*
     * GET-path helper: copy the value out while still inside the public
     * data-structure operation. search() returns a raw pointer whose object
     * can be migrated (and freed) as soon as the operation's TAG scope ends,
     * so callers must never dereference it after return.
     */
    int dict::searchCopy(void *key, size_t key_len, std::string &out)
    {
        void *v = search(key, key_len);
        if (v == nullptr)
        {
            return 0;
        }
        out.assign(static_cast<const char *>(v));
        return 1;
    }

} // namespace sl_fraser