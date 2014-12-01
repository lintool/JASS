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
#include "compress_simple8b.h"
#include "compress_qmx.h"

#ifdef _MSC_VER
	#include <direct.h>

	#define atoll(x) _atoi64(x)
	#define mkdir(x,y) _mkdir(x)
#endif

using namespace std;

#define BUFFER_SIZE (1024*1024*1024)
static char *buffer;

/*
	struct CI_HEAP_QUANTUM_INDEXER
	------------------------------
*/
struct CI_heap_quantum
{
uint16_t impact;						// the quantum impact score
uint8_t *offset;						// where the data is
uint64_t length;						// length of the compressed postings list (in bytes)
uint64_t length_with_padding;		// length of the compressed postings list (in bytes) (plus any necessary SSE padding)
uint32_t quantum_frequency;		// number of integers in the quantum (needed by QMX and ant ATIRE based decompressors)
} ;

static CI_heap_quantum postings_offsets[0x100];
uint32_t current_quantum;

char *termlist[1024 * 10];
uint32_t termlist_length = 0;

ostringstream *vocab_in_current_file = NULL;

uint8_t file_mode;					// which compressor we're using

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

while (fgets(buffer, BUFFER_SIZE, fp) != NULL)
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
printf("Usage: %s <index.dump> <docid.aspt> [<topicfile>] [-c|-s]", filename);
puts("Generate index.dump with atire_dictionary > index.dump");
puts("Generatedocid.aspt with atire_doclist");
puts("Generate <topicfile> with trec2query <trectopicfile>");
puts("-8 compress the postings using simple 8b");
puts("-c compress the postings using Variable Byte Encoding (default)");
puts("-s 'static' do not compress the postings");
puts("-q compress the postings using QMX");

return 1;
}

#define MAX_DOCIDS_PER_IMPACT (1024 * 1024 * 5)

uint32_t remember_should_compress = true;
uint32_t remember_buffer[MAX_DOCIDS_PER_IMPACT];
uint32_t *remember_into = remember_buffer;
ANT_compress *compressor;
uint8_t remember_compressed[MAX_DOCIDS_PER_IMPACT * sizeof(uint32_t)];
uint32_t sse_alignment = 0;

/*
	REMEMBER()
	----------
*/
void remember(uint32_t docid)
{
*remember_into++ = docid;

if (remember_into > remember_buffer + MAX_DOCIDS_PER_IMPACT)
	exit(printf("Exceeded the maximum number of postings allowed per quantum... make the constant large"));
}

