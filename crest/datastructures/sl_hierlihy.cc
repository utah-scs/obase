#include "sl_hierlihy.h"

sl_marked::sl_marked(void *key, void *val, int toplevel)
    : key(key), val(val), toplevel(toplevel) {}

namespace sl_hierlihy
{
    // Function to allocate a node without linking it
    sl_marked *dict::allocate_sl_marked_unlinked(void *key, size_t key_len, void *val, size_t val_len,
                                                 unsigned int size_pad_32, int toplevel, int transactional)
    {
        void *mem = nullptr;

        size_t node_size = size_pad_32;
        if (transactional)
        {
            size_t node_size_rm = node_size & 63;
            if (node_size_rm)
            {
                node_size += 64 - node_size_rm;
            }
        }

        mem = jem_calloc(1, node_size);
        if (mem == NULL)
        {
            perror("jem_malloc @ allocate_sl_marked_unlinked");
            exit(1);
        }

        void *key_ptr = jem_malloc(key_len);
        void *val_ptr = jem_malloc(val_len);
        memcpy(key_ptr, key, key_len);
        memcpy(val_ptr, val, val_len);

        sl_marked *node = new (mem) sl_marked(key_ptr, val_ptr, toplevel);

        node->marked = 0;
        node->fullylinked = 0;
        for (int i = 0; i < toplevel; i++)
        {
            node->next[i] = NULL;
        }
        INIT_LOCK(ND_GET_LOCK(node));
        MEM_BARRIER;
        return node;
    }

    // Function to allocate a node and link it
    sl_marked *dict::allocate_sl_marked(void *key, size_t key_len, void *val, size_t val_len,
                                        sl_marked *next, int levelmax,
                                        unsigned int size_pad_32, int toplevel, int transactional)
    {
        sl_marked *node;
        node = allocate_sl_marked_unlinked(key, key_len, val, val_len, size_pad_32,
                                           toplevel, transactional);
        for (int i = 0; i < toplevel; i++)
        {
            node->next[i] = next;
        }
        MEM_BARRIER;

        return node;
    }

    // Function to delete a node
    void dict::delete_sl_marked(sl_marked *node)
    {
        DESTROY_LOCK(ND_GET_LOCK(node));
        jem_free((void *)node);
    }

    // Random number utility
    int dict::rand_range(int n)
    {
        int r = rand();
        return (int)(r % n) + 1;
    }

    // NOP repeat utility for backoff
    void dict::nop_rep(int n)
    {
        for (int i = 0; i < n; i++)
        {
            PAUSE;
        }
    }

    // Constructor implementation
    dict::dict(unsigned int level_max)
    {
        sl_marked *min, *max;
        levelmax = level_max;
        size_pad_32 = sizeof(sl_marked) + levelmax * sizeof(sl_marked *);
        while (size_pad_32 & 31)
        {
            size_pad_32++;
        }

        // Allocate max key (sentinel)
        size_t max_key_len = strlen("\xff\xff\xff\xff") + 1;
        char *max_key = (char *)jem_malloc(max_key_len);
        if (max_key == NULL)
        {
            perror("jem_malloc @ SkipList() max_key");
            exit(1);
        }
        strcpy(max_key, "\xff\xff\xff\xff");
        char *min_val = (char *)jem_malloc(1); // Empty string for value
        char *max_val = (char *)jem_malloc(1); // Empty string for value
        min_val[0] = '\0';                     // Empty string
        max_val[0] = '\0';                     // Empty string

        max = allocate_sl_marked(max_key, max_key_len, max_val, 1,
                                 (sl_marked *)NULL, levelmax,
                                 size_pad_32, levelmax, 1);

        // Allocate min key (sentinel)
        char *min_key = (char *)jem_malloc(1); // Empty string
        if (min_key == NULL)
        {
            perror("jem_malloc @ SkipList() min_key");
            exit(1);
        }
        min_key[0] = '\0';

        min = allocate_sl_marked(min_key, 1, min_val, 1,
                                 max, levelmax, size_pad_32, levelmax, 1);

        max->fullylinked = 1;
        min->fullylinked = 1;
        head = min;

        // Initialize global lock
        lock = (pthread_mutex_t *)jem_malloc(sizeof(pthread_mutex_t));
        if (lock == NULL)
        {
            perror("jem_malloc @ SkipList() lock");
            exit(1);
        }
        GL_INIT_LOCK(lock);
    }

