/*
	MAIN_HEAP.C
	-----------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
	#include <intrin.h>
	#include <windows.h>
#else
	#ifdef __APPLE__
		#include <mach/mach.h>
		#include <mach/mach_time.h>
	#endif
	#include <unistd.h>
	#include <glob.h>
	#include <sys/times.h>
	#include <dlfcn.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <emmintrin.h>
#include <smmintrin.h>

#include "CI.h"
#include "compress_qmx.h"
#include "compress_qmx_d4.h"

uint16_t *CI_accumulators;				// the accumulators
uint16_t **CI_accumulator_pointers;	// an array of pointers into the accumulators (used to avoid computing docIDs)
uint32_t CI_top_k;							// the number of results to find (top-k)
uint32_t CI_results_list_length;		// the number of results we found (at most top-k)

uint8_t *CI_accumulator_clean_flags;	// is the "row" of the accumulator table
uint32_t CI_accumulators_shift;			// number of bits to shift (right) the docid by to get the CI_accumulator_clean_flags
uint32_t CI_accumulators_width;			// the "width" of the accumulator table
uint32_t CI_accumulators_height;		// the "height" of the accumulator table
ANT_heap<uint16_t *, add_rsv_compare> *CI_heap;

char **CI_documentlist;					// the list of document IDs (TREC document IDs)

CI_vocab_heap *CI_dictionary;					// the vocab array
uint32_t CI_unique_terms;
uint32_t CI_unique_documents;

#define ALIGN_16 __attribute__ ((aligned (16)))
ALIGN_16 uint32_t *CI_decompressed_postings;
ALIGN_16 uint8_t *postings;					// the postings themselves

#define MAX_TERMS_PER_QUERY 10
#define MAX_QUANTUM 0xFF

#ifdef _MSC_VER
	#define atoll(x) _atoi64(x)
	double log2(double n) { return log(n) / log(2.0); }
#else
	/*
		__RDTSC()
		---------
	*/
	inline uint64_t __rdtsc()
	{
	uint32_t lo, hi;
	__asm__ __volatile__
		(
		"cpuid\n"
		"rdtsc\n"
		: "=a" (lo), "=d" (hi)
		:
		: "%ebx", "%ecx"
		);
	return (uint64_t)hi << 32 | lo;
	}
#endif

/*
	TIMER_START()
	-------------
*/
inline uint64_t timer_start(void)
{
#ifdef __APPLE__
	return mach_absolute_time();
#elif defined(_MSC_VER)
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart;
#elseif defined(__linux__)
	struct timeval now;
	gettimeofday(&now, NULL);
	return ((uint64_t)now.tv_sec) * 1000 * 1000 + now.tv_usec;
#else
	return __rdtsc();
#endif
}

/*
	TIMER_STOP()
	------------
*/
uint64_t timer_stop(uint64_t now)
{
#ifdef __APPLE__
	return mach_absolute_time() - now;
#elif defined(_MSC_VER)
	LARGE_INTEGER current;
	QueryPerformanceCounter(&current);
	return current.QuadPart - now;
#elseif defined(__linux__)
	struct timeval now;
	gettimeofday(&now, NULL);
	return (((uint64_t)now.tv_sec) * 1000 * 1000 + now.tv_usec) - now;
#else
	return __rdtsc() - now;
#endif
}

#if !(defined( __APPLE__) || defined(_MSC_VER))
	/*
		TIMER_TICKS_PER_SECOND()
		------------------------
	*/
	uint64_t timer_ticks_per_second(void)
	{
	static uint64_t answer = 0;
	struct timespec period, remaining;
	uint64_t start, total;

	if (answer == 0)
		{
		memset(&period, 0, sizeof(period));
		memset(&remaining, 0, sizeof(remaining));

		period.tv_sec = 1;
		period.tv_nsec = 0;

		do
			{
			start = timer_start();
			nanosleep(&period, &remaining);
			total = timer_stop(start);
			}
		while (remaining.tv_sec != 0 || remaining.tv_nsec != 0);

		answer = total;
		}

	return answeer;
	}
#endif

/*
	TIMER_TICKS_TO_MICROSECONDS()
	-----------------------------
*/
uint64_t timer_ticks_to_microseconds(uint64_t count)
{
#ifdef __APPLE__
	static mach_timebase_info_data_t tick_count;

	mach_timebase_info(&tick_count);

	return (count / 1000) * (tick_count.numer / tick_count.denom);
#elif defined (_MSC_VER)
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);

	return (count * 1000000.0) / frequency.QuadPart;
#elseif defined(__linux__)
	return count;				// already in us!
#else
	return count * 1000000.0 / timer_ticks_per_second();
#endif
}

/*
	PRINT_OS_TIME()
	---------------
*/
void print_os_time(void)
{
#ifdef __APPLE__
	struct tms tmsbuf;
	long clock_speed = sysconf(_SC_CLK_TCK);

	if (times(&tmsbuf) > 0)
		{
		printf("OS reports kernel time: %.3f seconds\n", (double)tmsbuf.tms_stime / clock_speed);
		printf("OS reports user time  : %.3f seconds\n", (double)tmsbuf.tms_utime / clock_speed);
		}
#endif
}

