/*
	ANT_DICTIONARY.C
	----------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "maths.h"
#include "memory.h"
#include "search_engine.h"
#include "btree_iterator.h"
#include "search_engine_btree_leaf.h"
#include "impact_header.h"

#ifndef IMPACT_HEADER
	#error "set IMPACT_HEADER in the ATIRE makefile and start all over again"
#endif
#ifndef SPECIAL_COMPRESSION
	#error "set SPECIAL_COMPRESSION in the ATIRE makefile and start all over again"
#endif

/*
	PROCESS()
	---------
*/
long long process(ANT_compression_factory *factory, uint32_t quantum_count, ANT_compressable_integer *impact_header, ANT_compressable_integer *buffer, unsigned char *postings_list, long long trim_point, long verbose, long one_postings_per_line)
{
long long docid, max_docid, sum;
ANT_compressable_integer *current, *end;
ANT_compressable_integer *impact_value_ptr, *doc_count_ptr, *impact_offset_ptr;
ANT_compressable_integer *impact_offset_start;

max_docid = sum = 0;
impact_value_ptr = impact_header;
doc_count_ptr = impact_header + quantum_count;
impact_offset_start = impact_offset_ptr = impact_header + quantum_count * 2;

while (doc_count_ptr < impact_offset_start)
	{
	factory->decompress(buffer, postings_list + *impact_offset_ptr, *doc_count_ptr);

	docid = -1;
	current = buffer;
	end = buffer + *doc_count_ptr;
	while (current < end)
		{
		docid += *current++;
		if (one_postings_per_line)
			printf("\n<%lld,%lld>", docid, (long long)*impact_value_ptr);
		else
			printf("<%lld,%lld>", docid, (long long)*impact_value_ptr);

		if (docid > max_docid)
			max_docid = docid;
		}

	sum += *doc_count_ptr;
	if (sum >= trim_point)
		break;

	impact_value_ptr++;
	impact_offset_ptr++;
	doc_count_ptr++;
	}

return max_docid;
}
/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
ANT_compressable_integer *raw;
long long max = 0;
long long postings_list_size;
long long raw_list_size;
unsigned char *postings_list = NULL;
long param, filename = 0;
char *term;
ANT_search_engine_btree_leaf leaf;
ANT_compression_factory factory;
long metaphone, print_wide, print_postings, one_postings_per_line;
ANT_memory memory;
ANT_search_engine search_engine(&memory);

print_postings = print_wide = metaphone = one_postings_per_line = false;

search_engine.open("index.aspt");

ANT_btree_iterator iterator(&search_engine);

quantum_count_type the_quantum_count;
beginning_of_the_postings_type beginning_of_the_postings;
long long impact_header_info_size = ANT_impact_header::INFO_SIZE;
long long impact_header_size = ANT_impact_header::NUM_OF_QUANTUMS * sizeof(ANT_compressable_integer) * 3;
ANT_compressable_integer *impact_header_buffer = (ANT_compressable_integer *)malloc(impact_header_size);

postings_list_size = search_engine.get_postings_buffer_length();
raw_list_size = sizeof(*raw) * (search_engine.document_count() + ANT_COMPRESSION_FACTORY_END_PADDING);

postings_list = (unsigned char *)malloc((size_t)postings_list_size);
raw = (ANT_compressable_integer *)malloc((size_t)raw_list_size);

for (term = iterator.first(NULL); term != NULL; term = iterator.next())
	{
	iterator.get_postings_details(&leaf);
	if (*term == '~')
		break;
	else
		{
		printf("%s ", term);
		if (leaf.local_document_frequency < 3)
			printf("%lld %lld 0", leaf.local_collection_frequency, leaf.local_document_frequency);
		else
			printf("%lld %lld %lld", leaf.local_collection_frequency, leaf.local_document_frequency, leaf.postings_length);

		postings_list = search_engine.get_postings(&leaf, postings_list);

		// decompress the header
		the_quantum_count = ANT_impact_header::get_quantum_count(postings_list);
		beginning_of_the_postings = ANT_impact_header::get_beginning_of_the_postings(postings_list);
		factory.decompress(impact_header_buffer, postings_list + impact_header_info_size, the_quantum_count * 3);

		// print the postings
		max = process(&factory, the_quantum_count, impact_header_buffer, raw, postings_list + beginning_of_the_postings, leaf.local_document_frequency, print_postings, one_postings_per_line);

		putchar('\n');
		}
	}

return 0;
}
