#include "bpt_mass.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <cassert>

namespace bpt_mass
{

    //------------------------------------------------------------------
    // Version implementation
    //------------------------------------------------------------------
    Version::Version() noexcept
        : v_split{0}, v_insert{0}, is_border{false}, is_root{false}, deleted{false}, splitting{false}, inserting{false}, locked{false}
    {
        // Initialization order matches the struct declaration
    }

    bool Version::splitHappened(const Version &before, const Version &after)
    {
        assert(after.v_split >= before.v_split);
        return before.v_split != after.v_split;
    }

    uint32_t Version::operator^(const Version &rhs) const
    {
        return (body ^ rhs.body);
    }

    //------------------------------------------------------------------
    // SliceWithSize implementation
    //------------------------------------------------------------------
    SliceWithSize::SliceWithSize(KeySlice slice_, uint8_t size_)
        : slice(slice_), size(size_)
    {
        assert(1 <= size && size <= 8);
    }

    bool SliceWithSize::operator==(const SliceWithSize &rhs) const
    {
        return slice == rhs.slice && size == rhs.size;
    }

    bool SliceWithSize::operator!=(const SliceWithSize &rhs) const
    {
        return !(*this == rhs);
    }

    //------------------------------------------------------------------
    // Key implementation
    //------------------------------------------------------------------
    Key::Key()
        : lastSliceSize(0), cursor(0)
    {
    }

    Key::Key(std::vector<KeySlice> slices_, size_t lastSliceSize_) noexcept
        : slices(std::move(slices_)), lastSliceSize(lastSliceSize_), cursor(0)
    {
        assert(1 <= lastSliceSize && lastSliceSize <= 8);
    }

    Key::Key(const Key &other)
        : slices(other.slices), lastSliceSize(other.lastSliceSize), cursor(other.cursor)
    {
    }

    Key::Key(Key &&other) noexcept
        : slices(std::move(other.slices)), lastSliceSize(other.lastSliceSize), cursor(other.cursor)
    {
    }

    Key &Key::operator=(const Key &other)
    {
        if (this != &other)
        {
            slices = other.slices;
            lastSliceSize = other.lastSliceSize;
            cursor = other.cursor;
        }
        return *this;
    }

    Key &Key::operator=(Key &&other) noexcept
    {
        if (this != &other)
        {
            slices = std::move(other.slices);
            lastSliceSize = other.lastSliceSize;
            cursor = other.cursor;
        }
        return *this;
    }

    Key Key::fromBytes(const void *data, size_t length)
    {
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        std::vector<KeySlice> key_slices;

        // Process complete 8-byte chunks
        size_t fullChunks = length / 8;
        for (size_t i = 0; i < fullChunks; i++)
        {
            KeySlice slice = 0;
            for (size_t j = 0; j < 8; j++)
            {
                slice |= static_cast<KeySlice>(bytes[i * 8 + j]) << (j * 8);
            }
            key_slices.push_back(slice);
        }

        // Process the last partial chunk if exists
        size_t remaining = length % 8;
        if (remaining > 0)
        {
            KeySlice slice = 0;
            for (size_t j = 0; j < remaining; j++)
            {
                slice |= static_cast<KeySlice>(bytes[fullChunks * 8 + j]) << (j * 8);
            }
            key_slices.push_back(slice);
        }

        // If no remaining bytes, set lastSliceSize to 8 (complete slice)
        size_t lastSize = (remaining > 0) ? remaining : 8;
        return Key(key_slices, lastSize);
    }

    bool Key::hasNext() const
    {
        if (slices.size() == cursor + 1)
            return false;
        return true;
    }

    size_t Key::remainLength(size_t from) const
    {
        assert(from <= slices.size() - 1);
        return (slices.size() - from - 1) * 8 + lastSliceSize;
    }

    size_t Key::getCurrentSliceSize() const
    {
        if (hasNext())
        {
            return 8;
        }
        else
        {
            return lastSliceSize;
        }
    }

    SliceWithSize Key::getCurrentSlice() const
    {
        return SliceWithSize(slices[cursor], getCurrentSliceSize());
    }

    void Key::next()
    {
        assert(hasNext());
        ++cursor;
    }

    void Key::back()
    {
        assert(cursor != 0);
        --cursor;
    }

    void Key::reset()
    {
        cursor = 0;
    }

    bool Key::operator==(const Key &rhs) const
    {
        return lastSliceSize == rhs.lastSliceSize && cursor == rhs.cursor && slices == rhs.slices;
    }

    bool Key::operator!=(const Key &rhs) const
    {
        return !(*this == rhs);
    }

    //------------------------------------------------------------------
    // Value implementation
    //------------------------------------------------------------------
    Value::Value(void *data_, size_t length_)
        : length(length_)
    {
        // Make a copy of the data
        data = jem_malloc(length);
        if (data)
        {
            memcpy(data, data_, length);
        }
        else
        {
            throw std::bad_alloc();
        }
    }

    Value::~Value()
    {
        if (data)
        {
#if CREST_RAW_POINTERS
            jem_free(data);
#else
            data.destroy();
#endif
            data = nullptr;
        }
    }

    void *Value::getData()
    {
        return static_cast<void *>(data);
    }

    size_t Value::getLength() const
    {
        return length;
    }

    //------------------------------------------------------------------
    // LinkOrValue implementation
    //------------------------------------------------------------------
    LinkOrValue::LinkOrValue()
        : next_layer(nullptr)
    {
    }

    LinkOrValue::LinkOrValue(Node *next)
        : next_layer(next)
    {
    }

    LinkOrValue::LinkOrValue(Value *value_)
        : value(value_)
    {
    }

    LinkOrValue::LinkOrValue(const LinkOrValue &other)
        : next_layer(other.next_layer)
    {
    }

    //------------------------------------------------------------------
    // Permutation implementation
    //------------------------------------------------------------------
    Permutation::Permutation()
        : body(0)
    {
    }

    Permutation::Permutation(const Permutation &other)
        : body(other.body)
    {
    }

    Permutation &Permutation::operator=(const Permutation &other)
    {
        if (this != &other)
        {
            body = other.body;
        }
        return *this;
    }

    uint8_t Permutation::getNumKeys() const
    {
        return body & 0b1111LLU;
    }

    void Permutation::setNumKeys(size_t num)
    {
        assert(0 <= num && num <= 15);
        auto rm_num = body >> 4LU;
        rm_num = rm_num << 4LU;
        body = rm_num | num;
    }

    void Permutation::incNumKeys()
    {
        assert(getNumKeys() <= 14);
        setNumKeys(getNumKeys() + 1);
    }

    void Permutation::decNumKeys()
    {
        assert(1 <= getNumKeys());
        setNumKeys(getNumKeys() - 1);
    }

    uint8_t Permutation::getKeyIndex(size_t i) const
    {
        assert(0 <= i && i < getNumKeys());

        auto rshift = body >> (15 - i) * 4;
        auto result = rshift & 0b1111LLU;
        assert(0 <= result && result <= 14);
        return result;
    }

    void Permutation::setKeyIndex(size_t i, uint8_t true_index)
    {
        assert(0 <= i && i <= 14);
        assert(0 <= true_index && true_index <= 14);

        auto left = i == 0 ? 0 : (body >> (16 - i) * 4) << (16 - i) * 4;
        auto right = body & ((1LLU << ((15 - i) * 4)) - 1);
        auto middle = true_index * (1LLU << (15 - i) * 4);
        body = left | middle | right;

        auto check = (body >> (15 - i) * 4) & 0b1111LLU;
        assert(0 <= check && check <= 14);
    }

    void Permutation::removeIndex(uint8_t true_index)
    {
        uint8_t removed_ps_index = 0; // Initialize to avoid warning
        bool found = false;
        for (uint8_t i = 0; i < getNumKeys(); ++i)
        {
            if (getKeyIndex(i) == true_index)
            {
                removed_ps_index = i;
                found = true;
                break;
            }
        }

        assert(found); // Make sure we found the index

        for (uint8_t target = removed_ps_index; target + 1 <= getNumKeys() - 1; ++target)
        {
            setKeyIndex(target, getKeyIndex(target + 1));
        }
        decNumKeys();
    }

    uint8_t Permutation::operator()(size_t i) const
    {
        return getKeyIndex(i);
    }

    bool Permutation::isNotFull() const
    {
        auto num = getNumKeys();
        return num != 15;
    }

    bool Permutation::isFull() const
    {
        return !isNotFull();
    }

    void Permutation::insert(size_t insertion_point_ps, size_t index_ts)
    {
        for (size_t i = getNumKeys(); i > insertion_point_ps; --i)
        { // Right shift
            setKeyIndex(i, getKeyIndex(i - 1));
        }
        setKeyIndex(insertion_point_ps, index_ts);
        incNumKeys();
    }

    Permutation Permutation::sizeOne()
    {
        Permutation p{};
        p.setNumKeys(1);
        p.setKeyIndex(0, 0);
        return p;
    }

    Permutation Permutation::fromSorted(size_t n_keys)
    {
        Permutation p{};
        for (size_t i = 0; i < n_keys; ++i)
        {
            p.setKeyIndex(i, i);
        }
        p.setNumKeys(n_keys);
        return p;
    }