/*
	TREC_DUMP_RESULTS()
	-------------------
*/
void trec_dump_results(uint32_t topic_id, FILE *out, uint32_t output_length)
{
uint32_t current, id;

for (current = 0; current < (output_length < CI_results_list_length ? output_length : CI_results_list_length); current++)
	{
	id = CI_accumulator_pointers[current] -  CI_accumulators;
	fprintf(out, "%d Q0 %s %d %d COMPILED (ID:%u)\n", topic_id, CI_documentlist[id], current + 1, CI_accumulators[id], id);
	}
}

/*
	READ_ENTIRE_FILE()
	------------------
*/
char *read_entire_file(const char *filename, uint64_t *length = NULL)
{
char *block = NULL;
FILE *fp;
struct stat details;

if (filename == NULL)
	return NULL;

if ((fp = fopen(filename, "rb")) == NULL)
	return NULL;

if (fstat(fileno(fp), &details) == 0)
	if (details.st_size != 0)
		if ((block = new char [(size_t)(details.st_size + 1)]) != NULL)		// +1 for the '\0' on the end
			{
			if (length != NULL)
				*length = details.st_size;

			if (fread(block, details.st_size, 1, fp) == 1)
				block[details.st_size] = '\0';
			else
				{
				delete [] block;
				block = NULL;
				}
			}
fclose(fp);

return block;
}

/*
	CIT_PROCESS_LIST_COMPRESSED_VBYTE()
	-----------------------------------
*/
void CIt_process_list_compressed_vbyte(uint8_t *doclist, uint8_t *end, uint16_t impact, uint32_t integers)
{
uint32_t doc, sum;

sum = 0;
for (uint8_t *i = doclist; i < end;)
	{
	if (*i & 0x80)
		doc = *i++ & 0x7F;
	else
		{
		doc = *i++;
		while (!(*i & 0x80))
		   doc = (doc << 7) | *i++;
		doc = (doc << 7) | (*i++ & 0x7F);
		}
	sum += doc;
	
	add_rsv(sum, impact);
	}
}

/*
	CIT_PROCESS_LIST_DECOMPRESS_THEN_PROCESS()
	------------------------------------------
*/
void CIt_process_list_decompress_then_process(uint8_t *source, uint8_t *end, uint16_t impact, uint32_t integers)
{
uint32_t doc, sum;
uint32_t *integer, *destination = CI_decompressed_postings;

while (source < end)
	if (*source & 0x80)
		*destination++ = *source++ & 0x7F;
	else
		{
		*destination = *source++;
		while (!(*source & 0x80))
		   *destination = (*destination << 7) | *source++;
		*destination = (*destination << 7) | (*source++ & 0x7F);
		destination++;
		}

sum = 0;
integer = CI_decompressed_postings;
while (integer < destination)
	{
	sum += *integer++;
	add_rsv(sum, impact);
	}
}

/*
	CIT_PROCESS_LIST_NOT_COMPRESSED()
	---------------------------------
*/
void CIt_process_list_not_compressed(uint8_t *doclist, uint8_t *end, uint16_t impact, uint32_t integers)
{
uint32_t *i;

for (i = (uint32_t *)doclist; i < (uint32_t *)end; i++)
	add_rsv(*i, impact);
}

/*
	CIT_PROCESS_LIST_COMPRESSED_QMX()
	---------------------------------
*/
ANT_compress_qmx qmx_decoder;
void CIt_process_list_compressed_qmx(uint8_t *source, uint8_t *end, uint16_t impact, uint32_t integers)
{
uint32_t sum, *finish, *current;

qmx_decoder.decodeArray((uint32_t *)source, end - source, CI_decompressed_postings, integers);

sum = 0;
current = CI_decompressed_postings;
finish = current + integers;
while (current < finish)
	{
	sum += *current++;
	add_rsv(sum, impact);
	}
}

/*
	CIT_PROCESS_LIST_COMPRESSED_QMX_D4()
	------------------------------------
*/
ANT_compress_qmx_d4 qmx_d4_decoder;
void CIt_process_list_compressed_qmx_d4(uint8_t *source, uint8_t *end, uint16_t impact, uint32_t integers)
{
uint32_t sum, *finish, *current;

qmx_d4_decoder.decodeArray((uint32_t *)source, end - source, CI_decompressed_postings, integers);

current = CI_decompressed_postings;
finish = current + integers;
while (current < finish)
	add_rsv(*current++, impact);
}

/*
	CIT_PROCESS_LIST_COMPRESSED_QMX_D0()
	------------------------------------
*/
void CIt_process_list_compressed_qmx_d0(uint8_t *source, uint8_t *end, uint16_t impact, uint32_t integers)
{
uint32_t sum, *finish, *current;

qmx_decoder.decodeArray((uint32_t *)source, end - source, CI_decompressed_postings, integers);

current = CI_decompressed_postings;
finish = current + integers;
while (current < finish)
	add_rsv(*current++, impact);
}

