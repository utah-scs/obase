#include <stddef.h>

#include <limits>
#include <string>

#include "soda.h"

SodaBitmap::SodaBitmap(uint64_t numbits)
{
    blocksize_ = kDefaultBlockSize;
    mask_blocksize_ = (blocksize_ - 1);

    // Round up numbits to be multiples of blocksize (which is a power of 2)
    uint64_t rounded_numbits = (numbits - 1) | mask_blocksize_;
    if (rounded_numbits == std::numeric_limits<uint64_t>::max())
    {
        numbits_ = std::numeric_limits<uint64_t>::max();
        numblocks_ = numbits_ / blocksize_ + 1;
    }
    else
    {
        numbits_ = rounded_numbits + 1;
        numblocks_ = numbits_ / blocksize_;
    }
    write_ = true;
    numints_ = blocksize_ / kIntBits;
    blockbytes_ = blocksize_ / kCharBits;
    map_ = new uint64_t *[numblocks_];
    mapsize_ = 0; // to keep track of the space allocated
    lastmod_ = 0;
    numsetbits_ = 0;

    lg_blocksize_ = 0;

    // not the most efficient way of computing log but done only once per bitmap
    int tmp = blocksize_;
    while (tmp > 1)
    {
        tmp = tmp >> 1;
        lg_blocksize_++;
    }

    for (int i = 0; i < numblocks_; i++)
    {
        map_[i] = nullptr;
    }
}

SodaBitmap::~SodaBitmap()
{
    if (map_ != nullptr)
    {
        // Blocks are allocated with calloc (see Set1)
        for (uint64_t i = 0; i < numblocks_; i++)
            free(map_[i]);
    }
    delete[] map_;
}