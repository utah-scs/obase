#include "bpt_seq.h"
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <algorithm>

namespace bpt_seq
{

    // Constructor
    dict::dict(int degree) : root_(nullptr), degree_(degree)
    {
        if (degree < 3)
        {
            degree_ = 3; // Minimum degree for B+ tree
        }

        // Create an empty leaf node as root
        root_ = create_node(true);
    }

    // Destructor
    dict::~dict()
    {
        clear();
    }

    // Utility functions
    bool dict::empty() const
    {
        return root_ == nullptr || root_->num_keys == 0;
    }

    void dict::clear()
    {
        if (root_ != nullptr)
        {
            destroy_node(root_);
            root_ = create_node(true);
        }
    }

    // Static helper functions for string keys and values
    int dict::compare_keys(const void *key1, const void *key2)
    {
        return strcmp((const char *)key1, (const char *)key2);
    }

    size_t dict::key_size(const void *key)
    {
        return strlen((const char *)key) + 1; // Include null terminator
    }

    size_t dict::value_size(const void *value)
    {
        return strlen((const char *)value) + 1; // Include null terminator
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

    // Node management
    void *dict::key_at(Node *n, int i)
    {
        return n->is_leaf ? static_cast<void *>(n->keys[i]) : n->ikeys[i];
    }

    void *dict::take_key(Node *n, int i)
    {
        void *k;
        if (n->is_leaf)
        {
            k = static_cast<void *>(n->keys[i]);
            n->keys[i] = nullptr;
        }
        else
        {
            k = n->ikeys[i];
            n->ikeys[i] = nullptr;
        }
        return k;
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
        Node *node = new Node;
        node->is_leaf = is_leaf;
        node->num_keys = 0;
        node->max_keys = degree_ - 1;

        if (is_leaf)
        {
            node->keys = (Guide<void> *)jem_malloc(sizeof(Guide<void>) * node->max_keys);

            // Initialize each Guide object
            for (int i = 0; i < node->max_keys; i++)
            {
                new (&node->keys[i]) Guide<void>(); // Placement new to call constructor
            }

            // node->values = (void **)jem_malloc(sizeof(void *) * node->max_keys);
            node->values = (Guide<void> *)jem_malloc(sizeof(Guide<void>) * node->max_keys);

            // Initialize each value object
            for (int i = 0; i < node->max_keys; i++)
            {
                new (&node->values[i]) Guide<void>(); // Placement new to call constructor
            }

            node->next = nullptr;
        }
        else
        {
            // Raw separator copies -- deliberately NOT guided (see bpt_seq.h)
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

    // Find the index where key should be inserted in node
    int dict::find_index(Node *node, const void *key) const
    {
        int index = 0;

        // Find the first key greater than or equal to the search key
        while (index < node->num_keys &&
               compare_keys(key_at(node, index), key) < 0)
        {
            index++;
        }

        return index;
    }

    // Find the leaf node where key would belong
    dict::Node *dict::find_leaf_node(const void *key) const
    {
        if (root_ == nullptr)
        {
            return nullptr;
        }

        Node *current = root_;

        // Traverse down to leaf level
        while (!current->is_leaf)
        {
            int index = find_index(current, key);

            // If the key is found, move to the right child
            if (index < current->num_keys &&
                compare_keys(current->ikeys[index], key) == 0)
            {
                index++;
            }

            current = current->children[index];
        }

        return current;
    }

    // Core Operations

    // Search for a key in the B+ tree
    void *dict::search(const void *key, size_t key_len) const
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        if (root_ == nullptr)
        {
            return nullptr;
        }

        Node *leaf = find_leaf_node(key);

        // Search for key in leaf node
        int index = 0;
        while (index < leaf->num_keys &&
               compare_keys(leaf->keys[index], key) < 0)
        {
            index++;
        }

        // Return value if key found, otherwise return nullptr
        if (index < leaf->num_keys &&
            compare_keys(leaf->keys[index], key) == 0)
        {
            return leaf->values[index];
        }

        return nullptr;
    }

    // Insert a key-value pair into the B+ tree
    int dict::insert(const void *key, size_t key_len, const void *value, size_t value_len)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        // If tree is empty, create root
        if (root_ == nullptr)
        {
            root_ = create_node(true);
            if (root_ == nullptr)
            {
                return 0; // Failed to create root node
            }
        }

        // If root is full, handle root overflow before insertion
        if (root_->num_keys == root_->max_keys)
        {
            handle_root_overflow();
            if (root_ == nullptr || root_->children[0] == nullptr)
            {
                return 0; // Something went wrong during root overflow handling
            }
        }

        // Insert non-full
        return insert_non_full(root_, key, value);
    }

    // Split a child node during insertion
    void dict::split_child(Node *parent, int index)
    {
        Node *child = parent->children[index];
        Node *new_node = create_node(child->is_leaf);

        // Set up new node
        new_node->num_keys = (degree_ - 1) / 2;

        // Copy keys and values/children to new node
        int mid = degree_ / 2;

        // First, save the middle key for non-leaf splits
        void *middle_key = nullptr;
        if (!child->is_leaf)
        {
            middle_key = copy_key(child->ikeys[mid]);
        }

        // Copy the right half of the keys to the new node
        for (int i = 0; i < new_node->num_keys; i++)
        {
            // Only nullify immediately for leaf nodes
            // For non-leaf nodes, we'll nullify after we've handled the middle key
            if (child->is_leaf)
            {
                new_node->keys[i] = child->keys[mid + i];
                child->keys[mid + i] = nullptr;
                new_node->values[i] = child->values[mid + i];
                child->values[mid + i] = nullptr;
            }
            else
            {
                new_node->ikeys[i] = child->ikeys[mid + i];
            }
        }

        // Copy children pointers if not a leaf
        if (!child->is_leaf)
        {
            for (int i = 0; i <= new_node->num_keys; i++)
            {
                new_node->children[i] = child->children[mid + i];
            }

            // Now it's safe to nullify the keys in the old node
            for (int i = 0; i < new_node->num_keys; i++)
            {
                child->ikeys[mid + i] = nullptr;
            }
        }

        // Update leaf node linked list
        if (child->is_leaf)
        {
            new_node->next = child->next;
            child->next = new_node;
        }

        // Update child's key count
        child->num_keys = mid;

        // Insert the middle key into the parent
        // For non-leaf nodes, we shift everything right to make space
        for (int i = parent->num_keys; i > index; i--)
        {
            parent->children[i + 1] = parent->children[i];
        }

        parent->children[index + 1] = new_node;

        // For B+ trees with leaf splits, we need to copy the first key of new node
        if (child->is_leaf)
        {
            middle_key = copy_key(new_node->keys[0]);
        }

        // Make space for the new key in parent (parent is always interior)
        for (int i = parent->num_keys - 1; i >= index; i--)
        {
            parent->ikeys[i + 1] = parent->ikeys[i];
        }

        parent->ikeys[index] = middle_key;
        parent->num_keys++;
    }

    int dict::insert_non_full(Node *node, const void *key, const void *value)
    {
        int index = find_index(node, key);

        // Handle leaf node insertion
        if (node->is_leaf)
        {
            // Check if key already exists
            if (index < node->num_keys && compare_keys(node->keys[index], key) == 0)
            {
                // Replace value: swap then free (never destroy() a live
                // slot first -- transient SODA Set0 leaks ATC)
                void *old_val = static_cast<void *>(node->values[index]); // pins via ATC
                MemType old_type = node->values[index].getMemType();
                node->values[index] = copy_value(value);
                node->values[index].setHeapId(0);
                if (node->values[index] == nullptr)
                {
                    return 0; // Failed to allocate new value
                }
                g_sama->free(old_val, old_type);
                return 1; // Successfully updated
            }

            // Shift keys and values to make room for new entry
            for (int i = node->num_keys - 1; i >= index; i--)
            {
                node->keys[i + 1] = node->keys[i];
                node->values[i + 1] = node->values[i];

                node->keys[i] = nullptr;
                node->values[i] = nullptr;
            }

            // Insert new key and value
            node->keys[index] = copy_key(key);
            if (node->keys[index] == nullptr)
            {
                return 0; // Failed to allocate new key
            }

            node->values[index] = copy_value(value);
            if (node->values[index] == nullptr)
            {
                node->keys[index].destroy();
                // Shift everything back
                for (int i = index; i < node->num_keys; i++)
                {
                    node->keys[i] = node->keys[i + 1];
                    node->values[i] = node->values[i + 1];
                }
                return 0; // Failed to allocate new value
            }

            node->num_keys++;
            return 1; // Successfully inserted
        }
        // Handle internal node insertion
        else
        {
            // Find the child where the key belongs
            if (index < node->num_keys && compare_keys(node->ikeys[index], key) == 0)
            {
                index++; // If key exists, go to the right child
            }

            // Check if child is full
            if (node->children[index]->num_keys == node->children[index]->max_keys)
            {
                // Split child before insertion
                split_child(node, index);

                // After split, the new key might go into a different child
                if (compare_keys(node->ikeys[index], key) < 0)
                {
                    index++;
                }
            }

            // Recursively insert into the appropriate child
            return insert_non_full(node->children[index], key, value);
        }
    }

    // Handle the case when root node is full
    void dict::handle_root_overflow()
    {
        Node *old_root = root_;
        root_ = create_node(false);
        root_->children[0] = old_root;

        // Split the old root
        split_child(root_, 0);
    }

    // Remove a key from the B+ tree
    void *dict::remove(const void *key, size_t key_len)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        if (root_ == nullptr)
        {
            return nullptr;
        }

        bool result = remove_from_node(root_, key);

        // Handle case when root becomes empty
        if (root_->num_keys == 0)
        {
            handle_root_underflow();
        }


        // To be compatible with ProcessCommand returning key
        if (result)
            return copy_key(key);
        else
            return nullptr;
    }