    // Destructor implementation
    dict::~dict()
    {
        sl_marked *node, *next;
        node = head;
        while (node != NULL)
        {
            next = node->next[0];
            node->key.destroy();
            if (node->val)
                node->val.destroy();
            delete_sl_marked(node);
            node = next;
        }
        jem_free(lock);
    }

    // Search implementation
    void *dict::search(void *key, size_t key_len)
    {
        void *result = NULL;
        sl_marked *node = optimistic_left_search(key);

        if (node != NULL && !node->marked && node->fullylinked)
        {
            result = node->val;
        }
        return result;
    }

    /**
     * Inserts a key-value pair into the skiplist or updates an existing key with a new value
     *
     * @param key Pointer to the key to insert
     * @param key_len Length of the key in bytes
     * @param val Pointer to the value to insert
     * @param val_len Length of the value in bytes
     * @return int 1 if successfully inserted (or updated)
     */
    int dict::insert(void *key, size_t key_len, void *val, size_t val_len)
    {
        // Arrays to hold predecessor and successor nodes at each level
        sl_marked *succs[HERLIHY_MAX_MAX_LEVEL];
        sl_marked *preds[HERLIHY_MAX_MAX_LEVEL];
        sl_marked *node_found, *prev_pred, *new_node;
        sl_marked *pred, *succ;
        int toplevel, highest_locked, valid, found;
        unsigned int backoff; // For exponential backoff on retry

        // Randomly determine the height (level) for the new node
        toplevel = get_rand_level();
        backoff = 1; // Initialize backoff counter

        // Main insertion loop - may need multiple attempts due to concurrent modifications
        while (1)
        {
            // Phase 1: Search for the key and fill preds/succs arrays
            // The "1" parameter means "fast search" - stop when key is found
            found = optimistic_search(key, preds, succs, 1);

            // If the key already exists in the skiplist
            if (found != -1)
            {
                node_found = succs[found];
                // Check if the node is not marked for deletion
                if (!node_found->marked)
                {
                    // Wait until the node is fully linked (important for consistency)
                    while (!node_found->fullylinked)
                    {
                        PAUSE; // Processor hint to reduce spinning overhead
                    }

                    // Lock the node before updating its value
                    LOCK(ND_GET_LOCK(node_found));

                    // Verify the node hasn't been marked for deletion while we were waiting for the lock
                    if (node_found->marked)
                    {
                        UNLOCK(ND_GET_LOCK(node_found));
                        continue; // Node is being deleted, retry the operation
                    }

                    // Update existing node: swap then free (never
                    // destroy() a live slot first -- transient SODA Set0
                    // leaks ATC)
                    void *old_val = static_cast<void *>(node_found->val); // pins via ATC
                    MemType old_type = node_found->val.getMemType();
                    node_found->val = jem_malloc(val_len);
                    node_found->val.setHeapId(0);
                    memcpy(node_found->val, val, val_len);
                    g_sama->free(old_val, old_type);

                    // Release the lock
                    UNLOCK(ND_GET_LOCK(node_found));

                    return 1; // Return success (key updated)
                }
                // If the node is marked, it's being deleted, so we need to retry
                continue;
            }

            // Phase 2: Acquire locks for insertion
            GL_LOCK(lock); // First acquire the global lock
            highest_locked = -1;
            prev_pred = NULL;
            valid = 1;

            // Lock all predecessors up to the new node's level and validate the search
            for (int i = 0; valid && (i < toplevel); i++)
            {
                pred = preds[i];
                succ = succs[i];
                // Only lock each unique predecessor once
                if (pred != prev_pred)
                {
                    LOCK(ND_GET_LOCK(pred));
                    highest_locked = i;
                    prev_pred = pred;
                }
                // Validate that nodes haven't changed since our search
                valid = !pred->marked && !succ->marked &&
                        ((sl_marked *)pred->next[i] ==
                         (sl_marked *)succ);
            }

            // If validation failed, release locks and retry
            if (!valid)
            {
                GL_UNLOCK(lock);
                unlock_levels(preds, highest_locked);

                // Exponential backoff to reduce contention
                if (backoff > 5000)
                {
                    nop_rep(backoff & MAX_BACKOFF); // Execute pause instructions
                }
                backoff <<= 1; // Double the backoff period
                continue;
            }

            // Phase 3: Create and link the new node

            // Allocate the new node with our copies of key and value
            new_node = allocate_sl_marked_unlinked(key, key_len, val, val_len,
                                                   size_pad_32, toplevel, 0);

            // Set the next pointers for all levels of the new node
            for (int i = 0; i < toplevel; i++)
            {
                new_node->next[i] = succs[i];
            }
            MEM_BARRIER; // Ensure all writes are visible to other threads

            // Link the node into the skiplist by updating predecessor pointers
            for (int i = 0; i < toplevel; i++)
            {
                preds[i]->next[i] = new_node;
            }

            // Mark the node as fully linked so other operations can proceed with it
            new_node->fullylinked = 1;

            // Phase 4: Release locks and return
            GL_UNLOCK(lock);
            unlock_levels(preds, highest_locked);
            return 1; // Return success (new node inserted)
        }
    }