    Permutation Permutation::from(const std::vector<size_t> &vec)
    {
        Permutation p{};
        for (size_t i = 0; i < vec.size(); ++i)
        {
            p.setKeyIndex(i, vec[i]);
        }
        p.setNumKeys(vec.size());
        return p;
    }

    //------------------------------------------------------------------
    // BigSuffix implementation
    //------------------------------------------------------------------
    BigSuffix::BigSuffix()
        : lastSliceSize(0)
    {
    }

    BigSuffix::BigSuffix(const BigSuffix &other)
        : slices(other.slices), lastSliceSize(other.lastSliceSize)
    {
    }

    BigSuffix::BigSuffix(std::vector<KeySlice> &&slices_, size_t lastSliceSize_)
        : slices(std::move(slices_)), lastSliceSize(lastSliceSize_)
    {
        assert(1 <= lastSliceSize && lastSliceSize <= 8);
    }

    BigSuffix::~BigSuffix()
    {
        // Nothing to clean up, vector handles its own memory
    }

    SliceWithSize BigSuffix::getCurrentSlice()
    {
        if (hasNext())
        {
            std::lock_guard<std::mutex> lock(suffixMutex);
            return SliceWithSize(slices[0], 8);
        }
        else
        {
            std::lock_guard<std::mutex> lock(suffixMutex);
            return SliceWithSize(slices[0], lastSliceSize);
        }
    }

    size_t BigSuffix::remainLength()
    {
        std::lock_guard<std::mutex> lock(suffixMutex);
        return (slices.size() - 1) * 8 + lastSliceSize;
    }

    bool BigSuffix::hasNext()
    {
        std::lock_guard<std::mutex> lock(suffixMutex);
        return slices.size() >= 2;
    }

    void BigSuffix::next()
    {
        assert(hasNext());
        std::lock_guard<std::mutex> lock(suffixMutex);
        slices.erase(slices.begin());
    }

    void BigSuffix::insertTop(KeySlice slice)
    {
        std::lock_guard<std::mutex> lock(suffixMutex);
        slices.insert(slices.begin(), slice);
    }

    bool BigSuffix::isSame(const Key &key, size_t from)
    {
        // Key's size and suffix's size comparison
        if (key.remainLength(from) != this->remainLength())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(suffixMutex);
        for (size_t i = 0; i < slices.size(); ++i)
        {
            if (key.slices[i + from] != slices[i])
            {
                return false;
            }
        }

        return true;
    }

    BigSuffix *BigSuffix::from(const Key &key, size_t from)
    {
        std::vector<KeySlice> tmp{};
        for (size_t j = from; j < key.slices.size(); ++j)
        {
            tmp.push_back(key.slices[j]);
        }
        return new BigSuffix(std::move(tmp), key.lastSliceSize);
    }

    void BigSuffix::copySlices(std::vector<KeySlice> &out_slices, size_t &out_lastSliceSize)
    {
        std::lock_guard<std::mutex> lock(suffixMutex);
        out_slices = slices;
        out_lastSliceSize = lastSliceSize;
    }

    //------------------------------------------------------------------
    // KeySuffix implementation
    //------------------------------------------------------------------
    KeySuffix::KeySuffix()
    {
        reset();
    }

    void KeySuffix::set(size_t i, const Key &key, size_t from)
    {
        suffixes[i].store(BigSuffix::from(key, from), WRITE_MEMORY_ORDER);
    }

    void KeySuffix::set(size_t i, BigSuffix *const &ptr)
    {
        suffixes[i].store(ptr, WRITE_MEMORY_ORDER);
    }

    BigSuffix *KeySuffix::get(size_t i) const
    {
        return suffixes[i].load(READ_MEMORY_ORDER);
    }

    void KeySuffix::unref(size_t i)
    {
        assert(get(i) != nullptr);
        set(i, nullptr);
    }

    void KeySuffix::delete_ptr(size_t i)
    {
        auto ptr = get(i);
        assert(ptr != nullptr);
        delete ptr;
        set(i, nullptr);
    }

