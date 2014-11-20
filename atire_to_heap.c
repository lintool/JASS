/*
	ATIRE_TO_HEAP.C
	---------------
	atire dump format:
	term cf df <docid,impact>...<docid,impact>
*/
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#include "compress_variable_byte.h"

#ifdef _MSC_VER
	#include <direct.h>

	#define atoll(x) _atoi64(x)
	#define mkdir(x,y) _mkdir(x)
#endif

using namespace std;

static char buffer[1024 * 1024 * 1024];

/*
	struct CI_HEAP_QUANTUM_INDEXER
	------------------------------
*/
struct CI_heap_quantum
{
uint16_t impact;			// the quantum impact score
uint8_t *offset;			// where the data is
uint64_t length;			// length of the compressed postings list (in bytes)
} ;

static CI_heap_quantum postings_offsets[0x100];
uint32_t current_quantum;

char *termlist[1024 * 1024];
uint32_t termlist_length = 0;

ostringstream *vocab_in_current_file = NULL;

/*
	STRING_COMPARE()
	----------------
*/
int string_compare(const void *a, const void *b)
{
char **one = (char **)a;
char **two = (char **)b;

return strcmp(*one, *two);
}

/*
	LOAD_TOPIC_FILE()
	-----------------
*/
void load_topic_file(char *filename)
{
static const char *SEPERATORS = " \t\n\r";
FILE *fp;
char *term;

if ((fp = fopen(filename, "rb")) == NULL)
	exit(printf("Cannot open ATIRE topic file '%s'\n", filename));

while (fgets(buffer, sizeof(buffer), fp) != NULL)
	if ((term = strtok(buffer, SEPERATORS)) != NULL)			// discard the first token as its the topic ID
		for (term = strtok(NULL, SEPERATORS); term != NULL; term = strtok(NULL, SEPERATORS))
			termlist[termlist_length++] = strdup(term);

qsort(termlist, termlist_length, sizeof(*termlist), string_compare);
}

/*
	USAGE()
	-------
*/
uint8_t usage(char *filename)
{
printf("Usage: %s <index.dump> <docid.aspt> [<topicfile>] [-s|-S]", filename);
puts("Generate index.dump with atire_dictionary > index.dump");
puts("Generatedocid.aspt with atire_doclist");
puts("Generate <topicfile> with trec2query <trectopicfile>");

return 1;
}

#define MAX_DOCIDS_PER_IMPACT (1024*1024)

uint32_t remember_buffer[MAX_DOCIDS_PER_IMPACT];
uint32_t *remember_into = remember_buffer;
ANT_compress_variable_byte compressor;
uint8_t remember_compressed[MAX_DOCIDS_PER_IMPACT * sizeof(uint32_t)];

/*
	REMEMBER()
	----------
*/
void remember(uint32_t docid)
{
*remember_into++ = docid;
}

/*
	REMEMBER_COMPRESS()
	-------------------
*/
uint8_t *remember_compress(uint32_t *length)
{
uint32_t is, was;

/*
	Compute deltas
*/
was = 0;
for (uint32_t *current = remember_buffer; current < remember_into; current++)
	{
	is = *current;
	*current -= was;
	was = is;
	}


/*
	Now compress
*/
if ((*length = compressor.compress(remember_compressed, sizeof(remember_compressed), remember_buffer, remember_into - remember_buffer)) <= 0)
	exit(printf("Can't compress\n"));

/*
	rewind the buffer
*/
remember_into = remember_buffer;

return remember_compressed;
}

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
uint32_t docids_in_impact;
char *end_of_term, *buffer_address = (char *)buffer;
uint64_t line = 0;
uint64_t cf, df, docid, impact, first_time = true, max_docid = 0, max_q = 0;
FILE *fp, *vocab_dot_c, *postings_dot_bin, *doclist, *doclist_dot_c;
uint64_t postings_file_number = 0;
uint64_t previous_impact, which_impact, unique_terms_in_index = 0, end;
uint32_t data_length_in_bytes, quantum;
uint8_t *data;


if (argc <3 || argc > 5)
	exit(usage(argv[0]));

if (argc == 4)
	load_topic_file(argv[3]);

if ((fp = fopen(argv[1], "rb")) == NULL)
	exit(printf("Cannot open input file '%s'\n", argv[1]));

if ((doclist = fopen(argv[2], "rb")) == NULL)
	exit(printf("Cannot open input file '%s'\n", argv[2]));

/*
	do the doclist first as its fastest
*/
if ((doclist_dot_c = fopen("CIdoclist.c", "wb")) == NULL)
	exit(printf("Cannot open CIdoclist.c output file"));

fprintf(doclist_dot_c, "const char *CI_doclist[] =\n{\n");

first_time = true;
while (fgets(buffer, sizeof(buffer), doclist) != NULL)
	{
	if (first_time)
		{
		fprintf(doclist_dot_c, "\"%s\"", strtok(buffer, "\r\n"));
		first_time = false;
		}
	else
		fprintf(doclist_dot_c, ",\n\"%s\"", strtok(buffer, "\r\n"));
	}

fprintf(doclist_dot_c, "\n};\n");

/*
	Now do the postings lists
*/
if ((vocab_dot_c = fopen("CIvocab_heap.c", "wb")) == NULL)
	exit(printf("Cannot open CIvocab_heap.c output file\n"));

fprintf(vocab_dot_c, "#include <stdint.h>\n");
fprintf(vocab_dot_c, "#include \"CI.h\"\n");
fprintf(vocab_dot_c, "class CI_vocab_heap CI_dictionary[] = {\n");