    // Remove implementation
    void *dict::remove(void *key, size_t key_len)
    {
        sl_marked *succs[HERLIHY_MAX_MAX_LEVEL];
        sl_marked *preds[HERLIHY_MAX_MAX_LEVEL];
        sl_marked *node_to_remove, *prev_pred;
        sl_marked *pred, *succ;
        int is_marked, toplevel, highest_locked, valid, found;
        unsigned int backoff;
        void *val_to_return = NULL;

        node_to_remove = NULL;
        is_marked = 0;
        toplevel = -1;
        backoff = 1;

        while (1)
        {
            found = optimistic_search(key, preds, succs, 1);

            if (is_marked || (found != -1 && ok_to_delete(succs[found], found)))
            {
                GL_LOCK(lock);
                if (!is_marked)
                {
                    node_to_remove = succs[found];
                    LOCK(ND_GET_LOCK(node_to_remove));
                    toplevel = node_to_remove->toplevel;

                    if (node_to_remove->marked)
                    {
                        GL_UNLOCK(lock);
                        UNLOCK(ND_GET_LOCK(node_to_remove));
                        return NULL;
                    }
                    node_to_remove->marked = 1;
                    is_marked = 1;
                }

                // Physical deletion
                highest_locked = -1;
                prev_pred = NULL;
                valid = 1;
                for (int i = 0; valid && (i < toplevel); i++)
                {
                    pred = preds[i];
                    succ = succs[i];
                    if (pred != prev_pred)
                    {
                        LOCK(ND_GET_LOCK(pred));
                        highest_locked = i;
                        prev_pred = pred;
                    }
                    valid = (!pred->marked &&
                             ((sl_marked *)pred->next[i] ==
                              (sl_marked *)succ));
                }

                if (!valid)
                {
                    unlock_levels(preds, highest_locked);
                    if (backoff > 5000)
                    {
                        nop_rep(backoff & MAX_BACKOFF);
                    }
                    backoff <<= 1;
                    continue;
                }

                for (int i = toplevel - 1; i >= 0; i--)
                {
                    preds[i]->next[i] = node_to_remove->next[i];
                }

                // Save value to return
                val_to_return = node_to_remove->val;

                node_to_remove->key.destroy();

                UNLOCK(ND_GET_LOCK(node_to_remove));
                delete_sl_marked(node_to_remove);
                GL_UNLOCK(lock);
                unlock_levels(preds, highest_locked);

                return val_to_return;
            }
            else
            {
                return NULL;
            }
        }
    }

