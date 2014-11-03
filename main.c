/*
	MAIN.C
	------
*/
#include <stdio.h>
#include <stdlib.h>
#include "CI.h"

/*
	TREC_DUMP_RESULTS()
	-------------------
*/
void trec_dump_results(uint32_t topic_id, uint16_t *accumulator, uint32_t accumulators_length)
{
uint32_t current;

for (current = 0; current < accumulators_length; current++)
	if (accumulator[current] != 0)
		printf("%d Q0 %d <rank> %d COMPILED\n", topic_id, current, accumulator[current]);
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

if (argc != 2)
	exit(printf("Usage:%s <queryfile>\n", argv[0]));

if ((fp = fopen(argv[1], "r")) == NULL)
	exit(printf("Can't open query file:%s\n", argv[1]));

accumulators = new uint16_t[CI_unique_documents];

while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
	if ((id = strtok(buffer, SEPERATORS)) == NULL)
		continue;
	/*
		get the TREC query_id
	*/
	query_id = atoll(id);
	printf("ID:%llu\n", query_id);

	/*
		Initialise the accumulators
	*/
	memset(accumulators, 0, CI_unique_documents * sizeof(*accumulators));

	/*
		For each term, call the method to update the accumulators
	*/
	while ((term = strtok(NULL, SEPERATORS)) != NULL)
		if ((postings_list = (CI_vocab *)bsearch(term, CI_dictionary, CI_unique_terms, sizeof(*CI_dictionary), CI_vocab::compare_string)) != NULL)
			{
			printf("found    :'%s'\n", term);
			postings_list->method(accumulators);
			}
		else
			printf("not found:'%s'\n", term);
	/*
		Creat a TREC run file as output
	*/
	trec_dump_results(query_id, accumulators, CI_unique_documents);
	}

return 0;
}