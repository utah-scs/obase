#ifndef TRIE_ART_H
#define TRIE_ART_H

#include <atomic>
#include <string>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <array>
#include <tuple>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <limits>
#include <functional>
#include <unordered_map>
#include <thread>

#include "Guide.hpp"

namespace trie_art
{
    // Forward declarations
    class Key;

    // Structure to store key-value pairs directly at leaves
    struct KeyValuePair
    {
        Guide<void> key;
        size_t key_len;
        Guide<void> value;
        size_t value_len;

        KeyValuePair(void *k, size_t kl, void *v, size_t vl)
            : key(k), key_len(kl), value(v), value_len(vl) {}

        ~KeyValuePair()
        {
            if (key)
                key.destroy();
            if (value)
                value.destroy();
        }
    };

    // Type definitions
    using TID = uintptr_t; // Used to store KeyValuePair pointers

    // Constants
    static constexpr uint32_t maxStoredPrefixLength = 11;
    using Prefix = uint8_t[maxStoredPrefixLength];

    // Node types
    enum class NTypes : uint8_t
    {
        N4 = 0,
        N16 = 1,
        N48 = 2,
        N256 = 3
    };

    // Base Node class - simplified without versioning
    class N
    {
    protected:
        N(NTypes type, const uint8_t *prefix, uint32_t prefixLength);
        N(const N &) = delete;
        N(N &&) = delete;

        NTypes type;
        uint32_t prefixCount = 0;
        uint8_t count = 0;
        Prefix prefix;

    public:
        NTypes getType() const;
        uint32_t getCount() const;
        bool hasPrefix() const;
        const uint8_t *getPrefix() const;
        void setPrefix(const uint8_t *prefix, uint32_t length);
        void addPrefixBefore(N *node, uint8_t key);
        uint32_t getPrefixLength() const;

        // Functions for leaf handling
        static KeyValuePair *getLeaf(const N *n);
        static bool isLeaf(const N *n);
        static N *setLeaf(KeyValuePair *kv);

        // Child access methods
        static N *getChild(const uint8_t k, const N *node);
        static bool change(N *node, uint8_t key, N *val);
        static N *getAnyChild(const N *n);
        static void deleteChildren(N *node);
        static void deleteNode(N *node);
        static std::tuple<N *, uint8_t> getSecondChild(N *node, const uint8_t k);

        // Helper methods for children operations
        static void getChildren(const N *node, N **children, uint32_t &childrenCount);
        static uint64_t getChildren(const N *node, uint8_t start, uint8_t end,
                                    std::tuple<uint8_t, N *> children[],
                                    uint32_t &childrenCount);
        static KeyValuePair *getAnyChildTid(const N *n);
    };

    // N4 Node - stores up to 4 children, simple array structure
    class N4 : public N
    {
    public:
        uint8_t keys[4];
        N *children[4] = {nullptr, nullptr, nullptr, nullptr};

    public:
        N4(const uint8_t *prefix, uint32_t prefixLength);
        void insert(uint8_t key, N *n);
        template <class NODE>
        void copyTo(NODE *n) const;
        bool change(uint8_t key, N *val);
        N *getChild(const uint8_t k) const;
        void remove(uint8_t k);
        N *getAnyChild() const;
        bool isFull() const;
        bool isUnderfull() const;
        std::tuple<N *, uint8_t> getSecondChild(const uint8_t key) const;
        void deleteChildren();
        uint64_t getChildren(uint8_t start, uint8_t end, std::tuple<uint8_t, N *> *&children,
                             uint32_t &childrenCount) const;
        void getChildren(N **children, uint32_t &childrenCount) const;
    };

    // N16 Node - stores up to 16 children, uses SIMD for fast lookups
    class N16 : public N
    {
    public:
        uint8_t keys[16];
        N *children[16];

        static uint8_t flipSign(uint8_t keyByte);
        static inline unsigned ctz(uint16_t x);
        N *const *getChildPos(const uint8_t k) const;

    public:
        N16(const uint8_t *prefix, uint32_t prefixLength);
        void insert(uint8_t key, N *n);
        template <class NODE>
        void copyTo(NODE *n) const;
        bool change(uint8_t key, N *val);
        N *getChild(const uint8_t k) const;
        void remove(uint8_t k);
        N *getAnyChild() const;
        bool isFull() const;
        bool isUnderfull() const;
        void deleteChildren();
        uint64_t getChildren(uint8_t start, uint8_t end, std::tuple<uint8_t, N *> *&children,
                             uint32_t &childrenCount) const;
        void getChildren(N **children, uint32_t &childrenCount) const;
    };

    // N48 Node - stores up to 48 children, uses an index array for fast lookups
    class N48 : public N
    {
        uint8_t childIndex[256];
        N *children[48];

    public:
        static const uint8_t emptyMarker = 48;

        N48(const uint8_t *prefix, uint32_t prefixLength);
        void insert(uint8_t key, N *n);
        template <class NODE>
        void copyTo(NODE *n) const;
        bool change(uint8_t key, N *val);
        N *getChild(const uint8_t k) const;
        void remove(uint8_t k);
        N *getAnyChild() const;
        bool isFull() const;
        bool isUnderfull() const;
        void deleteChildren();
        uint64_t getChildren(uint8_t start, uint8_t end, std::tuple<uint8_t, N *> *&children,
                             uint32_t &childrenCount) const;
        void getChildren(N **children, uint32_t &childrenCount) const;
    };

    // N256 Node - stores up to 256 children, direct indexing for fastest lookups
    class N256 : public N
    {
        N *children[256];

