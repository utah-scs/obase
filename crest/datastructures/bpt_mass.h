// masstree.h
#ifndef BPT_MASS_MASSTREE_H
#define BPT_MASS_MASSTREE_H

#include <atomic>
#include <string>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <algorithm>
#include <cassert>
#include <optional>
#include <utility>

#include "Guide.hpp"

namespace bpt_mass
{

    // Forward declarations
    class Node;
    class InteriorNode;
    class BorderNode;
    class BigSuffix;
    class Value;

    // Memory order constants for atomic operations
    static constexpr std::memory_order READ_MEMORY_ORDER = std::memory_order_acquire;
    static constexpr std::memory_order WRITE_MEMORY_ORDER = std::memory_order_release;
    static constexpr size_t NODE_ORDER = 16; // B+ tree order

    /**
     * Version structure for optimistic concurrency control
     */
    struct Version
    {
        union
        {
            uint32_t body;
            struct
            {
                uint16_t v_split : 16; // Split counter
                uint8_t v_insert : 8;  // Insert counter
                bool : 2;              // Padding bits
                bool is_border : 1;    // Is this a border node
                bool is_root : 1;      // Is this a root node
                bool deleted : 1;      // Is this node deleted
                bool splitting : 1;    // Is this node currently splitting
                bool inserting : 1;    // Is this node currently inserting
                bool locked : 1;       // Is this node locked
            };
        };

        // Constant for locked version check
        static constexpr uint32_t has_locked = 0;

        // Constructor with default values
        Version() noexcept;

        // Check if split happened between two versions
        static bool splitHappened(const Version &before, const Version &after);

        // XOR operator to check version differences
        uint32_t operator^(const Version &rhs) const;
    };

    /**
     * Key slice representation - 64-bit chunks of the key
     */
    typedef uint64_t KeySlice;

    /**
     * Slice with size information
     */
    struct SliceWithSize
    {
        KeySlice slice;
        uint8_t size;

        SliceWithSize(KeySlice slice_, uint8_t size_);
        bool operator==(const SliceWithSize &rhs) const;
        bool operator!=(const SliceWithSize &rhs) const;
    };

    /**
     * Key representation for variable-length keys
     */
    class Key
    {
    public:
        std::vector<KeySlice> slices;
        size_t lastSliceSize;
        size_t cursor;

        Key();
        Key(std::vector<KeySlice> slices_, size_t lastSliceSize_) noexcept;
        Key(const Key &other);
        Key(Key &&other) noexcept;
        Key &operator=(const Key &other);
        Key &operator=(Key &&other) noexcept;

        // Create a key from raw data
        static Key fromBytes(const void *data, size_t length);

        // Check if there are more slices after current cursor
        bool hasNext() const;

        // Get remaining key length
        size_t remainLength(size_t from) const;

        // Get current slice size
        size_t getCurrentSliceSize() const;

        // Get current slice with size
        SliceWithSize getCurrentSlice() const;

        // Move to next slice
        void next();

        // Move to previous slice
        void back();

        // Reset cursor to beginning
        void reset();

        // Comparison operators
        bool operator==(const Key &rhs) const;
        bool operator!=(const Key &rhs) const;
    };

    /**
     * Value representation
     */
    class Value
    {
    public:
        Value(void *data, size_t length);
        ~Value();

        void *getData();
        size_t getLength() const;

    private:
        Guide<void> data;
        size_t length;
    };

    /**
     * Link or Value union (points to next layer or value)
     */
    union LinkOrValue
    {
        LinkOrValue();
        explicit LinkOrValue(Node *next);
        explicit LinkOrValue(Value *value_);
        LinkOrValue(const LinkOrValue &other);

        Node *next_layer;
        Value *value;
    };

    /**
     * Permutation for border nodes (maintains key order)
     */
    class Permutation
    {
    public:
        Permutation();
        Permutation(const Permutation &other);
        Permutation &operator=(const Permutation &other);

        // Get number of keys in the node
        uint8_t getNumKeys() const;

        // Set number of keys
        void setNumKeys(size_t num);

        // Increment key count
        void incNumKeys();

        // Decrement key count
        void decNumKeys();

