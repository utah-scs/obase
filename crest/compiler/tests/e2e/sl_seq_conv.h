/*
 * Pass 2 fixture: sl_seq (coarse-lock skip list) de-converted to plain
 * C++. Identical to datastructures/sl_seq.h except: the guide fields are
 * raw annotated pointers, guide-API calls in the .cc are their plain
 * equivalents (read/assign/jem_free), and the global-scope node type and
 * helpers live inside the namespace so the fixture links alongside the
 * real structure. guide-converter regenerates the guided form; the e2e
 * harness proves the result migration-correct.
 */
#ifndef _SKIPLIST_CONV_H_
#define _SKIPLIST_CONV_H_

#include <stdlib.h>
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "GuideAnnotations.h"

#define MAXLEVEL 32

namespace sl_seq_conv
{
    // Function to get a random number in a range
    inline unsigned int rand_range(unsigned int n)
    {
        return rand() % n;
    }

    // Skip list node structure using void* for key and value
    struct sl_node
    {
        OBASE_GUIDED void *key; // String key
        OBASE_GUIDED void *val; // String value
        int toplevel;
        struct sl_node *next[1];

        sl_node(void *key = nullptr, void *val = nullptr, int toplevel = 0);
    };

    // Calculate the size needed for a node with a given number of levels
    inline size_t node_size(int levels)
    {
        return sizeof(struct sl_node) + (levels - 1) * sizeof(struct sl_node *);
    }

    // Allocate a new node without linking it
    struct sl_node *allocate_node_unlinked(void *key, size_t key_len,
                                           void *val, size_t val_len, int toplevel);

    // Allocate and initialize a node with next pointers
    struct sl_node *allocate_node(void *key, size_t key_len,
                                  void *val, size_t val_len,
                                  struct sl_node *next,
                                  int levelmax,
                                  int toplevel);

    // Delete a node and its key/value if needed
    void node_delete(struct sl_node *node);

    class dict
    {
    private:
        struct sl_node *head;
        unsigned int levelmax;

        // This is the "SkipList Coarse" variant: the list itself is
        // unsynchronized, so every public operation takes a global lock.
        mutable std::mutex op_mutex;

        // Get random level for new nodes
        inline int get_rand_level()
        {
            int level = 1;
            for (int i = 0; i < levelmax - 1; i++)
            {
                if (rand_range(101) < 50)
                {
                    level++;
                }
                else
                {
                    break;
                }
            }
            // 1 <= level <= levelmax
            return level;
        }

    public:
        // Constructor
        dict(unsigned int level_max = MAXLEVEL);

        // Destructor
        ~dict();

        // Get the number of elements in the list
        int length();

        // Search for a key and return pointer to its value
        void *search(void *key, size_t key_len);
        // GET-path helper: copies the value out while still inside the
        // operation (raw pointers from search() must not escape it).
        int searchCopy(void *key, size_t key_len, std::string &out);

        // Insert or update a key-value pair
        int insert(void *key, size_t key_len, void *val, size_t val_len);

        // Remove a key-value pair and return pointer to the value
        void *remove(void *key, size_t key_len);

        // Range scan: copies up to n key/value pairs starting at the first
        // key >= start_key, in key order.
        int scanCopy(void *start_key, size_t key_len, int n,
                     std::vector<std::pair<std::string, std::string>> &out);
    };
}

#endif // _SKIPLIST_CONV_H_
