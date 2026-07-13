#include "trie_art.h"
#include <algorithm>
#include <functional>
#include <sched.h>
#include <cmath>

// For SIMD instructions
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h> // For SSE2 instructions
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h> // For ARM NEON
#endif

namespace trie_art
{
    //////////////////////////
    // N (Base Node Class) Implementation
    //////////////////////////

    N::N(NTypes type, const uint8_t *prefix, uint32_t prefixLength) : type(type)
    {
        setPrefix(prefix, prefixLength);
    }

    NTypes N::getType() const
    {
        return type;
    }

    void N::setPrefix(const uint8_t *prefix, uint32_t length)
    {
        if (length > 0)
        {
            memcpy(this->prefix, prefix, std::min(length, maxStoredPrefixLength));
            prefixCount = length;
        }
        else
        {
            prefixCount = 0;
        }
    }

    void N::addPrefixBefore(N *node, uint8_t key)
    {
        uint32_t prefixCopyCount = std::min(maxStoredPrefixLength, node->getPrefixLength() + 1);
        memmove(this->prefix + prefixCopyCount, this->prefix,
                std::min(this->getPrefixLength(), maxStoredPrefixLength - prefixCopyCount));
        memcpy(this->prefix, node->prefix, std::min(prefixCopyCount, node->getPrefixLength()));
        if (node->getPrefixLength() < maxStoredPrefixLength)
        {
            this->prefix[prefixCopyCount - 1] = key;
        }
        this->prefixCount += node->getPrefixLength() + 1;
    }

    uint32_t N::getPrefixLength() const
    {
        return prefixCount;
    }

    uint32_t N::getCount() const
    {
        return count;
    }

    bool N::hasPrefix() const
    {
        return prefixCount > 0;
    }

    const uint8_t *N::getPrefix() const
    {
        return prefix;
    }

    // Handle leaf nodes (identified by setting high bit of pointer)
    bool N::isLeaf(const N *n)
    {
        if (!n)
            return false;
        return (reinterpret_cast<uintptr_t>(n) & (static_cast<uintptr_t>(1) << 63)) == (static_cast<uintptr_t>(1) << 63);
    }

    N *N::setLeaf(KeyValuePair *kv)
    {
        if (!kv)
            return nullptr;
        return reinterpret_cast<N *>(reinterpret_cast<uintptr_t>(kv) | (static_cast<uintptr_t>(1) << 63));
    }

    KeyValuePair *N::getLeaf(const N *n)
    {
        if (!n)
            return nullptr;
        return reinterpret_cast<KeyValuePair *>(reinterpret_cast<uintptr_t>(n) & ~(static_cast<uintptr_t>(1) << 63));
    }

    // Child node operations
    N *N::getChild(const uint8_t k, const N *node)
    {
        if (!node)
            return nullptr;

        switch (node->getType())
        {
        case NTypes::N4:
        {
            auto n = static_cast<const N4 *>(node);
            return n->getChild(k);
        }
        case NTypes::N16:
        {
            auto n = static_cast<const N16 *>(node);
            return n->getChild(k);
        }
        case NTypes::N48:
        {
            auto n = static_cast<const N48 *>(node);
            return n->getChild(k);
        }
        case NTypes::N256:
        {
            auto n = static_cast<const N256 *>(node);
            return n->getChild(k);
        }
        }
        assert(false);
        __builtin_unreachable();
    }

    bool N::change(N *node, uint8_t key, N *val)
    {
        if (!node)
            return false;

        switch (node->getType())
        {
        case NTypes::N4:
        {
            auto n = static_cast<N4 *>(node);
            return n->change(key, val);
        }
        case NTypes::N16:
        {
            auto n = static_cast<N16 *>(node);
            return n->change(key, val);
        }
        case NTypes::N48:
        {
            auto n = static_cast<N48 *>(node);
            return n->change(key, val);
        }
        case NTypes::N256:
        {
            auto n = static_cast<N256 *>(node);
            return n->change(key, val);
        }
        }
        assert(false);
        __builtin_unreachable();
    }

    N *N::getAnyChild(const N *node)
    {
        if (!node)
            return nullptr;

        switch (node->getType())
        {
        case NTypes::N4:
        {
            auto n = static_cast<const N4 *>(node);
            return n->getAnyChild();
        }
        case NTypes::N16:
        {
            auto n = static_cast<const N16 *>(node);
            return n->getAnyChild();
        }
        case NTypes::N48:
        {
            auto n = static_cast<const N48 *>(node);
            return n->getAnyChild();
        }
        case NTypes::N256:
        {
            auto n = static_cast<const N256 *>(node);
            return n->getAnyChild();
        }
        }
        assert(false);
        __builtin_unreachable();
    }

    // Helper to retrieve any leaf KeyValuePair to handle optimistic prefix matching
    KeyValuePair *N::getAnyChildTid(const N *n)
    {
        if (!n)
            return nullptr;

        const N *nextNode = n;
        while (nextNode && !isLeaf(nextNode))
        {
            nextNode = getAnyChild(nextNode);
        }

        return nextNode ? getLeaf(nextNode) : nullptr;
    }

    void N::deleteChildren(N *node)
    {
        if (!node || N::isLeaf(node))
        {
            return;
        }
        switch (node->getType())
        {
        case NTypes::N4:
        {
            auto n = static_cast<N4 *>(node);
            n->deleteChildren();
            return;
        }
        case NTypes::N16:
        {
            auto n = static_cast<N16 *>(node);
            n->deleteChildren();
            return;
        }
        case NTypes::N48:
        {
            auto n = static_cast<N48 *>(node);
            n->deleteChildren();
            return;
        }
        case NTypes::N256:
        {
            auto n = static_cast<N256 *>(node);
            n->deleteChildren();
            return;
        }
        }
        assert(false);
        __builtin_unreachable();
    }

    void N::deleteNode(N *node)
    {
        if (!node || N::isLeaf(node))
        {
            return;
        }
        switch (node->getType())
        {
        case NTypes::N4:
        {
            auto n = static_cast<N4 *>(node);
            delete n;
            return;
        }
        case NTypes::N16:
        {
            auto n = static_cast<N16 *>(node);
            delete n;
            return;
        }
        case NTypes::N48:
        {
            auto n = static_cast<N48 *>(node);
            delete n;
            return;
        }
        case NTypes::N256:
        {
            auto n = static_cast<N256 *>(node);
            delete n;
            return;
        }
        }
        delete node;
    }

    std::tuple<N *, uint8_t> N::getSecondChild(N *node, const uint8_t key)
    {
        if (!node)
            return std::make_tuple(nullptr, 0);

        switch (node->getType())
        {
        case NTypes::N4:
        {
            auto n = static_cast<N4 *>(node);
            return n->getSecondChild(key);
        }
        default:
        {
            assert(false);
            __builtin_unreachable();
        }
        }
    }

