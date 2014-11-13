/*
	MAIN.C
	------
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
	#include <sys/times.h>
#endif

#include "CI.h"

#define MAX_TERMS_PER_QUERY 10
#define MAX_QUANTUM 0xFF

CI_globals globals;

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

for (current = 0; current < (output_length < globals.CI_results_list_length ? output_length : globals.CI_results_list_length); current++)
	{
	id = globals.CI_accumulator_pointers[current] -  globals.CI_accumulators;
	fprintf(out, "%d Q0 %s %d %d COMPILED (ID:%u)\n", topic_id, CI_doclist[id], current + 1, globals.CI_accumulators[id], id);
	}
}

/*
	QUANTUM_COMPARE()
	-----------------
*/
int quantum_compare(const void *a, const void *b)
{
CI_impact_method **lhs = (CI_impact_method **)a;
CI_impact_method **rhs = (CI_impact_method **)b;

return (*lhs)->impact < (*rhs)->impact ? 1 : (*lhs)->impact == (*rhs)->impact ? 0 : -1;
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
struct CI_impact_method **quantum_order, **current_quantum;
uint64_t max_remaining_impact;
uint16_t **quantum_check_pointers;
uint64_t early_terminate;
uint16_t **partial_rsv;

if (argc != 2 && argc != 3)
	exit(printf("Usage:%s <queryfile> [<top-k-number>]\n", argv[0]));

if ((fp = fopen(argv[1], "r")) == NULL)
	exit(printf("Can't open query file:%s\n", argv[1]));

if ((out = fopen("ranking.txt", "w")) == NULL )
  exit(printf("Can't open output file.\n"));

/*
	Compute the details of the accumulator table
*/
globals.CI_accumulators_shift = log2(sqrt((double)CI_unique_documents));
globals.CI_accumulators_width = 1 << globals.CI_accumulators_shift;
globals.CI_accumulators_height = (CI_unique_documents + globals.CI_accumulators_width) / globals.CI_accumulators_width;
accumulators_needed = globals.CI_accumulators_width * globals.CI_accumulators_height;				// guaranteed to be larger than the highest accumulagtor that can be initialised
globals.CI_accumulator_clean_flags = new uint8_t[globals.CI_accumulators_height];

/*
	Now prime the search engine
*/
globals.CI_accumulators = new uint16_t[accumulators_needed];
globals.CI_accumulator_pointers = new uint16_t * [accumulators_needed];
globals.CI_top_k = argc == 2 ? CI_unique_documents + 1 : atoll(argv[2]);

/*
	For QaaT early termination we need K+1 elements in the heap so that we can check that nothing else can get into the top-k.
*/
globals.CI_top_k++;
globals.CI_heap = new ANT_heap<uint16_t *, add_rsv_compare>(*globals.CI_accumulator_pointers, globals.CI_top_k);

/*
	Allocate the quantum at a time table
*/
quantum_order = new struct CI_impact_method *[MAX_TERMS_PER_QUERY * MAX_QUANTUM];
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

	rewind(fp);
	rewind(out);

	while (fgets(buffer, sizeof(buffer), fp) != NULL)
		{
		full_query_without_io_timer = timer_start();
		if ((id = strtok(buffer, SEPERATORS)) == NULL)
			continue;

		total_number_of_topics++;
		globals.CI_results_list_length = 0;

		/*
			get the TREC query_id
		*/
		query_id = atoll(id);

		/*
			Initialise the accumulators
		*/
		timer = timer_start();
		memset(globals.CI_accumulator_clean_flags, 0, globals.CI_accumulators_height);
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
			postings_list = (CI_vocab *)bsearch(term, CI_dictionary, CI_unique_terms, sizeof(*CI_dictionary), CI_vocab::compare_string);
			stats_vocab_time += timer_stop(timer);

			/*
				Initialise the QaaT (Quantum at a Time) structures
			*/
			timer = timer_start();
			if (postings_list != NULL)
				{
				/*
					Copy this term's pointes to the quantum list
				*/
				memcpy(current_quantum, postings_list->methods, postings_list->impacts * sizeof(*quantum_order));

				/*
					Compute the maximum possibe impact score (that is, assume one document has the maximum impact of each term)
				*/
				max_remaining_impact += (*current_quantum)->impact;

				/*
					Advance to the place we want to place the next quantum set
				*/
				current_quantum += postings_list->impacts;
				}
			}
		/*
			NULL termainate the list of quantums
		*/
		*current_quantum = NULL;

		/*
			Sort the quantum list from highest to lowest
		*/
		qsort(quantum_order, current_quantum - quantum_order, sizeof(*quantum_order), quantum_compare);

		stats_quantum_prep_time += timer_stop(timer);

		/*
			Now process each quantum, one at a time
		*/
		for (current_quantum = quantum_order; *current_quantum != NULL; current_quantum++)
			{
			stats_quantum_count++;
			timer = timer_start();
			(*(*current_quantum)->method)(&globals);
			stats_postings_time += timer_stop(timer);

			/*
				Check to see if its posible for the remaining impacts to affect the order of the top-k
			*/
			timer = timer_start();
			/*
				Subtract the current impact score and then add the next impact score for the current term
			*/
			max_remaining_impact -= (*current_quantum)->impact;
			max_remaining_impact += ((*current_quantum) + 1)->impact;

			if (globals.CI_results_list_length > globals.CI_top_k - 1)
				{
				stats_quantum_check_count++;
				/*
					We need to run through the top-(k+1) to see if its possible for any re-ordering to occur
					1. copy the accumulators;
					2. sort the top k + 1
					3. go through consequative rsvs checking to see if reordering is possible (check rsv[k] - rsv[k + 1])
				*/
				memcpy(quantum_check_pointers, globals.CI_accumulator_pointers, globals.CI_top_k * sizeof(*quantum_check_pointers));
				top_k_qsort(quantum_check_pointers, globals.CI_top_k, globals.CI_top_k);

				early_terminate = true;

				for (partial_rsv = quantum_check_pointers; partial_rsv < quantum_check_pointers + globals.CI_top_k - 1; partial_rsv++)
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
		top_k_qsort(globals.CI_accumulator_pointers, globals.CI_results_list_length, globals.CI_top_k - 1);
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
		trec_dump_results(query_id, out, globals.CI_top_k - 1);		// subtract 1 from top_k because we added 1 for the early termination checks
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