if ((postings_dot_bin = fopen("CIpostings.bin", "wb")) == NULL)
	exit(printf("Cannot open CIpostings.h output file\n"));

while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
	line++;
	if (buffer[strlen(buffer) - 1] != '\n')
		exit(printf("line %lld: no line ending in the first %lu bytes, something is wrong, exiting", line, sizeof(buffer)));
	if (*buffer == '~')
		continue;

	if ((end_of_term = strchr(buffer, ' ')) != NULL)
		{
		*end_of_term++ = '\0';
		cf = atoll(end_of_term);
		if ((end_of_term = strchr(end_of_term, ' ')) != NULL)
			{
			df = atoll(end_of_term);

			if ((termlist_length == 0) || (bsearch(&buffer_address, termlist, termlist_length, sizeof(*termlist), string_compare) != NULL))										// only add to the output file if the term is in the term list (or we have no term list)
				{
				unique_terms_in_index++;
				current_quantum = 0;
				uint64_t sum_of_lengths = 0;
				docids_in_impact = 0;
				previous_impact = ULONG_MAX;
				ostringstream term_method_list;

				end_of_term = strchr(end_of_term + 1, '<');
				while (end_of_term != NULL)
					{
					if ((end_of_term = strchr(end_of_term, '<')) != NULL)
						{
						docid = atoll(end_of_term + 1);
						if  (docid > max_docid)
							max_docid = docid;

						end_of_term = strchr(end_of_term + 1, ',');

						impact = atoll(end_of_term + 1);
						if (impact > max_q)
							max_q = impact;

						if (impact != previous_impact)
							{
							if (previous_impact != ULONG_MAX)
								{
								data = remember_compress(&data_length_in_bytes);
								postings_offsets[current_quantum].impact = previous_impact;
								postings_offsets[current_quantum].offset = new uint8_t [data_length_in_bytes];
								memcpy(postings_offsets[current_quantum].offset, data, data_length_in_bytes);
								postings_offsets[current_quantum].length = data_length_in_bytes;
								sum_of_lengths += data_length_in_bytes;
								current_quantum++;
								}
							previous_impact = impact;
							}
						remember(docid);
						docids_in_impact++;
						}
					}
				/*
					Don't forget the final quantum
				*/
				data = remember_compress(&data_length_in_bytes);
				postings_offsets[current_quantum].impact = impact;
				postings_offsets[current_quantum].offset = new uint8_t [data_length_in_bytes];
				memcpy(postings_offsets[current_quantum].offset, data, data_length_in_bytes);
				postings_offsets[current_quantum].length = data_length_in_bytes;
				sum_of_lengths += data_length_in_bytes;
				current_quantum++;

				/*
					Write out to disk
				*/
				/*
					Tell the vocab where we are
				*/
				uint64_t postings_list = ftell(postings_dot_bin);
				fprintf(vocab_dot_c, "{\"%s\",%llu, %u},\n", buffer, postings_list, current_quantum);

				/*
					Write a pointer to each quantum header
				*/
				uint32_t header_size = sizeof(postings_offsets->impact) + sizeof(uint64_t) + sizeof(postings_offsets->length);
				uint64_t quantum_pointer = sizeof(quantum_pointer) * current_quantum;		// start at the end of the list of pointers

				quantum_pointer += postings_list;			// offsets are relative to the start of the file
				for (quantum = 0; quantum < current_quantum; quantum++)
					{
					fwrite(&quantum_pointer, sizeof(quantum_pointer), 1, postings_dot_bin);
					quantum_pointer += header_size;										// the next one is this many bytes further on
					}
				/*
					Now write out the quantim headers
				*/
				uint64_t offset = header_size  * (current_quantum + 1) + sizeof(quantum_pointer) * current_quantum;		// start the data at the end of the quantum headers (which includes a zero termnator);
				offset += postings_list;					// offset from the start of the file
				for (quantum = 0; quantum < current_quantum; quantum++)
					{
					fwrite(&postings_offsets[quantum].impact, sizeof(postings_offsets[quantum].impact), 1, postings_dot_bin);
					fwrite(&offset, sizeof(offset), 1, postings_dot_bin);

					end = offset + postings_offsets[quantum].length;
					fwrite(&end, sizeof(end), 1, postings_dot_bin);

					offset += postings_offsets[quantum].length;
					}
					
				/*
					Terminate the quantum header list with a bunch of zeros
				*/
				uint8_t zero[] = {0,0,  0,0,0,0,0,0,0,0,   0,0,0,0,0,0,0,0};
				fwrite(&zero, sizeof(zero), 1, postings_dot_bin);
				
				/*
					Now write the compressed postings lists
				*/
				for (quantum = 0; quantum < current_quantum; quantum++)
					{
					fwrite(postings_offsets[quantum].offset, postings_offsets[quantum].length, 1, postings_dot_bin);
					delete [] postings_offsets[quantum].offset;
					}
				}
			}
		}
	}

fprintf(vocab_dot_c, "};\n\n");
fprintf(vocab_dot_c, "uint32_t CI_unique_terms = %llu;\n", unique_terms_in_index);
fprintf(vocab_dot_c, "uint32_t CI_unique_documents = %llu;\n", max_docid + 1);			// +1 because we count from zero
fprintf(vocab_dot_c, "uint32_t CI_max_q = %llu;\n", max_q);

fclose(vocab_dot_c);
fclose(fp);
fclose(postings_dot_bin);

return 0;
}