/*
	CIT_PROCESS_LIST_COMPRESSED_SIMPLE8B()
	--------------------------------------
*/
void CIt_process_list_compressed_simple8b(uint8_t *source, uint8_t *end, uint16_t impact, uint32_t integers)
{
uint64_t *compressed_sequence = (uint64_t *)source;
uint32_t mask_type, sum;
uint64_t value;

sum = 0;
while (compressed_sequence < (uint64_t *)end)
	{
	// Load next compressed int, pull out the mask type used, shift to the values
	value = *compressed_sequence++;
	mask_type = value & 0xF;
	value >>= 4;

	// Unrolled loop to enable pipelining
	switch (mask_type)
		{
		case 0x0:
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			break;
		case 0x1:
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			sum += 1; add_rsv(sum, impact);
			break;
		case 0x2:
			sum += value & 0x1; add_rsv(sum, impact);
			sum += (value >> 1) & 0x1; add_rsv(sum, impact);
			sum += (value >> 2) & 0x1; add_rsv(sum, impact);
			sum += (value >> 3) & 0x1; add_rsv(sum, impact);
			sum += (value >> 4) & 0x1; add_rsv(sum, impact);
			sum += (value >> 5) & 0x1; add_rsv(sum, impact);
			sum += (value >> 6) & 0x1; add_rsv(sum, impact);
			sum += (value >> 7) & 0x1; add_rsv(sum, impact);
			sum += (value >> 8) & 0x1; add_rsv(sum, impact);
			sum += (value >> 9) & 0x1; add_rsv(sum, impact);
			sum += (value >> 10) & 0x1; add_rsv(sum, impact);
			sum += (value >> 11) & 0x1; add_rsv(sum, impact);
			sum += (value >> 12) & 0x1; add_rsv(sum, impact);
			sum += (value >> 13) & 0x1; add_rsv(sum, impact);
			sum += (value >> 14) & 0x1; add_rsv(sum, impact);
			sum += (value >> 15) & 0x1; add_rsv(sum, impact);
			sum += (value >> 16) & 0x1; add_rsv(sum, impact);
			sum += (value >> 17) & 0x1; add_rsv(sum, impact);
			sum += (value >> 18) & 0x1; add_rsv(sum, impact);
			sum += (value >> 19) & 0x1; add_rsv(sum, impact);
			sum += (value >> 20) & 0x1; add_rsv(sum, impact);
			sum += (value >> 21) & 0x1; add_rsv(sum, impact);
			sum += (value >> 22) & 0x1; add_rsv(sum, impact);
			sum += (value >> 23) & 0x1; add_rsv(sum, impact);
			sum += (value >> 24) & 0x1; add_rsv(sum, impact);
			sum += (value >> 25) & 0x1; add_rsv(sum, impact);
			sum += (value >> 26) & 0x1; add_rsv(sum, impact);
			sum += (value >> 27) & 0x1; add_rsv(sum, impact);
			sum += (value >> 28) & 0x1; add_rsv(sum, impact);
			sum += (value >> 29) & 0x1; add_rsv(sum, impact);
			sum += (value >> 30) & 0x1; add_rsv(sum, impact);
			sum += (value >> 31) & 0x1; add_rsv(sum, impact);
			sum += (value >> 32) & 0x1; add_rsv(sum, impact);
			sum += (value >> 33) & 0x1; add_rsv(sum, impact);
			sum += (value >> 34) & 0x1; add_rsv(sum, impact);
			sum += (value >> 35) & 0x1; add_rsv(sum, impact);
			sum += (value >> 36) & 0x1; add_rsv(sum, impact);
			sum += (value >> 37) & 0x1; add_rsv(sum, impact);
			sum += (value >> 38) & 0x1; add_rsv(sum, impact);
			sum += (value >> 39) & 0x1; add_rsv(sum, impact);
			sum += (value >> 40) & 0x1; add_rsv(sum, impact);
			sum += (value >> 41) & 0x1; add_rsv(sum, impact);
			sum += (value >> 42) & 0x1; add_rsv(sum, impact);
			sum += (value >> 43) & 0x1; add_rsv(sum, impact);
			sum += (value >> 44) & 0x1; add_rsv(sum, impact);
			sum += (value >> 45) & 0x1; add_rsv(sum, impact);
			sum += (value >> 46) & 0x1; add_rsv(sum, impact);
			sum += (value >> 47) & 0x1; add_rsv(sum, impact);
			sum += (value >> 48) & 0x1; add_rsv(sum, impact);
			sum += (value >> 49) & 0x1; add_rsv(sum, impact);
			sum += (value >> 50) & 0x1; add_rsv(sum, impact);
			sum += (value >> 51) & 0x1; add_rsv(sum, impact);
			sum += (value >> 52) & 0x1; add_rsv(sum, impact);
			sum += (value >> 53) & 0x1; add_rsv(sum, impact);
			sum += (value >> 54) & 0x1; add_rsv(sum, impact);
			sum += (value >> 55) & 0x1; add_rsv(sum, impact);
			sum += (value >> 56) & 0x1; add_rsv(sum, impact);
			sum += (value >> 57) & 0x1; add_rsv(sum, impact);
			sum += (value >> 58) & 0x1; add_rsv(sum, impact);
			sum += (value >> 59) & 0x1; add_rsv(sum, impact);
			break;
		case 0x3:
			sum += value & 0x3; add_rsv(sum, impact);
			sum += (value >> 2) & 0x3; add_rsv(sum, impact);
			sum += (value >> 4) & 0x3; add_rsv(sum, impact);
			sum += (value >> 6) & 0x3; add_rsv(sum, impact);
			sum += (value >> 8) & 0x3; add_rsv(sum, impact);
			sum += (value >> 10) & 0x3; add_rsv(sum, impact);
			sum += (value >> 12) & 0x3; add_rsv(sum, impact);
			sum += (value >> 14) & 0x3; add_rsv(sum, impact);
			sum += (value >> 16) & 0x3; add_rsv(sum, impact);
			sum += (value >> 18) & 0x3; add_rsv(sum, impact);
			sum += (value >> 20) & 0x3; add_rsv(sum, impact);
			sum += (value >> 22) & 0x3; add_rsv(sum, impact);
			sum += (value >> 24) & 0x3; add_rsv(sum, impact);
			sum += (value >> 26) & 0x3; add_rsv(sum, impact);
			sum += (value >> 28) & 0x3; add_rsv(sum, impact);
			sum += (value >> 30) & 0x3; add_rsv(sum, impact);
			sum += (value >> 32) & 0x3; add_rsv(sum, impact);
			sum += (value >> 34) & 0x3; add_rsv(sum, impact);
			sum += (value >> 36) & 0x3; add_rsv(sum, impact);
			sum += (value >> 38) & 0x3; add_rsv(sum, impact);
			sum += (value >> 40) & 0x3; add_rsv(sum, impact);
			sum += (value >> 42) & 0x3; add_rsv(sum, impact);
			sum += (value >> 44) & 0x3; add_rsv(sum, impact);
			sum += (value >> 46) & 0x3; add_rsv(sum, impact);
			sum += (value >> 48) & 0x3; add_rsv(sum, impact);
			sum += (value >> 50) & 0x3; add_rsv(sum, impact);
			sum += (value >> 52) & 0x3; add_rsv(sum, impact);
			sum += (value >> 54) & 0x3; add_rsv(sum, impact);
			sum += (value >> 56) & 0x3; add_rsv(sum, impact);
			sum += (value >> 58) & 0x3; add_rsv(sum, impact);
			break;
		case 0x4:
			sum += value & 0x7; add_rsv(sum, impact);
			sum += (value >> 3) & 0x7; add_rsv(sum, impact);
			sum += (value >> 6) & 0x7; add_rsv(sum, impact);
			sum += (value >> 9) & 0x7; add_rsv(sum, impact);
			sum += (value >> 12) & 0x7; add_rsv(sum, impact);
			sum += (value >> 15) & 0x7; add_rsv(sum, impact);
			sum += (value >> 18) & 0x7; add_rsv(sum, impact);
			sum += (value >> 21) & 0x7; add_rsv(sum, impact);
			sum += (value >> 24) & 0x7; add_rsv(sum, impact);
			sum += (value >> 27) & 0x7; add_rsv(sum, impact);
			sum += (value >> 30) & 0x7; add_rsv(sum, impact);
			sum += (value >> 33) & 0x7; add_rsv(sum, impact);
			sum += (value >> 36) & 0x7; add_rsv(sum, impact);
			sum += (value >> 39) & 0x7; add_rsv(sum, impact);
			sum += (value >> 42) & 0x7; add_rsv(sum, impact);
			sum += (value >> 45) & 0x7; add_rsv(sum, impact);
			sum += (value >> 48) & 0x7; add_rsv(sum, impact);
			sum += (value >> 51) & 0x7; add_rsv(sum, impact);
			sum += (value >> 54) & 0x7; add_rsv(sum, impact);
			sum += (value >> 57) & 0x7; add_rsv(sum, impact);
			break;
		case 0x5:
			sum += value & 0xF; add_rsv(sum, impact);
			sum += (value >> 4) & 0xF; add_rsv(sum, impact);
			sum += (value >> 8) & 0xF; add_rsv(sum, impact);
			sum += (value >> 12) & 0xF; add_rsv(sum, impact);
			sum += (value >> 16) & 0xF; add_rsv(sum, impact);
			sum += (value >> 20) & 0xF; add_rsv(sum, impact);
			sum += (value >> 24) & 0xF; add_rsv(sum, impact);
			sum += (value >> 28) & 0xF; add_rsv(sum, impact);
			sum += (value >> 32) & 0xF; add_rsv(sum, impact);
			sum += (value >> 36) & 0xF; add_rsv(sum, impact);
			sum += (value >> 40) & 0xF; add_rsv(sum, impact);
			sum += (value >> 44) & 0xF; add_rsv(sum, impact);
			sum += (value >> 48) & 0xF; add_rsv(sum, impact);
			sum += (value >> 52) & 0xF; add_rsv(sum, impact);
			sum += (value >> 56) & 0xF; add_rsv(sum, impact);
			break;
		case 0x6:
			sum += value & 0x1F; add_rsv(sum, impact);
			sum += (value >> 5) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 10) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 15) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 20) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 25) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 30) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 35) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 40) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 45) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 50) & 0x1F; add_rsv(sum, impact);
			sum += (value >> 55) & 0x1F; add_rsv(sum, impact);
			break;
		case 0x7:
			sum += value & 0x3F; add_rsv(sum, impact);
			sum += (value >> 6) & 0x3F; add_rsv(sum, impact);
			sum += (value >> 12) & 0x3F; add_rsv(sum, impact);
			sum += (value >> 18) & 0x3F; add_rsv(sum, impact);
			sum += (value >> 24) & 0x3F; add_rsv(sum, impact);
			sum += (value >> 30) & 0x3F; add_rsv(sum, impact);
			sum += (value >> 36) & 0x3F; add_rsv(sum, impact);
			sum += (value >> 42) & 0x3F; add_rsv(sum, impact);
			sum += (value >> 48) & 0x3F; add_rsv(sum, impact);
			sum += (value >> 54) & 0x3F; add_rsv(sum, impact);
			break;
		case 0x8:
			sum += value & 0x7F; add_rsv(sum, impact);
			sum += (value >> 7) & 0x7F; add_rsv(sum, impact);
			sum += (value >> 14) & 0x7F; add_rsv(sum, impact);
			sum += (value >> 21) & 0x7F; add_rsv(sum, impact);
			sum += (value >> 28) & 0x7F; add_rsv(sum, impact);
			sum += (value >> 35) & 0x7F; add_rsv(sum, impact);
			sum += (value >> 42) & 0x7F; add_rsv(sum, impact);
			sum += (value >> 49) & 0x7F; add_rsv(sum, impact);
			break;
		case 0x9:
			sum += value & 0xFF; add_rsv(sum, impact);
			sum += (value >> 8) & 0xFF; add_rsv(sum, impact);
			sum += (value >> 16) & 0xFF; add_rsv(sum, impact);
			sum += (value >> 24) & 0xFF; add_rsv(sum, impact);
			sum += (value >> 32) & 0xFF; add_rsv(sum, impact);
			sum += (value >> 40) & 0xFF; add_rsv(sum, impact);
			sum += (value >> 48) & 0xFF; add_rsv(sum, impact);
			break;
		case 0xA:
			sum += value & 0x3FF; add_rsv(sum, impact);
			sum += (value >> 10) & 0x3FF; add_rsv(sum, impact);
			sum += (value >> 20) & 0x3FF; add_rsv(sum, impact);
			sum += (value >> 30) & 0x3FF; add_rsv(sum, impact);
			sum += (value >> 40) & 0x3FF; add_rsv(sum, impact);
			sum += (value >> 50) & 0x3FF; add_rsv(sum, impact);
			break;
		case 0xB:
			sum += value & 0xFFF; add_rsv(sum, impact);
			sum += (value >> 12) & 0xFFF; add_rsv(sum, impact);
			sum += (value >> 24) & 0xFFF; add_rsv(sum, impact);
			sum += (value >> 36) & 0xFFF; add_rsv(sum, impact);
			sum += (value >> 48) & 0xFFF; add_rsv(sum, impact);
			break;
		case 0xC:
			sum += value & 0x7FFF; add_rsv(sum, impact);
			sum += (value >> 15) & 0x7FFF; add_rsv(sum, impact);
			sum += (value >> 30) & 0x7FFF; add_rsv(sum, impact);
			sum += (value >> 45) & 0x7FFF; add_rsv(sum, impact);
			break;
		case 0xD:
			sum += value & 0xFFFFF; add_rsv(sum, impact);
			sum += (value >> 20) & 0xFFFFF; add_rsv(sum, impact);
			sum += (value >> 40) & 0xFFFFF; add_rsv(sum, impact);
			break;
		case 0xE:
			sum += value & 0x3FFFFFFF; add_rsv(sum, impact);
			sum += (value >> 30) & 0x3FFFFFFF; add_rsv(sum, impact);
			break;
		case 0xF:
			sum += value & 0xFFFFFFFFFFFFFFFL; add_rsv(sum, impact);
			break;
		}
	}
}

