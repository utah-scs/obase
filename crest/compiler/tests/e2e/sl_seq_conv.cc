#include "sl_seq_conv.h"
#include <jemalloc/jemalloc.h>

namespace sl_seq_conv
{
    sl_node::sl_node(void *key, void *val, int toplevel) : key(key), val(val), toplevel(toplevel) {}

    // Allocate a new node without linking it
    struct sl_node *allocate_node_unlinked(void *key, size_t key_len,
                                           void *val, size_t val_len, int toplevel)
    {
        // Calculate node size with padding to ensure alignment
        size_t size = node_size(toplevel);
        size_t size_pad_32 = size;
        while (size_pad_32 & 31)
        {
            size_pad_32++;
        }

        void *mem = jem_calloc(1, size_pad_32);
        if (NULL == mem)
        {
            perror("jem_calloc @ allocate_node_unlinked");
            exit(1);
        }

        void *key_ptr = jem_malloc(key_len);
        void *val_ptr = jem_malloc(val_len);

        memcpy(key_ptr, key, key_len);
        memcpy(val_ptr, val, val_len);

        sl_node *node = new (mem) sl_node(key_ptr, val_ptr, toplevel);

        if (!node->key || !node->val)
        {
            jem_free(node->key);
            jem_free(node->val);
            perror("jem_malloc @ allocate_node_unlinked");
            return 0; // Memory allocation failed
        }

        // Set node properties
        node->toplevel = toplevel;

        // Initialize next pointers to NULL
        for (int i = 0; i < toplevel; i++)
        {
            node->next[i] = NULL;
        }

        return node;
    }

    // Allocate and initialize a node with next pointers
    struct sl_node *allocate_node(void *key, size_t key_len, void *val, size_t val_len,
                                  struct sl_node *next,
                                  int levelmax,
                                  int toplevel)
    {
        struct sl_node *node = allocate_node_unlinked(key, key_len, val, val_len, toplevel);

        for (int i = 0; i < toplevel; i++)
        {
            node->next[i] = next;
        }

        return node;
    }

    // Delete a node and its key/value if needed
    void node_delete(struct sl_node *node)
    {
        if (node->key)
        {
            jem_free(node->key);
            node->key = nullptr;
        }
        if (node->val)
        {
            jem_free(node->val);
            node->val = nullptr;
        }
        jem_free((void *)node);
    }

    // Constructor
    dict::dict(unsigned int level_max)
    {
        levelmax = level_max;

        // Create sentinel nodes (min and max values)
        char *min_key = (char *)jem_malloc(1); // Empty string for min key
        char *max_key = (char *)jem_malloc(2); // Max character for max key

        char *val = (char *)jem_malloc(1); // Empty string for value

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

        val[0] = '\0'; // Empty string

        struct sl_node *max = allocate_node(max_key, 2, val, 1, NULL, levelmax, levelmax);
        struct sl_node *min = allocate_node(min_key, 1, val, 1, max, levelmax, levelmax);

        head = min;
    }

    // Destructor
    dict::~dict()
    {
        struct sl_node *node, *next;

        node = head;
        while (node != NULL)
        {
            next = node->next[0];
            node_delete(node); // Free both key and value
            node = next;
        }
    }

    // Get the number of elements in the list
    int dict::length()
    {
        int count = 0;
        struct sl_node *node;

        node = head->next[0]; // Skip head sentinel

        // Count all nodes except the sentinel nodes (head and tail)
        while (node->next[0] != NULL)
        {
            count++;
            node = node->next[0];
        }

        return count - 1; // Subtract 1 to exclude max sentinel
    }

    // Search for a key and return pointer to its value
    void *dict::search(void *key, size_t key_len)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        struct sl_node *node, *next;

        node = head;
        for (int i = node->toplevel - 1; i >= 0; i--)
        {
            next = node->next[i];
            while (next->next[0] != NULL && strcmp((char *)next->key, (char *)key) < 0)
            {
                node = next;
                next = node->next[i];
            }
        }

