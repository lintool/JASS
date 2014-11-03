/*
	MAIN.C
	------
*/
#include <stdio.h>
#include <stdlib.h>
#include <CI.h>

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
const char *SEPERATORS = " \t";
FILE *fp;
char *term, *id;
uint64_t query_id;
uint16_t accumulators;

if (argc != 2)
	exit(printf("Usage:%s <queryfile>\n", argv[0]));

if ((fp = fopen(argv[1], "w")) == NULL)
	exit(printf("Can't open query file:%s\n", argv[1]));

while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
	if ((id = strtok(buffer, SEPERATORS)) == NULL)
		continue;

	/*
		get the TREC query_id
	*/
	query_id = atoll(id);

	/*
		Initialise the accumulators
	*/
	memset(accumulators, 0, CI_unique_documents * sizeof(*accumulators));

	/*
		For each term, call the method to update the accumulators
	*/
	while (term = strtok(NULL, SEPERATORS)) == NULL)
		if ((postings_list = bsearch(term, dictionary, CI_unique_terms, sizeof(*dictionary), CI_vocab::compare_string)) != NULL)
			postings_list->method(accumulators);
	}

return 0;
}