/*
	REMEMBER_COMPRESS()
	-------------------
*/
uint8_t *remember_compress(uint32_t *length, uint32_t *padded_length, uint32_t *integers_in_quantum)
{
uint32_t is, was, compressed_size;

*integers_in_quantum = remember_into - remember_buffer;

if (!remember_should_compress)
	memcpy(remember_compressed, remember_buffer, compressed_size = (sizeof(*remember_buffer) * (remember_into - remember_buffer)));
else
	{
	/*
		Compute deltas.  We have two algorithms - the first is the "usual" compute consequiteive deltas (so called "D1".
		The second is "D4", where there are 4 "D1" threads running in parallel.  i.e. (d5, d6, d7, d8) = (x5, x6, x7, x8) -  (x1, x2, x3, x4), etc.
		see: Daniel Lemire, Leonid Boytsov, Nathan Kurz, SIMD Compression and the Intersection of Sorted Integers, arXiv: 1401.6399, 2014 http://arxiv.org/abs/1401.6399
	*/
	if (file_mode == 'q')
		{
		/*
			Use D4
		*/
		for (uint32_t d_start = 0; d_start < 4; d_start++)
			{
			was = 0;
			for (uint32_t *current = remember_buffer + d_start; current < remember_into; current += 4)
				{
				is = *current;
				*current -= was;
				was = is;
				}
			}
		}
	else
		{
		/*
			Use D1
		*/
		was = 0;
		for (uint32_t *current = remember_buffer; current < remember_into; current++)
			{
			is = *current;
			*current -= was;
			was = is;
			}
		}

	/*
		Now compress
	*/
	compressed_size = compressor->compress(remember_compressed, sizeof(remember_compressed), remember_buffer, remember_into - remember_buffer);
	
	if (compressed_size <= 0)
		exit(printf("Can't compress\n"));
	}
	
/*
	If we need to SSE-word align then do so
*/
*length = compressed_size;
if (sse_alignment != 0)
	compressed_size = ((compressed_size + sse_alignment - 1) / sse_alignment) * sse_alignment;
*padded_length = compressed_size;

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
char *end_of_term, *buffer_address;
uint64_t line = 0;
uint64_t cf, df, docid, impact, first_time = true, max_docid = 0, max_q = 0;
FILE *fp, *vocab_dot_c, *postings_dot_bin, *doclist, *doclist_dot_c;
uint64_t postings_file_number = 0;
uint64_t previous_impact, which_impact, unique_terms_in_index = 0, end;
uint32_t data_length_in_bytes, data_length_in_bytes_with_padding, quantum, parameter;
uint8_t *data;

buffer = new char [1024 * 1024 * 1024];
buffer_address = buffer;

if (argc <3 || argc > 5)
	exit(usage(argv[0]));

/*
	Default is Variable Byte
*/
file_mode = 'c';
remember_should_compress = true;
compressor = new ANT_compress_variable_byte;

/*
	Check the parametes to see if are anything else
*/
for (parameter = 3; parameter < argc; parameter++)
	if (strcmp(argv[parameter], "-s") == 0)
		{
		file_mode = 's';
		remember_should_compress = false;
		}
	else if (strcmp(argv[parameter], "-8") == 0)
		{
		file_mode = '8';
		remember_should_compress = true;
		compressor = new ANT_compress_simple8b;
		}
	else if (strcmp(argv[parameter], "-q") == 0)
		{
		file_mode = 'q';
		remember_should_compress = true;
		compressor = new ANT_compress_qmx;
		sse_alignment = 16;
		}
	else if (strcmp(argv[parameter], "-c") == 0)
		{
		file_mode = 'c';
		remember_should_compress = true;
		compressor = new ANT_compress_variable_byte;
		}
	else if (strcmp(argv[parameter], "-SSE") == 0)
		sse_alignment = 16;
	else
		load_topic_file(argv[parameter]);

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
while (fgets(buffer, BUFFER_SIZE, doclist) != NULL)
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
fclose(doclist_dot_c);

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

/*
	Tell the postings list which compression strategy is being used
*/
fwrite(&file_mode, 1, 1, postings_dot_bin);

/*
	Now generate the file
*/
while (fgets(buffer, BUFFER_SIZE, fp) != NULL)
	{
	line++;
	if (buffer[strlen(buffer) - 1] != '\n')
		exit(printf("line %lld: no line ending in the first %d bytes, something is wrong, exiting", line, BUFFER_SIZE));
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
								data = remember_compress(&data_length_in_bytes, &data_length_in_bytes_with_padding, &postings_offsets[current_quantum].quantum_frequency);
								postings_offsets[current_quantum].impact = previous_impact;
								postings_offsets[current_quantum].offset = new uint8_t [data_length_in_bytes];
								memcpy(postings_offsets[current_quantum].offset, data, data_length_in_bytes);
								postings_offsets[current_quantum].length = data_length_in_bytes;
								postings_offsets[current_quantum].length_with_padding = data_length_in_bytes_with_padding;
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
				data = remember_compress(&data_length_in_bytes, &data_length_in_bytes_with_padding, &postings_offsets[current_quantum].quantum_frequency);
				postings_offsets[current_quantum].impact = impact;
				postings_offsets[current_quantum].offset = new uint8_t [data_length_in_bytes];
				memcpy(postings_offsets[current_quantum].offset, data, data_length_in_bytes);
				postings_offsets[current_quantum].length = data_length_in_bytes;
				postings_offsets[current_quantum].length_with_padding = data_length_in_bytes_with_padding;
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
				uint32_t header_size = sizeof(postings_offsets->impact) + sizeof(uint64_t) + sizeof(postings_offsets->length) + sizeof(postings_offsets->quantum_frequency);
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
				uint64_t sse_offset, sse_padding;
				uint64_t offset = header_size  * (current_quantum + 1) + sizeof(quantum_pointer) * current_quantum;		// start the data at the end of the quantum headers (which includes a zero termnator);
				offset += postings_list;					// offset from the start of the file

				if (sse_alignment != 0)
					{
					sse_offset = ((offset + sse_alignment - 1) / sse_alignment) * sse_alignment;
					sse_padding = sse_offset - offset;
					offset = sse_offset;
					}

				for (quantum = 0; quantum < current_quantum; quantum++)
					{
					fwrite(&postings_offsets[quantum].impact, sizeof(postings_offsets[quantum].impact), 1, postings_dot_bin);
					fwrite(&offset, sizeof(offset), 1, postings_dot_bin);

					end = offset + postings_offsets[quantum].length;
					fwrite(&end, sizeof(end), 1, postings_dot_bin);

					fwrite(&postings_offsets[quantum].quantum_frequency, sizeof(postings_offsets[quantum].quantum_frequency), 1, postings_dot_bin);

					offset += postings_offsets[quantum].length_with_padding;
					}
					
				/*
					Terminate the quantum header list with a bunch of zeros
				*/
				uint8_t zero[] = {0,0,  0,0,0,0,0,0,0,0,   0,0,0,0,0,0,0,0,   0,0,0,0};
				fwrite(&zero, header_size, 1, postings_dot_bin);

				if (sse_alignment != 0 && sse_padding != 0)
					{
					if (sse_padding > sizeof(zero))
						exit(printf("Padding is too large, fix the source code (sorry)\n"));
					fwrite(zero, sse_padding, 1, postings_dot_bin);
					}

				/*
					Now write the compressed postings lists
				*/
				for (quantum = 0; quantum < current_quantum; quantum++)
					{
					fwrite(postings_offsets[quantum].offset, postings_offsets[quantum].length_with_padding, 1, postings_dot_bin);
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

delete [] buffer;

return 0;
}