/*
	struct CI_QUANTUM_HEADER
	------------------------
*/
class CI_quantum_header
{
public:
	uint16_t impact;
	uint64_t offset;
	uint64_t end;
	uint32_t quantum_frequency;
} __attribute__((packed));

/*
	QUANTUM_COMPARE()
	-----------------
*/
int quantum_compare(const void *a, const void *b)
{
CI_quantum_header *lhs = (CI_quantum_header *)((*(uint64_t *)a) + postings);
CI_quantum_header *rhs = (CI_quantum_header *)((*(uint64_t *)b) + postings);

/*
	sort from highest to lowest impact, but break ties by placing the lowest quantum-frequency first and the highest quantum-drequency last
*/
return lhs->impact > rhs->impact ? -1 : lhs->impact < rhs->impact ? 1 : lhs->quantum_frequency > rhs->quantum_frequency ? 1 : lhs->quantum_frequency == rhs->quantum_frequency ? 0 : -1;
}

/*
	PRINT_POSTINGS_LIST()
	---------------------
	uint16_t impact;			// the quantum impact score
	uint64_t offset;			// where the data is
	uint64_t length;			// length of the compressed postings list (in bytes)
*/
void print_postings_list(CI_vocab_heap *postings_list)
{
CI_quantum_header *header;

uint32_t current;
uint64_t *data;

printf("\n\noffset:0x%llX\n", postings_list->offset);  fflush(stdout);
printf("impacts:%llu\n", postings_list->impacts);
for (uint32_t x = 0; x < 64; x++)
	printf("%02X ", *(postings + postings_list->offset + x));
puts("");

data = (uint64_t *)(postings + postings_list->offset);
for (current = 0; current < postings_list->impacts; current++)
	{
	header = (CI_quantum_header *)(postings + (data[current]));
	printf("OFFSET:%llx Impact:%hx Offset:%llx End:%llx docids:%u\n", data[current], header->impact, header->offset, header->end, header->quantum_frequency);

	for (uint8_t *byte = postings + header->offset; byte < postings + header->end; byte++)
		{
		printf("0x%02X, ", *byte);
		if (*byte & 0x80)
			printf("\n");
		}
	puts("");
	}
}

