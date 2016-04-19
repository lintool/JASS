/*
	COMPRESS_ELIAS_FANO.H
	---------------------
*/
#ifndef COMPRESS_ELIAS_FANO_H_
#define COMPRESS_ELIAS_FANO_H_

#include "compress.h"

/*
	class ANT_COMPRESS_ELIAS_FANO
	-----------------------------
*/
class ANT_compress_elias_fano : public ANT_compress
{
public:
	ANT_compress_elias_fano() {}
	virtual ~ANT_compress_elias_fano() {}

	virtual long long compress(unsigned char *destination, long long destination_length, uint32_t *source, long long source_integers);
	virtual void decompress(uint32_t *destination, unsigned char *source, long long destination_integers);

	uint32_t next(unsigned char *destination);
	void reset(uint32_t upper_bound, uint32_t size);
	void set_block(unsigned char *source);

private:
	uint32_t size;
	uint32_t numLowerBits;
	uint32_t lower;
	uint32_t upper;
	uint32_t numUpperBits;
	uint32_t upper_bound_;

	uint64_t block_;
	size_t outer_;
	size_t inner_;
	size_t position_;
	uint32_t uvalue_;

	uint8_t *buf;
	uint32_t bytes;

	static const uint32_t PADDING = 7;

	uint32_t size_;

	void add(uint32_t value, unsigned char *destination);
	void write_bits_56(unsigned char *data, size_t pos, uint64_t value);
} ;

#endif  /* COMPRESS_ELIAS_FANO_H_ */
