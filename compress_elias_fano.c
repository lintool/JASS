/*
	COMPRESS_ELIAS_FANO.C
	---------------------
*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "compress_elias_fano.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/*
	ANT_COMPRESS_ELIAS_FANO::RESET()
	--------------------------------
*/
void ANT_compress_elias_fano::reset(uint32_t upper_bound, uint32_t size)
{
size = size;
auto x = upper_bound / size;
numLowerBits = MIN( x ? 8 * sizeof(unsigned int) - __builtin_clz(x) : 0 , 56);

auto upper_size_bits = (upper_bound >> numLowerBits) + size;
upper = (upper_size_bits + 7) / 8;
lower = (numLowerBits * size + 7) / 8;

bytes = lower + upper;

upper_bound_ = upper_bound;
size_ = 0;

block_ = 0;
outer_ = 0;
inner_ = -1;
position_ = -1;
uvalue_ = 0;

/* printf("upper bytes: %u\nlower bytes: %u\n", upper, lower); */
}

/*
	ANT_COMPRESS_ELIAS_FANO::COMPRESS()
	-----------------------------------
*/
long long ANT_compress_elias_fano::compress(unsigned char *destination, long long destination_length, uint32_t *source, long long source_integers) 
{
long long space = sizeof(uint32_t) + bytes + ANT_compress_elias_fano::PADDING;

if (space > destination_length)
	{
	printf("Not enough space! (need: %lld, given: %lld)\n", space, destination_length);
	return -1;
	}

for (int i = 1; i < source_integers; i++)
	if (source[i] <= source[i - 1])
		exit(printf("Non-monotonic! (%u %u @ %d)\n", source[i-1], source[i], i));

/* write the upper bound used to derive needed values */
*((uint32_t *)destination) = upper_bound_;
destination = (unsigned char *)(((uint32_t *)destination) + 1);

for (int i = 0; i < source_integers; i++)
	add(source[i], destination);

/* for (int ch = 0; ch < space; ch++) */
/* 	{ */
/* 	printf("%02x ", destination[ch]); */
/* 	if ((ch + 1) % 0x10 == 0) */
/* 		printf("\n"); */
/* 	} */

return space;
}

void ANT_compress_elias_fano::set_block(unsigned char *source)
{
block_ = *(uint64_t *)source;
}

/*
	ANT_COMPRESS_ELIAS_FANO::DECOMPRESS()
	-------------------------------------
*/
void ANT_compress_elias_fano::decompress(uint32_t *destination, unsigned char *source, long long destination_integers)
{
set_block(source);
for (long long i = 0; i < destination_integers; i++)
	destination[i] = next(source);

}

/*
*/
uint32_t ANT_compress_elias_fano::next(unsigned char *source)
{
uint32_t lowerMask_ = (1 << numLowerBits) - 1;

while (block_ == 0)
	{
	outer_ += sizeof(block_);
	block_ = *(uint64_t *)(source + outer_);
	}
++position_;
inner_ = __builtin_ctzll(block_);
block_ = block_ & (block_ - 1);

uvalue_ = (uint32_t)(8 * outer_ + inner_ - position_);

size_t pos = position_ * numLowerBits;
unsigned char *ptr = source + upper + (pos / 8);
uint64_t ptrv = *(uint64_t *)ptr;

return (lowerMask_ & (ptrv >> (pos % 8))) | (uvalue_ << numLowerBits);
}

/*
   ANT_COMPRESS_ELIAS_FANO::ADD()
   ------------------------------
*/
void ANT_compress_elias_fano::add(uint32_t value, unsigned char *destination)
{
const uint32_t upperBits = value >> numLowerBits;
const size_t pos = upperBits + size_;
destination[pos / 8] |= 1U << (pos % 8);
if (numLowerBits != 0)
	{
	uint32_t lowerBits = value & ((1 << numLowerBits) - 1);
	write_bits_56(destination + upper, size_ * numLowerBits, lowerBits);
	}
++size_;
};

/*
	ANT_COMPRESS_ELIAS_FANO::WRITE_BITS_56()
	----------------------------------------
*/
void ANT_compress_elias_fano::write_bits_56(unsigned char *data, size_t pos, uint64_t value)
{
unsigned char * const ptr = data + (pos / 8);
uint64_t ptrv = *(uint64_t *)ptr;
ptrv |= value << (pos % 8);
memcpy(ptr, &ptrv, sizeof(ptrv));
};