/*
	READ_DOCLIST()
	--------------
*/
void read_doclist(void)
{
char *doclist;
uint64_t total_docs;
uint64_t length;
uint64_t *offset_base;

printf("Load doclist..."); fflush(stdout);
if ((doclist = read_entire_file("CIdoclist.bin", &length)) == 0)
	exit(printf("Can't read CIdoclist.bin"));

CI_unique_documents = total_docs = *((uint64_t *)(doclist + length - sizeof(uint64_t)));

CI_documentlist = new char * [total_docs];

offset_base = (uint64_t *)(doclist + length - (total_docs  * sizeof(uint64_t) + sizeof(uint64_t)));
for (uint64_t id = 0; id < total_docs; id++)
	CI_documentlist[id] = doclist + offset_base[id];

puts("done"); fflush(stdout);
}


/*
	READ_VOCAB()
	------------
*/
void read_vocab(void)
{
char *vocab, *vocab_terms;
uint64_t total_terms;
uint64_t length;
uint64_t *base;
uint64_t term;

printf("Load vocab..."); fflush(stdout);
if ((vocab = read_entire_file("CIvocab.bin", &length)) == NULL)
	exit(printf("Can't read CIvocab.bin"));
if ((vocab_terms = read_entire_file("CIvocab_terms.bin")) == NULL)
	exit(printf("Can't read CIvocab_terms.bin"));

CI_unique_terms = total_terms = length / (sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint64_t));

