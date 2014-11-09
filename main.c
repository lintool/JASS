/*
	MAIN.C
	------
*/
#ifdef __APPLE__
	#include <mach/mach.h>
	#include <mach/mach_time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/times.h>
#include "CI.h"

uint16_t *CI_accumulators;
uint16_t **CI_accumulator_pointers;
uint32_t CI_top_k;
ANT_heap<uint16_t *, add_rsv_compare> *CI_heap;
uint32_t CI_results_list_length;

uint32_t CI_accumulators_shift;
uint32_t CI_accumulators_width;
uint32_t CI_accumulators_height;
uint8_t *CI_accumulator_clean_flags;

/*
	TIMER_START()
	-------------
*/
inline uint64_t timer_start(void)
{
return mach_absolute_time();
}

/*
	TIMER_STOP()
	------------
*/
uint64_t timer_stop(uint64_t now)
{
return mach_absolute_time() - now;
}

/*
	TIMER_TICKS_PER_MICROSECOND()
	-----------------------------
*/
uint64_t timer_ticks_per_microsecond(void)
{
static mach_timebase_info_data_t tick_count;

mach_timebase_info(&tick_count);

return 1000.0 * tick_count.numer / tick_count.denom;
}

/*
	PRINT_OS_TIME()
	---------------
*/
void print_os_time(void)
{
struct tms tmsbuf;
long clock_speed = sysconf(_SC_CLK_TCK);

if (times(&tmsbuf) > 0)
	{
	printf("OS reports kernel time: %.3f seconds\n", (double)tmsbuf.tms_stime / clock_speed);
	printf("OS reports user time  : %.3f seconds\n", (double)tmsbuf.tms_utime / clock_speed);
	}
	}

/*
	TREC_DUMP_RESULTS()
	-------------------
*/
void trec_dump_results(uint32_t topic_id, FILE *out)
{
uint32_t current, id;
uint32_t output_length;

output_length = CI_results_list_length < 10 ? CI_results_list_length : 10;			// at most 10 results will be printed per query
output_length = CI_results_list_length;

for (current = 0; current < CI_results_list_length; current++)
	{
	id = CI_accumulator_pointers[current] - CI_accumulators;
	fprintf(out, "%d Q0 %s %d %d COMPILED (ID:%u)\n", topic_id, CI_doclist[id], current + 1, CI_accumulators[id], id);
	}
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
CI_vocab *postings_list;
uint64_t timer, full_query_without_io_timer, us_convert;
uint64_t stats_accumulator_time;
uint64_t stats_vocab_time;
uint64_t stats_postings_time;
uint64_t stats_sort_time;
uint64_t total_number_of_topics;
uint64_t stats_total_time_to_search;
uint64_t stats_total_time_to_search_without_io;
uint32_t accumulators_needed;
uint64_t experimental_repeat = 0, times_to_repeat_experiment = 2;

if (argc != 2 && argc != 3)
	exit(printf("Usage:%s <queryfile> [<top-k-number>]\n", argv[0]));

if ((fp = fopen(argv[1], "r")) == NULL)
	exit(printf("Can't open query file:%s\n", argv[1]));

if ((out = fopen("ranking.txt", "w")) == NULL )
  exit(printf("Can't open output file.\n"));

/*
	Compute the details of the accumulator table
*/
CI_accumulators_shift = log2(sqrt(CI_unique_documents));
CI_accumulators_width = 1 << CI_accumulators_shift;
CI_accumulators_height = (CI_unique_documents + CI_accumulators_width) / CI_accumulators_width;
accumulators_needed = CI_accumulators_width * CI_accumulators_height;				// guaranteed to be larger than the highest accumulagtor that can be initialised
CI_accumulator_clean_flags = new uint8_t[CI_accumulators_height];

/*
	Now prime the search engine
*/
CI_accumulators = new uint16_t[accumulators_needed];
CI_accumulator_pointers = new uint16_t * [accumulators_needed];
CI_top_k = argc == 2 ? CI_unique_documents + 1 : atoll(argv[2]);
CI_heap = new ANT_heap<uint16_t *, add_rsv_compare>(*CI_accumulator_pointers, CI_top_k);

/*
	Now start searching
*/
while (experimental_repeat < times_to_repeat_experiment)
	{
	puts("Repeat");
	experimental_repeat++;
	stats_accumulator_time = 0;
	stats_vocab_time = 0;
	stats_postings_time = 0;
	stats_sort_time = 0;
	stats_total_time_to_search = 0;
	stats_total_time_to_search_without_io = 0;
	total_number_of_topics = 0;

	rewind(fp);
	rewind(out);

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
			At this point we know the number of hits (CI_results_list_length) and they can be decode out of the CI_accumulator_pointers array
			where CI_accumulator_pointers[0] points into CI_accumulators[] and therefore CI_accumulator_pointers[0] - CI_accumulators is the docid
			and *CI_accumulator_pointers[0] is the rsv.
		*/
		stats_total_time_to_search_without_io += timer_stop(full_query_without_io_timer);

		/*
			Creat a TREC run file as output
		*/
//		trec_dump_results(query_id, out);
		}
	}

fclose(out);
fclose(fp);

stats_total_time_to_search += timer_stop(full_query_timer);

us_convert = timer_ticks_per_microsecond();

printf("Averages over %llu queries\n", total_number_of_topics);
printf("Accumulator initialisation           : %4llu us (%8llu ticks)\n", stats_accumulator_time / total_number_of_topics / us_convert, stats_accumulator_time / total_number_of_topics);
printf("Vocabulary lookup                    : %4llu us (%8llu ticks)\n", stats_vocab_time / total_number_of_topics / us_convert, stats_vocab_time / total_number_of_topics);
printf("Process postings                     : %4llu us (%8llu ticks)\n", stats_postings_time / total_number_of_topics / us_convert, stats_postings_time / total_number_of_topics);
printf("Order the top-k                      : %4llu us (%8llu ticks)\n", stats_sort_time / total_number_of_topics / us_convert, stats_sort_time / total_number_of_topics);
printf("Total time excluding I/O             : %4llu us (%8llu ticks)\n", stats_total_time_to_search_without_io / total_number_of_topics / us_convert, stats_total_time_to_search_without_io / total_number_of_topics);
printf("Total time including I/O and repeats : %4llu us (%8llu ticks)\n", stats_total_time_to_search / total_number_of_topics / us_convert, stats_total_time_to_search / total_number_of_topics);
print_os_time();

return 0;
}

