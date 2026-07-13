#ifndef BPT_OCC_H
#define BPT_OCC_H

/*
## OCC Design Elements:
1. **Versioned Latches**: Each node has a `Latch` structure containing an atomic
lock and version counter for detecting concurrent modifications.

2. **Path Tracking**: The `PathEntry` structure maintains traversal state,
recording nodes visited, their versions, and whether they're locked.

3. **Thread-Local Context**: Each thread maintains its own context with:
   - Path information for traversal
   - Nodes scheduled for deletion
   - Current epoch for memory reclamation

4. **Epoch-Based Memory Management**: Safely reclaims memory by tracking when
nodes are safe to delete. A node is only physically freed when all active
transactions have a newer epoch.

5. **Optimistic Reads**: The `search` and `range` operations:
   - Read nodes without locking
   - Verify node versions before and after access
   - Retry when concurrent modifications detected

6. **Pessimistic Writes**: The `insert` and `remove` operations:
   - Navigate optimistically to target nodes
   - Lock nodes that need modification
   - Validate path hasn't changed
   - Perform modifications
   - Release locks and increment versions

7. **Root Protection**: A dedicated mutex protects root modifications,
addressing a special case in tree structure.
*/

#include <atomic>
#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <thread>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>

#include "Guide.hpp"

namespace bpt_occ
{
    // Global epoch-based memory management

    class dict
    {
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

        // Range query - API unchanged
        void **range(const void *start_key, const void *end_key, int *count) const;

        // Range scan: copies up to n key/value pairs starting at the first
        // key >= start_key, in key order, inside the operation's TAG scope.
        int scanCopy(const void *start_key, size_t key_len, int n,
                     std::vector<std::pair<std::string, std::string>> &out);

        // Utility functions - API unchanged
        bool empty() const;
        void clear();

    private:
        struct EpochManager
        {
            static std::atomic<uint64_t> global_epoch;
            static void advance_epoch();
            static uint64_t get_current_epoch();
        };

        // Latch structure for optimistic concurrency control
        struct Latch
        {
            std::atomic<uint8_t> lock;
            std::atomic<uint32_t> version;
        };

        // Enhanced Node structure with latch
        struct Node
        {
            Latch latch; // Concurrency control
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

            struct Node *next; // For leaf nodes: linked list for range queries
            struct Node *prev; // For leaf nodes: backward link for easier rebalancing

            // Timestamp for deletion (used for memory reclamation)
            std::atomic<uint64_t> delete_timestamp;
        };

        // Structure to track path during traversal
        struct PathEntry
        {
            Node *node;
            uint32_t version;
            int index;
            bool locked;
        };

        // Thread-local context for traversals
        struct ThreadContext
        {
            std::vector<PathEntry> path;
            std::vector<Node *> nodes_to_free;
            uint64_t epoch;

            ThreadContext() : epoch(0) {}
            ~ThreadContext();
        };

        // Root node and tree properties
        std::atomic<Node *> root_;
        int degree_;

        // Mutex for root modifications
        mutable std::mutex root_mutex_;

        // Thread registry for memory management
        mutable std::mutex thread_registry_mutex_;
        mutable std::vector<ThreadContext *> thread_registry_;

        // Memory management
        void register_thread(ThreadContext *context) const;
        void unregister_thread(ThreadContext *context) const;
        ThreadContext *get_thread_context() const;
        void schedule_for_deletion(Node *node);
        void reclaim_memory(ThreadContext *context);

        // Latch operations
        bool try_lock_node(Node *node) const;
        void lock_node(Node *node) const;
        void unlock_node(Node *node) const;
        uint32_t get_version(Node *node) const;
        bool is_locked(Node *node) const;
        bool node_changed(Node *node, uint32_t version) const;

        // Enhanced helper functions
        static int compare_keys(const void *key1, const void *key2);
        static size_t key_size(const void *key);
        static size_t value_size(const void *value);
        static void *copy_key(const void *key);
        static void *copy_value(const void *value);

        // Node management with OCC
        Node *create_node(bool is_leaf);

        // Key accessors that work on either node kind (leaf keys are guided,
        // interior separators are raw)
        static void *key_at(Node *n, int i);            // read, no ownership change
        static void put_key(Node *n, int i, void *key); // ownership in
        void destroy_node(Node *node, bool free_values = true);
        int find_index(Node *node, const void *key) const;

        // OCC-aware tree operations
        Node *find_leaf_node(const void *key, std::vector<PathEntry> &path) const;
        bool validate_path(const std::vector<PathEntry> &path) const;
        void acquire_path_locks(std::vector<PathEntry> &path, int up_to_depth);
        void release_path_locks(std::vector<PathEntry> &path);

        // B+ tree operation helpers
        void *search_with_occ(const void *key, size_t key_len) const;
        int insert_with_occ(const void *key, size_t key_len, const void *value, size_t value_len);
        void *remove_with_occ(const void *key, size_t key_len);

        // Internal operation implementations
        int insert_recursive(Node *node, const void *key, const void *value,
                             std::vector<PathEntry> &path, int depth);
        void split_child(Node *parent, int index);
        bool remove_from_node(Node *node, const void *key, std::vector<PathEntry> &path, int depth);
        void merge_nodes(Node *parent, int index);
        void redistribute_keys(Node *parent, int index, bool from_next);

        // Handle special cases with OCC
        void handle_root_overflow();
        void handle_root_underflow();

        // Prevent copying
        dict(const dict &);
        dict &operator=(const dict &);
    };

} // namespace bpt_occ

#endif // BPT_OCC_H