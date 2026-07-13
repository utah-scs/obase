#ifndef BPT_SEQ_H
#define BPT_SEQ_H

#include <cstddef>
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <stdio.h>

#include "Guide.hpp"

/*
###############
When spliting nodes, the entire key and values are moved rather than just
moving the pointers. NEED TO CHANGE THIS!! Affects page utilization.
###############
*/
namespace bpt_seq
{

    class dict
    {
    private:
        // This is the "B+Tree Coarse" variant: the tree itself is
        // unsynchronized, so every public operation takes a global lock.
        mutable std::mutex op_mutex;

    public:
        // Constructor & Destructor
        dict(int degree = 4);
        ~dict();

        // Core B+ Tree operations
        void *search(const void *key, size_t key_len) const;
        // GET-path helper: copies the value out while still inside the
        // public operation's TAG scope (raw pointers from search() must not
        // escape the operation; migration may free them afterwards).
        int searchCopy(const void *key, size_t key_len, std::string &out);
        int insert(const void *key, size_t key_len, const void *value, size_t value_len);
        void *remove(const void *key, size_t key_len);

        // Range query - returns array of values for keys in range [start_key, end_key]
        // count will be set to the number of values in the result
        void **range(const void *start_key, const void *end_key, int *count) const;

        // Range scan: copies up to n key/value pairs starting at the first
        // key >= start_key, in key order, inside the operation's TAG scope.
        int scanCopy(const void *start_key, size_t key_len, int n,
                     std::vector<std::pair<std::string, std::string>> &out);

        // Utility functions
        bool empty() const;
        void clear();

    private:
        // Node structure definition
        struct Node
        {
            bool is_leaf;
            int num_keys; // Current number of keys
            int max_keys; // Maximum keys per node (degree-1)

            /*
             * Keys: LEAF keys are guided (per-record data that participates
             * in migration); INTERIOR separator keys are raw copies. A
             * separator is routing metadata dereferenced by every traversal
             * that passes it -- guiding it manufactures hotness (the access
             * bit tracks traversals, not application accesses), ping-pongs
             * separators between heaps every scan window, and keeps the
             * promotion rate above target so the COLD heap is never paged
             * out. See bpt_mass for the same choice (keys inline in nodes).
             */
            union
            {
                Guide<void> *keys; // For leaf nodes (guided)
                void **ikeys;        // For internal nodes (raw separator copies)
            };

            // Union to save space - a node has either children OR values
            union
            {
                struct Node **children; // For internal nodes
                Guide<void> *values;  // For leaf nodes
            };

            struct Node *next; // For leaf nodes: pointer to next leaf (for range queries)
        };

        // Key accessors that work on either node kind (leaf keys are guided,
        // interior separators are raw)
        static void *key_at(Node *n, int i);            // read, no ownership change
        static void *take_key(Node *n, int i);          // ownership out, slot nulled
        static void put_key(Node *n, int i, void *key); // ownership in

        // Tree properties
        Node *root_;
        int degree_; // Maximum number of children for a node

        // Helper functions for key/value operations
        static int compare_keys(const void *key1, const void *key2);
        static size_t key_size(const void *key);
        static size_t value_size(const void *value);
        static void *copy_key(const void *key);
        static void *copy_value(const void *value);

        // Node management helpers
        Node *create_node(bool is_leaf);
        void destroy_node(Node *node, bool free_values = true);
        int find_index(Node *node, const void *key) const;

        // B+ Tree operations helpers
        void *search_in_node(Node *node, const void *key) const;
        int insert_non_full(Node *node, const void *key, const void *value);
        void split_child(Node *parent, int index);
        bool remove_from_node(Node *node, const void *key);
        void merge_nodes(Node *parent, int index);
        void redistribute_keys(Node *parent, int index, bool from_next);

        // Handle special cases of root changes
        void handle_root_overflow();
        void handle_root_underflow();

        // Range query helpers
        Node *find_leaf_node(const void *key) const;

        // Prevent copying
        dict(const dict &);
        dict &operator=(const dict &);
    };

} // namespace bpt_seq

#endif // BPT_SEQ_H