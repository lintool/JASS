/*
	MAIN.C
	------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "CI.h"


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
void trec_dump_results(uint32_t topic_id, uint16_t *accumulator, uint32_t accumulators_length)
{
uint32_t current;

for (current = 0; current < accumulators_length; current++)
	if (accumulator[current] != 0)
		printf("%d Q0 %s <rank> %d COMPILED\n", topic_id, CI_doclist[current], accumulator[current]);
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
uint16_t *accumulators;
CI_vocab *postings_list;
uint64_t timer, full_query_timer, us_convert;
uint64_t stats_accumulator_time = 0;
uint64_t stats_vocab_time = 0;
uint64_t stats_postings_time = 0;
uint64_t total_number_of_topics = 0;
uint64_t total_time_to_search = 0;

if (argc != 2)
	exit(printf("Usage:%s <queryfile>\n", argv[0]));

if ((fp = fopen(argv[1], "r")) == NULL)
	exit(printf("Can't open query file:%s\n", argv[1]));

accumulators = new uint16_t[CI_unique_documents];

full_query_timer = timer_start();
while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
	if ((id = strtok(buffer, SEPERATORS)) == NULL)
		continue;

	total_number_of_topics++;
	/*
		get the TREC query_id
	*/
	query_id = atoll(id);

	/*
		Initialise the accumulators
	*/
	timer = timer_start();
	memset(accumulators, 0, CI_unique_documents * sizeof(*accumulators));
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
			postings_list->method(accumulators);
			stats_postings_time += timer_stop(timer);
			}
		}

	/*
		Creat a TREC run file as output
	*/
	trec_dump_results(query_id, accumulators, CI_unique_documents);
	}
total_time_to_search += timer_stop(full_query_timer);

us_convert = timer_ticks_per_microsecond();

printf("Averages over %llu queries\n", total_number_of_topics);
printf("Accumulator initialisation: %lluus (%lluticks)\n", stats_accumulator_time / total_number_of_topics / us_convert, stats_accumulator_time / total_number_of_topics);
printf("Vocabulary lookup         : %lluus (%lluticks)\n", stats_vocab_time / total_number_of_topics / us_convert, stats_vocab_time / total_number_of_topics);
printf("Process Postings          : %lluus (%lluticks)\n", stats_postings_time / total_number_of_topics / us_convert, stats_postings_time / total_number_of_topics);
printf("Total time including I/O  : %lluus (%lluticks)\n", total_time_to_search / total_number_of_topics / us_convert, total_time_to_search / total_number_of_topics);

return 0;
}