    public:
        N256(const uint8_t *prefix, uint32_t prefixLength);
        void insert(uint8_t key, N *val);
        template <class NODE>
        void copyTo(NODE *n) const;
        bool change(uint8_t key, N *n);
        N *getChild(const uint8_t k) const;
        void remove(uint8_t k);
        N *getAnyChild() const;
        bool isFull() const;
        bool isUnderfull() const;
        void deleteChildren();
        uint64_t getChildren(uint8_t start, uint8_t end, std::tuple<uint8_t, N *> *&children,
                             uint32_t &childrenCount) const;
        void getChildren(N **children, uint32_t &childrenCount) const;
    };

    // Key handling
    class Key
    {
    public:
        static constexpr uint32_t stackLen = 128;
        uint32_t len = 0;
        uint8_t *data;
        uint8_t stackKey[stackLen];

        Key(uint64_t k);
        void setInt(uint64_t k);
        Key();
        ~Key();
        Key(const Key &key) = delete;
        Key(Key &&key);
        void set(const char bytes[], const std::size_t length);
        void operator=(const char key[]);
        bool operator==(const Key &k) const;
        uint8_t &operator[](std::size_t i);
        const uint8_t &operator[](std::size_t i) const;
        uint32_t getKeyLen() const;
        void setKeyLen(uint32_t len);
    };

    // Main dictionary class - adaptive radix tree implementation with reader-writer lock
    class dict
    {
    public:
        using LoadKeyFunction = void (*)(TID tid, Key &key);

    private:
        N *root;
        std::shared_mutex rw_mutex; // Reader-writer lock for concurrency control
        LoadKeyFunction loadKey;

        // Helper method for thread yielding
        void yield(int count);

        // Ordered DFS for range scans: collects up to n leaves with
        // key >= start, pruning subtrees that the (stored) prefix bytes
        // prove are entirely below start. Truncated prefixes fall back to
        // unpruned descent with the leaf-level key guard.
        void scanCollect(N *node, const uint8_t *start, size_t start_len,
                         size_t depth, bool bounded, size_t n,
                         std::vector<std::pair<std::string, std::string>> &out);

        // Helper method to check key match
        KeyValuePair *checkKey(KeyValuePair *kvp, const Key &k);

        // Create a KeyValuePair with deep copies
        KeyValuePair *createKeyValuePair(void *key, size_t key_len, void *value, size_t value_len);

        // Prefix checking enums and methods
        enum class CheckPrefixResult : uint8_t
        {
            Match,
            NoMatch,
            OptimisticMatch
        };

        enum class CheckPrefixPessimisticResult : uint8_t
        {
            Match,
            NoMatch,
        };

        enum class PCCompareResults : uint8_t
        {
            Smaller,
            Equal,
            Bigger,
        };

        enum class PCEqualsResults : uint8_t
        {
            BothMatch,
            Contained,
            NoMatch
        };

        CheckPrefixResult checkPrefix(N *n, const Key &k, uint32_t &level);

        CheckPrefixPessimisticResult checkPrefixPessimistic(
            N *n, const Key &k, uint32_t &level, uint8_t &nonMatchingKey,
            Prefix &nonMatchingPrefix, LoadKeyFunction loadKey);

        PCCompareResults checkPrefixCompare(
            const N *n, const Key &k, uint8_t fillKey, uint32_t &level,
            LoadKeyFunction loadKey);

        PCEqualsResults checkPrefixEquals(
            const N *n, uint32_t &level, const Key &start, const Key &end,
            LoadKeyFunction loadKey);

        // Growth and shrink helpers for node replacement
        template <typename curN, typename biggerN>
        void insertGrow(curN *n, N *parentNode, uint8_t keyParent, uint8_t key, N *val);

        template <typename curN, typename smallerN>
        void removeAndShrink(curN *n, N *parentNode, uint8_t keyParent, uint8_t key);

    public:
        // Constructor with custom key loader function
        dict(LoadKeyFunction loadKey = nullptr);

        // Destructor
        ~dict();

        // Delete copy constructor and assignment operator
        dict(const dict &) = delete;
        dict &operator=(const dict &) = delete;

        // Move constructor
        dict(dict &&t);

        // Core API - the four required operations

        // Insert a key-value pair
        // Returns: 1 on success, 0 on failure
        int insert(void *key, size_t key_len, void *value, size_t value_len);

        // Find a key
        // Returns: pointer to value if found, nullptr if not found
        void *search(void *key, size_t key_len);
        // GET-path helper: copies the value out while still inside the
        // public operation's TAG scope (raw pointers from search() must not
        // escape the operation; migration may free them afterwards).
        int searchCopy(void *key, size_t key_len, std::string &out);

        // Range scan: copies up to n key/value pairs starting at the first
        // key >= start_key, in key order, inside the operation's TAG scope.
        int scanCopy(void *start_key, size_t key_len, int n,
                     std::vector<std::pair<std::string, std::string>> &out);

        // Remove a key
        // Returns: pointer to removed value if successful, nullptr if not found
        void *remove(void *key, size_t key_len);

        // Scan a range of keys
        // Results are stored in the provided vector
        // Returns: true if more results exist beyond maxResults, false otherwise
        bool scan(void *startKey, size_t startKeyLen,
                  void *endKey, size_t endKeyLen,
                  std::vector<std::pair<void *, void *>> &results,
                  size_t maxResults = 100);
    };

} // namespace trie_art

#endif // TRIE_ART_H