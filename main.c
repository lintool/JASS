/*
	MAIN.C
	------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "CI.h"

uint16_t *CI_accumulators;
uint16_t **CI_accumulator_pointers;
uint32_t CI_top_k;
ANT_heap<uint16_t *, add_rsv_compare> *CI_heap;
uint32_t CI_results_list_length;


#ifndef _MSC_VER
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
return __rdtsc();
}

/*
	TIMER_STOP()
	------------
*/
uint64_t timer_stop(uint64_t now)
{
return __rdtsc() - now;
}

/*
	TIMER_TICKS_PER_MICROSECOND()
	-----------------------------
*/
uint64_t timer_ticks_per_microsecond(void)
{
struct timespec period, remaining;
uint64_t start, total;

memset(&period, 0, sizeof(period));
memset(&remaining, 0, sizeof(remaining));

period.tv_sec = 0;
period.tv_nsec = 1000 * 1000;				// wait for 1 million nanoseconds (which is 1 millisecond)

do
	{
	start = timer_start();
	nanosleep(&period, &remaining);
	total = timer_stop(start);
	}
while (remaining.tv_sec != 0 || remaining.tv_nsec != 0);

return total / 1000; //because there are 1000 microseconds in a millisecond
}

/*
	TREC_DUMP_RESULTS()
	-------------------
*/
void trec_dump_results(uint32_t topic_id)
{
uint32_t current, id;

for (current = 0; current < CI_results_list_length; current++)
	{
	id = CI_accumulator_pointers[current] - CI_accumulators;
	printf("%d Q0 %s %d %d COMPILED (ID:%u)\n", topic_id, CI_doclist[id], current + 1, CI_accumulators[id], id);
	}
puts("");
}

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
static char buffer[1024];
const char *SEPERATORS = " \t\r\n";
FILE *fp;
char *term, *id;
uint64_t query_id;
CI_vocab *postings_list;
uint64_t timer, full_query_timer, us_convert;
uint64_t stats_accumulator_time = 0;
uint64_t stats_vocab_time = 0;
uint64_t stats_postings_time = 0;
uint64_t stats_sort_time = 0;
uint64_t total_number_of_topics = 0;
uint64_t total_time_to_search = 0;

if (argc != 2)
	exit(printf("Usage:%s <queryfile>\n", argv[0]));

if ((fp = fopen(argv[1], "r")) == NULL)
	exit(printf("Can't open query file:%s\n", argv[1]));

CI_accumulators = new uint16_t[CI_unique_documents];
CI_accumulator_pointers = new uint16_t * [CI_unique_documents];
CI_top_k = CI_unique_documents + 1;
CI_heap = new ANT_heap<uint16_t *, add_rsv_compare>(*CI_accumulator_pointers, CI_top_k);

full_query_timer = timer_start();
while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
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
	memset(CI_accumulators, 0, CI_unique_documents * sizeof(*CI_accumulators));
	stats_accumulator_time += timer_stop(timer);

	/*
		For each term, call the method to update the accumulators
	*/
	while ((term = strtok(NULL, SEPERATORS)) != NULL)
		{
		timer = timer_start();
		postings_list = (CI_vocab *)bsearch(term, CI_dictionary, CI_unique_terms, sizeof(*CI_dictionary), CI_vocab::compare_string);
		stats_vocab_time += timer_stop(timer);
		if (postings_list != NULL)
			{
			timer = timer_start();
			postings_list->method();
			stats_postings_time += timer_stop(timer);
			}
		}

	/*
		sort the accumulator pointers to put the highest RSV document at the top of the list
	*/
	timer = timer_start();
	top_k_qsort(CI_accumulator_pointers, CI_results_list_length, CI_top_k);
	stats_sort_time += timer_stop(timer);

	/*
		Creat a TREC run file as output
	*/
	trec_dump_results(query_id);
	}
total_time_to_search += timer_stop(full_query_timer);

us_convert = timer_ticks_per_microsecond();

printf("Averages over %llu queries\n", total_number_of_topics);
printf("Accumulator initialisation: %lluus (%lluticks)\n", stats_accumulator_time / total_number_of_topics / us_convert, stats_accumulator_time / total_number_of_topics);
printf("Vocabulary lookup         : %lluus (%lluticks)\n", stats_vocab_time / total_number_of_topics / us_convert, stats_vocab_time / total_number_of_topics);
printf("Process Postings          : %lluus (%lluticks)\n", stats_postings_time / total_number_of_topics / us_convert, stats_postings_time / total_number_of_topics);
printf("Order the top-k           : %lluus (%lluticks)\n", stats_sort_time / total_number_of_topics / us_convert, stats_sort_time / total_number_of_topics);
printf("Total time including I/O  : %lluus (%lluticks)\n", total_time_to_search / total_number_of_topics / us_convert, total_time_to_search / total_number_of_topics);

return 0;
}