    uint64_t N::getChildren(const N *node, uint8_t start, uint8_t end, std::tuple<uint8_t, N *> children[], uint32_t &childrenCount)
    {
        childrenCount = 0;
        if (!node)
            return 0;

        switch (node->getType())
        {
        case NTypes::N4:
        {
            auto n = static_cast<const N4 *>(node);
            return n->getChildren(start, end, children, childrenCount);
        }
        case NTypes::N16:
        {
            auto n = static_cast<const N16 *>(node);
            return n->getChildren(start, end, children, childrenCount);
        }
        case NTypes::N48:
        {
            auto n = static_cast<const N48 *>(node);
            return n->getChildren(start, end, children, childrenCount);
        }
        case NTypes::N256:
        {
            auto n = static_cast<const N256 *>(node);
            return n->getChildren(start, end, children, childrenCount);
        }
        }
        assert(false);
        __builtin_unreachable();
    }

    void N::getChildren(const N *node, N **children, uint32_t &childrenCount)
    {
        childrenCount = 0;
        if (!node)
            return;

        switch (node->getType())
        {
        case NTypes::N4:
        {
            auto n = static_cast<const N4 *>(node);
            n->getChildren(children, childrenCount);
            return;
        }
        case NTypes::N16:
        {
            auto n = static_cast<const N16 *>(node);
            n->getChildren(children, childrenCount);
            return;
        }
        case NTypes::N48:
        {
            auto n = static_cast<const N48 *>(node);
            n->getChildren(children, childrenCount);
            return;
        }
        case NTypes::N256:
        {
            auto n = static_cast<const N256 *>(node);
            n->getChildren(children, childrenCount);
            return;
        }
        }
        assert(false);
        __builtin_unreachable();
    }

    //////////////////////////
    // Key Implementation
    //////////////////////////

    Key::Key(uint64_t k)
    {
        setInt(k);
    }

    void Key::setInt(uint64_t k)
    {
        data = stackKey;
        len = 8;
        *reinterpret_cast<uint64_t *>(stackKey) = __builtin_bswap64(k);
    }

    Key::Key() : len(0), data(stackKey) {}

    Key::~Key()
    {
        if (len > stackLen && data != stackKey)
        {
            delete[] data;
            data = nullptr;
        }
    }

    Key::Key(Key &&key)
    {
        len = key.len;
        if (len > stackLen)
        {
            data = key.data;
            key.data = nullptr;
        }
        else
        {
            memcpy(stackKey, key.stackKey, key.len);
            data = stackKey;
        }
    }

    void Key::set(const char bytes[], const std::size_t length)
    {
        if (len > stackLen && data != stackKey)
        {
            delete[] data;
        }
        if (length <= stackLen)
        {
            memcpy(stackKey, bytes, length);
            data = stackKey;
        }
        else
        {
            data = new uint8_t[length];
            memcpy(data, bytes, length);
        }
        len = length;
    }

    void Key::operator=(const char key[])
    {
        if (len > stackLen && data != stackKey)
        {
            delete[] data;
        }
        len = strlen(key);
        if (len <= stackLen)
        {
            memcpy(stackKey, key, len);
            data = stackKey;
        }
        else
        {
            data = new uint8_t[len];
            memcpy(data, key, len);
        }
    }

    bool Key::operator==(const Key &k) const
    {
        if (k.getKeyLen() != getKeyLen())
        {
            return false;
        }
        return std::memcmp(&k[0], data, getKeyLen()) == 0;
    }

    uint8_t &Key::operator[](std::size_t i)
    {
        if (i >= len)
            printf("Key::operator[]: %zu >= %u\n", i, len);
        return data[i];
    }

    const uint8_t &Key::operator[](std::size_t i) const
    {
        assert(i < len);
        return data[i];
    }

    uint32_t Key::getKeyLen() const
    {
        return len;
    }

    void Key::setKeyLen(uint32_t newLen)
    {
        if (len == newLen)
            return;
        if (len > stackLen && data != stackKey)
        {
            delete[] data;
        }
        len = newLen;
        if (len > stackLen)
        {
            data = new uint8_t[len];
        }
        else
        {
            data = stackKey;
        }
    }

    //////////////////////////
    // N4 Implementation
    //////////////////////////

    N4::N4(const uint8_t *prefix, uint32_t prefixLength) : N(NTypes::N4, prefix, prefixLength) {}

    void N4::insert(uint8_t key, N *n)
    {
        unsigned pos;
        for (pos = 0; (pos < count) && (keys[pos] < key); pos++)
            ;
        memmove(keys + pos + 1, keys + pos, count - pos);
        memmove(children + pos + 1, children + pos, (count - pos) * sizeof(N *));
        keys[pos] = key;
        children[pos] = n;
        count++;
    }

    template <class NODE>
    void N4::copyTo(NODE *n) const
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            n->insert(keys[i], children[i]);
        }
    }

    bool N4::change(uint8_t key, N *val)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (keys[i] == key)
            {
                children[i] = val;
                return true;
            }
        }
        return false;
    }

    N *N4::getChild(const uint8_t k) const
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (keys[i] == k)
            {
                return children[i];
            }
        }
        return nullptr;
    }

    void N4::remove(uint8_t k)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (keys[i] == k)
            {
                memmove(keys + i, keys + i + 1, count - i - 1);
                memmove(children + i, children + i + 1, (count - i - 1) * sizeof(N *));
                count--;
                return;
            }
        }
    }

    N *N4::getAnyChild() const
    {
        if (count == 0)
            return nullptr;

        N *anyChild = nullptr;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (children[i] != nullptr)
            {
                if (N::isLeaf(children[i]))
                {
                    return children[i];
                }
                else
                {
                    anyChild = children[i];
                }
            }
        }
        return anyChild;
    }

    bool N4::isFull() const
    {
        return count == 4;
    }

    bool N4::isUnderfull() const
    {
        return false;
    }

    std::tuple<N *, uint8_t> N4::getSecondChild(const uint8_t key) const
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (keys[i] != key)
            {
                return std::make_tuple(children[i], keys[i]);
            }
        }
        return std::make_tuple(nullptr, 0);
    }

    void N4::deleteChildren()
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (children[i] != nullptr)
            {
                N::deleteChildren(children[i]);
                N::deleteNode(children[i]);
            }
        }
    }

    uint64_t N4::getChildren(uint8_t start, uint8_t end, std::tuple<uint8_t, N *> *&children, uint32_t &childrenCount) const
    {
        childrenCount = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (this->keys[i] >= start && this->keys[i] <= end)
            {
                children[childrenCount] = std::make_tuple(this->keys[i], this->children[i]);
                childrenCount++;
            }
        }
        return 0; // No version needed anymore
    }

    void N4::getChildren(N **_children, uint32_t &childrenCount) const
    {
        int cnt = 0;
        for (int i = 0; i < count; i++)
        {
            if (children[i] != nullptr)
                _children[cnt++] = children[i];
        }
        childrenCount = cnt;
    }

    //////////////////////////
    // N16 Implementation
    //////////////////////////

    N16::N16(const uint8_t *prefix, uint32_t prefixLength) : N(NTypes::N16, prefix, prefixLength)
    {
        memset(keys, 0, sizeof(keys));
        memset(children, 0, sizeof(children));
    }

    uint8_t N16::flipSign(uint8_t keyByte)
    {
        // Flip the sign bit, enables signed SSE comparison
        return keyByte ^ 128;
    }

    unsigned N16::ctz(uint16_t x)
    {
        // Count trailing zeros, only defined for x>0
#ifdef __GNUC__
        return __builtin_ctz(x);
#else
        // Adapted from Hacker's Delight
        unsigned n = 1;
        if ((x & 0xFF) == 0)
        {
            n += 8;
            x = x >> 8;
        }
        if ((x & 0x0F) == 0)
        {
            n += 4;
            x = x >> 4;
        }
        if ((x & 0x03) == 0)
        {
            n += 2;
            x = x >> 2;
        }
        return n - (x & 1);
#endif
    }

    void N16::insert(uint8_t key, N *n)
    {
        uint8_t keyByteFlipped = flipSign(key);

        // SIMD implementation for x86
#if defined(__x86_64__) || defined(_M_X64)
        __m128i cmp = _mm_cmplt_epi8(_mm_set1_epi8(keyByteFlipped), _mm_loadu_si128(reinterpret_cast<__m128i *>(keys)));
        uint16_t bitfield = _mm_movemask_epi8(cmp) & (0xFFFF >> (16 - count));
        unsigned pos = bitfield ? ctz(bitfield) : count;
#else
        // Fallback for other architectures
        unsigned pos = 0;
        while (pos < count && flipSign(keys[pos]) < keyByteFlipped)
            pos++;
#endif

        memmove(keys + pos + 1, keys + pos, count - pos);
        memmove(children + pos + 1, children + pos, (count - pos) * sizeof(uintptr_t));
        keys[pos] = keyByteFlipped;
        children[pos] = n;
        count++;
    }

    template <class NODE>
    void N16::copyTo(NODE *n) const
    {
        for (unsigned i = 0; i < count; i++)
        {
            n->insert(flipSign(keys[i]), children[i]);
        }
    }

    bool N16::change(uint8_t key, N *val)
    {
        N **childPos = const_cast<N **>(getChildPos(key));
        if (childPos == nullptr)
            return false;
        *childPos = val;
        return true;
    }

    N *const *N16::getChildPos(const uint8_t k) const
    {
        uint8_t keyByteFlipped = flipSign(k);

        // SIMD implementation for x86
#if defined(__x86_64__) || defined(_M_X64)
        __m128i cmp = _mm_cmpeq_epi8(_mm_set1_epi8(keyByteFlipped),
                                     _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys)));
        unsigned bitfield = _mm_movemask_epi8(cmp) & ((1 << count) - 1);
        if (bitfield)
        {
            return &children[ctz(bitfield)];
        }
