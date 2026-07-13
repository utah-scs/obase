#include "bpt_occ.h"
#include <cassert>
#include <algorithm>
#include <stdexcept>
#include <thread>

#include <iostream>

namespace bpt_occ
{
    // Initialize static member of dict::EpochManager
    std::atomic<uint64_t> dict::EpochManager::global_epoch(1);

    void dict::EpochManager::advance_epoch()
    {
        global_epoch.fetch_add(1, std::memory_order_acq_rel);
    }

    uint64_t dict::EpochManager::get_current_epoch()
    {
        return global_epoch.load(std::memory_order_acquire);
    }

    //===============================================================
    // Constructor & Destructor
    //===============================================================

    dict::dict(int degree) : degree_(degree)
    {
        if (degree < 3)
        {
            degree_ = 3; // Minimum degree for B+ tree
        }

        // Create an empty leaf node as root
        Node *root = create_node(true);
        root_.store(root, std::memory_order_release);
    }

    dict::~dict()
    {
        clear();

        // Clean up any registered thread contexts
        std::lock_guard<std::mutex> lock(thread_registry_mutex_);
        thread_registry_.clear();
    }

    //===============================================================
    // Thread Context Management
    //===============================================================

    dict::ThreadContext::~ThreadContext()
    {
        // Clean up any nodes pending deletion
        for (Node *node : nodes_to_free)
        {
            delete node;
        }
        nodes_to_free.clear();
    }

    void dict::register_thread(ThreadContext *context) const
    {
        std::lock_guard<std::mutex> lock(thread_registry_mutex_);
        thread_registry_.push_back(context);
    }

    void dict::unregister_thread(ThreadContext *context) const
    {
        std::lock_guard<std::mutex> lock(thread_registry_mutex_);
        auto it = std::find(thread_registry_.begin(), thread_registry_.end(), context);
        if (it != thread_registry_.end())
        {
            thread_registry_.erase(it);
        }
    }

    dict::ThreadContext *dict::get_thread_context() const
    {
        // Use thread_local to maintain per-thread context
        thread_local std::unique_ptr<ThreadContext> context(new ThreadContext());
        thread_local bool registered = false;

        if (!registered)
        {
            register_thread(context.get());
            registered = true;
        }

        // Update the thread's epoch to the current global epoch
        context->epoch = dict::EpochManager::get_current_epoch();

        return context.get();
    }

    //===============================================================
    // Memory Management
    //===============================================================

    void dict::schedule_for_deletion(Node *node)
    {
        // Mark the node with the current epoch
        node->delete_timestamp.store(dict::EpochManager::get_current_epoch(), std::memory_order_release);

        // Add to the thread's deletion list
        ThreadContext *context = get_thread_context();
        context->nodes_to_free.push_back(node);

        // Periodically try to reclaim memory
        if (context->nodes_to_free.size() > 100)
        { // Arbitrary threshold
            reclaim_memory(context);
        }
    }