    // Handle the case when root node becomes empty
    void dict::handle_root_underflow()
    {
        if (root_->is_leaf)
        {
            // Root is a leaf and empty - tree is empty
            return;
        }

        // Root has no keys but has a single child - make child the new root
        Node *old_root = root_;
        root_ = root_->children[0];

        // Free old root (but don't free values)
        old_root->num_keys = 0;
        destroy_node(old_root, false);
    }

    // Remove key from a node recursively
    bool dict::remove_from_node(Node *node, const void *key)
    {
        int index = find_index(node, key);
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

                // Set the original position to nullptr after copying
                // This is safe because we're working left-to-right and not using those values anymore
                node->keys[i] = nullptr;
                node->values[i] = nullptr;
            }

            node->num_keys--;
            return true;
        }

        // We're at an internal node
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

            // Remove from right child
            if (remove_from_node(node->children[index + 1], current->keys[0]))
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
            if (remove_from_node(node->children[index], key))
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

    // Redistribute keys between siblings or merge nodes
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
                // Move a key from parent to child (ownership transfer)
                put_key(child, child->num_keys, parent->ikeys[index]);

                if (child->is_leaf)
                {
                    // For leaf nodes, move values directly
                    child->values[child->num_keys] = right_sibling->values[0];
                    parent->ikeys[index] = copy_key(right_sibling->keys[1]);
                }
                else
                {
                    // For internal nodes, move child pointers as well
                    child->children[child->num_keys + 1] = right_sibling->children[0];
                    parent->ikeys[index] = right_sibling->ikeys[0];
                    right_sibling->ikeys[0] = nullptr;
                }

                child->num_keys++;

                // Shift keys in right sibling
                for (int i = 1; i < right_sibling->num_keys; i++)
                {
                    if (right_sibling->is_leaf)
                    {
                        right_sibling->keys[i - 1] = right_sibling->keys[i];
                        right_sibling->values[i - 1] = right_sibling->values[i];

                        // Nullify original position
                        right_sibling->keys[i] = nullptr;
                        right_sibling->values[i] = nullptr;
                    }
                    else
                    {
                        right_sibling->ikeys[i - 1] = right_sibling->ikeys[i];
                        right_sibling->ikeys[i] = nullptr;
                    }
                }

                if (!right_sibling->is_leaf)
                {
                    for (int i = 1; i <= right_sibling->num_keys; i++)
                    {
                        right_sibling->children[i - 1] = right_sibling->children[i];
                    }
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
                // Make space in child
                for (int i = child->num_keys; i > 0; i--)
                {
                    if (child->is_leaf)
                    {
                        child->keys[i] = child->keys[i - 1];
                        child->values[i] = child->values[i - 1];

                        // Nullify original position
                        child->keys[i - 1] = nullptr;
                        child->values[i - 1] = nullptr;
                    }
                    else
                    {
                        child->ikeys[i] = child->ikeys[i - 1];
                        child->ikeys[i - 1] = nullptr;
                    }
                }

                if (!child->is_leaf)
                {
                    for (int i = child->num_keys + 1; i > 0; i--)
                    {
                        child->children[i] = child->children[i - 1];
                    }
                }

                // Move a key from parent to child (ownership transfer)
                put_key(child, 0, parent->ikeys[index - 1]);

                if (child->is_leaf)
                {
                    // For leaf nodes, move values directly
                    child->values[0] = left_sibling->values[left_sibling->num_keys - 1];
                    parent->ikeys[index - 1] = copy_key(left_sibling->keys[left_sibling->num_keys - 1]);
                }
                else
                {
                    // For internal nodes, move child pointers as well
                    child->children[0] = left_sibling->children[left_sibling->num_keys];
                    parent->ikeys[index - 1] = left_sibling->ikeys[left_sibling->num_keys - 1];
                    left_sibling->ikeys[left_sibling->num_keys - 1] = nullptr;
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

    // Merge two adjacent child nodes
    void dict::merge_nodes(Node *parent, int index)
    {
        Node *left_child = parent->children[index];
        Node *right_child = parent->children[index + 1];

        // Move parent's key to left child (ownership transfer)
        put_key(left_child, left_child->num_keys, parent->ikeys[index]);

        // Copy all keys and values from right child to left child
        // (left and right children are always the same kind)
        for (int i = 0; i < right_child->num_keys; i++)
        {
            if (left_child->is_leaf)
            {
                left_child->keys[left_child->num_keys + 1 + i] = right_child->keys[i];
                left_child->values[left_child->num_keys + 1 + i] = right_child->values[i];
            }
            else
            {
                left_child->ikeys[left_child->num_keys + 1 + i] = right_child->ikeys[i];
                right_child->ikeys[i] = nullptr;
            }
        }

        // Copy child pointers if not leaf nodes
        if (!left_child->is_leaf)
        {
            for (int i = 0; i <= right_child->num_keys; i++)
            {
                left_child->children[left_child->num_keys + 1 + i] = right_child->children[i];
            }
        }
        else
        {
            // Update leaf node linked list
            left_child->next = right_child->next;
        }

        // Update left child's key count
        left_child->num_keys += 1 + right_child->num_keys;

        // Prevent right_child values from being freed when we destroy it
        right_child->num_keys = 0;

        // Remove parent key and shift everything left
        for (int i = index + 1; i < parent->num_keys; i++)
        {
            parent->ikeys[i - 1] = parent->ikeys[i];
            parent->children[i] = parent->children[i + 1];

            // Nullify original position
            parent->ikeys[i] = nullptr;
        }
        // Nullify the last position
        parent->children[parent->num_keys] = nullptr;

        parent->num_keys--;

        // Destroy right child (but not its values as they were moved)
        destroy_node(right_child, false);
    }

    // Range query - returns array of values for keys in range [start_key, end_key]
    void **dict::range(const void *start_key, const void *end_key, int *count) const
    {
        if (root_ == nullptr || count == nullptr)
        {
            if (count)
                *count = 0;
            return nullptr;
        }

        // Find leaf node containing start key
        Node *leaf = find_leaf_node(start_key);
        if (leaf == nullptr)
        {
            *count = 0;
            return nullptr;
        }

        // Collect all values in range
        void **result = nullptr;
        int capacity = 0;
        int size = 0;

        // Continue until we reach the end of the range or the end of the tree
        while (leaf != nullptr)
        {
            for (int i = 0; i < leaf->num_keys; i++)
            {
                // Skip keys smaller than start_key
                if (compare_keys(leaf->keys[i], start_key) < 0)
                {
                    continue;
                }

                // Stop if we've reached end_key
                if (compare_keys(leaf->keys[i], end_key) > 0)
                {
                    *count = size;
                    return result;
                }

                // Add value to result array
                if (size >= capacity)
                {
                    capacity = capacity == 0 ? 4 : capacity * 2;
                    void **new_result = (void **)realloc(result, capacity * sizeof(void *));
                    if (new_result == nullptr)
                    {
                        jem_free(result);
                        *count = 0;
                        return nullptr;
                    }
                    result = new_result;
                }

                result[size++] = leaf->values[i];
            }

            // Move to next leaf
            leaf = leaf->next;
        }

        *count = size;
        return result;
    }


    /*
     * Range scan: descend to the leaf containing start_key, then walk the
     * leaf chain copying up to n key/value pairs. Copies happen inside this
     * public operation's TAG scope. The tree is the coarse-lock variant, so
     * the walk holds the global lock.
     */
    int dict::scanCopy(const void *start_key, size_t key_len, int n,
                       std::vector<std::pair<std::string, std::string>> &out)
    {
        std::lock_guard<std::mutex> guard(op_mutex);
        Node *leaf = find_leaf_node(start_key);
        int copied = 0;
        while (leaf != nullptr && copied < n)
        {
            for (int i = 0; i < leaf->num_keys && copied < n; i++)
            {
                if (compare_keys(key_at(leaf, i), start_key) < 0)
                {
                    continue; // only the first leaf can hold smaller keys
                }
                out.emplace_back(static_cast<char *>(key_at(leaf, i)),
                                 static_cast<char *>(static_cast<void *>(leaf->values[i])));
                copied++;
            }
            leaf = leaf->next;
        }
        return copied;
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

} // namespace bpt_seq