        next = node->next[0]; // Move to the next node at level 0

        // Check if we found the key
        if (next->next[0] != NULL && strcmp((char *)next->key, (char *)key) == 0)
        {
            return next->val;
        }
        return NULL;
    }

    // Insert or update a key-value pair
    int dict::insert(void *key, size_t key_len, void *val, size_t val_len)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        int level, result = 0;
        struct sl_node *node, *next;
        struct sl_node *preds[MAXLEVEL], *succs[MAXLEVEL];

        // Find position to insert
        node = head;
        for (int i = node->toplevel - 1; i >= 0; i--)
        {
            next = node->next[i];
            while (next->next[0] != NULL && strcmp((char *)next->key, (char *)key) < 0)
            {
                node = next;
                next = node->next[i];
            }
            preds[i] = node;
            succs[i] = next;
        }

        // Check if key already exists
        next = preds[0]->next[0];
        if (next->next[0] != NULL && strcmp((char *)next->key, (char *)key) == 0)
        {
            char *val_copy = (char *)jem_malloc(val_len);

            if (!val_copy)
            {
                perror("jem_malloc @ insert");
            }

            memcpy(val_copy, val, val_len);

            // Read the old value, publish the replacement, free the old one
            void *old_val = next->val;
            next->val = val_copy;
            jem_free(old_val);
            result = 1; // Return 1 for update
        }
        else
        {
            // Key doesn't exist, insert new node
            level = get_rand_level();
            node = allocate_node_unlinked(key, key_len, val, val_len, level);

            for (int i = 0; i < level; i++)
            {
                node->next[i] = succs[i];
                preds[i]->next[i] = node;
            }
            result = 1; // Return 1 for insert
        }

        return result;
    }

    // Remove a key-value pair and return pointer to the value
    void *dict::remove(void *key, size_t key_len)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        void *result = NULL;
        struct sl_node *node, *next;
        struct sl_node *preds[MAXLEVEL], *succs[MAXLEVEL];

        // Find position of the key
        node = head;
        for (int i = node->toplevel - 1; i >= 0; i--)
        {
            next = node->next[i];
            while (next->next[0] != NULL && strcmp((char *)next->key, (char *)key) < 0)
            {
                node = next;
                next = node->next[i];
            }
            preds[i] = node;
            succs[i] = next;
        }

        // Check if key exists
        next = preds[0]->next[0];
        if (next->next[0] != NULL && strcmp((char *)next->key, (char *)key) == 0)
        {
            // Update pointers to remove node
            for (int i = 0; i < next->toplevel; i++)
            {
                preds[i]->next[i] = next->next[i];
            }

            // copy value and return
            result = jem_malloc(strlen((char *)next->val));
            memcpy(result, next->val, strlen((char *)next->val));

            // Free the key, value, and node
            node_delete(next);
        }

        return result;
    }

    // Range scan: walk level 0 from the first key >= start_key and copy up
    // to n key/value pairs.
    int dict::scanCopy(void *start_key, size_t key_len, int n,
                       std::vector<std::pair<std::string, std::string>> &out)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        struct sl_node *node, *next;

        node = head;
        for (int i = node->toplevel - 1; i >= 0; i--)
        {
            next = node->next[i];
            while (next->next[0] != NULL && strcmp((char *)next->key, (char *)start_key) < 0)
            {
                node = next;
                next = node->next[i];
            }
        }

        next = node->next[0];
        int copied = 0;
        while (copied < n && next->next[0] != NULL)
        {
            out.emplace_back((char *)next->key, (char *)next->val);
            copied++;
            next = next->next[0];
        }
        return copied;
    }

    /*
     * GET-path helper: copy the value out while still inside the public
     * data-structure operation. search() returns a raw pointer that must
     * never be dereferenced after the operation returns.
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

}
