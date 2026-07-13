// A sparse bitmap saves space over a normal bitmap by partitioning the bitmap
// into blocks and not allocating space for blocks in which no bits are set.

#ifndef OBASE_SODA_H_
#define OBASE_SODA_H_

#include <unistd.h>
#include <cstdlib>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <limits.h>
#include <stdint.h>

class SodaBitmap
{
public:

    static constexpr uint64_t kCharBits = 8;
    static constexpr uint64_t kIntBits = 64;
    static constexpr uint64_t kLogIntBits = 6;
    static constexpr uint64_t kMaskIntBits = 0x3F;
    static constexpr uint64_t kMinBlockSize = kIntBits * 2048; // 128
    static constexpr uint64_t kDefaultBlockSize = 1 << 20;     // 2^20 = 1,048,576 bits = 128 KiB

    // allocates an in-memory bitmap - in this case, the bitmap is read/write.
    // We silently round up numbits to be a multiple of the internal blocksize.
    // We expect sparse bitmaps to be dense in parts with large holes in between.
    explicit SodaBitmap(uint64_t numbits);

    ~SodaBitmap();

    // disallow
    SodaBitmap(const SodaBitmap &) = delete;
    SodaBitmap operator=(const SodaBitmap &) = delete;

    // NOTE: mutators (Guide construction/assignment/destruction) call
    // Set1/Set0 concurrently from many threads while the OC reads via
    // Get/FindNextSetBit. All word and block-pointer accesses must therefore
    // be atomic: a plain |=/&= loses concurrent updates (a lost Set0 makes
    // the OC scan a freed guide slot; a lost Set1 hides an object forever).

    __attribute__((always_inline)) bool Get(uint64_t index) const
    {
        if (index >= numbits_)
        {
            return false;
        }

        // we have made sure in the constructors that blocksize_ is a power of 2
        // lg_blocksize_ and mask_blocksize_ are computed once in the constructor.
        uint64_t offset = index >> lg_blocksize_;
        uint64_t *lmap = __atomic_load_n(&map_[offset], __ATOMIC_ACQUIRE);

        if (lmap == nullptr)
        {
            return false;
        }
        else
        {
            uint64_t lindex = index & mask_blocksize_;             // bit index within block
            uint64_t wordoffset = lindex >> kLogIntBits;           // which word
            uint64_t wordmask = (1ULL << (lindex & kMaskIntBits)); // which bit
            return (__atomic_load_n(&lmap[wordoffset], __ATOMIC_RELAXED) & wordmask);
        }
    }