        // Get key index at position
        uint8_t getKeyIndex(size_t i) const;

        // Set key index at position
        void setKeyIndex(size_t i, uint8_t true_index);

        // Remove index from permutation
        void removeIndex(uint8_t true_index);

        // Operator to get key index
        uint8_t operator()(size_t i) const;

        // Check if permutation is not full
        bool isNotFull() const;

        // Check if permutation is full
        bool isFull() const;

        // Insert an element at a specific point
        void insert(size_t insertion_point_ps, size_t index_ts);

        // Create a permutation with one element
        static Permutation sizeOne();

        // Create a sorted permutation
        static Permutation fromSorted(size_t n_keys);

        // Create from vector
        static Permutation from(const std::vector<size_t> &vec);

    private:
        uint64_t body;
    };

    /**
     * Big suffix for handling long keys
     */
    class BigSuffix
    {
    public:
        BigSuffix();
        BigSuffix(const BigSuffix &other);
        BigSuffix(std::vector<KeySlice> &&slices_, size_t lastSliceSize_);
        ~BigSuffix();

        SliceWithSize getCurrentSlice();
        size_t remainLength();
        bool hasNext();
        void next();
        void insertTop(KeySlice slice);
        bool isSame(const Key &key, size_t from);

        // Copy the stored slices (scan key reconstruction; takes the
        // suffix mutex, mutates nothing)
        void copySlices(std::vector<KeySlice> &out_slices, size_t &out_lastSliceSize);

        static BigSuffix *from(const Key &key, size_t from);

    private:
        std::mutex suffixMutex;
        std::vector<KeySlice> slices;
        const size_t lastSliceSize;
    };

    /**
     * Key suffix management for border nodes
     */
    class KeySuffix
    {
    public:
        KeySuffix();

        // Set suffix from key
        void set(size_t i, const Key &key, size_t from);

        // Set suffix pointer
        void set(size_t i, BigSuffix *const &ptr);

        // Get suffix pointer
        BigSuffix *get(size_t i) const;

        // Remove reference to suffix
        void unref(size_t i);

        // Delete suffix pointer
        void delete_ptr(size_t i);

        // Delete all suffixes
        void deleteAll();

        // Reset all suffixes
        void reset();

    private:
        std::array<std::atomic<BigSuffix *>, NODE_ORDER - 1> suffixes;
    };

    /**
     * Garbage collector for safe memory management
     */
    class GarbageCollector
    {
    public:
        GarbageCollector();
        GarbageCollector(GarbageCollector &&other) = delete;
        GarbageCollector(const GarbageCollector &other) = delete;
        GarbageCollector &operator=(GarbageCollector &&other) = delete;
        GarbageCollector &operator=(const GarbageCollector &other) = delete;

        // Add objects for deferred deletion
        void add(BorderNode *b);
        void add(InteriorNode *i);
        void add(Value *v);
        void add(BigSuffix *suffix);

        // Check if object is tracked
        bool contain(BorderNode const *n) const;
        bool contain(InteriorNode const *n) const;
        bool contain(Value const *n) const;
        bool contain(BigSuffix const *suffix) const;

        // Run garbage collection
        void run() noexcept;

    private:
        std::vector<BorderNode *> borders;
        std::vector<InteriorNode *> interiors;
        std::vector<Value *> values;
        std::vector<BigSuffix *> suffixes;
    };

    // Alias for GarbageCollector
    using GC = GarbageCollector;

    /**
     * Result of extract operation
     */
    enum ExtractResult : uint8_t
    {
        NOTFOUND,
        VALUE,
        LAYER,
        UNSTABLE
    };

    /**
     * Base Node class for both Interior and Border nodes
     */
    class Node
    {
    public:
        Node();
        virtual ~Node() = default;
        Node(const Node &other) = delete;
        Node &operator=(const Node &other) = delete;
        Node(Node &&other) = delete;
        Node &operator=(Node &&other) = delete;

        // Version management
        Version stableVersion() const;
        void lock();
        void unlock();

        // Parent management
        InteriorNode *lockedParent() const;
        BorderNode *lockedUpperNode() const;
        InteriorNode *getParent() const;
        void setParent(InteriorNode *p);