CI_dictionary = new CI_vocab_heap[total_terms];
for (term = 0; term < total_terms; term++)
	{
	base = (uint64_t *)(vocab + (3 * sizeof(uint64_t)) * term);

	CI_dictionary[term].term = vocab_terms + base[0];
	CI_dictionary[term].offset = base[1];
	CI_dictionary[term].impacts = base[2];
	}

puts("done"); fflush(stdout);
}

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
uint64_t full_query_timer = timer_start();

static char buffer[1024];
const char *SEPERATORS = " \t\r\n";
FILE *fp, *out;
char *term, *id;
uint64_t query_id;
CI_vocab_heap *postings_list;
uint64_t timer, full_query_without_io_timer;
uint64_t stats_accumulator_time;
uint64_t stats_vocab_time;
uint64_t stats_postings_time;
uint64_t stats_sort_time;
uint64_t total_number_of_topics;
uint64_t stats_total_time_to_search;
uint64_t stats_total_time_to_search_without_io;
uint32_t accumulators_needed;
uint64_t stats_quantum_prep_time;
uint64_t stats_early_terminate_check_time, stats_quantum_check_count, stats_quantum_count, stats_early_terminations;
uint64_t experimental_repeat = 0, times_to_repeat_experiment = 2;
uint64_t *quantum_order, *current_quantum;
uint64_t max_remaining_impact;
uint16_t **quantum_check_pointers;
uint64_t early_terminate;
uint16_t **partial_rsv;
CI_quantum_header *current_header;
void (*process_postings_list)(uint8_t *doclist, uint8_t *end, uint16_t impact, uint32_t integers);
uint32_t parameter;

printf("Load postings..."); fflush(stdout);
if ((postings = (uint8_t *)read_entire_file("CIpostings.bin")) == NULL)
	exit(printf("Cannot open postings file 'CIpostings.bin'\n"));
puts("done"); fflush(stdout);

if (argc < 1 || argc > 4)
	exit(printf("Usage:%s <queryfile> [<top-k-number>] [-d<ecompress then process>]\n", argv[0]));

if ((fp = fopen(argv[1], "r")) == NULL)
	exit(printf("Can't open query file:%s\n", argv[1]));

if ((out = fopen("ranking.txt", "w")) == NULL )
  exit(printf("Can't open output file.\n"));

/*
	Sort out how to decode the postings (either compressed or not)
*/
if (*postings == 's')
	{
	puts("Uncompressed Index");
	process_postings_list = CIt_process_list_not_compressed;
	}