    __attribute__((always_inline)) void Set1(uint64_t index)
    {
        if (index >= numbits_)
        {
            // Guide slot outside the covered address span (or the bitmap is
            // not constructed yet -- e.g., a guide created during static
            // initialization). Better to lose tracking for it than to
            // corrupt the top-level block array, but never silently: an
            // unregistered-but-dereferenced guide leaks ATC increments
            // (decrements are skipped when the bit is clear).
            static bool warned = false;
            if (!__atomic_test_and_set(&warned, __ATOMIC_RELAXED))
            {
                fprintf(stderr,
                        "SODA: guide slot index %llu out of range (numbits=%llu) -- "
                        "guide will be untracked; was it created before static init "
                        "completed?\n",
                        (unsigned long long)index, (unsigned long long)numbits_);
            }
            return;
        }
        uint64_t offset = index >> lg_blocksize_;
        uint64_t *lmap = __atomic_load_n(&map_[offset], __ATOMIC_ACQUIRE);
        if (lmap == nullptr)
        {
            // Allocate and install the block with a CAS so concurrent
            // first-writers do not clobber each other's blocks.
            uint64_t *fresh = (uint64_t *)calloc(numints_, sizeof(uint64_t));
            uint64_t *expected = nullptr;
            if (__atomic_compare_exchange_n(&map_[offset], &expected, fresh, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            {
                lmap = fresh;
                __atomic_fetch_add(&mapsize_, numints_ * sizeof(*lmap), __ATOMIC_RELAXED);
            }
            else
            {
                free(fresh);
                lmap = expected; // another thread won the install
            }
        }

        uint64_t lindex = index & mask_blocksize_;
        uint64_t wordoffset = lindex >> kLogIntBits;
        uint64_t wordmask = (1ULL << (lindex & kMaskIntBits));

        uint64_t prev = __atomic_fetch_or(&lmap[wordoffset], wordmask, __ATOMIC_RELEASE);
        if (!(prev & wordmask))
        {
            __atomic_fetch_add(&numsetbits_, 1, __ATOMIC_RELAXED);
        }
    }

    __attribute__((always_inline)) void Set0(uint64_t index)
    {
        if (index >= numbits_)
        {
            return;
        }
        uint64_t offset = index >> lg_blocksize_;
        uint64_t *lmap = __atomic_load_n(&map_[offset], __ATOMIC_ACQUIRE);
        if (lmap == nullptr)
        { // If the block is not allocated, return as all bits are assumed to be 0
            return;
        }

        uint64_t lindex = index & mask_blocksize_;
        uint64_t wordoffset = lindex >> kLogIntBits;
        uint64_t wordmask = (1ULL << (lindex & kMaskIntBits));

        uint64_t prev = __atomic_fetch_and(&lmap[wordoffset], ~wordmask, __ATOMIC_RELEASE);
        if (prev & wordmask)
        {
            __atomic_fetch_sub(&numsetbits_, 1, __ATOMIC_RELAXED);
        }
    }

    void Set(uint64_t index, bool value)
    {
        if (value)
        {
            Set1(index);
        }
        else
        {
            Set0(index);
        }
    }

    // NB: To use these methods in a loop you must increment the index after each
    // use, as in:
    //
    //  for ( uint32 index = 0 ; map->FindNextSetBit(&index) ; ++index ) {
    //    DoSomethingWith(index);
    //  }
    //
    bool FindNextSetBitBeforeLimit(uint64_t *index, uint64_t limit) const
    {
        return FindNextBitBeforeLimit(index, limit, true);
    }

    bool FindNextSetBit(uint64_t *index) const
    {
        return FindNextSetBitBeforeLimit(index, numbits_);
    }

    bool FindNextUnsetBitBeforeLimit(uint64_t *index, uint64_t limit) const
    {
        return FindNextBitBeforeLimit(index, limit, false);
    }

    bool FindNextUnsetBit(uint64_t *index) const
    {
        return FindNextUnsetBitBeforeLimit(index, numbits_);
    }

    uint64_t numbits() const { return numbits_; }       // number of bits in the bitmap
    uint64_t numsetbits() const { return numsetbits_; } // numbits that are set
    uint64_t mapsize() const { return mapsize_; }       // num bits used for the bitmap

private:
    // FindNextBitBeforeLimit: Finds the first offset >= "*index" and
    // < "limit" that has its bit set to 'value'.  If found, sets "*index" to this
    // offset and returns true.  Otherwise, does not modify "*index" and returns
    // false.  REQUIRES: "limit" <= bits().

    /* TODO:
     Potential upgrade: if you find all the words with all bits set to 0 in a block,
     then free the block and update the map. This way you can reduce the memory used.
     */

    bool FindNextBitBeforeLimit(uint64_t *index, uint64_t limit, bool value) const
    {
        assert(limit <= numbits_); // Assuming numbits_ is defined as the total number of bits handled by the bitmap.
        if ((*index) >= limit)
            return false;
        uint64_t last_block = (limit - 1) >> lg_blocksize_;

        // Start at the given index
        uint64_t next_index = (*index);
        uint64_t block = next_index >> lg_blocksize_;

        // Search through each block
        do
        {
            uint64_t *lmap = __atomic_load_n(&map_[block], __ATOMIC_ACQUIRE);

            if (lmap == nullptr)
            {
                if (!value)
                {
                    *index = next_index;
                    return true;
                }
                else
                {
                    // Skip to the next block
                    block++;
                    next_index = block * blocksize_;
                    continue;
                }
            }

            uint64_t block_end = (block + 1) * blocksize_;
            // Do a linear search until we find a set bit or we finish the block
            do
            {
                uint64_t lindex = next_index & mask_blocksize_; // bit index within the block
                uint64_t wordoffset = lindex >> kLogIntBits;    // which word

                uint64_t word = __atomic_load_n(&lmap[wordoffset], __ATOMIC_RELAXED);

                // Skip entire word if it's all zeros or all ones, depending on the search for set/unset bits
                if (word == (value ? 0ULL : 0xffffffffffffffffULL))
                {
                    next_index += kIntBits - (next_index & kMaskIntBits); // Move to next word boundary
                    continue;
                }

                // Perform a right shift by the number of bits already scanned within the word
                uint64_t shifted_word = word >> (lindex & kMaskIntBits);
                uint64_t result_index;

                if (value)
                { // Searching for the first set bit
                    if (shifted_word == 0)
                    {
                        // If the shifted word is zero, skip to the next word
                        next_index += kIntBits - (next_index & kMaskIntBits);
                        continue;
                    }
                    result_index = __builtin_ctzll(shifted_word);
                }
                else
                { // Searching for the first unset bit by inverting bits
                    shifted_word = ~shifted_word;
                    if (shifted_word == 0)
                    {
                        // If the inverted shifted word is zero, skip to the next word
                        next_index += kIntBits - (next_index & kMaskIntBits);
                        continue;
                    }
                    result_index = __builtin_ctzll(shifted_word);
                }

                // Adjust the found position by adding the initial bit offset within the word
                *index = (block * blocksize_) + (wordoffset * kIntBits) + (lindex & kMaskIntBits) + result_index;
                return true;

            } while (next_index < block_end && next_index < limit);

            // Move on to the next block
            block++;
            next_index = block * blocksize_;
        } while (block <= last_block);

        // Not found
        return false;
    }

    bool write_;              // will we do writes?
    uint64_t numbits_;        // number of bits in the bitmap
    uint64_t numints_;        // number of ints allocated for a block
    uint64_t numblocks_;      // number of blocks
    uint64_t **map_;          // two-level bitmap
    uint64_t blocksize_;      // # bits in a block
    uint64_t lg_blocksize_;   // lg_2 of blocksize
    uint64_t mask_blocksize_; // (blocksize_ - 1) - for rapid mod
    uint64_t mapsize_;        // total size of the bit blocks
    uint64_t blockbytes_;     // number of bytes in a block
    uint64_t numsetbits_;     // how many bits are currently set
    time_t lastmod_;          // when the bitmap file was last modified
};

#endif // OBASE_SODA_H_