    // Length implementation
    int dict::length()
    {
        int count = 0;
        sl_marked *node;

        node = head->next[0];
        while (node->next[0] != NULL)
        {
            if (node->fullylinked && !node->marked)
            {
                count++;
            }
            node = node->next[0];
        }
        return count;
    }

    // Private helper method implementations
    int dict::get_rand_level()
    {
        int level = 1;
        for (int i = 0; i < levelmax - 1; i++)
        {
            if ((rand_range(100) - 1) < 50)
            {
                level++;
            }
            else
            {
                break;
            }
        }
        return level;
    }

    int dict::ok_to_delete(sl_marked *node, int found)
    {
        return node->fullylinked && ((node->toplevel - 1) == found) && !node->marked;
    }

    int dict::optimistic_search(void *key, sl_marked **preds,
                                sl_marked **succs, int fast)
    {
    restart:
        int found;
        sl_marked *pred, *curr;

        found = -1;
        pred = head;
        for (int i = (pred->toplevel - 1); i >= 0; i--)
        {
            curr = pred->next[i];
            while (strcmp((char *)key, static_cast<char *>(static_cast<void *>(curr->key))) > 0)
            {
                pred = curr;
                curr = pred->next[i];
            }
            if (preds != NULL)
            {
                preds[i] = pred;
                if (unlikely(pred->marked))
                {
                    goto restart;
                }
            }
            succs[i] = curr;
            if (found == -1 && strcmp((char *)key, static_cast<char *>(static_cast<void *>(curr->key))) == 0)
            {
                found = i;
            }
        }
        return found;
    }

    sl_marked *dict::optimistic_left_search(void *key)
    {
        sl_marked *pred, *curr, *node = NULL;
        pred = head;
        for (int i = (pred->toplevel - 1); i >= 0; i--)
        {
            curr = pred->next[i];
            int cmp_result;
            while ((cmp_result = strcmp((char *)key, static_cast<char *>(static_cast<void *>(curr->key)))) > 0)
            {
                pred = curr;
                curr = pred->next[i];
            }

            if (cmp_result == 0)
            {
                node = curr;
                break;
            }
        }
        return node;
    }

    void dict::unlock_levels(sl_marked **nodes, int highestlevel)
    {
        sl_marked *old = NULL;
        for (int i = 0; i <= highestlevel; i++)
        {
            if (old != nodes[i])
            {
                UNLOCK(ND_GET_LOCK(nodes[i]));
            }
            old = nodes[i];
        }
    }

        /*
     * Range scan: descend to the last node < start_key, then walk level 0
     * copying up to n fully-linked, unmarked key/value pairs inside this
     * operation's TAG scope. The walk stops at the max sentinel (the only
     * node with next[0] == NULL).
     */
    int dict::scanCopy(void *start_key, size_t key_len, int n,
                       std::vector<std::pair<std::string, std::string>> &out)
    {
        sl_marked *pred = head, *curr = NULL;
        for (int i = (pred->toplevel - 1); i >= 0; i--)
        {
            curr = pred->next[i];
            while (curr->next[0] != NULL &&
                   strcmp((char *)start_key, static_cast<char *>(static_cast<void *>(curr->key))) > 0)
            {
                pred = curr;
                curr = pred->next[i];
            }
        }

        curr = pred->next[0];
        int copied = 0;
        while (curr != NULL && curr->next[0] != NULL && copied < n)
        {
            if (curr->fullylinked && !curr->marked &&
                strcmp(static_cast<char *>(static_cast<void *>(curr->key)), (char *)start_key) >= 0)
            {
                out.emplace_back(static_cast<char *>(static_cast<void *>(curr->key)),
                                 static_cast<char *>(static_cast<void *>(curr->val)));
                copied++;
            }
            curr = curr->next[0];
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

} // namespace sl_hierlihy