    void KeySuffix::deleteAll()
    {
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            auto p = get(i);
            if (p != nullptr)
            {
                delete_ptr(i);
            }
        }
    }

    void KeySuffix::reset()
    {
        for (auto &suffix : suffixes)
        {
            suffix.store(nullptr, WRITE_MEMORY_ORDER);
        }
    }

    //------------------------------------------------------------------
    // GarbageCollector implementation
    //------------------------------------------------------------------
    GarbageCollector::GarbageCollector() = default;

    void GarbageCollector::add(BorderNode *b)
    {
        assert(!contain(b));
        assert(b->getDeleted());
        borders.push_back(b);
    }

    void GarbageCollector::add(InteriorNode *i)
    {
        assert(!contain(i));
        assert(i->getDeleted());
        interiors.push_back(i);
    }

    void GarbageCollector::add(Value *v)
    {
        assert(!contain(v));
        values.push_back(v);
    }

    void GarbageCollector::add(BigSuffix *suffix)
    {
        assert(!contain(suffix));
        suffixes.push_back(suffix);
    }

    bool GarbageCollector::contain(BorderNode const *n) const
    {
        return std::find(borders.begin(), borders.end(), n) != borders.end();
    }

    bool GarbageCollector::contain(InteriorNode const *n) const
    {
        return std::find(interiors.begin(), interiors.end(), n) != interiors.end();
    }

    bool GarbageCollector::contain(Value const *n) const
    {
        return std::find(values.begin(), values.end(), n) != values.end();
    }

    bool GarbageCollector::contain(BigSuffix const *suffix) const
    {
        return std::find(suffixes.begin(), suffixes.end(), suffix) != suffixes.end();
    }

    void GarbageCollector::run() noexcept
    {
        for (auto &b : borders)
        {
            // Destructor will release values and suffixes
            delete b;
        }
        borders.clear();

        for (auto &i : interiors)
        {
            delete i;
        }
        interiors.clear();

        for (auto &v : values)
        {
            delete v;
        }
        values.clear();

        for (auto &s : suffixes)
        {
            delete s;
        }
        suffixes.clear();
    }

    //------------------------------------------------------------------
    // Node implementation
    //------------------------------------------------------------------
    Node::Node()
        : version{}, parent{nullptr}, upperLayer{nullptr}
    {
    }

    Version Node::stableVersion() const
    {
        auto v = getVersion();
        while (v.inserting || v.splitting)
        {
            v = getVersion();
        }
        return v;
    }

    void Node::lock()
    {
        // No need to check if this is nullptr
        for (;;)
        {
            auto expected = getVersion();
            if (expected.locked)
            {
                continue;
            }
            else
            {
                // Lock is free
                auto desired = expected;
                expected.locked = false;
                desired.locked = true;
                if (version.compare_exchange_weak(expected, desired))
                {
                    break;
                }
            }
        }
    }

    void Node::unlock()
    {
        auto copy_v = getVersion();
        assert(copy_v.locked);
        assert(!(copy_v.inserting && copy_v.splitting));
        if (copy_v.inserting)
        {
            ++copy_v.v_insert;
        }
        else if (copy_v.splitting)
        {
            ++copy_v.v_split;
        }
        copy_v.locked = false;
        copy_v.inserting = false;
        copy_v.splitting = false;

        version.store(copy_v, WRITE_MEMORY_ORDER);
    }

    InteriorNode *Node::lockedParent() const
    {
    retry:
        auto p = getParent();
        if (p != nullptr)
        {
            p->lock();
        }
        // Parent changed underneath us
        if (p != getParent())
        {
            assert(p != nullptr);
            p->unlock();
            goto retry;
        }
        return p;
    }

    BorderNode *Node::lockedUpperNode() const
    {
    retry:
        auto p = getUpperLayer();
        if (p != nullptr)
        {
            p->lock();
        }
        // Upper layer changed underneath us
        if (p != getUpperLayer())
        {
            assert(p != nullptr);
            p->unlock();
            goto retry;
        }
        return p;
    }

    InteriorNode *Node::getParent() const
    {
        return parent.load(READ_MEMORY_ORDER);
    }

    void Node::setParent(InteriorNode *p)
    {
        // setParent requires parent to be locked
        if (p != nullptr)
        {
            assert(reinterpret_cast<Node *>(p)->isLocked());
        }
        parent.store(p, WRITE_MEMORY_ORDER);
    }

    BorderNode *Node::getUpperLayer() const
    {
        return upperLayer.load(READ_MEMORY_ORDER);
    }

    void Node::setUpperLayer(BorderNode *p)
    {
        // setUpperLayer requires upper layer to be locked
        if (p != nullptr)
        {
            assert(reinterpret_cast<Node *>(p)->isLocked());
        }
        upperLayer.store(p, WRITE_MEMORY_ORDER);
    }

    void Node::setIsBorder(bool is_border)
    {
        auto v = getVersion();
        v.is_border = is_border;
        version.store(v, WRITE_MEMORY_ORDER);
    }

    bool Node::getIsBorder() const
    {
        auto v = getVersion();
        return v.is_border;
    }

    void Node::setIsRoot(bool is_root)
    {
        auto v = getVersion();
        v.is_root = is_root;
        version.store(v, WRITE_MEMORY_ORDER);
    }

    bool Node::getIsRoot() const
    {
        auto v = getVersion();
        return v.is_root;
    }

    Version Node::getVersion() const
    {
        return version.load(READ_MEMORY_ORDER);
    }

    void Node::setVersion(const Version &v)
    {
        version.store(v, WRITE_MEMORY_ORDER);
    }

    bool Node::isLocked() const
    {
        auto v = getVersion();
        return v.locked;
    }

    bool Node::isUnlocked() const
    {
        return !isLocked();
    }

    bool Node::getSplitting() const
    {
        auto v = getVersion();
        return v.splitting;
    }

    void Node::setSplitting(bool splitting)
    {
        auto v = getVersion();
        assert(v.locked);
        v.splitting = splitting;
        version.store(v, WRITE_MEMORY_ORDER);
    }

    bool Node::getInserting() const
    {
        auto v = getVersion();
        return v.inserting;
    }

    void Node::setInserting(bool inserting)
    {
        auto v = getVersion();
        assert(v.locked);
        v.inserting = inserting;
        version.store(v, WRITE_MEMORY_ORDER);
    }

    bool Node::getDeleted() const
    {
        auto v = getVersion();
        return v.deleted;
    }

    void Node::setDeleted(bool deleted)
    {
        auto v = getVersion();
        v.deleted = deleted;
        version.store(v, WRITE_MEMORY_ORDER);
    }

    //------------------------------------------------------------------
    // InteriorNode implementation
    //------------------------------------------------------------------
    InteriorNode::InteriorNode()
        : Node(), n_keys(0)
    {
        setIsBorder(false);
        resetKeySlices();
        resetChildren();
    }

    Node *InteriorNode::findChild(KeySlice slice)
    {
        auto num_keys = getNumKeys();
        for (size_t i = 0; i < num_keys; ++i)
        {
            if (slice < key_slice[i].load(READ_MEMORY_ORDER))
            {
                return getChild(i);
            }
        }

        return getChild(num_keys);
    }

    bool InteriorNode::isNotFull() const
    {
        return getNumKeys() != NODE_ORDER - 1;
    }

    bool InteriorNode::isFull() const
    {
        return !isNotFull();
    }

    bool InteriorNode::debug_has_skip() const
    {
        for (size_t i = 1; i < NODE_ORDER - 2; ++i)
        {
            auto now = getKeySlice(i);
            if (getKeySlice(i - 1) && now == 0 && getKeySlice(i + 1) != 0)
            {
                return true;
            }
        }
        return false;
    }

    void InteriorNode::printNode() const
    {
        printf("/");
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            printf("%lu/", getKeySlice(i)); // Changed from %llu to %lu
        }
        printf("\\\n");
    }

    size_t InteriorNode::findChildIndex(Node *a_child) const
    {
        size_t index = 0;
        while (index <= getNumKeys() && getChild(index) != a_child)
        {
            ++index;
        }
        assert(getChild(index) == a_child);
        return index;
    }

    uint8_t InteriorNode::getNumKeys() const
    {
        return n_keys.load(READ_MEMORY_ORDER);
    }

    void InteriorNode::setNumKeys(uint8_t nKeys)
    {
        n_keys.store(nKeys, WRITE_MEMORY_ORDER);
    }

    void InteriorNode::incNumKeys()
    {
        n_keys.fetch_add(1);
    }

    void InteriorNode::decNumKeys()
    {
        n_keys.fetch_sub(1);
    }

    KeySlice InteriorNode::getKeySlice(size_t index) const
    {
        return key_slice[index].load(READ_MEMORY_ORDER);
    }

    void InteriorNode::resetKeySlices()
    {
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            setKeySlice(i, 0);
        }
    }

    void InteriorNode::setKeySlice(size_t index, const KeySlice &slice)
    {
        key_slice[index].store(slice, WRITE_MEMORY_ORDER);
    }

    Node *InteriorNode::getChild(size_t index) const
    {
        return child[index].load(READ_MEMORY_ORDER);
    }

    void InteriorNode::setChild(size_t index, Node *c)
    {
        assert(0 <= index && index <= 15);
        child[index].store(c, WRITE_MEMORY_ORDER);
    }

    bool InteriorNode::debug_contain_child(Node *c) const
    {
        for (size_t i = 0; i < NODE_ORDER; ++i)
        {
            if (getChild(i) == c)
                return true;
        }
        return false;
    }

    void InteriorNode::resetChildren()
    {
        for (size_t i = 0; i < NODE_ORDER; ++i)
        {
            setChild(i, nullptr);
        }
    }

    //------------------------------------------------------------------
    // BorderNode implementation
    //------------------------------------------------------------------
    BorderNode::BorderNode()
        : Node(), permutation(Permutation::sizeOne()), next{nullptr}, prev{nullptr}
    {
        setIsBorder(true);
        resetKeyLen();
        resetKeySlice();
        resetLVs();
        getKeySuffixes().reset();
    }

    BorderNode::~BorderNode()
    {
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            assert(getKeyLen(i) != key_len_layer);
            auto value = getLV(i).value;
            if (value != nullptr)
            {
                delete value;
                setLV(i, LinkOrValue{});
            }
        }

        getKeySuffixes().deleteAll();
    }

    std::pair<ExtractResult, LinkOrValue> BorderNode::extractLinkOrValueFor(const Key &key)
    {
        auto triple = extractLinkOrValueWithIndexFor(key);
        return std::pair(std::get<0>(triple), std::get<1>(triple));
    }

    std::tuple<ExtractResult, LinkOrValue, size_t> BorderNode::extractLinkOrValueWithIndexFor(const Key &key)
    {
        auto current = key.getCurrentSlice();
        auto p = getPermutation();

        if (!key.hasNext())
        { // No next key slice
            for (size_t i = 0; i < p.getNumKeys(); ++i)
            {
                auto true_index = p(i);

                if (getKeySlice(true_index) == current.slice && getKeyLen(true_index) == current.size)
                {
                    return std::tuple(VALUE, getLV(true_index), true_index);
                }
            }
        }
        else
        { // Next key slice exists
            for (size_t i = 0; i < p.getNumKeys(); ++i)
            {
                auto true_index = p(i);

                if (getKeySlice(true_index) == current.slice)
                {
                    if (getKeyLen(true_index) == BorderNode::key_len_has_suffix)
                    {
                        // Check suffix
                        auto suffix = getKeySuffixes().get(true_index);
                        if (suffix != nullptr && suffix->isSame(key, key.cursor + 1))
                        {
                            return std::tuple(VALUE, getLV(true_index), true_index);
                        }
                    }

                    if (getKeyLen(true_index) == BorderNode::key_len_layer)
                    {
                        return std::tuple(LAYER, getLV(true_index), true_index);
                    }

                    if (getKeyLen(true_index) == BorderNode::key_len_unstable)
                    {
                        return std::tuple(UNSTABLE, LinkOrValue{}, 0);
                    }
                }
            }
        }

        return std::tuple(NOTFOUND, LinkOrValue{}, 0);
    }

    KeySlice BorderNode::lowestKey() const
    {
        auto p = getPermutation();
        return getKeySlice(p(0));
    }

    void BorderNode::connectPrevAndNext() const
    {
        // Lock prev to handle possible concurrent splits
    retry_prev_lock:
        auto prev_ = getPrev();
        if (prev_ != nullptr)
        {
            prev_->lock();
            if (prev_->getDeleted() || prev_ != getPrev())
            {
                prev_->unlock();
                goto retry_prev_lock;
            }
            else
            {
                auto next_ = getNext();
                prev_->setNext(next_);
                if (next_ != nullptr)
                {
                    assert(!next_->getDeleted());
                    next_->setPrev(prev_);
                }
                prev_->unlock();
            }
        }
        else
        {
            if (getNext() != nullptr)
            {
                getNext()->setPrev(nullptr);
            }
        }
    }

    std::pair<size_t, bool> BorderNode::insertPoint() const
    {
        assert(getPermutation().isNotFull());
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            auto len = getKeyLen(i);
            if (len == 0)
            {
                return std::make_pair(i, false);
            }
            if (10 <= len && len <= 18)
            {
                return std::make_pair(i, true);
            }
        }
        assert(false);
        return std::make_pair(0, false); // Should never reach here
    }

    void BorderNode::printNode() const
    {
        printf("|");
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            printf("%lu|", getKeySlice(i)); // Changed from %llu to %lu
        }
        printf("\n");
    }

    void BorderNode::sort()
    {
        auto p = getPermutation();
        assert(p.isFull());
        assert(isLocked());
        assert(getSplitting());

        uint8_t temp_key_len[NODE_ORDER - 1] = {};
        uint64_t temp_key_slice[NODE_ORDER - 1] = {};
        LinkOrValue temp_lv[NODE_ORDER - 1] = {};
        BigSuffix *temp_suffix[NODE_ORDER - 1] = {};

        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            auto index_ts = p(i);
            temp_key_len[i] = getKeyLen(index_ts);
            temp_key_slice[i] = getKeySlice(index_ts);
            temp_lv[i] = getLV(index_ts);
            temp_suffix[i] = getKeySuffixes().get(index_ts);
        }

        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            setKeyLen(i, temp_key_len[i]);
            setKeySlice(i, temp_key_slice[i]);
            setLV(i, temp_lv[i]);
            getKeySuffixes().set(i, temp_suffix[i]);
        }

        setPermutation(Permutation::fromSorted(NODE_ORDER - 1));
    }

    void BorderNode::markKeyRemoved(uint8_t i)
    {
        auto len = getKeyLen(i);
        assert(1 <= len && len <= key_len_has_suffix);
        setKeyLen(i, len + 9);
    }

    bool BorderNode::isKeyRemoved(uint8_t i) const
    {
        auto len = getKeyLen(i);
        return (10 <= len && len <= 18);
    }

    size_t BorderNode::findNextLayerIndex(Node *next_layer) const
    {
        assert(this->isLocked());

        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            if (getLV(i).next_layer == next_layer)
            {
                return i;
            }
        }

        assert(false);
        return 0; // Should never reach here
    }

    uint8_t BorderNode::getKeyLen(size_t i) const
    {
        return key_len[i].load(READ_MEMORY_ORDER);
    }

    void BorderNode::setKeyLen(size_t i, const uint8_t &len)
    {
        key_len[i].store(len, WRITE_MEMORY_ORDER);
    }

    void BorderNode::resetKeyLen()
    {
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            setKeyLen(i, 0);
        }
    }

    KeySlice BorderNode::getKeySlice(size_t i) const
    {
        return key_slice[i].load(READ_MEMORY_ORDER);
    }

    void BorderNode::setKeySlice(size_t i, const KeySlice &slice)
    {
        key_slice[i].store(slice, WRITE_MEMORY_ORDER);
    }

    void BorderNode::resetKeySlice()
    {
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            setKeySlice(i, 0);
        }
    }

    LinkOrValue BorderNode::getLV(size_t i) const
    {
        // In the header, lv is not atomic, so direct access
        return lv[i];
    }

    void BorderNode::setLV(size_t i, const LinkOrValue &lv_)
    {
        // In the header, lv is not atomic, so direct access
        lv[i] = lv_;
    }

    void BorderNode::resetLVs()
    {
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            setLV(i, LinkOrValue{});
        }
    }

    BorderNode *BorderNode::getNext() const
    {
        return next.load(READ_MEMORY_ORDER);
    }

    void BorderNode::setNext(BorderNode *next_)
    {
        next.store(next_, WRITE_MEMORY_ORDER);
    }

    BorderNode *BorderNode::getPrev() const
    {
        return prev.load(READ_MEMORY_ORDER);
    }

    void BorderNode::setPrev(BorderNode *prev_)
    {
        prev.store(prev_, WRITE_MEMORY_ORDER);
    }

    bool BorderNode::CASNext(BorderNode *expected, BorderNode *desired)
    {
        return next.compare_exchange_weak(expected, desired);
    }

    KeySuffix &BorderNode::getKeySuffixes()
    {
        return key_suffixes;
    }

    const KeySuffix &BorderNode::getKeySuffixes() const
    {
        return key_suffixes;
    }

    Permutation BorderNode::getPermutation() const
    {
        // In the header, permutation is not atomic, so direct access
        return permutation;
    }

    void BorderNode::setPermutation(const Permutation &p)
    {
        // In the header, permutation is not atomic, so direct access
        permutation = p;
    }

    //------------------------------------------------------------------
    // dict implementation
    //------------------------------------------------------------------

    // Initialize the thread-local garbage collector
    thread_local GC dict::gc;

    dict::dict()
        : root(nullptr)
    {
    }

    dict::~dict()
    {
        // Run garbage collection on destruction
        gc.run();
    }

    int dict::insert(void *key, size_t key_len, void *value, size_t value_len)
    {
        // Create key from raw bytes
        Key k = Key::fromBytes(key, key_len);

        // Create value object (makes a copy of the data)
        Value *v = new Value(value, value_len);

    retry:
        auto old_root = root.load(std::memory_order_acquire);
        auto pair = put_at_layer0(old_root, k, v);
        if (pair.first)
        {
            goto retry; // Retry from upper layer
        }

        auto new_root = pair.second;
        k.reset();

        // Tree was empty
        if (old_root == nullptr)
        {
            auto cas_success = root.compare_exchange_weak(old_root, new_root);
            if (!cas_success)
            {
                // Tree was updated by another thread
                assert(new_root != nullptr);
                assert(new_root->getIsBorder());
                new_root->setDeleted(true);
                gc.add(reinterpret_cast<BorderNode *>(new_root));
                goto retry;
            }
        }
        else if (old_root != new_root)
        {
            // Root was changed (e.g., by a split)
            assert(old_root != nullptr);
            root.store(new_root, std::memory_order_release);
        }

        // Run garbage collection
        gc.run();
        return 1; // Success
    }

    void *dict::search(void *key, size_t key_len)
    {
        // Create key from raw bytes
        Key k = Key::fromBytes(key, key_len);

        auto root_ = root.load(std::memory_order_acquire);
        auto v = get(root_, k);
        k.reset();

        if (v == nullptr)
        {
            return nullptr;
        }

        void *result = v->getData();

        return result;
    }

    void *dict::remove(void *key, size_t key_len)
    {
        // Create key from raw bytes
        Key k = Key::fromBytes(key, key_len);

    retry:
        auto old_root = root.load(std::memory_order_acquire);
        if (old_root == nullptr)
        {
            return nullptr; // Nothing to remove
        }

        // First check if the key exists
        Value *v = get(old_root, k);
        if (v == nullptr)
        {
            return nullptr; // Key doesn't exist
        }

        // Make a copy of the value to return
        void *result = jem_malloc(v->getLength());
        if (result)
        {
            memcpy(result, v->getData(), v->getLength());
        }

        // Reset and do the removal
        k.reset();
        auto new_root = remove_at_layer0(old_root, k).second;
        k.reset();

        if (old_root != new_root)
        {
            auto cas_success = root.compare_exchange_weak(old_root, new_root);
            if (!cas_success)
            {
                if (result)
                    jem_free(result);
                goto retry;
            }
        }

        // Run garbage collection
        gc.run();
        return result;
    }

    //------------------------------------------------------------------
    // Helper functions for dict
    //------------------------------------------------------------------

    std::pair<BorderNode *, Version> dict::findBorder(Node *root, const Key &key)
    {
    retry:
        auto n = root;
        auto v = n->stableVersion();

        if (!v.is_root)
        {
            root = root->getParent();
            goto retry;
        }

    descend:
        if (n->getIsBorder())
        {
            return std::pair(reinterpret_cast<BorderNode *>(n), v);
        }

        auto interior_n = reinterpret_cast<InteriorNode *>(n);
        // Find the appropriate child node
        auto n1 = interior_n->findChild(key.getCurrentSlice().slice);
        Version v1 = n1 != nullptr ? n1->stableVersion() : Version();

        if ((n->getVersion() ^ v) <= Version::has_locked)
        {
            assert(n1 != nullptr);
            n = n1;
            v = v1;
            goto descend;
        }

        auto v2 = n->stableVersion();
        if (v2.v_split != v.v_split)
        {
            goto retry;
        }

        v = v2;
        goto descend;
    }

    Value *dict::get(Node *root, Key &k)
    {
        if (root == nullptr)
        {
            // Layer0 is empty
            assert(k.cursor == 0);
            return nullptr;
        }

    retry:
        auto n_v = findBorder(root, k);
        auto n = n_v.first;
        auto v = n_v.second;

    forward:
        if (v.deleted)
        {
            if (v.is_root)
            {
                // Key moved to upper layer or Layer0 is gone
                return nullptr;
            }
            else
            {
                goto retry;
            }
        }

        auto t_lv = n->extractLinkOrValueFor(k);
        auto t = t_lv.first;
        auto lv = t_lv.second;

        if ((n->getVersion() ^ v) > Version::has_locked)
        {
            v = n->stableVersion();
            auto next = n->getNext();
            while (!v.deleted && next != nullptr && k.getCurrentSlice().slice >= next->lowestKey())
            {
                n = next;
                v = n->stableVersion();
                next = n->getNext();
            }
            goto forward;
        }
        else if (t == NOTFOUND)
        {
            return nullptr;
        }
        else if (t == VALUE)
        {
            return lv.value;
        }
        else if (t == LAYER)
        {
            root = lv.next_layer;
            // Advance k to next slice
            k.next();
            goto retry;
        }
        else
        {
            assert(t == UNSTABLE);
            goto forward;
        }
    }

    BorderNode *dict::start_new_tree(const Key &key, Value *value)
    {
        auto root = new BorderNode{};
        root->setIsRoot(true);

        auto cursor = key.getCurrentSlice();
        if (1 <= cursor.size && cursor.size <= 7)
        {
            root->setKeyLen(0, cursor.size);
            root->setKeySlice(0, cursor.slice);
            root->setLV(0, LinkOrValue(value));
        }
        else
        {
            assert(cursor.size == 8);
            if (key.hasNext())
            {
                root->setKeySlice(0, cursor.slice);
                root->setKeyLen(0, BorderNode::key_len_has_suffix);
                root->setLV(0, LinkOrValue(value));
                root->getKeySuffixes().set(0, key, 1);
            }
            else
            {
                root->setKeyLen(0, 8);
                root->setKeySlice(0, cursor.slice);
                root->setLV(0, LinkOrValue(value));
            }
        }

        return root;
    }

    std::optional<size_t> dict::check_break_invariant(BorderNode *const borderNode, const Key &key)
    {
        assert(borderNode->isLocked());
        auto p = borderNode->getPermutation();
        if (key.hasNext())
        {
            auto cursor = key.getCurrentSlice();
            for (size_t i = 0; i < p.getNumKeys(); ++i)
            {
                auto true_index = p(i);
                if ((borderNode->getKeyLen(true_index) == BorderNode::key_len_has_suffix ||
                     borderNode->getKeyLen(true_index) == BorderNode::key_len_layer) &&
                    borderNode->getKeySlice(true_index) == cursor.slice)
                {
                    return true_index;
                }
            }
        }
        return std::nullopt;
    }

    void dict::handle_break_invariant(BorderNode *n, Key &key, size_t old_index)
    {
        assert(n->isLocked());
        if (n->getKeyLen(old_index) == BorderNode::key_len_has_suffix)
        {
            // Create a new empty border node
            auto n1 = new BorderNode{};
            n1->setIsRoot(true);
            n1->setUpperLayer(n);

            auto k2_val = n->getLV(old_index).value;
            auto k2_suffix_copy = new BigSuffix(*n->getKeySuffixes().get(old_index));

            if (k2_suffix_copy->hasNext())
            {
                n1->setKeyLen(0, BorderNode::key_len_has_suffix);
                n1->setKeySlice(0, k2_suffix_copy->getCurrentSlice().slice);
                k2_suffix_copy->next();
                n1->getKeySuffixes().set(0, k2_suffix_copy);
                n1->setLV(0, LinkOrValue(k2_val));
            }
            else
            {
                n1->setKeyLen(0, k2_suffix_copy->getCurrentSlice().size);
                n1->setKeySlice(0, k2_suffix_copy->getCurrentSlice().slice);
                n1->setLV(0, LinkOrValue(k2_val));
                delete k2_suffix_copy;
            }

            // Mark unstable, then update
            n->setKeyLen(old_index, BorderNode::key_len_unstable);
            n->setLV(old_index, LinkOrValue(n1));
            n->setKeyLen(old_index, BorderNode::key_len_layer);

            // Clean up
            gc.add(n->getKeySuffixes().get(old_index));
            n->getKeySuffixes().unref(old_index);
        }
        else
        {
            assert(n->getKeyLen(old_index) == BorderNode::key_len_layer);
            // Nothing to do, insert will continue at next layer
        }
    }

    void dict::insert_into_border(BorderNode *border, const Key &key, Value *value)
    {
        assert(border->isLocked());
        assert(!border->getSplitting());
        assert(!border->getInserting());
        auto p = border->getPermutation();
        assert(p.isNotFull());

        size_t insertion_point_ps = 0;
        size_t num_keys = p.getNumKeys();
        auto cursor = key.getCurrentSlice();
        while (insertion_point_ps < num_keys &&
               border->getKeySlice(p(insertion_point_ps)) < cursor.slice)
        {
            ++insertion_point_ps;
        }

        auto pair = border->insertPoint();
        auto insertion_point_ts = pair.first;
        auto reuse = pair.second;

        if (reuse)
        {
            border->setInserting(true);
            auto suffix = border->getKeySuffixes().get(insertion_point_ts);
            if (suffix != nullptr)
            {
                gc.add(suffix);
            }
            gc.add(border->getLV(insertion_point_ts).value);
        }

        // Clear any existing suffix
        border->getKeySuffixes().set(insertion_point_ts, nullptr);

        if (1 <= cursor.size && cursor.size <= 7)
        {
            border->setKeyLen(insertion_point_ts, cursor.size);
            border->setKeySlice(insertion_point_ts, cursor.slice);
            border->setLV(insertion_point_ts, LinkOrValue(value));
        }
        else
        {
            assert(cursor.size == 8);
            if (key.hasNext())
            {
                border->setKeySlice(insertion_point_ts, cursor.slice);
                border->setKeyLen(insertion_point_ts, BorderNode::key_len_has_suffix);
                border->getKeySuffixes().set(insertion_point_ts, key, key.cursor + 1);
                border->setLV(insertion_point_ts, LinkOrValue(value));
            }
            else
            {
                border->setKeyLen(insertion_point_ts, 8);
                border->setKeySlice(insertion_point_ts, cursor.slice);
                border->setLV(insertion_point_ts, LinkOrValue(value));
            }
        }

        p.insert(insertion_point_ps, insertion_point_ts);
        border->setPermutation(p);
    }

    size_t dict::cut(size_t len)
    {
        if (len % 2 == 0)
            return len / 2;
        else
            return len / 2 + 1;
    }

    void dict::create_slice_table(BorderNode *const n, std::vector<std::pair<KeySlice, size_t>> &table,
                                  std::vector<KeySlice> &found)
    {
        auto p = n->getPermutation();
        assert(p.isFull());
        for (size_t i = 0; i < NODE_ORDER - 1; ++i)
        {
            if (!std::count(found.begin(), found.end(), n->getKeySlice(i)))
            { // NOT FOUND
                table.emplace_back(n->getKeySlice(i), i);
                found.push_back(n->getKeySlice(i));
            }
        }
        // Already sorted
    }

    size_t dict::split_point(KeySlice new_slice, const std::vector<std::pair<KeySlice, size_t>> &table,
                             const std::vector<KeySlice> &found)
    {
        auto min_slice = *std::min_element(found.begin(), found.end());
        auto max_slice = *std::max_element(found.begin(), found.end());

        if (new_slice < min_slice)
        {
            return 1;
        }
        else if (new_slice == min_slice)
        {
            return table[1].second + 1;
        }
        else if (min_slice < new_slice && new_slice < max_slice)
        {
            if (std::count(found.begin(), found.end(), new_slice))
            { // new_slice exists
                for (size_t i = 0; i < table.size(); ++i)
                {
                    if (table[i].first == new_slice)
                    {
                        return table[i].second;
                    }
                }
            }
            else
            {
                for (size_t i = 0; i < table.size(); ++i)
                {
                    if (table[i].first > new_slice)
                    {
                        return table[i].second + 1;
                    }
                }
            }
        }
        else if (new_slice == max_slice)
        {
            for (size_t i = 0; i < table.size(); ++i)
            {
                if (table[i].first == new_slice)
                {
                    return table[i].second;
                }
            }
        } // else

        assert(new_slice > max_slice);
        return 15;
    }

    void dict::split_keys_among(BorderNode *n, BorderNode *n1, const Key &k, Value *value)
    {
        auto p = n->getPermutation();
        assert(p.isFull());
        assert(n->isLocked());
        assert(n->getSplitting());
        assert(n1->isLocked());
        assert(n1->getSplitting());

        // Sort the to-be-split node for simplicity
        n->sort();

        uint8_t temp_key_len[NODE_ORDER] = {};
        uint64_t temp_key_slice[NODE_ORDER] = {};
        LinkOrValue temp_lv[NODE_ORDER] = {};
        BigSuffix *temp_suffix[NODE_ORDER] = {};

        // Find insertion point
        size_t insertion_index = 0;
        while (insertion_index < NODE_ORDER - 1 &&
               n->getKeySlice(insertion_index) < k.getCurrentSlice().slice)
        {
            ++insertion_index;
        }

        // Copy to temp arrays
        for (size_t i = 0, j = 0; i < p.getNumKeys(); ++i, ++j)
        {
            if (j == insertion_index)
                ++j;
            temp_key_len[j] = n->getKeyLen(i);
            temp_key_slice[j] = n->getKeySlice(i);
            temp_lv[j] = n->getLV(i);
            temp_suffix[j] = n->getKeySuffixes().get(i);
        }

        // Insert the new key
        auto cursor = k.getCurrentSlice();
        if (1 <= cursor.size && cursor.size <= 7)
        {
            temp_key_len[insertion_index] = cursor.size;
            temp_key_slice[insertion_index] = cursor.slice;
            temp_lv[insertion_index].value = value;
        }
        else
        {
            assert(cursor.size == 8);
            if (k.hasNext())
            {
                temp_key_slice[insertion_index] = cursor.slice;
                temp_key_len[insertion_index] = BorderNode::key_len_has_suffix;
                temp_suffix[insertion_index] = BigSuffix::from(k, k.cursor + 1);
                temp_lv[insertion_index].value = value;
            }
            else
            {
                temp_key_len[insertion_index] = 8;
                temp_key_slice[insertion_index] = cursor.slice;
                temp_lv[insertion_index].value = value;
            }
        }

        // Determine split point
        std::vector<std::pair<KeySlice, size_t>> table{};
        std::vector<KeySlice> found{};
        create_slice_table(n, table, found);
        size_t split = split_point(cursor.slice, table, found);

        // Clear both nodes
        n->resetKeyLen();
        n->resetKeySlice();
        n->resetLVs();
        n->getKeySuffixes().reset();

        n1->resetKeyLen();
        n1->resetKeySlice();
        n1->resetLVs();
        n1->getKeySuffixes().reset();

        // Distribute keys to first node
        for (size_t i = 0; i < split; ++i)
        {
            n->setKeyLen(i, temp_key_len[i]);
            n->setKeySlice(i, temp_key_slice[i]);
            n->setLV(i, temp_lv[i]);
            n->getKeySuffixes().set(i, temp_suffix[i]);
            if (temp_key_len[i] == BorderNode::key_len_layer)
            {
                n->getLV(i).next_layer->setUpperLayer(n);
            }
        }
        n->setPermutation(Permutation::fromSorted(split));

        // Distribute keys to second node
        for (size_t i = split, j = 0; i < NODE_ORDER; ++i, ++j)
        {
            n1->setKeyLen(j, temp_key_len[i]);
            n1->setKeySlice(j, temp_key_slice[i]);
            n1->setLV(j, temp_lv[i]);
            n1->getKeySuffixes().set(j, temp_suffix[i]);
            if (n1->getKeyLen(j) == BorderNode::key_len_layer)
            {
                n1->getLV(j).next_layer->setUpperLayer(n1);
            }
        }
        n1->setPermutation(Permutation::fromSorted(NODE_ORDER - split));

        // Connect prev/next pointers
        n1->setNext(n->getNext());
        n1->setPrev(n);
        n->setNext(n1);
        if (n1->getNext() != nullptr)
        {
            n1->getNext()->setPrev(n1);
        }
    }

    void dict::split_keys_among(InteriorNode *p, InteriorNode *p1, KeySlice slice, Node *n1,
                                size_t n_index, std::optional<KeySlice> &k_prime)
    {
        assert(!p->isNotFull());
        assert(p->isLocked());
        assert(p->getSplitting());
        assert(p1->isLocked());
        assert(p1->getSplitting());

        // Temp arrays for redistribution
        uint64_t temp_key_slice[NODE_ORDER] = {};
        Node *temp_child[NODE_ORDER + 1] = {};

        // Copy children to temp array
        for (size_t i = 0, j = 0; i < p->getNumKeys() + 1; ++i, ++j)
        {
            if (j == n_index + 1)
                ++j;
            temp_child[j] = p->getChild(i);
        }

        // Copy keys to temp array
        for (size_t i = 0, j = 0; i < p->getNumKeys(); ++i, ++j)
        {
            if (j == n_index)
                ++j;
            temp_key_slice[j] = p->getKeySlice(i);
        }

        // Insert new child and key
        temp_child[n_index + 1] = n1;
        temp_key_slice[n_index] = slice;

        // Clear original node
        p->setNumKeys(0);
        p->resetKeySlices();
        p->resetChildren();

        // Determine split point
        size_t split = cut(NODE_ORDER);

        // Distribute to first node
        size_t i, j;
        for (i = 0; i < split - 1; ++i)
        {
            p->setChild(i, temp_child[i]);
            p->getChild(i)->setParent(p);
            p->setKeySlice(i, temp_key_slice[i]);
            p->incNumKeys();
        }
        p->setChild(i, temp_child[i]);
        temp_child[i]->setParent(p);

        // Key to pull up to parent
        k_prime = temp_key_slice[split - 1];

        // Distribute to second node
        for (++i, j = 0; i < NODE_ORDER; ++i, ++j)
        {
            p1->setChild(j, temp_child[i]);
            p1->getChild(j)->setParent(p1);
            p1->setKeySlice(j, temp_key_slice[i]);
            p1->incNumKeys();
        }
        p1->setChild(j, temp_child[i]);
        temp_child[i]->setParent(p1);

        // Verify children
        for (auto &n : temp_child)
        {
            if (n != nullptr)
            {
                assert(n->getParent()->debug_contain_child(n));
            }
        }
    }

    InteriorNode *dict::create_root_with_children(Node *left, KeySlice slice, Node *right)
    {
        assert(left->getIsRoot());
        assert(left->getParent() == nullptr);
        assert(right->getParent() == nullptr);
        assert(left->isLocked());
        assert(right->isLocked());

        auto root = new InteriorNode{};
        root->lock(); // For setParent convenience

        auto upper = left->lockedUpperNode();
        root->setIsRoot(true);
        root->setUpperLayer(upper);

        if (upper != nullptr)
        {
            auto left_index = upper->findNextLayerIndex(left);
            upper->setLV(left_index, LinkOrValue(root));
        }

        root->setNumKeys(1);
        root->setKeySlice(0, slice);
        root->setChild(0, left);
        root->setChild(1, right);

        // Set parent then unset is_root
        left->setParent(root);
        left->setUpperLayer(nullptr);
        right->setParent(root);
        left->setIsRoot(false);
        right->setIsRoot(false);

        if (upper != nullptr)
        {
            upper->unlock();
        }

        root->unlock();
        assert(left->getUpperLayer() == nullptr);
        return root;
    }

    void dict::insert_into_parent(InteriorNode *p, Node *n1, KeySlice slice, size_t n_index)
    {
        assert(p->isNotFull());
        assert(p->isLocked());
        assert(p->getInserting());
        assert(n1->isLocked());

        // Shift right to make space
        for (size_t i = p->getNumKeys(); i > n_index; --i)
        {
            p->setChild(i + 1, p->getChild(i));
            p->setKeySlice(i, p->getKeySlice(i - 1));
        }

        // Insert new child and key
        p->setChild(n_index + 1, n1);
        p->setKeySlice(n_index, slice);
        p->incNumKeys();

        // Set parent
        n1->setParent(p);
        assert(!p->debug_has_skip());
    }

    Node *dict::split(Node *n, const Key &k, Value *value)
    {
        assert(n->isLocked());
        Node *n1 = new BorderNode{};
        n->setSplitting(true);

        // Initialize n1
        n1->setVersion(n->getVersion());

        // Split the keys
        split_keys_among(
            reinterpret_cast<BorderNode *>(n),
            reinterpret_cast<BorderNode *>(n1), k, value);

        std::optional<KeySlice> pull_up = std::nullopt;

    ascend:
        assert(n->isLocked());
        assert(n1->isLocked());
        InteriorNode *p = n->lockedParent();
        if (p != nullptr)
            assert(p->debug_contain_child(n));

        if (p == nullptr)
        {
            // Create new root
            auto up = pull_up ? pull_up.value() : reinterpret_cast<BorderNode *>(n1)->getKeySlice(0);
            p = create_root_with_children(n, up, n1);
            n->unlock();
            n1->unlock();
            return p;
        }
        else if (p->isNotFull())
        {
            // Insert into parent
            p->setInserting(true);
            size_t n_index = p->findChildIndex(n);
            auto up = pull_up ? pull_up.value() : reinterpret_cast<BorderNode *>(n1)->getKeySlice(0);
            insert_into_parent(p, n1, up, n_index);
            n->unlock();
            n1->unlock();
            p->unlock();
            return nullptr;
        }
        else
        { // Parent is full, need to split it too
            assert(p->isFull());
            p->setSplitting(true);
            size_t n_index = p->findChildIndex(n);
            n->unlock();

            // Create new interior node
            Node *p1 = new InteriorNode{};
            p1->setVersion(p->getVersion());

            // Split interior node
            auto up = pull_up ? pull_up.value() : reinterpret_cast<BorderNode *>(n1)->getKeySlice(0);
            split_keys_among(
                reinterpret_cast<InteriorNode *>(p),
                reinterpret_cast<InteriorNode *>(p1), up, n1, n_index, pull_up);

            n1->unlock();
            assert(n->getParent()->debug_contain_child(n));
            assert(n1->getParent()->debug_contain_child(n1));

            // Continue propagating split up the tree
            n = p;
            n1 = p1;
            goto ascend;
        }
    }

    std::pair<bool, Node *> dict::put(Node *root, Key &k, Value *value)
    {
        if (root == nullptr)
        {
            // Layer0 is empty
            assert(k.cursor == 0);
            return std::make_pair(false, start_new_tree(k, value));
        }

    retry:
        auto n_v = findBorder(root, k);
        auto n = n_v.first;
        auto v = n_v.second;
        n->lock();
        /*
         * v deliberately stays findBorder's version: splitHappened below
         * must compare the version the routing was based on against the
         * current one. Refreshing v after lock() blinds that check to a
         * split that occurred between findBorder and the lock -- the
         * insert then lands in a node whose key range moved to a split
         * sibling, and the key becomes permanently unreachable (it was
         * lost roughly once per million concurrent inserts). Deleted and
         * is_root are checked from the CURRENT version instead: both can
         * change in the same window.
         */

    forward:
        assert(n->isLocked());
        auto p = n->getPermutation();
        Version v_now = n->getVersion();
        if (v_now.deleted)
        {
            n->unlock();
            if (v_now.is_root)
            {
                // Key should go to upper layer or Layer0 is gone
                return std::make_pair(true, nullptr);
            }
            else
            {
                goto retry;
            }
        }

        auto t_lv_i = n->extractLinkOrValueWithIndexFor(k);
        auto t = std::get<0>(t_lv_i);
        auto lv = std::get<1>(t_lv_i);
        auto index = std::get<2>(t_lv_i);

        if (Version::splitHappened(v, v_now))
        {
            // Split happened between findBorder and lock: hand over to the
            // right until the node covering k is found.
            n->unlock();
            /*
             * Refresh v for the node we stand on, THEN re-read next. If the
             * key stays in the original (left) node the walk does not
             * advance, and without the refresh v would stay pre-split --
             * splitHappened would fire forever (livelock). Reading next
             * after v means a second split in this window leaves v stale
             * relative to it, so the recheck at forward catches it and we
             * walk again.
             */
            v = n->stableVersion();
            auto next = n->getNext();
            assert(next != nullptr); // Must have a next after split
            while (!v.deleted && next != nullptr && k.getCurrentSlice().slice >= next->lowestKey())
            {
                n = next;
                v = n->stableVersion();
                next = n->getNext();
            }
            n->lock();
            goto forward;
        }
        else if (t == NOTFOUND)
        {
            // Check if inserting would break invariant
            auto check = check_break_invariant(n, k);
            if (check)
            {
                auto old_index = check.value();
                handle_break_invariant(n, k, old_index);
                auto next_layer = n->getLV(old_index).next_layer;
                n->unlock();
                k.next();
                auto pair = put(next_layer, k, value);
                if (pair.first)
                {
                    k.back();
                    goto retry;
                }
                return std::make_pair(false, root);
            }
            else
            {
                if (p.isNotFull())
                {
                    // Insert directly
                    insert_into_border(n, k, value);
                    n->unlock();
                    return std::make_pair(false, root);
                }
                else
                {
                    // Need to split
                    auto may_new_root = split(n, k, value);
                    if (may_new_root != nullptr)
                    {
                        return std::make_pair(false, may_new_root);
                    }
                    return std::make_pair(false, root);
                }
            }
        }
        else if (t == VALUE)
        {
            // Update existing value
            gc.add(n->getLV(index).value);
            n->setLV(index, LinkOrValue(value));
            n->unlock();
            return std::make_pair(false, root);
        }
        else if (t == LAYER)
        {
            // Continue in next layer
            n->unlock();
            k.next();
            auto pair = put(lv.next_layer, k, value);
            if (pair.first)
            {
                k.back();
                goto retry;
            }
            return std::make_pair(false, root);
        }
        else
        {
            // t == UNSTABLE
            assert(false);
            return std::make_pair(false, root);
        }
    }

    std::pair<bool, Node *> dict::put_at_layer0(Node *root, Key &k, Value *value)
    {
        return put(root, k, value);
    }

    void dict::handle_delete_layer_in_remove(BorderNode *n)
    {
        auto p = n->getPermutation();
        assert(n->getParent() == nullptr);
        assert(p.getNumKeys() == 1);
        assert(n->getIsRoot());
        assert(n->getUpperLayer() != nullptr);
        assert(n->isLocked());

        BigSuffix *upper_suffix;
        if (n->getKeyLen(p(0)) == BorderNode::key_len_has_suffix)
        {
            auto old_suffix = n->getKeySuffixes().get(p(0));
            old_suffix->insertTop(n->getKeySlice(p(0)));
            // Reuse suffix
            upper_suffix = old_suffix;
        }
        else
        {
            // Key is terminal, can't be layer
            assert(1 <= n->getKeyLen(p(0)) && n->getKeyLen(p(0)) <= 8);
            upper_suffix = new BigSuffix({n->getKeySlice(p(0))}, n->getKeyLen(p(0)));
        }

        // Lock upper layer
        auto upper = n->lockedUpperNode();

        // Update upper layer
        auto n_index = upper->findNextLayerIndex(n);
        upper->setKeyLen(n_index, BorderNode::key_len_unstable);
        assert(upper->getKeySuffixes().get(n_index) == nullptr);
        upper->getKeySuffixes().set(n_index, upper_suffix);
        upper->setLV(n_index, n->getLV(p(0)));
        upper->setKeyLen(n_index, BorderNode::key_len_has_suffix);

        // Clear original value
        n->setLV(p(0), LinkOrValue{});
        n->getKeySuffixes().set(p(0), nullptr);

        n->setDeleted(true);
        gc.add(n);

        // Unlock nodes
        n->unlock();
        upper->unlock();
    }

    std::pair<bool, Node *> dict::delete_border_node_in_remove(BorderNode *n)
    {
        assert(n->isLocked());
        auto per = n->getPermutation();
        // All elements already removed
        assert(per.getNumKeys() == 0);

        if (n->getIsRoot())
        {
            // Only at Layer 0
            assert(n->getParent() == nullptr);
            assert(n->getUpperLayer() == nullptr);
            n->setDeleted(true);
            gc.add(n);
            n->unlock();
            return std::make_pair(true, nullptr);
        }

        // Lock parent
        auto p = n->getParent();
        p->lock();
        auto n_index = p->findChildIndex(n);

        if (p->getNumKeys() >= 2)
        {
            // Shift keys and children left
            p->setInserting(true);
            if (n_index == 0)
            {
                for (size_t i = 0; i <= 13; ++i)
                {
                    p->setKeySlice(i, p->getKeySlice(i + 1));
                }
                for (size_t i = 0; i <= 14; ++i)
                {
                    p->setChild(i, p->getChild(i + 1));
                }
            }
            else
            {
                for (size_t i = n_index - 1; i <= 13; ++i)
                {
                    p->setKeySlice(i, p->getKeySlice(i + 1));
                }
                for (size_t i = n_index; i <= 14; ++i)
                {
                    p->setChild(i, p->getChild(i + 1));
                }
            }
            p->decNumKeys();

            p->unlock();
            n->connectPrevAndNext();
            n->setDeleted(true);
            gc.add(n);
            n->unlock();
            return std::make_pair(false, nullptr);
        }
        else
        {
            assert(p->getNumKeys() == 1);
            auto pull_up_index = n_index == 1 ? 0 : 1;
            auto pull_up_node = p->getChild(pull_up_index);

            if (p->getIsRoot())
            {
                assert(p->getParent() == nullptr);
                // Root changes, need to update upper layer
                if (p->getUpperLayer() == nullptr)
                {
                    // Layer0
                    pull_up_node->setIsRoot(true);
                    pull_up_node->setParent(nullptr);
                    pull_up_node->setUpperLayer(nullptr);

                    p->setDeleted(true);
                    gc.add(p);
                    p->unlock();

                    n->connectPrevAndNext();
                    n->setDeleted(true);
                    gc.add(n);
                    n->unlock();

                    return std::make_pair(false, pull_up_node);
                }
                else
                {
                    // Upper layer update
                    auto upper = p->lockedUpperNode();
                    auto p_index = upper->findNextLayerIndex(p);

                    pull_up_node->setIsRoot(true);
                    pull_up_node->setParent(nullptr);
                    upper->setLV(p_index, LinkOrValue(pull_up_node));

                    upper->unlock();
                    p->setDeleted(true);
                    gc.add(p);
                    p->unlock();

                    n->connectPrevAndNext();
                    n->setDeleted(true);
                    gc.add(n);
                    n->unlock();

                    return std::make_pair(false, pull_up_node);
                }
            }
            else
            {
                // Update grandparent
                auto pp = p->getParent();
                pp->lock();
                auto p_index = pp->findChildIndex(p);
                pp->setChild(p_index, pull_up_node);
                pull_up_node->setParent(pp);

                pp->unlock();
                p->setDeleted(true);
                gc.add(p);
                p->unlock();

                n->connectPrevAndNext();
                n->setDeleted(true);
                gc.add(n);
                n->unlock();

                return std::make_pair(false, nullptr);
            }
        }
    }

    std::pair<bool, Node *> dict::remove(Node *root, Key &k)
    {
        if (root == nullptr)
        {
            // Only in Layer0
            assert(k.cursor == 0);
            return std::make_pair(false, nullptr);
        }

    retry:
        auto n_v = findBorder(root, k);
        auto n = n_v.first;
        auto v = n_v.second;
        n->lock();
        // v stays findBorder's version so splitHappened can detect a split
        // in the findBorder->lock window (see the same pattern in put()).

    forward:
        assert(n->isLocked());
        Version v_now = n->getVersion();
        if (v_now.deleted)
        {
            n->unlock();
            if (v_now.is_root)
            {
                // Already handled by another remove
                return std::make_pair(false, root);
            }
            else
            {
                goto retry;
            }
        }

        auto t_lv_i = n->extractLinkOrValueWithIndexFor(k);
        auto t = std::get<0>(t_lv_i);
        auto lv = std::get<1>(t_lv_i);
        auto index = std::get<2>(t_lv_i);

        if (Version::splitHappened(v, v_now))
        {
            // Split happened between findBorder and lock: hand over to the
            // right until the node covering k is found. v is refreshed
            // before next is read -- see the same walk in put().
            n->unlock();
            v = n->stableVersion();
            auto next = n->getNext();
            assert(next != nullptr); // Must have a next after split
            while (!v.deleted && next != nullptr && k.getCurrentSlice().slice >= next->lowestKey())
            {
                n = next;
                v = n->stableVersion();
                next = n->getNext();
            }
            n->lock();
            goto forward;
        }
        else if (t == NOTFOUND)
        {
            // Key not found, nothing to remove
            n->unlock();
            return std::make_pair(false, root);
        }
        else if (t == VALUE)
        {
            auto p = n->getPermutation();
            if (n->getIsRoot() && p.getNumKeys() == 1 && k.cursor != 0)
            {
                // Delete layer if not Layer0
                handle_delete_layer_in_remove(n);
                return std::make_pair(true, nullptr);
            }

            // Mark key as removed and update permutation
            n->markKeyRemoved(index);
            p.removeIndex(index);
            n->setPermutation(p);

            // Check if node is now empty
            auto current_num_keys = p.getNumKeys();
            if (current_num_keys == 0)
            {
                auto pair = delete_border_node_in_remove(n);
                if (pair.first)
                {
                    return pair;
                }
            }
            else
            {
                n->unlock();
            }

            return std::make_pair(false, root);
        }
        else if (t == LAYER)
        {
            // Continue in next layer
            n->unlock();
            k.next();
            auto pair = remove(lv.next_layer, k);
            if (pair.first)
            {
                k.back();
                goto retry;
            }
            return std::make_pair(false, root);
        }
        else
        {
            // t == UNSTABLE
            assert(false);
            return std::make_pair(false, root);
        }
    }

    std::pair<bool, Node *> dict::remove_at_layer0(Node *root, Key &k)
    {
        return remove(root, k);
    }

    void dict::print_sub_tree(Node *root)
    {
        if (root->getIsBorder())
        {
            auto border = reinterpret_cast<BorderNode *>(root);
            border->printNode();
        }
        else
        {
            auto interior = reinterpret_cast<InteriorNode *>(root);
            interior->printNode();
        }
    }


    //------------------------------------------------------------------
    // Range scan
    //------------------------------------------------------------------

    // Unpack the low `len` bytes of a little-endian-packed slice
    static void appendSliceBytes(KeySlice slice, size_t len, std::string &out)
    {
        for (size_t j = 0; j < len && j < 8; j++)
        {
            out.push_back((char)((slice >> (j * 8)) & 0xFF));
        }
    }

    // Compare two keys in TREE order: slice values, then slice count, then
    // final slice size (Masstree's ordering; not byte-lexicographic)
    static int cmpSliceKeys(const std::vector<KeySlice> &a, size_t a_last,
                            const std::vector<KeySlice> &b, size_t b_last)
    {
        size_t common = std::min(a.size(), b.size());
        for (size_t i = 0; i < common; i++)
        {
            if (a[i] != b[i])
            {
                return a[i] < b[i] ? -1 : 1;
            }
        }
        if (a.size() != b.size())
        {
            return a.size() < b.size() ? -1 : 1;
        }
        if (a_last != b_last)
        {
            return a_last < b_last ? -1 : 1;
        }
        return 0;
    }

    /*
     * Walk one layer's border chain collecting keys >= bound. Each border
     * node is snapshotted under version validation (stableVersion before,
     * raw version after; any change discards the snapshot and retries), so
     * concurrent inserts/splits never produce torn entries. Value bytes are
     * copied inside the snapshot loop -- i.e. inside this public operation's
     * TAG scope, with the guide dereference pinning the object. Layer
     * entries recurse with the accumulated byte prefix; the bound narrows
     * to the matching subtree and disappears once the scan passes it.
     */
    void dict::scanLayer(Node *layer_root, const std::string &prefix_bytes,
                         const Key *bound, size_t n,
                         std::vector<std::pair<std::string, std::string>> &out)
    {
        if (layer_root == nullptr || out.size() >= n)
        {
            return;
        }

        BorderNode *bn = nullptr;
        if (bound != nullptr)
        {
            bn = findBorder(layer_root, *bound).first;
        }
        else
        {
            Node *cur = layer_root;
            while (cur != nullptr && !cur->getIsBorder())
            {
                cur = static_cast<InteriorNode *>(cur)->getChild(0);
            }
            bn = static_cast<BorderNode *>(cur);
        }

        // The bound's remaining slices within this layer
        std::vector<KeySlice> bound_slices;
        size_t bound_last = 0;
        if (bound != nullptr)
        {
            for (size_t i = bound->cursor; i < bound->slices.size(); i++)
            {
                bound_slices.push_back(bound->slices[i]);
            }
            bound_last = bound->lastSliceSize;
        }

        while (bn != nullptr && out.size() < n)
        {
            struct Ent
            {
                KeySlice slice;
                uint8_t klen; // 1..8 inline, 9 suffix, 255 layer
                std::vector<KeySlice> suffix;
                size_t suffix_last;
                std::string value;
                Node *layer;
            };
            std::vector<Ent> ents;
            BorderNode *next_bn = nullptr;

            int attempts = 0;
            while (true)
            {
                ents.clear();
                Version v = bn->stableVersion();
                if (v.deleted)
                {
                    return; // node is being unlinked; end this layer's walk
                }
                Permutation perm = bn->getPermutation();
                bool torn = false;
                for (size_t i = 0; i < perm.getNumKeys(); i++)
                {
                    uint8_t t = perm(i);
                    uint8_t klen = bn->getKeyLen(t);
                    if (klen == 0 || klen == BorderNode::key_len_unstable ||
                        (10 <= klen && klen <= 18))
                    {
                        continue; // empty, mid-insert, or removed
                    }
                    Ent e;
                    e.slice = bn->getKeySlice(t);
                    e.klen = klen;
                    e.layer = nullptr;
                    e.suffix_last = 0;
                    if (klen == BorderNode::key_len_layer)
                    {
                        e.layer = bn->getLV(t).next_layer;
                        if (e.layer == nullptr)
                        {
                            torn = true;
                            break;
                        }
                    }
                    else
                    {
                        Value *val = bn->getLV(t).value;
                        if (val == nullptr)
                        {
                            torn = true;
                            break;
                        }
                        char *data = static_cast<char *>(val->getData());
                        size_t len = val->getLength();
                        e.value.assign(data, len > 0 ? len - 1 : 0);
                        if (klen == BorderNode::key_len_has_suffix)
                        {
                            BigSuffix *suf = bn->getKeySuffixes().get(t);
                            if (suf == nullptr)
                            {
                                torn = true;
                                break;
                            }
                            suf->copySlices(e.suffix, e.suffix_last);
                        }
                    }
                    ents.push_back(std::move(e));
                }
                next_bn = bn->getNext();
                Version v2 = bn->getVersion();
                if (!torn && (v ^ v2) == 0)
                {
                    break;
                }
                if (++attempts > 64)
                {
                    return; // writer-saturated node; return what we have
                }
            }

            for (auto &e : ents)
            {
                if (out.size() >= n)
                {
                    return;
                }
                if (e.klen == BorderNode::key_len_layer)
                {
                    Key sub;
                    const Key *sub_bound = nullptr;
                    if (bound != nullptr && !bound_slices.empty())
                    {
                        if (e.slice < bound_slices[0])
                        {
                            continue; // whole layer < bound
                        }
                        if (e.slice == bound_slices[0] && bound_slices.size() > 1)
                        {
                            sub = *bound;
                            sub.next();
                            sub_bound = &sub;
                        }
                    }
                    std::string sub_prefix = prefix_bytes;
                    appendSliceBytes(e.slice, 8, sub_prefix);
                    scanLayer(e.layer, sub_prefix, sub_bound, n, out);
                    continue;
                }

                // Value entry: reconstruct the key, apply the bound, emit
                std::vector<KeySlice> ekey{e.slice};
                size_t elast = e.klen;
                if (e.klen == BorderNode::key_len_has_suffix)
                {
                    for (KeySlice s : e.suffix)
                    {
                        ekey.push_back(s);
                    }
                    elast = e.suffix_last;
                }
                if (bound != nullptr && !bound_slices.empty() &&
                    cmpSliceKeys(ekey, elast, bound_slices, bound_last) < 0)
                {
                    continue;
                }
                std::string kb = prefix_bytes;
                for (size_t s = 0; s < ekey.size(); s++)
                {
                    appendSliceBytes(ekey[s], (s == ekey.size() - 1) ? elast : 8, kb);
                }
                // stored keys carry the trailing NUL; strip it for the wire
                if (!kb.empty() && kb.back() == '\0')
                {
                    kb.pop_back();
                }
                out.emplace_back(std::move(kb), std::move(e.value));
            }

            bn = next_bn;
        }
    }

    int dict::scanCopy(void *start_key, size_t key_len, int n,
                       std::vector<std::pair<std::string, std::string>> &out)
    {
        if (n <= 0)
        {
            return 0;
        }
        Node *r = root.load(std::memory_order_acquire);
        if (r == nullptr)
        {
            return 0;
        }
        Key start = Key::fromBytes(start_key, key_len);
        scanLayer(r, "", &start, (size_t)n, out);
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

} // namespace bpt_mass