else if (*postings == 'c')
	{
	puts("Variable Byte Compressed Index");
	process_postings_list = CIt_process_list_compressed_vbyte;
	}
else if (*postings == '8')
	{
	puts("Simple-8b Compressed Index");
	process_postings_list = CIt_process_list_compressed_simple8b;
	}
else if (*postings == 'q')
	{
	puts("QMX Compressed Index");
	process_postings_list = CIt_process_list_compressed_qmx;
	}
else if (*postings == 'Q')
	{
	puts("QMX-D4 Compressed Index");
	process_postings_list = CIt_process_list_compressed_qmx_d4;
	}
else if (*postings == 'R')
	{
	puts("QMX-D0 Compressed Index");
	process_postings_list = CIt_process_list_compressed_qmx_d0;
	}
else
	exit(printf("This index appears to be invalid as it is neither compressed nor not compressed!\n"));

read_doclist();
read_vocab();

CI_top_k = CI_unique_documents + 1;

for (parameter = 2; parameter < argc; parameter++)
	if (strcmp(argv[parameter], "-d") == 0)
		if (*postings == 'c')
			process_postings_list = CIt_process_list_decompress_then_process;
		else
			exit(printf("Cannot decompress then process as the postings are not compressed"));
	else
		CI_top_k = atoll(argv[parameter]);

/*
	Compute the details of the accumulator table
*/
CI_accumulators_shift = log2(sqrt((double)CI_unique_documents));
CI_accumulators_width = 1 << CI_accumulators_shift;
CI_accumulators_height = (CI_unique_documents + CI_accumulators_width) / CI_accumulators_width;
accumulators_needed = CI_accumulators_width * CI_accumulators_height;				// guaranteed to be larger than the highest accumulagtor that can be initialised
CI_accumulator_clean_flags = new uint8_t[CI_accumulators_height];

/*
	Create a buffer to store the decompressed postings lists
*/
CI_decompressed_postings = new uint32_t[CI_unique_documents + 1024];		// we add because some decompressors are allowed to overflow this buffer

/*
	Now prime the search engine
*/
CI_accumulators = new uint16_t[accumulators_needed];
CI_accumulator_pointers = new uint16_t * [accumulators_needed];

/*
	For QaaT early termination we need K+1 elements in the heap so that we can check that nothing else can get into the top-k.
*/
CI_top_k++;
CI_heap = new ANT_heap<uint16_t *, add_rsv_compare>(*CI_accumulator_pointers, CI_top_k);

/*
	Allocate the quantum at a time table
*/
quantum_order = new uint64_t [MAX_TERMS_PER_QUERY * MAX_QUANTUM];
quantum_check_pointers = new uint16_t * [accumulators_needed];