        // Layer management
        BorderNode *getUpperLayer() const;
        void setUpperLayer(BorderNode *p);

        // Node type and state
        void setIsBorder(bool is_border);
        bool getIsBorder() const;
        void setIsRoot(bool is_root);
        bool getIsRoot() const;

        // Version access and flags
        Version getVersion() const;
        void setVersion(const Version &v);
        bool isLocked() const;
        bool isUnlocked() const;
        bool getSplitting() const;
        void setSplitting(bool splitting);
        bool getInserting() const;
        void setInserting(bool inserting);
        bool getDeleted() const;
        void setDeleted(bool deleted);

    private:
        std::atomic<Version> version;
        std::atomic<InteriorNode *> parent;
        std::atomic<BorderNode *> upperLayer;
    };

    /**
     * Interior node (non-leaf node)
     */
    class InteriorNode : public Node
    {
    public:
        InteriorNode();
        ~InteriorNode() override = default;

        // Find child node for a key slice
        Node *findChild(KeySlice slice);

        // Check node capacity
        bool isNotFull() const;
        bool isFull() const;

        // Debug helpers
        bool debug_has_skip() const;
        void printNode() const;

        // Find index of a child node
        size_t findChildIndex(Node *a_child) const;

        // Key count management
        uint8_t getNumKeys() const;
        void setNumKeys(uint8_t nKeys);
        void incNumKeys();
        void decNumKeys();

        // Key slice management
        KeySlice getKeySlice(size_t index) const;
        void resetKeySlices();
        void setKeySlice(size_t index, const KeySlice &slice);

        // Child management
        Node *getChild(size_t index) const;
        void setChild(size_t index, Node *c);
        bool debug_contain_child(Node *c) const;
        void resetChildren();

    private:
        std::atomic<uint8_t> n_keys;
        std::array<std::atomic<uint64_t>, NODE_ORDER - 1> key_slice;
        std::array<std::atomic<Node *>, NODE_ORDER> child;
    };

    /**
     * Border node (leaf node)
     */
    class BorderNode : public Node
    {
    public:
        // Special key length values
        static constexpr uint8_t key_len_layer = 255;
        static constexpr uint8_t key_len_unstable = 254;
        static constexpr uint8_t key_len_has_suffix = 9;

        BorderNode();
        ~BorderNode() override;

        // Extract value or link for a key
        std::pair<ExtractResult, LinkOrValue> extractLinkOrValueFor(const Key &key);
        std::tuple<ExtractResult, LinkOrValue, size_t> extractLinkOrValueWithIndexFor(const Key &key);

        // Get lowest key in node
        KeySlice lowestKey() const;

        // Connect prev and next nodes (used when deleting node)
        void connectPrevAndNext() const;

        // Find insert position
        std::pair<size_t, bool> insertPoint() const;

        // Debug helper
        void printNode() const;

        // Sort keys in node
        void sort();

        // Key removal
        void markKeyRemoved(uint8_t i);
        bool isKeyRemoved(uint8_t i) const;

        // Find layer index
        size_t findNextLayerIndex(Node *next_layer) const;

        // Key length management
        uint8_t getKeyLen(size_t i) const;
        void setKeyLen(size_t i, const uint8_t &len);
        void resetKeyLen();

        // Key slice management
        KeySlice getKeySlice(size_t i) const;
        void setKeySlice(size_t i, const KeySlice &slice);
        void resetKeySlice();

        // Link or value management
        LinkOrValue getLV(size_t i) const;
        void setLV(size_t i, const LinkOrValue &lv_);
        void resetLVs();

        // Next/prev pointers management
        BorderNode *getNext() const;
        void setNext(BorderNode *next_);
        BorderNode *getPrev() const;
        void setPrev(BorderNode *prev_);
        bool CASNext(BorderNode *expected, BorderNode *desired);

        // Key suffix management
        KeySuffix &getKeySuffixes();
        const KeySuffix &getKeySuffixes() const;

        // Permutation management
        Permutation getPermutation() const;
        void setPermutation(const Permutation &p);