#else
        // Fallback for other architectures
        for (unsigned i = 0; i < count; i++)
        {
            if (keys[i] == keyByteFlipped)
            {
                return &children[i];
            }
        }
#endif
        return nullptr;
    }

    N *N16::getChild(const uint8_t k) const
    {
        N *const *childPos = getChildPos(k);
        if (childPos == nullptr)
        {
            return nullptr;
        }
        else
        {
            return *childPos;
        }
    }

    void N16::remove(uint8_t k)
    {
        N *const *leafPlace = getChildPos(k);
        if (leafPlace == nullptr)
            return;

        std::size_t pos = leafPlace - children;
        memmove(keys + pos, keys + pos + 1, count - pos - 1);
        memmove(children + pos, children + pos + 1, (count - pos - 1) * sizeof(N *));
        count--;
    }

    N *N16::getAnyChild() const
    {
        if (count == 0)
            return nullptr;

        for (int i = 0; i < count; ++i)
        {
            if (children[i] != nullptr)
            {
                if (N::isLeaf(children[i]))
                {
                    return children[i];
                }
            }
        }
        for (int i = 0; i < count; ++i)
        {
            if (children[i] != nullptr)
            {
                return children[i];
            }
        }
        return nullptr;
    }

    bool N16::isFull() const
    {
        return count == 16;
    }

    bool N16::isUnderfull() const
    {
        return count == 3;
    }

    void N16::deleteChildren()
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (children[i] != nullptr)
            {
                N::deleteChildren(children[i]);
                N::deleteNode(children[i]);
            }
        }
    }

    uint64_t N16::getChildren(uint8_t start, uint8_t end, std::tuple<uint8_t, N *> *&children, uint32_t &childrenCount) const
    {
        childrenCount = 0;
        auto startPos = getChildPos(start);
        auto endPos = getChildPos(end);
        if (startPos == nullptr)
        {
            startPos = this->children;
        }
        if (endPos == nullptr)
        {
            endPos = this->children + (count - 1);
        }
        for (auto p = startPos; p <= endPos; ++p)
        {
            if (p < this->children + count)
            {
                children[childrenCount] = std::make_tuple(flipSign(keys[p - this->children]), *p);
                childrenCount++;
            }
        }
        return 0; // No version needed anymore
    }

    void N16::getChildren(N **_children, uint32_t &childrenCount) const
    {
        int cnt = 0;
        for (int i = 0; i < count; i++)
        {
            if (children[i] != nullptr)
                _children[cnt++] = children[i];
        }
        childrenCount = cnt;
    }

    //////////////////////////
    // N48 Implementation
    //////////////////////////

    N48::N48(const uint8_t *prefix, uint32_t prefixLength) : N(NTypes::N48, prefix, prefixLength)
    {
        memset(childIndex, emptyMarker, sizeof(childIndex));
        memset(children, 0, sizeof(children));
    }

    void N48::insert(uint8_t key, N *n)
    {
        unsigned pos = count;
        if (pos < 48 && children[pos])
        {
            for (pos = 0; pos < 48 && children[pos] != nullptr; pos++)
                ;
        }

        if (pos < 48)
        {
            children[pos] = n;
            childIndex[key] = (uint8_t)pos;
            count++;
        }
    }

    template <class NODE>
    void N48::copyTo(NODE *n) const
    {
        for (unsigned i = 0; i < 256; i++)
        {
            if (childIndex[i] != emptyMarker)
            {
                n->insert(i, children[childIndex[i]]);
            }
        }
    }

    bool N48::change(uint8_t key, N *val)
    {
        if (childIndex[key] == emptyMarker)
            return false;
        children[childIndex[key]] = val;
        return true;
    }

    N *N48::getChild(const uint8_t k) const
    {
        if (childIndex[k] == emptyMarker)
        {
            return nullptr;
        }
        else
        {
            return children[childIndex[k]];
        }
    }

    void N48::remove(uint8_t k)
    {
        if (childIndex[k] == emptyMarker)
            return;

        children[childIndex[k]] = nullptr;
        childIndex[k] = emptyMarker;
        count--;
    }

    N *N48::getAnyChild() const
    {
        if (count == 0)
            return nullptr;

        N *anyChild = nullptr;
        for (unsigned i = 0; i < 256; i++)
        {
            if (childIndex[i] != emptyMarker && children[childIndex[i]] != nullptr)
            {
                if (N::isLeaf(children[childIndex[i]]))
                {
                    return children[childIndex[i]];
                }
                else
                {
                    anyChild = children[childIndex[i]];
                };
            }
        }
        return anyChild;
    }

    bool N48::isFull() const
    {
        return count == 48;
    }

    bool N48::isUnderfull() const
    {
        return count == 12;
    }

    void N48::deleteChildren()
    {
        for (unsigned i = 0; i < 256; i++)
        {
            if (childIndex[i] != emptyMarker && children[childIndex[i]] != nullptr)
            {
                N::deleteChildren(children[childIndex[i]]);
                N::deleteNode(children[childIndex[i]]);
            }
        }
    }

    uint64_t N48::getChildren(uint8_t start, uint8_t end, std::tuple<uint8_t, N *> *&children, uint32_t &childrenCount) const
    {
        childrenCount = 0;
        for (unsigned i = start; i <= end; i++)
        {
            if (this->childIndex[i] != emptyMarker && this->children[this->childIndex[i]] != nullptr)
            {
                children[childrenCount] = std::make_tuple(i, this->children[this->childIndex[i]]);
                childrenCount++;
            }
        }
        return 0; // No version needed anymore
    }

    void N48::getChildren(N **_children, uint32_t &childrenCount) const
    {
        int cnt = 0;
        for (int i = 0; i < 256; i++)
        {
            if (childIndex[i] != emptyMarker && children[childIndex[i]] != nullptr)
            {
                _children[cnt++] = children[childIndex[i]];
            }
        }
        childrenCount = cnt;
    }

    //////////////////////////
    // N256 Implementation
    //////////////////////////

    N256::N256(const uint8_t *prefix, uint32_t prefixLength) : N(NTypes::N256, prefix, prefixLength)
    {
        memset(children, '\0', sizeof(children));
    }

    void N256::insert(uint8_t key, N *val)
    {
        children[key] = val;
        count++;
    }

    template <class NODE>
    void N256::copyTo(NODE *n) const
    {
        for (int i = 0; i < 256; ++i)
        {
            if (children[i] != nullptr)
            {
                n->insert(i, children[i]);
            }
        }
    }

    bool N256::change(uint8_t key, N *n)
    {
        children[key] = n;
        return true;
    }

    N *N256::getChild(const uint8_t k) const
    {
        return children[k];
    }

    void N256::remove(uint8_t k)
    {
        children[k] = nullptr;
        count--;
    }

    N *N256::getAnyChild() const
    {
        if (count == 0)
            return nullptr;

        N *anyChild = nullptr;
        for (uint64_t i = 0; i < 256; ++i)
        {
            if (children[i] != nullptr)
            {
                if (N::isLeaf(children[i]))
                {
                    return children[i];
                }
                else
                {
                    anyChild = children[i];
                }
            }
        }
        return anyChild;
    }

    bool N256::isFull() const
    {
        return false;
    }

    bool N256::isUnderfull() const
    {
        return count == 37;
    }

    void N256::deleteChildren()
    {
        for (uint64_t i = 0; i < 256; ++i)
        {
            if (children[i] != nullptr)
            {
                N::deleteChildren(children[i]);
                N::deleteNode(children[i]);
            }
        }
    }

    uint64_t N256::getChildren(uint8_t start, uint8_t end, std::tuple<uint8_t, N *> *&children, uint32_t &childrenCount) const
    {
        childrenCount = 0;
        for (unsigned i = start; i <= end; i++)
        {
            if (this->children[i] != nullptr)
            {
                children[childrenCount] = std::make_tuple(i, this->children[i]);
                childrenCount++;
            }
        }
        return 0; // No version needed anymore
    }

    void N256::getChildren(N **_children, uint32_t &childrenCount) const
    {
        uint32_t cnt = 0;
        for (int i = 0; i < 256; i++)
        {
            if (children[i] != nullptr)
            {
                _children[cnt++] = children[i];
            }
        }
        childrenCount = cnt;
    }

    //////////////////////////
    // dict Implementation
    //////////////////////////

    dict::dict(LoadKeyFunction loadKey)
        : root(new N256(nullptr, 0)),
          loadKey(loadKey) {}

    dict::~dict()
    {
        // Hold write lock to ensure no other thread is accessing the structure
        std::unique_lock<std::shared_mutex> lock(rw_mutex);

        N::deleteChildren(root);
        N::deleteNode(root);
    }

    // Fix for move constructor
    dict::dict(dict &&t) : root(t.root), loadKey(t.loadKey)
    {
        t.root = nullptr;
    }

    void dict::yield(int count)
    {
        if (count > 3)
            sched_yield();
    }

    // Create a deep copy of key and value
    KeyValuePair *dict::createKeyValuePair(void *key, size_t key_len, void *value, size_t value_len)
    {
        // Create a deep copy of key and value
        void *keyCopy = jem_malloc(key_len);
        if (!keyCopy)
            return nullptr;

        memcpy(keyCopy, key, key_len);

        void *valueCopy = jem_malloc(value_len);
        if (!valueCopy)
        {
            jem_free(keyCopy);
            return nullptr;
        }

        memcpy(valueCopy, value, value_len);

        return new KeyValuePair(keyCopy, key_len, valueCopy, value_len);
    }

    // Helper method to check if a KeyValuePair matches a key
    KeyValuePair *dict::checkKey(KeyValuePair *kvp, const Key &k)
    {
        if (!kvp)
            return nullptr;

        if (k.getKeyLen() == kvp->key_len &&
            memcmp(static_cast<const void *>(kvp->key), k.data, k.getKeyLen()) == 0)
        {
            return const_cast<KeyValuePair *>(kvp);
        }
        return nullptr;
    }

    template <typename curN, typename biggerN>
    void dict::insertGrow(curN *n, N *parentNode, uint8_t keyParent, uint8_t key, N *val)
    {
        if (!n->isFull())
        {
            n->insert(key, val);
            return;
        }

        auto nBig = new biggerN(n->getPrefix(), n->getPrefixLength());
        n->copyTo(nBig);
        nBig->insert(key, val);

        if (parentNode)
        {
            N::change(parentNode, keyParent, nBig);
        }
        else
        {
            // This should only happen for the root node
            root = nBig;
        }
        delete n;
    }

    template <typename curN, typename smallerN>
    void dict::removeAndShrink(curN *n, N *parentNode, uint8_t keyParent, uint8_t key)
    {
        if (!n->isUnderfull() || parentNode == nullptr)
        {
            n->remove(key);
            return;
        }

        auto nSmall = new smallerN(n->getPrefix(), n->getPrefixLength());
        n->copyTo(nSmall);
        nSmall->remove(key);

        N::change(parentNode, keyParent, nSmall);
        delete n;
    }

    int dict::insert(void *key, size_t key_len, void *value, size_t value_len)
    {
        if (!key || !value || key_len == 0 || value_len == 0)
            return 0;

        // Create a Key object from the raw key
        Key k;
        k.set(static_cast<const char *>(key), key_len);

        // Create the key-value pair with deep copies
        KeyValuePair *kvp = createKeyValuePair(key, key_len, value, value_len);
        if (!kvp)
            return 0;

        // Acquire write lock for the entire operation
        std::unique_lock<std::shared_mutex> lock(rw_mutex);

        int restartCount = 0;
        if (restartCount++)
            yield(restartCount);

        N *node = nullptr;
        N *nextNode = root;
        N *parentNode = nullptr;
        uint8_t parentKey = 0, nodeKey = 0;
        uint32_t level = 0;

        while (true)
        {
            parentNode = node;
            parentKey = nodeKey;
            node = nextNode;

            if (!node)
            {
                delete kvp;
                return 0;
            }

            uint32_t nextLevel = level;
            uint8_t nonMatchingKey;
            Prefix remainingPrefix;
            auto res = checkPrefixPessimistic(node, k, nextLevel, nonMatchingKey, remainingPrefix, this->loadKey); // increases level

            switch (res)
            {
            case CheckPrefixPessimisticResult::NoMatch:
            {
                // Create new node which will be parent of node, Set common prefix, level
                auto newNode = new N4(node->getPrefix(), nextLevel - level);
                if (!newNode)
                {
                    delete kvp;
                    return 0;
                }

                // Add node and (kvp, *k) as children
                newNode->insert(k[nextLevel], N::setLeaf(kvp));
                newNode->insert(nonMatchingKey, node);

                // Update parentNode to point to the new node
                if (parentNode)
                {
                    N::change(parentNode, parentKey, newNode);
                }
                else
                {
                    root = newNode;
                }

                // Update prefix of node
                node->setPrefix(remainingPrefix, node->getPrefixLength() - ((nextLevel - level) + 1));
                return 1;
            }
            case CheckPrefixPessimisticResult::Match:
                break;
            }
            level = nextLevel;

            if (level >= k.getKeyLen())
            {
                // This shouldn't happen in a normal insert operation
                delete kvp;
                return 0;
            }

            nodeKey = k[level];
            nextNode = N::getChild(nodeKey, node);

            if (nextNode == nullptr)
            {
                // Simple case: insert the new leaf node
                switch (node->getType())
                {
                case NTypes::N4:
                {
                    auto n = static_cast<N4 *>(node);
                    insertGrow<N4, N16>(n, parentNode, parentKey, nodeKey, N::setLeaf(kvp));
                    break;
                }
                case NTypes::N16:
                {
                    auto n = static_cast<N16 *>(node);
                    insertGrow<N16, N48>(n, parentNode, parentKey, nodeKey, N::setLeaf(kvp));
                    break;
                }
                case NTypes::N48:
                {
                    auto n = static_cast<N48 *>(node);
                    insertGrow<N48, N256>(n, parentNode, parentKey, nodeKey, N::setLeaf(kvp));
                    break;
                }
                case NTypes::N256:
                {
                    auto n = static_cast<N256 *>(node);
                    n->insert(nodeKey, N::setLeaf(kvp));
                    break;
                }
                }
                return 1;
            }

            if (N::isLeaf(nextNode))
            {
                // Check if key already exists
                KeyValuePair *existingKvp = N::getLeaf(nextNode);

                if (existingKvp && existingKvp->key_len == key_len &&
                    memcmp(key, existingKvp->key, key_len) == 0)
                {
                    // Key exists - update value (upsert)
                    N::change(node, k[level], N::setLeaf(kvp));

                    // Immediately delete the old KeyValuePair
                    delete existingKvp;
                    return 1;
                }

                // If keys differ, create a new node
                Key otherKey;
                if (loadKey)
                {
                    // Use custom key loader if provided
                    uintptr_t tid = reinterpret_cast<uintptr_t>(existingKvp);
                    loadKey(tid, otherKey);
                }
                else if (existingKvp)
                {
                    // Otherwise use the existing key-value pair key
                    otherKey.set(static_cast<const char *>(static_cast<void *>(existingKvp->key)), existingKvp->key_len);
                }
                else
                {
                    // If existingKvp is somehow null, we can't continue safely
                    delete kvp;
                    return 0;
                }

                level++;
                uint32_t prefixLength = 0;
                // Find the shared prefix length
                while (level + prefixLength < k.getKeyLen() &&
                       level + prefixLength < otherKey.getKeyLen() &&
                       k[level + prefixLength] == otherKey[level + prefixLength])
                {
                    prefixLength++;
                }

                auto n4 = new N4(&k[level], prefixLength);
                if (!n4)
                {
                    delete kvp;
                    return 0;
                }

                n4->insert(k[level + prefixLength], N::setLeaf(kvp));
                n4->insert(otherKey[level + prefixLength], nextNode);
                N::change(node, k[level - 1], n4);
                return 1;
            }
            level++;
        }
    }

    void *dict::search(void *key, size_t key_len)
    {
        if (!key || key_len == 0)
            return nullptr;

        // Create a Key object from the raw key
        Key k;
        k.set(static_cast<const char *>(key), key_len);

        // Acquire read lock for the entire operation
        std::shared_lock<std::shared_mutex> lock(rw_mutex);

        N *node = nullptr;
        N *nextNode = root;
        uint32_t level = 0;
        bool optimisticPrefixMatch = false;

        node = root;
        if (!node)
            return nullptr;

        while (true)
        {
            switch (checkPrefix(node, k, level))
            { // increases level
            case CheckPrefixResult::NoMatch:
                return nullptr;
            case CheckPrefixResult::OptimisticMatch:
                optimisticPrefixMatch = true;
                // fallthrough
            case CheckPrefixResult::Match:
                if (k.getKeyLen() <= level)
                {
                    return nullptr;
                }
                nextNode = N::getChild(k[level], node);

                if (nextNode == nullptr)
                {
                    return nullptr;
                }

                if (N::isLeaf(nextNode))
                {
                    // Get the KeyValuePair from the leaf
                    KeyValuePair *kvp = N::getLeaf(nextNode);

                    if (level < k.getKeyLen() - 1 || optimisticPrefixMatch)
                    {
                        // Need to verify key match
                        if (kvp && kvp->key_len == key_len &&
                            memcmp(static_cast<const void *>(kvp->key), key, key_len) == 0)
                        {
                            return kvp->value;
                        }
                        return nullptr;
                    }

                    // Direct match
                    if (kvp && kvp->key_len == key_len &&
                        memcmp(kvp->key, key, key_len) == 0)
                    {
                        return kvp->value;
                    }
                    return nullptr;
                }
                node = nextNode;
                level++;
                break;
            }
        }
    }

    void *dict::remove(void *key, size_t key_len)
    {
        if (!key || key_len == 0)
            return nullptr;

        // Create a Key object from the raw key
        Key k;
        k.set(static_cast<const char *>(key), key_len);

        // Acquire write lock for the entire operation
        std::unique_lock<std::shared_mutex> lock(rw_mutex);

        int restartCount = 0;
        if (restartCount++)
            yield(restartCount);

        N *node = nullptr;
        N *nextNode = root;
        N *parentNode = nullptr;
        uint8_t parentKey = 0, nodeKey = 0;
        uint32_t level = 0;
        void *result = nullptr;

        while (true)
        {
            parentNode = node;
            parentKey = nodeKey;
            node = nextNode;

            if (!node)
                return nullptr;

            switch (checkPrefix(node, k, level))
            { // increases level
            case CheckPrefixResult::NoMatch:
                return nullptr;
            case CheckPrefixResult::OptimisticMatch:
                // fallthrough
            case CheckPrefixResult::Match:
            {
                if (level >= k.getKeyLen())
                    return nullptr;

                nodeKey = k[level];
                nextNode = N::getChild(nodeKey, node);

                if (nextNode == nullptr)
                {
                    return nullptr;
                }

                if (N::isLeaf(nextNode))
                {
                    // Get the KeyValuePair from the leaf
                    KeyValuePair *kvp = N::getLeaf(nextNode);

                    // Verify it's actually the key we want to remove
                    if (!kvp || kvp->key_len != key_len ||
                        memcmp(kvp->key, key, key_len) != 0)
                    {
                        return nullptr;
                    }

                    // Save value to return
                    if (kvp)
                    {
                        // Make a copy of the value to return
                        void *valueCopy = jem_malloc(kvp->value_len);
                        if (valueCopy)
                        {
                            memcpy(valueCopy, kvp->value, kvp->value_len);
                            result = valueCopy;
                        }
                    }

                    if (node->getCount() == 2 && parentNode != nullptr)
                    {
                        // Special case: node has only two children and we're removing one

                        // 1. check remaining entries
                        N *secondNodeN;
                        uint8_t secondNodeK;
                        std::tie(secondNodeN, secondNodeK) = N::getSecondChild(node, nodeKey);
                        if (N::isLeaf(secondNodeN))
                        {
                            // Simply replace the node with the second child
                            N::change(parentNode, parentKey, secondNodeN);

                            // Immediately delete the KeyValuePair and node
                            delete kvp;
                            delete node;
                        }
                        else
                        {
                            // Replace the node with the second child and update prefix
                            N::change(parentNode, parentKey, secondNodeN);
                            secondNodeN->addPrefixBefore(node, secondNodeK);

                            // Immediately delete the KeyValuePair and node
                            delete kvp;
                            delete node;
                        }
                    }
                    else
                    {
                        // Normal case: just remove the child
                        switch (node->getType())
                        {
                        case NTypes::N4:
                        {
                            auto n = static_cast<N4 *>(node);
                            removeAndShrink<N4, N4>(n, parentNode, parentKey, nodeKey);
                            break;
                        }
                        case NTypes::N16:
                        {
                            auto n = static_cast<N16 *>(node);
                            removeAndShrink<N16, N4>(n, parentNode, parentKey, nodeKey);
                            break;
                        }
                        case NTypes::N48:
                        {
                            auto n = static_cast<N48 *>(node);
                            removeAndShrink<N48, N16>(n, parentNode, parentKey, nodeKey);
                            break;
                        }
                        case NTypes::N256:
                        {
                            auto n = static_cast<N256 *>(node);
                            removeAndShrink<N256, N48>(n, parentNode, parentKey, nodeKey);
                            break;
                        }
                        }

                        // Immediately delete the KeyValuePair
                        delete kvp;
                    }
                    return result;
                }
                level++;
            }
            }
        }
    }

    bool dict::scan(void *startKey, size_t startKeyLen, void *endKey, size_t endKeyLen,
                    std::vector<std::pair<void *, void *>> &results, size_t maxResults)
    {
        // Early return if maxResults is 0
        if (maxResults == 0)
            return false;

        // Acquire read lock for the entire operation
        std::shared_lock<std::shared_mutex> lock(rw_mutex);

        // Initialize startKey and endKey
        Key start, end;
        if (startKey)
        {
            start.set(static_cast<const char *>(startKey), startKeyLen);
        }
        else
        {
            // If no start key, use empty string (will start from beginning)
            start.set("", 0);
        }

        if (endKey)
        {
            end.set(static_cast<const char *>(endKey), endKeyLen);
        }
        else
        {
            // If no end key, use max key (will end at last entry)
            uint8_t maxKeyBytes[1] = {255};
            end.set(reinterpret_cast<const char *>(maxKeyBytes), 1);
        }

        // Verify the start key is less than or equal to end key
        for (uint32_t i = 0; i < std::min(start.getKeyLen(), end.getKeyLen()); ++i)
        {
            if (start[i] > end[i])
            {
                return false; // Invalid range
            }
            else if (start[i] < end[i])
            {
                break;
            }
        }

        // For collecting scan results
        struct KeyValueResult
        {
            KeyValuePair *kvp;
            KeyValueResult(KeyValuePair *k) : kvp(k) {}
        };

        std::vector<KeyValueResult> kvpResults;
        kvpResults.reserve(maxResults);
        size_t resultsFound = 0;
        KeyValuePair *nextKVP = nullptr;

        // Function to collect nodes recursively
        std::function<void(const N *)> copy = [&](const N *node)
        {
            if (!node)
                return;

            if (N::isLeaf(node))
            {
                if (resultsFound == maxResults)
                {
                    nextKVP = N::getLeaf(node);
                    return;
                }
                KeyValuePair *kvp = N::getLeaf(node);
                if (kvp)
                {
                    kvpResults.push_back(KeyValueResult(kvp));
                    resultsFound++;
                }
            }
            else
            {
                std::tuple<uint8_t, N *> children[256];
                uint32_t childrenCount = 0;
                N::getChildren(node, 0u, 255u, children, childrenCount);

                for (uint32_t i = 0; i < childrenCount; ++i)
                {
                    const N *n = std::get<1>(children[i]);
                    copy(n);
                    if (nextKVP != nullptr)
                    {
                        break;
                    }
                }
            }
        };

        uint32_t level = 0;
        N *node = nullptr;
        N *nextNode = root;
        N *parentNode = nullptr;

        while (true)
        {
            parentNode = node;
            node = nextNode;

            if (!node)
                break;

            auto prefixResult = PCEqualsResults::BothMatch; // Default

            // Check prefix
            if (node->hasPrefix())
            {
                uint32_t nodeLevel = level;
                for (uint32_t i = 0; i < node->getPrefixLength(); ++i)
                {
                    uint8_t startLevel = (start.getKeyLen() > nodeLevel) ? start[nodeLevel] : 0;
                    uint8_t endLevel = (end.getKeyLen() > nodeLevel) ? end[nodeLevel] : 255;

                    uint8_t curKey = (i < maxStoredPrefixLength) ? node->getPrefix()[i] : 0;

                    if (curKey > startLevel && curKey < endLevel)
                    {
                        prefixResult = PCEqualsResults::Contained;
                        break;
                    }
                    else if (curKey < startLevel || curKey > endLevel)
                    {
                        prefixResult = PCEqualsResults::NoMatch;
                        break;
                    }
                    nodeLevel++;
                }
                level = nodeLevel;
            }

            switch (prefixResult)
            {
            case PCEqualsResults::NoMatch:
                return false;
            case PCEqualsResults::Contained:
                copy(node);
                break;
            case PCEqualsResults::BothMatch:
            {
                uint8_t startLevel = (start.getKeyLen() > level) ? start[level] : 0;
                uint8_t endLevel = (end.getKeyLen() > level) ? end[level] : 255;

                if (startLevel != endLevel)
                {
                    std::tuple<uint8_t, N *> children[256];
                    uint32_t childrenCount = 0;
                    N::getChildren(node, startLevel, endLevel, children, childrenCount);

                    for (uint32_t i = 0; i < childrenCount; ++i)
                    {
                        const uint8_t k = std::get<0>(children[i]);
                        N *n = std::get<1>(children[i]);

                        if (!n)
                            continue;

                        if (k == startLevel)
                        {
                            // Find start path
                            if (N::isLeaf(n))
                            {
                                // Check if this key is >= startKey
                                KeyValuePair *kvp = N::getLeaf(n);
                                if (kvp)
                                {
                                    Key leafKey;
                                    leafKey.set(static_cast<const char *>(static_cast<void *>(kvp->key)), kvp->key_len);

                                    // Compare with startKey
                                    bool isGreaterOrEqual = true;
                                    for (uint32_t j = 0; j < std::min(leafKey.getKeyLen(), start.getKeyLen()); ++j)
                                    {
                                        if (leafKey[j] < start[j])
                                        {
                                            isGreaterOrEqual = false;
                                            break;
                                        }
                                        else if (leafKey[j] > start[j])
                                        {
                                            break;
                                        }
                                    }

                                    if (isGreaterOrEqual)
                                    {
                                        kvpResults.push_back(KeyValueResult(kvp));
                                        resultsFound++;
                                        if (resultsFound == maxResults)
                                        {
                                            nextKVP = kvp;
                                            break;
                                        }
                                    }
                                }
                            }
                            else
                            {
                                // Complex logic for finding start path
                                // For simplicity, just copy the node for now
                                copy(n);
                            }
                        }
                        else if (k > startLevel && k < endLevel)
                        {
                            copy(n);
                        }
                        else if (k == endLevel)
                        {
                            // Find end path
                            if (N::isLeaf(n))
                            {
                                // Check if this key is <= endKey
                                KeyValuePair *kvp = N::getLeaf(n);
                                if (kvp)
                                {
                                    Key leafKey;
                                    leafKey.set(static_cast<const char *>(static_cast<void *>(kvp->key)), kvp->key_len);

                                    // Compare with endKey
                                    bool isLessOrEqual = true;
                                    for (uint32_t j = 0; j < std::min(leafKey.getKeyLen(), end.getKeyLen()); ++j)
                                    {
                                        if (leafKey[j] > end[j])
                                        {
                                            isLessOrEqual = false;
                                            break;
                                        }
                                        else if (leafKey[j] < end[j])
                                        {
                                            break;
                                        }
                                    }

                                    if (isLessOrEqual)
                                    {
                                        kvpResults.push_back(KeyValueResult(kvp));
                                        resultsFound++;
                                        if (resultsFound == maxResults)
                                        {
                                            nextKVP = kvp;
                                            break;
                                        }
                                    }
                                }
                            }
                            else
                            {
                                // Complex logic for finding end path
                                // For simplicity, just copy the node for now
                                copy(n);
                            }
                        }

                        if (nextKVP != nullptr)
                        {
                            break;
                        }
                    }
                }
                else
                {
                    nextNode = N::getChild(startLevel, node);
                    level++;
                    continue;
                }
                break;
            }
            }
            break;
        }

        // Process collected KeyValuePairs and convert to key-value pairs
        results.clear();
        results.reserve(kvpResults.size());

        for (const auto &result : kvpResults)
        {
            KeyValuePair *kvp = result.kvp;
            if (kvp)
            {
                // Make copies of key and value for the result
                void *keyCopy = jem_malloc(kvp->key_len);
                if (!keyCopy)
                    continue;

                memcpy(keyCopy, kvp->key, kvp->key_len);

                void *valueCopy = jem_malloc(kvp->value_len);
                if (!valueCopy)
                {
                    jem_free(keyCopy);
                    continue;
                }

                memcpy(valueCopy, kvp->value, kvp->value_len);

                results.emplace_back(keyCopy, valueCopy);
            }
        }

        // Return true if there are more results to fetch
        return nextKVP != nullptr;
    }

    dict::CheckPrefixResult dict::checkPrefix(N *n, const Key &k, uint32_t &level)
    {
        if (!n)
            return CheckPrefixResult::NoMatch;

        if (n->hasPrefix())
        {
            if (k.getKeyLen() <= level + n->getPrefixLength())
            {
                return CheckPrefixResult::NoMatch;
            }
            for (uint32_t i = 0; i < std::min(n->getPrefixLength(), maxStoredPrefixLength); ++i)
            {
                if (level < k.getKeyLen() && n->getPrefix()[i] != k[level])
                {
                    return CheckPrefixResult::NoMatch;
                }
                ++level;
            }
            if (n->getPrefixLength() > maxStoredPrefixLength)
            {
                level = level + (n->getPrefixLength() - maxStoredPrefixLength);
                return CheckPrefixResult::OptimisticMatch;
            }
        }
        return CheckPrefixResult::Match;
    }

    dict::CheckPrefixPessimisticResult dict::checkPrefixPessimistic(
        N *n, const Key &k, uint32_t &level, uint8_t &nonMatchingKey,
        Prefix &nonMatchingPrefix, LoadKeyFunction loadKey)
    {
        if (!n)
            return CheckPrefixPessimisticResult::NoMatch;

        if (n->hasPrefix())
        {
            uint32_t prevLevel = level;
            Key kt;
            for (uint32_t i = 0; i < n->getPrefixLength(); ++i)
            {
                if (i == maxStoredPrefixLength)
                {
                    KeyValuePair *anyKVP = N::getAnyChildTid(n);
                    if (!anyKVP)
                    {
                        return CheckPrefixPessimisticResult::Match;
                    }

                    if (loadKey)
                    {
                        // Use custom key loader if provided
                        uintptr_t tid = reinterpret_cast<uintptr_t>(anyKVP);
                        loadKey(tid, kt);
                    }
                    else if (anyKVP)
                    {
                        // Use the key from the KeyValuePair
                        kt.set(static_cast<const char *>(static_cast<void *>(anyKVP->key)), anyKVP->key_len);
                    }
                    else
                    {
                        return CheckPrefixPessimisticResult::Match;
                    }
                }

                // Make sure we don't access beyond the key's length
                if (level >= k.getKeyLen())
                {
                    return CheckPrefixPessimisticResult::NoMatch;
                }

                uint8_t curKey = i >= maxStoredPrefixLength ? kt[level] : n->getPrefix()[i];
                if (curKey != k[level])
                {
                    nonMatchingKey = curKey;
                    if (n->getPrefixLength() > maxStoredPrefixLength)
                    {
                        if (i < maxStoredPrefixLength)
                        {
                            KeyValuePair *anyKVP = N::getAnyChildTid(n);
                            if (!anyKVP)
                            {
                                return CheckPrefixPessimisticResult::Match;
                            }

                            if (loadKey)
                            {
                                // Use custom key loader if provided
                                uintptr_t tid = reinterpret_cast<uintptr_t>(anyKVP);
                                loadKey(tid, kt);
                            }
                            else if (anyKVP)
                            {
                                // Use the key from the KeyValuePair
                                kt.set(static_cast<const char *>(static_cast<void *>(anyKVP->key)), anyKVP->key_len);
                            }
                            else
                            {
                                return CheckPrefixPessimisticResult::Match;
                            }
                        }
                        memcpy(nonMatchingPrefix, &kt[0] + level + 1, std::min((n->getPrefixLength() - (level - prevLevel) - 1), maxStoredPrefixLength));
                    }
                    else
                    {
                        memcpy(nonMatchingPrefix, n->getPrefix() + i + 1, n->getPrefixLength() - i - 1);
                    }
                    return CheckPrefixPessimisticResult::NoMatch;
                }
                ++level;
            }
        }
        return CheckPrefixPessimisticResult::Match;
    }

    dict::PCCompareResults dict::checkPrefixCompare(const N *n, const Key &k, uint8_t fillKey, uint32_t &level,
                                                    LoadKeyFunction loadKey)
    {
        if (!n)
            return PCCompareResults::Equal;

        if (n->hasPrefix())
        {
            Key kt;
            for (uint32_t i = 0; i < n->getPrefixLength(); ++i)
            {
                if (i == maxStoredPrefixLength)
                {
                    KeyValuePair *anyKVP = N::getAnyChildTid(n);
                    if (!anyKVP)
                    {
                        return PCCompareResults::Equal;
                    }

                    if (loadKey)
                    {
                        // Use custom key loader if provided
                        uintptr_t tid = reinterpret_cast<uintptr_t>(anyKVP);
                        loadKey(tid, kt);
                    }
                    else if (anyKVP)
                    {
                        // Use the key from the KeyValuePair
                        kt.set(static_cast<const char *>(static_cast<void *>(anyKVP->key)), anyKVP->key_len);
                    }
                    else
                    {
                        return PCCompareResults::Equal;
                    }
                }
                uint8_t kLevel = (k.getKeyLen() > level) ? k[level] : fillKey;
                uint8_t curKey = i >= maxStoredPrefixLength ? kt[level] : n->getPrefix()[i];
                if (curKey < kLevel)
                {
                    return PCCompareResults::Smaller;
                }
                else if (curKey > kLevel)
                {
                    return PCCompareResults::Bigger;
                }
                ++level;
            }
        }
        return PCCompareResults::Equal;
    }

    dict::PCEqualsResults dict::checkPrefixEquals(const N *n, uint32_t &level, const Key &start, const Key &end,
                                                  LoadKeyFunction loadKey)
    {
        if (!n)
            return PCEqualsResults::NoMatch;

        if (n->hasPrefix())
        {
            Key kt;
            for (uint32_t i = 0; i < n->getPrefixLength(); ++i)
            {
                if (i == maxStoredPrefixLength)
                {
                    KeyValuePair *anyKVP = N::getAnyChildTid(n);
                    if (!anyKVP)
                    {
                        return PCEqualsResults::BothMatch;
                    }

                    if (loadKey)
                    {
                        // Use custom key loader if provided
                        uintptr_t tid = reinterpret_cast<uintptr_t>(anyKVP);
                        loadKey(tid, kt);
                    }
                    else if (anyKVP)
                    {
                        // Use the key from the KeyValuePair
                        kt.set(static_cast<const char *>(static_cast<void *>(anyKVP->key)), anyKVP->key_len);
                    }
                    else
                    {
                        return PCEqualsResults::BothMatch;
                    }
                }
                uint8_t startLevel = (start.getKeyLen() > level) ? start[level] : 0;
                uint8_t endLevel = (end.getKeyLen() > level) ? end[level] : 255;

                uint8_t curKey = i >= maxStoredPrefixLength ? kt[level] : n->getPrefix()[i];
                if (curKey > startLevel && curKey < endLevel)
                {
                    return PCEqualsResults::Contained;
                }
                else if (curKey < startLevel || curKey > endLevel)
                {
                    return PCEqualsResults::NoMatch;
                }
                ++level;
            }
        }
        return PCEqualsResults::BothMatch;
    }


    /*
     * Ordered DFS for range scans. Children are visited in byte order, so
     * emission is lexicographic. `bounded` means this subtree may still
     * contain keys < start: prefix bytes and child bytes prune it. A node
     * whose prefix is truncated (prefixCount > maxStoredPrefixLength)
     * cannot be compared reliably, so its subtree is walked unpruned and
     * the leaf-level strcmp guard keeps the output correct.
     */
    void dict::scanCollect(N *node, const uint8_t *start, size_t start_len,
                           size_t depth, bool bounded, size_t n,
                           std::vector<std::pair<std::string, std::string>> &out)
    {
        if (out.size() >= n)
        {
            return;
        }
        if (N::isLeaf(node))
        {
            KeyValuePair *kvp = N::getLeaf(node);
            char *k = static_cast<char *>(static_cast<void *>(kvp->key));
            if (strcmp(k, (const char *)start) >= 0)
            {
                out.emplace_back(std::string(k),
                                 std::string(static_cast<char *>(static_cast<void *>(kvp->value))));
            }
            return;
        }

        uint32_t plen = node->hasPrefix() ? node->getPrefixLength() : 0;
        if (bounded && plen > 0)
        {
            uint32_t stored = std::min(plen, maxStoredPrefixLength);
            uint32_t avail = (depth < start_len) ? (uint32_t)(start_len - depth) : 0;
            uint32_t cmpLen = std::min(stored, avail);
            int c = cmpLen ? memcmp(node->getPrefix(), start + depth, cmpLen) : 0;
            if (c < 0)
            {
                return; // whole subtree < start
            }
            if (c > 0 || avail <= stored)
            {
                bounded = false; // whole subtree >= start
            }
            else if (plen > stored)
            {
                bounded = false; // truncated prefix: verify at leaves instead
            }
        }
        depth += plen;

        std::tuple<uint8_t, N *> children[256];
        uint32_t cnt = 0;
        N::getChildren(node, 0u, 255u, children, cnt);
        uint8_t boundByte = (bounded && depth < start_len) ? start[depth] : 0;
        for (uint32_t i = 0; i < cnt && out.size() < n; i++)
        {
            uint8_t b = std::get<0>(children[i]);
            N *child = std::get<1>(children[i]);
            if (bounded && b < boundByte)
            {
                continue; // subtree entirely < start
            }
            scanCollect(child, start, start_len, depth + 1,
                        bounded && b == boundByte, n, out);
        }
    }

    /*
     * Range scan: copies up to n key/value pairs starting at the first
     * key >= start_key, in key order, inside the operation's TAG scope.
     */
    int dict::scanCopy(void *start_key, size_t key_len, int n,
                       std::vector<std::pair<std::string, std::string>> &out)
    {
        std::shared_lock<std::shared_mutex> lock(rw_mutex);
        if (root == nullptr || n <= 0)
        {
            return 0;
        }
        scanCollect(root, (const uint8_t *)start_key, key_len, 0, true, (size_t)n, out);
        return (int)out.size();
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

} // namespace trie_art