    void dict::reclaim_memory(ThreadContext *context)
    {
        // Find the minimum epoch across all active threads
        uint64_t min_epoch = UINT64_MAX;
        {
            std::lock_guard<std::mutex> lock(thread_registry_mutex_);
            for (const auto &thread_ctx : thread_registry_)
            {
                min_epoch = std::min(min_epoch, thread_ctx->epoch);
            }
        }

        // Free nodes from epochs that are no longer accessible
        auto it = context->nodes_to_free.begin();
        while (it != context->nodes_to_free.end())
        {
            Node *node = *it;
            if (node->delete_timestamp.load(std::memory_order_acquire) < min_epoch)
            {
                destroy_node(node);
                it = context->nodes_to_free.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    //===============================================================
    // Latch Operations
    //===============================================================

    bool dict::try_lock_node(Node *node) const
    {
        uint8_t expected = 0;
        return node->latch.lock.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
    }

    void dict::lock_node(Node *node) const
    {
        uint8_t expected = 0;
        while (!node->latch.lock.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        {
            expected = 0;
            std::this_thread::yield(); // Be friendly to other threads
        }
    }

    void dict::unlock_node(Node *node) const
    {
        // Increment version number when unlocking to signal modification
        node->latch.version.fetch_add(1, std::memory_order_release);
        node->latch.lock.store(0, std::memory_order_release);
    }

    uint32_t dict::get_version(Node *node) const
    {
        return node->latch.version.load(std::memory_order_acquire);
    }

    bool dict::is_locked(Node *node) const
    {
        return node->latch.lock.load(std::memory_order_acquire) == 1;
    }

    bool dict::node_changed(Node *node, uint32_t old_version) const
    {
        return is_locked(node) || old_version != get_version(node);
    }

    //===============================================================
    // Node Management
    //===============================================================

    void *dict::key_at(Node *n, int i)
    {
        return n->is_leaf ? static_cast<void *>(n->keys[i]) : n->ikeys[i];
    }

    void dict::put_key(Node *n, int i, void *key)
    {
        if (n->is_leaf)
        {
            n->keys[i] = key;
        }
        else
        {
            n->ikeys[i] = key;
        }
    }

    dict::Node *dict::create_node(bool is_leaf)
    {
        Node *node = new Node();
        node->is_leaf = is_leaf;
        node->num_keys = 0;
        node->max_keys = degree_ - 1;
        node->latch.lock.store(0, std::memory_order_relaxed);
        node->latch.version.store(0, std::memory_order_relaxed);
        node->delete_timestamp.store(0, std::memory_order_relaxed);

        if (is_leaf)
        {
            node->keys = (Guide<void> *)jem_malloc(sizeof(Guide<void>) * node->max_keys);

            // Initialize each Guide object
            for (int i = 0; i < node->max_keys; i++)
            {
                new (&node->keys[i]) Guide<void>(); // Placement new to call constructor
            }

            node->values = (Guide<void> *)jem_malloc(sizeof(Guide<void>) * node->max_keys);

            // Initialize each value object
            for (int i = 0; i < node->max_keys; i++)
            {
                new (&node->values[i]) Guide<void>(); // Placement new to call constructor
            }

            node->next = nullptr;
            node->prev = nullptr;
        }
        else
        {
            // Raw separator copies -- deliberately NOT guided (see bpt_occ.h)
            node->ikeys = (void **)jem_malloc(sizeof(void *) * node->max_keys);
            for (int i = 0; i < node->max_keys; i++)
            {
                node->ikeys[i] = nullptr;
            }

            node->children = (Node **)jem_malloc(sizeof(Node *) * degree_);

            // Initialize child pointers to nullptr
            for (int i = 0; i < degree_; i++)
            {
                node->children[i] = nullptr;
            }
        }

        return node;
    }

    void dict::destroy_node(Node *node, bool free_values)
    {
        if (node == nullptr)
        {
            return;
        }

        // Free all keys
        for (int i = 0; i < node->num_keys; i++)
        {
            if (node->is_leaf)
            {
                node->keys[i].destroy();

                if (free_values)
                {
                    node->values[i].destroy();
                }
            }
            else
            {
                jem_free(node->ikeys[i]); // raw separator copy
            }
        }

        // Recursively destroy all children if not a leaf
        if (!node->is_leaf)
        {
            for (int i = 0; i <= node->num_keys; i++)
            {
                destroy_node(node->children[i], free_values);
            }
            jem_free(node->children);
        }
        else
        {
            jem_free(node->values);
        }

        jem_free(node->keys);
        delete node;
    }

    void dict::clear()
    {
        Node *root = root_.load(std::memory_order_acquire);
        if (root != nullptr)
        {
            destroy_node(root);
            root_ = create_node(true);
        }
    }

    bool dict::empty() const
    {
        Node *root = root_.load(std::memory_order_acquire);
        return root == nullptr || root->num_keys == 0;
    }

    //===============================================================
    // Static Helper Functions
    //===============================================================

    int dict::compare_keys(const void *key1, const void *key2)
    {
        return strcmp(static_cast<const char *>(key1), static_cast<const char *>(key2));
    }

    size_t dict::key_size(const void *key)
    {
        return strlen(static_cast<const char *>(key)) + 1; // Include null terminator
    }

    size_t dict::value_size(const void *value)
    {
        return strlen(static_cast<const char *>(value)) + 1; // Include null terminator
    }

    void *dict::copy_key(const void *key)
    {
        size_t len = key_size(key);
        void *new_key = jem_malloc(len);
        if (new_key)
        {
            memcpy(new_key, key, len);
        }
        return new_key;
    }

    void *dict::copy_value(const void *value)
    {
        size_t len = value_size(value);
        void *new_value = jem_malloc(len);
        if (new_value)
        {
            memcpy(new_value, value, len);
        }
        return new_value;
    }

    int dict::find_index(Node *node, const void *key) const
    {
        int index = 0;

        if (node->is_leaf)
        {
            // For leaf nodes, find the exact position
            while (index < node->num_keys && compare_keys(node->keys[index], key) < 0)
            {
                index++;
            }
        }
        else
        {
            // For internal nodes (routing), use <= comparison for separator routing
            while (index < node->num_keys && compare_keys(node->ikeys[index], key) <= 0)
            {
                index++;
            }
        }

        return index;
    }

    //===============================================================
    // Path Management & Validation
    //===============================================================

    dict::Node *dict::find_leaf_node(const void *key, std::vector<PathEntry> &path) const
    {
        Node *current = root_.load(std::memory_order_acquire);
        if (current == nullptr)
        {
            return nullptr;
        }

        path.clear();

        // Traverse down to leaf level optimistically (without locks)
        while (!current->is_leaf)
        {
            uint32_t version = get_version(current);
            if (is_locked(current))
            {
                // Node is locked, retry from root
                return find_leaf_node(key, path);
            }

            // Find the child where key belongs
            int index = find_index(current, key);

            // Save path entry
            PathEntry entry;
            entry.node = current;
            entry.version = version;
            entry.index = index;
            entry.locked = false;
            path.push_back(entry);

            // Move to next level
            current = current->children[index];

            // Verify the parent hasn't changed
            if (node_changed(path.back().node, path.back().version))
            {
                // Version changed, retry from root
                return find_leaf_node(key, path);
            }
        }

        // Record the leaf node in the path
        uint32_t version = get_version(current);
        if (is_locked(current))
        {
            // Leaf is locked, retry
            return find_leaf_node(key, path);
        }

        PathEntry entry;
        entry.node = current;
        entry.version = version;
        entry.index = find_index(current, key);
        entry.locked = false;
        path.push_back(entry);

        return current;
    }

    bool dict::validate_path(const std::vector<PathEntry> &path) const
    {
        // Verify that none of the nodes in the path have changed
        for (const auto &entry : path)
        {
            if (!entry.locked && node_changed(entry.node, entry.version))
            {
                return false;
            }
        }
        return true;
    }

    void dict::acquire_path_locks(std::vector<PathEntry> &path, int up_to_depth)
    {
        // Lock nodes from leaf up to specified depth
        for (int i = path.size() - 1; i >= (int)path.size() - up_to_depth && i >= 0; i--)
        {
            if (!path[i].locked)
            {
                lock_node(path[i].node);
                path[i].locked = true;

                // Verify that path is still valid after acquiring the lock
                // If parent changed, this might not be the correct node anymore
                if (i > 0 && !path[i - 1].locked && node_changed(path[i - 1].node, path[i - 1].version))
                {
                    release_path_locks(path);
                    throw std::runtime_error("Path invalidated during lock acquisition");
                }
            }
        }
    }

    void dict::release_path_locks(std::vector<PathEntry> &path)
    {
        // Release locks in reverse order (towards root)
        for (auto it = path.rbegin(); it != path.rend(); ++it)
        {
            if (it->locked)
            {
                unlock_node(it->node);
                it->locked = false;
            }
        }
    }

    //===============================================================
    // Core Operations with OCC
    //===============================================================

    void *dict::search(const void *key, size_t key_len) const
    {
        ThreadContext *context = get_thread_context();

        while (true)
        {
            try
            {
                std::vector<PathEntry> &path = context->path;
                Node *leaf = find_leaf_node(key, path);

                if (leaf == nullptr)
                {
                    return nullptr;
                }

                // Search for key in leaf node
                int index = path.back().index;

                // Validate that we found the key
                if (index < leaf->num_keys && compare_keys(leaf->keys[index], key) == 0)
                {
                    return leaf->values[index];
                }
                else
                {
                    return nullptr;
                }
            }
            catch (const std::runtime_error &)
            {
                continue;
            }
        }
    }

    int dict::insert(const void *key, size_t key_len, const void *value, size_t value_len)
    {
        ThreadContext *context = get_thread_context();

        while (true)
        {
            try
            {
                std::vector<PathEntry> &path = context->path;

                // Special case for empty tree
                if (root_.load(std::memory_order_acquire) == nullptr)
                {
                    std::lock_guard<std::mutex> lock(root_mutex_);
                    if (root_.load(std::memory_order_acquire) == nullptr)
                    {
                        Node *new_root = create_node(true);
                        root_.store(new_root, std::memory_order_release);
                    }
                    continue;
                }

                // Check if root needs splitting
                Node *root = root_.load(std::memory_order_acquire);
                if (root->num_keys == root->max_keys)
                {
                    std::lock_guard<std::mutex> lock(root_mutex_);
                    if (root_.load(std::memory_order_acquire)->num_keys == root->max_keys)
                    {
                        handle_root_overflow();
                    }
                    continue;
                }

                // Always start from the root and traverse down,
                // splitting full nodes as you go
                path.clear();
                int result = insert_recursive(root, key, value, path, 0);
                return result;
            }
            catch (const std::runtime_error &)
            {
                // Retry on validation failures
                continue;
            }
        }
    }

    int dict::insert_recursive(Node *node, const void *key, const void *value,
                               std::vector<PathEntry> &path, int depth)
    {
        // Record this node in our path
        uint32_t version = get_version(node);
        if (is_locked(node))
        {
            throw std::runtime_error("Node locked during traversal");
        }

        PathEntry entry;
        entry.node = node;
        entry.version = version;
        entry.index = find_index(node, key);
        entry.locked = false;
        path.push_back(entry);

        // If this is a leaf, we can insert directly
        if (node->is_leaf)
        {
            // Lock leaf for modification
            lock_node(node);
            path.back().locked = true;

            // Validate path to this leaf is still valid
            for (int i = 0; i < path.size() - 1; i++)
            {
                if (node_changed(path[i].node, path[i].version))
                {
                    release_path_locks(path);
                    throw std::runtime_error("Path changed during traversal");
                }
            }

            // Now we can safely modify the leaf
            int idx = path.back().index;

            // Replace existing entry if key already exists
            if (idx < node->num_keys && compare_keys(node->keys[idx], key) == 0)
            {
                // Swap then free (never destroy() a live slot first --
                // transient SODA Set0 leaks ATC)
                void *old_val = static_cast<void *>(node->values[idx]); // pins via ATC
                MemType old_type = node->values[idx].getMemType();
                node->values[idx] = copy_value(value);
                node->values[idx].setHeapId(0); // Set appropriate heap ID if needed
                g_sama->free(old_val, old_type);
                release_path_locks(path);
                return 1;
            }

            // Simple insertion if space available
            if (node->num_keys < node->max_keys)
            {
                // Standard insertion code
                for (int i = node->num_keys - 1; i >= idx; i--)
                {
                    node->keys[i + 1] = node->keys[i];
                    node->values[i + 1] = node->values[i];

                    // Set the original positions to nullptr
                    node->keys[i] = nullptr;
                    node->values[i] = nullptr;
                }

                node->keys[idx] = copy_key(key);
                node->values[idx] = copy_value(value);
                node->values[idx].setHeapId(0); // Set appropriate heap ID if needed
                node->num_keys++;
                release_path_locks(path);
                return 1;
            }

            // Leaf is full, but we should never get here because
            // parent would have split this node during traversal
            release_path_locks(path);
            throw std::runtime_error("Leaf is full - should have been split by parent");
        }

        // This is an internal node - find child where key belongs
        int child_idx = path.back().index;

        // Get child node and check if it's full
        Node *child = node->children[child_idx];

        // If child is full, we need to split it before continuing
        if (child->num_keys == child->max_keys)
        {
            // Lock the current node
            lock_node(node);
            path.back().locked = true;

            // Validate path is still good
            for (int i = 0; i < path.size() - 1; i++)
            {
                if (node_changed(path[i].node, path[i].version))
                {
                    release_path_locks(path);
                    throw std::runtime_error("Path changed during traversal");
                }
            }

            // Double-check child is still full
            if (child->num_keys == child->max_keys)
            {
                // Split the full child
                split_child(node, child_idx);

                // Decide which child to use after split
                if (compare_keys(node->ikeys[child_idx], key) < 0)
                {
                    child_idx++;
                    child = node->children[child_idx];
                }
            }

            // Release lock on current node
            unlock_node(node);
            path.back().locked = false;
        }

        // Verify node hasn't changed during our check
        if (node_changed(node, version))
        {
            throw std::runtime_error("Node changed during traversal");
        }

        // Continue recursion to proper child
        return insert_recursive(child, key, value, path, depth + 1);
    }

    void *dict::remove(const void *key, size_t key_len)
    {
        ThreadContext *context = get_thread_context();

        while (true)
        {
            try
            {
                std::vector<PathEntry> &path = context->path;
                Node *leaf = find_leaf_node(key, path);

                if (leaf == nullptr)
                {
                    return nullptr; // Tree is empty
                }

                // Lock the leaf node
                acquire_path_locks(path, 1);

                // Validate the path is still valid
                if (!validate_path(path))
                {
                    release_path_locks(path);
                    continue; // Retry if path invalid
                }

                // Find the key in the leaf
                int index = path.back().index;

                // If key not found, return nullptr
                if (index >= leaf->num_keys || compare_keys(leaf->keys[index], key) != 0)
                {
                    release_path_locks(path);
                    return nullptr;
                }

                // Save the value to return
                void *old_value = leaf->values[index];

                // If this would create an underflow, we need more locks
                if (leaf->num_keys <= degree_ / 2)
                {
                    try
                    {
                        // Release leaf lock to avoid deadlock
                        release_path_locks(path);

                        // Find path again (might have changed)
                        leaf = find_leaf_node(key, path);

                        // Lock the necessary parts of the path for rebalancing
                        int locks_needed = 2; // Leaf and its parent at minimum

                        acquire_path_locks(path, locks_needed);

                        // Now remove and handle underflows
                        bool removed = remove_from_node(path[path.size() - 1].node, key, path, path.size() - 1);

                        // Check if root became empty
                        Node *root = root_.load(std::memory_order_acquire);
                        if (!root->is_leaf && root->num_keys == 0)
                        {
                            std::lock_guard<std::mutex> lock(root_mutex_);
                            handle_root_underflow();
                        }

                        release_path_locks(path);
                        return removed ? old_value : nullptr;
                    }
                    catch (const std::runtime_error &)
                    {
                        release_path_locks(path);
                        continue; // Path invalidated, retry
                    }
                }
                else
                {
                    // Simple removal, no underflow concerns
                    Node *node = path.back().node;

                    // Free key and value
                    node->keys[index].destroy();
                    void *value_copy = node->values[index];
                    node->values[index] = nullptr;

                    // Shift keys and values to fill the gap
                    for (int i = index + 1; i < node->num_keys; i++)
                    {
                        node->keys[i - 1] = node->keys[i];
                        node->values[i - 1] = node->values[i];

                        // Nullify original position
                        node->keys[i] = nullptr;
                        node->values[i] = nullptr;
                    }

                    node->num_keys--;
                    release_path_locks(path);
                    return value_copy;
                }
            }
            catch (const std::runtime_error &)
            {
                // Path was invalidated, retry
                continue;
            }
        }
    }

    bool dict::remove_from_node(Node *node, const void *key, std::vector<PathEntry> &path, int depth)
    {
        int index = path[depth].index;
        bool key_found = (index < node->num_keys && compare_keys(key_at(node, index), key) == 0);

        // If we're at a leaf node
        if (node->is_leaf)
        {
            if (!key_found)
            {
                return false; // Key not found
            }

            // Free key and value
            node->keys[index].destroy();
            node->values[index].destroy();

            // Shift keys and values to fill the gap
            for (int i = index + 1; i < node->num_keys; i++)
            {
                node->keys[i - 1] = node->keys[i];
                node->values[i - 1] = node->values[i];

                // Nullify original position
                node->keys[i] = nullptr;
                node->values[i] = nullptr;
            }

            node->num_keys--;
            return true;
        }
        // We're at an internal node
        else
        {
            if (key_found)
            {
                // Find successor in leftmost leaf of right subtree
                Node *current = node->children[index + 1];
                while (!current->is_leaf)
                {
                    current = current->children[0];
                }

                // Replace key with successor (first key in leaf).
                // node is interior here: separator is a raw copy.
                jem_free(node->ikeys[index]);
                node->ikeys[index] = copy_key(current->keys[0]);

                // Remove successor from leaf
                if (remove_from_node(node->children[index + 1], current->keys[0], path, depth + 1))
                {
                    // Check if we need to rebalance this child
                    if (node->children[index + 1]->num_keys < degree_ / 2)
                    {
                        redistribute_keys(node, index + 1, true);
                    }
                    return true;
                }
            }
            else
            {
                // Continue search in child
                if (remove_from_node(node->children[index], key, path, depth + 1))
                {
                    // Check if we need to rebalance this child
                    if (node->children[index]->num_keys < degree_ / 2)
                    {
                        redistribute_keys(node, index, false);
                    }
                    return true;
                }
            }

            return false;
        }
    }

    void **dict::range(const void *start_key, const void *end_key, int *count) const
    {
        if (count == nullptr)
        {
            return nullptr;
        }

        *count = 0;
        ThreadContext *context = get_thread_context();

        while (true)
        {
            try
            {
                std::vector<PathEntry> &path = context->path;
                Node *leaf = find_leaf_node(start_key, path);

                if (leaf == nullptr)
                {
                    return nullptr; // Tree is empty
                }

                // Starting index in the first leaf
                int start_idx = path.back().index;
                uint32_t version = get_version(leaf);

                // Initialize result collection
                std::vector<void *> results;
                Node *current_leaf = leaf;
                int current_idx = start_idx;

                while (current_leaf != nullptr)
                {
                    // Capture leaf version for validation
                    uint32_t leaf_version = get_version(current_leaf);

                    if (is_locked(current_leaf))
                    {
                        // Leaf is locked, retry
                        break;
                    }

                    // Collect matching keys from this leaf
                    while (current_idx < current_leaf->num_keys)
                    {
                        // If we've passed end_key, we're done
                        if (end_key != nullptr && compare_keys(current_leaf->keys[current_idx], end_key) > 0)
                        {
                            goto validation;
                        }

                        results.push_back(current_leaf->values[current_idx]);
                        current_idx++;
                    }

                    // Move to next leaf
                    current_leaf = current_leaf->next;
                    current_idx = 0;

                    // Validate current leaf hasn't changed
                    if (current_leaf && node_changed(current_leaf, leaf_version))
                    {
                        // Leaf changed during scan, retry
                        break;
                    }
                }

            validation:
                // Validate initial leaf hasn't changed
                if (node_changed(leaf, version))
                {
                    // Retry entire operation
                    continue;
                }

                // Copy results to return array
                *count = results.size();
                if (*count == 0)
                {
                    return nullptr;
                }

                void **result_array = static_cast<void **>(jem_malloc(sizeof(void *) * (*count)));
                for (int i = 0; i < *count; i++)
                {
                    result_array[i] = results[i];
                }

                return result_array;
            }
            catch (const std::runtime_error &)
            {
                // Path was invalidated, retry
                continue;
            }
        }
    }

    //===============================================================
    // Tree Modification Helpers
    //===============================================================

    void dict::handle_root_overflow()
    {
        Node *old_root = root_.load(std::memory_order_acquire);
        lock_node(old_root);

        // Double-check condition after acquiring lock
        if (old_root->num_keys < old_root->max_keys)
        {
            unlock_node(old_root);
            return;
        }

        Node *new_root = create_node(false);
        new_root->children[0] = old_root;

        // Split the old root
        split_child(new_root, 0);

        // Publish the new root
        root_.store(new_root, std::memory_order_release);

        unlock_node(old_root);
    }

    void dict::handle_root_underflow()
    {
        Node *old_root = root_.load(std::memory_order_acquire);

        // If root is a leaf, we're done
        if (old_root->is_leaf)
        {
            return;
        }

        // Root has no keys but has a single child - make child the new root
        Node *new_root = old_root->children[0];
        root_.store(new_root, std::memory_order_release);

        // Free old root (but don't free values)
        old_root->num_keys = 0;
        schedule_for_deletion(old_root);
    }

    void dict::split_child(Node *parent, int index)
    {
        Node *child = parent->children[index];
        Node *new_node = create_node(child->is_leaf);

        // Calculate mid point
        int mid = degree_ / 2;

        // Set up new node
        new_node->num_keys = child->num_keys - mid;

        // Copy keys/values from right half of child to new node
        for (int i = 0; i < new_node->num_keys; i++)
        {
            if (child->is_leaf)
            {
                new_node->keys[i] = child->keys[mid + i];
                new_node->values[i] = child->values[mid + i];
                // Nullify in child
                child->keys[mid + i] = nullptr;
                child->values[mid + i] = nullptr;
            }
            else
            {
                new_node->ikeys[i] = child->ikeys[mid + i];
            }
        }

        // If internal node, copy children pointers
        if (!child->is_leaf)
        {
            for (int i = 0; i <= new_node->num_keys; i++)
            {
                new_node->children[i] = child->children[mid + i];
            }

            // Nullify in child
            for (int i = 0; i < new_node->num_keys; i++)
            {
                child->ikeys[mid + i] = nullptr;
            }
        }

        // Update leaf node linked list if needed
        if (child->is_leaf)
        {
            new_node->next = child->next;
            if (new_node->next)
            {
                new_node->next->prev = new_node;
            }
            child->next = new_node;
            new_node->prev = child;
        }

        // Update child's key count
        child->num_keys = mid;

        // Make space in parent for new separator key and child pointer
        // (parent is always interior)
        for (int i = parent->num_keys; i > index; i--)
        {
            parent->children[i + 1] = parent->children[i];
            parent->ikeys[i] = parent->ikeys[i - 1];

            // Nullify original position
            parent->ikeys[i - 1] = nullptr;
        }

        // Add new child to parent
        parent->children[index + 1] = new_node;

        parent->ikeys[index] = copy_key(key_at(new_node, 0));

        parent->num_keys++;
    }

    void dict::redistribute_keys(Node *parent, int index, bool from_next)
    {
        Node *child = parent->children[index];

        // Try to borrow from siblings
        if (from_next && index < parent->num_keys)
        {
            // Borrow from right sibling
            Node *right_sibling = parent->children[index + 1];

            if (right_sibling->num_keys > degree_ / 2)
            {
                // Save parent separator key (raw; direct transfer)
                void *parent_key = parent->ikeys[index];
                parent->ikeys[index] = nullptr; // Nullify original

                if (child->is_leaf)
                {
                    // For leaf nodes:
                    // 1. Move parent key to child (re-guided as a leaf key)
                    child->keys[child->num_keys] = parent_key;

                    // 2. Move first value from right sibling to child
                    child->values[child->num_keys] = right_sibling->values[0];
                    right_sibling->values[0] = nullptr; // Nullify original

                    // 3. Update parent with first key from right sibling (but keep the original in right sibling)
                    parent->ikeys[index] = copy_key(right_sibling->keys[0]);
                }
                else
                {
                    // For internal nodes:
                    // 1. Move parent key to child
                    child->ikeys[child->num_keys] = parent_key;

                    // 2. Move first child pointer from right sibling to child
                    child->children[child->num_keys + 1] = right_sibling->children[0];

                    // 3. Move first key from right sibling to parent (direct transfer)
                    parent->ikeys[index] = right_sibling->ikeys[0];
                    right_sibling->ikeys[0] = nullptr; // Nullify original
                }

                child->num_keys++;

                // Shift keys and values/children in right sibling
                if (child->is_leaf)
                {
                    for (int i = 0; i < right_sibling->num_keys - 1; i++)
                    {
                        right_sibling->keys[i] = right_sibling->keys[i + 1];
                        right_sibling->values[i] = right_sibling->values[i + 1];

                        // Nullify original positions
                        right_sibling->keys[i + 1] = nullptr;
                        right_sibling->values[i + 1] = nullptr;
                    }
                }
                else
                {
                    for (int i = 0; i < right_sibling->num_keys - 1; i++)
                    {
                        right_sibling->ikeys[i] = right_sibling->ikeys[i + 1];
                        right_sibling->ikeys[i + 1] = nullptr; // Nullify
                    }

                    for (int i = 0; i < right_sibling->num_keys; i++)
                    {
                        right_sibling->children[i] = right_sibling->children[i + 1];
                    }
                    right_sibling->children[right_sibling->num_keys] = nullptr; // Nullify last child
                }

                right_sibling->num_keys--;
                return;
            }
        }
        else if (!from_next && index > 0)
        {
            // Borrow from left sibling
            Node *left_sibling = parent->children[index - 1];

            if (left_sibling->num_keys > degree_ / 2)
            {
                // Save parent separator key (raw; direct transfer)
                void *parent_key = parent->ikeys[index - 1];
                parent->ikeys[index - 1] = nullptr; // Nullify original

                // Make space in child
                if (child->is_leaf)
                {
                    for (int i = child->num_keys; i > 0; i--)
                    {
                        child->keys[i] = child->keys[i - 1];
                        child->values[i] = child->values[i - 1];

                        // Nullify original positions
                        child->keys[i - 1] = nullptr;
                        child->values[i - 1] = nullptr;
                    }
                }
                else
                {
                    for (int i = child->num_keys; i > 0; i--)
                    {
                        child->ikeys[i] = child->ikeys[i - 1];
                        child->ikeys[i - 1] = nullptr; // Nullify
                    }

                    for (int i = child->num_keys + 1; i > 0; i--)
                    {
                        child->children[i] = child->children[i - 1];
                    }
                }

                if (child->is_leaf)
                {
                    // For leaf nodes:
                    // 1. Move parent key to child (re-guided as a leaf key)
                    child->keys[0] = parent_key;

                    // 2. Move last value from left sibling to child
                    child->values[0] = left_sibling->values[left_sibling->num_keys - 1];
                    left_sibling->values[left_sibling->num_keys - 1] = nullptr; // Nullify

                    // 3. Update parent with last key from left sibling (but keep original in left sibling)
                    parent->ikeys[index - 1] = copy_key(left_sibling->keys[left_sibling->num_keys - 1]);
                }
                else
                {
                    // For internal nodes:
                    // 1. Move parent key to child
                    child->ikeys[0] = parent_key;

                    // 2. Move last child pointer from left sibling to child
                    child->children[0] = left_sibling->children[left_sibling->num_keys];
                    left_sibling->children[left_sibling->num_keys] = nullptr; // Nullify

                    // 3. Move last key from left sibling to parent (direct transfer)
                    parent->ikeys[index - 1] = left_sibling->ikeys[left_sibling->num_keys - 1];
                    left_sibling->ikeys[left_sibling->num_keys - 1] = nullptr; // Nullify
                }

                child->num_keys++;
                left_sibling->num_keys--;
                return;
            }
        }

        // If borrowing fails, merge nodes
        if (from_next && index < parent->num_keys)
        {
            merge_nodes(parent, index);
        }
        else if (index > 0)
        {
            merge_nodes(parent, index - 1);
        }
    }

    void dict::merge_nodes(Node *parent, int index)
    {
        Node *left_child = parent->children[index];
        Node *right_child = parent->children[index + 1];

        // 1. Move parent's key to left child (direct transfer)
        put_key(left_child, left_child->num_keys, parent->ikeys[index]);
        parent->ikeys[index] = nullptr; // Nullify original

        // 2. Copy all keys and values from right child to left child (direct
        //    transfers; both children are always the same kind)
        for (int i = 0; i < right_child->num_keys; i++)
        {
            if (left_child->is_leaf)
            {
                left_child->keys[left_child->num_keys + 1 + i] = right_child->keys[i];
                right_child->keys[i] = nullptr; // Nullify original

                left_child->values[left_child->num_keys + 1 + i] = right_child->values[i];
                right_child->values[i] = nullptr; // Nullify original
            }
            else
            {
                left_child->ikeys[left_child->num_keys + 1 + i] = right_child->ikeys[i];
                right_child->ikeys[i] = nullptr; // Nullify original
            }
        }

        // 3. Copy child pointers if not leaf nodes (direct transfers)
        if (!left_child->is_leaf)
        {
            for (int i = 0; i <= right_child->num_keys; i++)
            {
                left_child->children[left_child->num_keys + 1 + i] = right_child->children[i];
                right_child->children[i] = nullptr; // Nullify original
            }
        }
        else
        {
            // Update leaf node linked list
            left_child->next = right_child->next;
            if (right_child->next)
            {
                right_child->next->prev = left_child;
            }
        }

        // 4. Update left child's key count
        left_child->num_keys += 1 + right_child->num_keys;

        // 5. Shift parent keys and child pointers (direct transfers)
        for (int i = index + 1; i < parent->num_keys; i++)
        {
            parent->ikeys[i - 1] = parent->ikeys[i];
            parent->ikeys[i] = nullptr; // Nullify original
            parent->children[i] = parent->children[i + 1];
        }

        // 6. Clear the last child pointer
        parent->children[parent->num_keys] = nullptr;

        parent->num_keys--;

        // 7. Mark right child with zero keys (prevent double-free)
        right_child->num_keys = 0;

        // 8. Schedule right child for deletion
        schedule_for_deletion(right_child);
    }


    /*
     * Range scan: optimistic leaf-chain walk with per-node version
     * validation (same discipline as range()). A node observed locked or
     * changed mid-read restarts the whole scan; copies are made inside
     * this public operation's TAG scope, so discarded copies were still
     * read from pinned objects.
     */
    int dict::scanCopy(const void *start_key, size_t key_len, int n,
                       std::vector<std::pair<std::string, std::string>> &out)
    {
        ThreadContext *context = get_thread_context();
        while (true)
        {
            out.clear();
            std::vector<PathEntry> &path = context->path;
            Node *leaf = find_leaf_node(start_key, path);
            if (leaf == nullptr)
            {
                return 0; // empty tree
            }

            bool retry = false;
            Node *cur = leaf;
            while (cur != nullptr && (int)out.size() < n)
            {
                uint32_t version = get_version(cur);
                if (is_locked(cur))
                {
                    retry = true;
                    break;
                }
                for (int i = 0; i < cur->num_keys && (int)out.size() < n; i++)
                {
                    if (compare_keys(key_at(cur, i), start_key) < 0)
                    {
                        continue; // only the first leaf can hold smaller keys
                    }
                    out.emplace_back(static_cast<char *>(key_at(cur, i)),
                                     static_cast<char *>(static_cast<void *>(cur->values[i])));
                }
                Node *next = cur->next;
                if (node_changed(cur, version))
                {
                    retry = true;
                    break;
                }
                cur = next;
            }
            if (!retry)
            {
                return (int)out.size();
            }
            std::this_thread::yield();
        }
    }

    /*
     * GET-path helper: copy the value out while still inside the public
     * data-structure operation. search() returns a raw pointer whose object
     * can be migrated (and freed) as soon as the operation's TAG scope ends,
     * so callers must never dereference it after return.
     */
    int dict::searchCopy(const void *key, size_t key_len, std::string &out)
    {
        void *v = search(key, key_len);
        if (v == nullptr)
        {
            return 0;
        }
        out.assign(static_cast<const char *>(v));
        return 1;
    }

} // namespace bpt_occ