/*
	Now start searching
*/
while (experimental_repeat < times_to_repeat_experiment)
	{
	experimental_repeat++;
	stats_accumulator_time = 0;
	stats_vocab_time = 0;
	stats_postings_time = 0;
	stats_sort_time = 0;
	stats_total_time_to_search = 0;
	stats_total_time_to_search_without_io = 0;
	total_number_of_topics = 0;
	stats_quantum_prep_time = 0;
	stats_early_terminate_check_time = 0;
	stats_quantum_check_count = 0;
	stats_quantum_count = 0;
	stats_early_terminations = 0;

	rewind(fp);					// the query file
	rewind(out);				// the TREC run file

	while (fgets(buffer, sizeof(buffer), fp) != NULL)
		{
		full_query_without_io_timer = timer_start();
		if ((id = strtok(buffer, SEPERATORS)) == NULL)
			continue;

		total_number_of_topics++;
		CI_results_list_length = 0;

		/*
			get the TREC query_id
		*/
		query_id = atoll(id);

		/*
			Initialise the accumulators
		*/
		timer = timer_start();
		memset(CI_accumulator_clean_flags, 0, CI_accumulators_height);
		stats_accumulator_time += timer_stop(timer);

		/*
			For each term, drag out the pointer list and add it to the list of quantums to process
		*/
		max_remaining_impact = 0;
		current_quantum = quantum_order;
		early_terminate = false;

		while ((term = strtok(NULL, SEPERATORS)) != NULL)
			{
			timer = timer_start();
			postings_list = (CI_vocab_heap *)bsearch(term, CI_dictionary, CI_unique_terms, sizeof(*CI_dictionary), CI_vocab_heap::compare_string);
			stats_vocab_time += timer_stop(timer);

// print_postings_list(postings_list);

			/*
				Initialise the QaaT (Quantum at a Time) structures
			*/
			timer = timer_start();
			if (postings_list != NULL)
				{
				/*
					Copy this term's pointers to the quantum list
				*/
				memcpy(current_quantum, postings + postings_list->offset, postings_list->impacts * sizeof(*quantum_order));

				/*
					Compute the maximum possibe impact score (that is, assume one document has the maximum impact of each term)
				*/
				max_remaining_impact += ((CI_quantum_header *)(postings + *current_quantum))->impact;

				/*
					Advance to the place we want to place the next quantum set
				*/
				current_quantum += postings_list->impacts;
				}
			}
		/*
			NULL termainate the list of quantums
		*/
		*current_quantum = 0;

		/*
			Sort the quantum list from highest to lowest
		*/
		qsort(quantum_order, current_quantum - quantum_order, sizeof(*quantum_order), quantum_compare);

		stats_quantum_prep_time += timer_stop(timer);
		/*
			Now process each quantum, one at a time
		*/
		for (current_quantum = quantum_order; *current_quantum != 0; current_quantum++)
			{
			stats_quantum_count++;

			current_header = (CI_quantum_header *)(postings + *current_quantum);
			timer = timer_start();
			(*process_postings_list)(postings + current_header->offset, postings + current_header->end, current_header->impact, current_header->quantum_frequency);
			stats_postings_time += timer_stop(timer);

			/*
				Check to see if its posible for the remaining impacts to affect the order of the top-k
			*/
			timer = timer_start();
			/*
				Subtract the current impact score and then add the next impact score for the current term
			*/
			max_remaining_impact -= current_header->impact;
			max_remaining_impact += (current_header + 1)->impact;

			if (CI_results_list_length > CI_top_k - 1)
				{
				stats_quantum_check_count++;
				/*
					We need to run through the top-(k+1) to see if its possible for any re-ordering to occur
					1. copy the accumulators;
					2. sort the top k + 1
					3. go through consequative rsvs checking to see if reordering is possible (check rsv[k] - rsv[k + 1])
				*/
				memcpy(quantum_check_pointers, CI_accumulator_pointers, CI_top_k * sizeof(*quantum_check_pointers));
				top_k_qsort(quantum_check_pointers, CI_top_k, CI_top_k);

				early_terminate = true;

				for (partial_rsv = quantum_check_pointers; partial_rsv < quantum_check_pointers + CI_top_k - 1; partial_rsv++)
					if (**partial_rsv - **(partial_rsv + 1) < max_remaining_impact)		// We're sorted from largest to smallest so a[x] - a[x+1] >= 0
						{
						early_terminate = false;
						break;
						}
				}
			stats_early_terminate_check_time += timer_stop(timer);
			if (early_terminate)
				{
				stats_early_terminations++;
				break;
				}
			}

		/*
			sort the accumulator pointers to put the highest RSV document at the top of the list
		*/
		timer = timer_start();
		top_k_qsort(CI_accumulator_pointers, CI_results_list_length, CI_top_k - 1);
		stats_sort_time += timer_stop(timer);

		/*
			At this point we know the number of hits (CI_results_list_length) and they can be decode out of the CI_accumulator_pointers array
			where CI_accumulator_pointers[0] points into CI_accumulators[] and therefore CI_accumulator_pointers[0] - CI_accumulators is the docid
			and *CI_accumulator_pointers[0] is the rsv.
		*/
		stats_total_time_to_search_without_io += timer_stop(full_query_without_io_timer);

		/*
			Creat a TREC run file as output
		*/
		trec_dump_results(query_id, out, CI_top_k - 1);		// subtract 1 from top_k because we added 1 for the early termination checks
		}
	}

fclose(out);
fclose(fp);

stats_total_time_to_search += timer_stop(full_query_timer);
print_os_time();

printf("Averages over %llu queries\n", total_number_of_topics);
printf("Accumulator initialisation per query : %10llu us (%llu ticks)\n", timer_ticks_to_microseconds(stats_accumulator_time / total_number_of_topics), stats_accumulator_time / total_number_of_topics);
printf("Vocabulary lookup per query          : %10llu us (%llu ticks)\n", timer_ticks_to_microseconds(stats_vocab_time / total_number_of_topics), stats_vocab_time / total_number_of_topics);
printf("QaaT prep time per query             : %10llu us (%llu ticks)\n", timer_ticks_to_microseconds(stats_quantum_prep_time / total_number_of_topics), stats_quantum_prep_time / total_number_of_topics);
printf("Process postings per query           : %10llu us (%llu ticks)\n", timer_ticks_to_microseconds(stats_postings_time / total_number_of_topics), stats_postings_time / total_number_of_topics);
printf("QaaT early terminate check per query : %10llu us (%llu ticks)\n", timer_ticks_to_microseconds(stats_early_terminate_check_time / total_number_of_topics), stats_early_terminate_check_time / total_number_of_topics);
printf("Order the top-k per query            : %10llu us (%llu ticks)\n", timer_ticks_to_microseconds(stats_sort_time / total_number_of_topics), stats_sort_time / total_number_of_topics);
printf("Total time excluding I/O per query   : %10llu us (%llu ticks)\n", timer_ticks_to_microseconds(stats_total_time_to_search_without_io / total_number_of_topics), stats_total_time_to_search_without_io / total_number_of_topics);
printf("Total run time                       : %10llu us (%llu ticks)\n", timer_ticks_to_microseconds(stats_total_time_to_search), stats_total_time_to_search);

printf("Total number of QaaT early terminate checks : %10llu\n", stats_quantum_check_count);
printf("Total number of QaaT early terminations     : %10llu\n", stats_early_terminations);
printf("Total number of quantums processed          : %10llu\n", stats_quantum_count);

return 0;
}