    private:
        std::array<std::atomic<uint8_t>, NODE_ORDER - 1> key_len;
        Permutation permutation;
        std::array<std::atomic<uint64_t>, NODE_ORDER - 1> key_slice;
        std::array<LinkOrValue, NODE_ORDER - 1> lv;
        std::atomic<BorderNode *> next;
        std::atomic<BorderNode *> prev;
        KeySuffix key_suffixes;
    };

    /**
     * The main dictionary class implementing Masstree
     */
    class dict
    {
    public:
        dict();
        ~dict();

        // Delete copy/move constructors and assignment operators
        dict(const dict &) = delete;
        dict &operator=(const dict &) = delete;
        dict(dict &&) = delete;
        dict &operator=(dict &&) = delete;

        // Public API
        int insert(void *key, size_t key_len, void *value, size_t value_len);
        void *search(void *key, size_t key_len);
        // GET-path helper: copies the value out while still inside the
        // public operation's TAG scope (raw pointers from search() must not
        // escape the operation; migration may free them afterwards).
        int searchCopy(void *key, size_t key_len, std::string &out);
        void *remove(void *key, size_t key_len);

        // Range scan: copies up to n key/value pairs starting at the first
        // key >= start_key in the TREE's order, inside the operation's TAG
        // scope. Note Masstree's order is by little-endian-packed 8-byte
        // slices, not byte-lexicographic; keys that share prefixes (like
        // YCSB's "record:field" compound keys) still form contiguous runs.
        int scanCopy(void *start_key, size_t key_len, int n,
                     std::vector<std::pair<std::string, std::string>> &out);

    private:
        // Root of the tree
        std::atomic<Node *> root;

        // Thread-local garbage collector
        thread_local static GC gc;

        // Find border node for a key
        std::pair<BorderNode *, Version> findBorder(Node *node, const Key &key);

        // Range-scan helper: walk one layer's border chain (recursing into
        // deeper layers), collecting keys >= bound in tree order. bound is
        // the remaining slices of the start key within this layer (nullptr
        // once the scan has passed it).
        void scanLayer(Node *layer_root, const std::string &prefix_bytes,
                       const Key *bound, size_t n,
                       std::vector<std::pair<std::string, std::string>> &out);

        // Core operations
        std::pair<bool, Node *> put_at_layer0(Node *root, Key &k, Value *value);
        std::pair<bool, Node *> put(Node *root, Key &k, Value *value);
        Value *get(Node *root, Key &k);
        std::pair<bool, Node *> remove_at_layer0(Node *root, Key &k);
        std::pair<bool, Node *> remove(Node *root, Key &k);

        // Helper functions for insertion
        BorderNode *start_new_tree(const Key &key, Value *value);
        std::optional<size_t> check_break_invariant(BorderNode *const borderNode, const Key &key);
        void handle_break_invariant(BorderNode *n, Key &key, size_t old_index);
        void insert_into_border(BorderNode *border, const Key &key, Value *value);

        // Splitting nodes
        size_t cut(size_t len);
        Node *split(Node *n, const Key &k, Value *value);
        void split_keys_among(BorderNode *n, BorderNode *n1, const Key &k, Value *value);
        void split_keys_among(InteriorNode *p, InteriorNode *p1, KeySlice slice,
                              Node *n1, size_t n_index, std::optional<KeySlice> &k_prime);

        // Creating new internal nodes
        InteriorNode *create_root_with_children(Node *left, KeySlice slice, Node *right);
        void insert_into_parent(InteriorNode *p, Node *n1, KeySlice slice, size_t n_index);

        // Helper functions for deletion
        void handle_delete_layer_in_remove(BorderNode *n);
        std::pair<bool, Node *> delete_border_node_in_remove(BorderNode *n);

        // Utility functions
        void create_slice_table(BorderNode *const n, std::vector<std::pair<KeySlice, size_t>> &table,
                                std::vector<KeySlice> &found);
        size_t split_point(KeySlice new_slice, const std::vector<std::pair<KeySlice, size_t>> &table,
                           const std::vector<KeySlice> &found);
        void print_sub_tree(Node *root);
    };

} // namespace bpt_mass

#endif // BPT_MASS_MASSTREE_H