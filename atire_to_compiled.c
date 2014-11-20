/*
	ATIRE_TO_COMPILED.C
	-------------------
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

uint32_t seperate_files = false;									// set this to false to get all the postings into a single file
static char buffer[1024 * 1024 * 1024];

char *termlist[1024 * 1024];
uint32_t termlist_length = 0;

#define TERMS_PER_SOURCE_CODE_FILE 1000					/* when compiling everything (seperate_files = false) put this numnber of terms in each source code file */

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
	OPEN_POSTINGS_DOT_C()
	---------------------
*/
FILE *open_postings_dot_c(char *term)
{
FILE *postings_dot_c;
char buffer[1024];

sprintf(buffer, "CIpostings/CIt_%s.c", term);
if ((postings_dot_c = fopen(buffer, "wb")) == NULL)
	exit(printf("Cannot open '%s' output file\n", buffer));

fprintf(postings_dot_c, "#include <stdint.h>\n");

fprintf(postings_dot_c, "#include \"../CI.h\"\n\n");

delete vocab_in_current_file;
vocab_in_current_file = new ostringstream;
*vocab_in_current_file << "\nCI_vocab CI_dictionary_" << term << "[] = \n{ \n";

return postings_dot_c;
}

/*
	CLOSE_POSTINGS_DOT_C()
	----------------------
*/
void close_postings_dot_c(FILE *fp)
{
fprintf(fp, "%s{0,0}\n};\n\n", vocab_in_current_file->str().c_str());

fclose(fp);
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
puts("-s to turn the postings into uncompressed static data rather than code");
puts("-c to turn the postings into compressed static data");

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
	exit(printf("Can't compress"));

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
uint32_t parameter, static_data, static_data_compressed, docids_in_impact;
char *end_of_term, *buffer_address = (char *)buffer;
uint64_t line = 0;
uint64_t cf, df, docid, impact, first_time = true, max_docid = 0, max_q = 0;
FILE *fp, *vocab_dot_c, *postings_dot_c, *postings_dot_h, *doclist, *doclist_dot_c, *makefile, *makefile_include;
uint32_t include_postings;
uint64_t postings_file_number = 0;
uint64_t previous_impact, impacts_for_this_term, which_impact, unique_terms_in_index = 0;

if (argc <3 || argc > 5)
	exit(usage(argv[0]));

seperate_files = false;
static_data = static_data_compressed = false;

if (argc >= 4)
	{
	for (parameter = 3; parameter < argc; parameter++)
		if (strcmp(argv[parameter], "-s") == 0)
			static_data = true;
		else if (strcmp(argv[parameter], "-c") == 0)
			static_data_compressed = true;
		else
			{
			/*
				If the user gives me a <topicfile> then put each term into a seperate file.
			*/
			seperate_files = true;
			load_topic_file(argv[parameter]);
			}
	}

if (static_data && static_data_compressed)
	exit(printf("Can't have both the uncompressed and compressed static data at the same time"));

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
if ((vocab_dot_c = fopen("CIvocab.c", "wb")) == NULL)
	exit(printf("Cannot open CIvocab.c output file\n"));

fprintf(vocab_dot_c, "#include <stdint.h>\n");
fprintf(vocab_dot_c, "#include \"CI.h\"\n");
fprintf(vocab_dot_c, "#include \"CIpostings.h\"\n\n");

postings_dot_c = NULL;
mkdir("CIpostings", S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
if ((makefile = fopen("CIpostings/makefile", "wb")) == NULL)
	exit(printf("Cannot open 'CIpostings/makefile' output file\n"));
#ifdef _MSC_VER
	fprintf(makefile, "include makefile.include\n\nCI_FLAGS = -c /Ot /Tp\n\n");
#else
	fprintf(makefile, "include makefile.include\n\n");
	fprintf(makefile, "CI_FLAGS = -dynamiclib -undefined dynamic_lookup -x c++");
	if (static_data_compressed || static_data)
		fprintf(makefile, " -O3 -DCI_FORCEINLINE");
	fprintf(makefile, "\n\n");
#endif

if ((makefile_include = fopen("CIpostings/makefile.include", "wb")) == NULL)
	exit(printf("Cannot open 'CIpostings/makefile.include' output file\n"));
#ifdef _MSC_VER
	fprintf(makefile_include, "../CIpostings.obj : ");
#else
	fprintf(makefile_include, "../CIpostings.o : ");
#endif

if ((postings_dot_h = fopen("CIpostings.h", "wb")) == NULL)
	exit(printf("Cannot open CIpostings.h output file\n"));
fprintf(postings_dot_h, "#include <stdint.h>\n\n");

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
				include_postings = true;
			else
				include_postings = false;

			impacts_for_this_term = 0;
			if (include_postings)
				{
				if ((end_of_term = strchr(end_of_term + 1, '<')) != NULL)
					{
					if (seperate_files)
						{
						postings_dot_c = open_postings_dot_c(buffer);
						#ifdef _MSC_VER
							fprintf(makefile, "CIt_%s.obj : CIt_%s.c\n\t $(CXX) $(CXXFLAGS) $(CI_FLAGS) CIt_%s.c\n\n", buffer, buffer, buffer);
							fprintf(makefile_include, " CIt_%s.obj", buffer);
						#else
							fprintf(makefile, "CIt_%s.dylib : CIt_%s.c\n\t $(CXX) $(CXXFLAGS) -o CIt_%s.dylib $(CI_FLAGS) CIt_%s.c\n\n", buffer, buffer, buffer, buffer);
							fprintf(makefile_include, " CIt_%s.dylib", buffer);
						#endif
						}
					else if (((line - 1) % TERMS_PER_SOURCE_CODE_FILE) == 0)
						{
						char filename[1024];

						sprintf(filename, "%llu", postings_file_number);
						if (postings_dot_c != NULL)
							close_postings_dot_c(postings_dot_c);
						postings_dot_c = open_postings_dot_c(filename);

						#ifdef _MSC_VER
							fprintf(makefile, "CIt_%llu.obj : CIt_%llu.c\n\t $(CXX) $(CXXFLAGS) $(CI_FLAGS)  CIt_%llu.c\n\n", postings_file_number, postings_file_number, postings_file_number);
							fprintf(makefile_include, " CIt_%llu.obj", postings_file_number);
						#else
							fprintf(makefile, "CIt_%llu.dylib : CIt_%llu.c\n\t $(CXX) $(CXXFLAGS) -o CIt_%llu.dylib $(CI_FLAGS) CIt_%llu.c\n\n", postings_file_number, postings_file_number, postings_file_number, postings_file_number);
							fprintf(makefile_include, " CIt_%llu.dylib", postings_file_number);
						#endif
						postings_file_number++;
						}
					docids_in_impact = 0;
					previous_impact = ULONG_MAX;
					ostringstream term_method_list;

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
								if (previous_impact == ULONG_MAX)
									{
									fprintf(postings_dot_h, "extern void *CIt_ip_%s[];\n", buffer);
									term_method_list << "static struct CI_impact_method CIt_i_" << buffer << "[] =\n{\n";
									}
								else
									{
									if (static_data_compressed)
										{
										uint8_t *data, *byte;
										uint32_t data_length_in_bytes;

										fprintf(postings_dot_c, "uint32_t doc, sum;\nstatic unsigned char doclist[] = {\n");
										data = remember_compress(&data_length_in_bytes);
										for (byte = data; byte < data + data_length_in_bytes; byte++)
											{
											fprintf(postings_dot_c, "0x%02X, ", *byte);
											if (*byte & 0x80)
												fprintf(postings_dot_c, "\n");
											}

										fprintf(postings_dot_c, "};\n");
										fprintf(postings_dot_c, "sum = 0;\n");
										fprintf(postings_dot_c, "for (uint8_t *i = doclist; i < doclist + %u;)\n", data_length_in_bytes);
										fprintf(postings_dot_c, "	{\n");
										fprintf(postings_dot_c, "	if (*i & 0x80)\n");
										fprintf(postings_dot_c, "		doc = *i++ & 0x7F;\n");
										fprintf(postings_dot_c, "	else\n");
										fprintf(postings_dot_c, "		{\n");
										fprintf(postings_dot_c, "		doc = *i++;\n");
										fprintf(postings_dot_c, "		while (!(*i & 0x80))\n");
										fprintf(postings_dot_c, "		   doc = (doc << 7) | *i++;\n");
										fprintf(postings_dot_c, "		doc = (doc << 7) | (*i++ & 0x7F);\n");
										fprintf(postings_dot_c, "		}\n");
										fprintf(postings_dot_c, "	sum += doc;\n");
										fprintf(postings_dot_c, "	add_rsv(sum, %llu);\n", previous_impact);
										fprintf(postings_dot_c, "	}\n");
										}
									else if (static_data)
										{
										fprintf(postings_dot_c, "};\n");
										fprintf(postings_dot_c, "for (uint32_t *i = doclist; i < doclist + %u; i++)\n", docids_in_impact);
										fprintf(postings_dot_c, "\tadd_rsv(*i, %llu);\n", previous_impact);
										}

									fprintf(postings_dot_c, "}\n\n");
									docids_in_impact = 0;
									}

								fprintf(postings_dot_c, "static void CIt_%s_i_%llu(void)\n{\n", buffer, impact);
								if (static_data)
									fprintf(postings_dot_c, "static uint32_t doclist[] = {\n");

								term_method_list << "{" << impact << ", CIt_" << buffer << "_i_" << impact << "},\n";
								previous_impact = impact;
								impacts_for_this_term++;
								}
							if (static_data)
								fprintf(postings_dot_c, "%llu,\n", docid);
							else if (static_data_compressed)
								remember(docid);
							else
								fprintf(postings_dot_c, "add_rsv(%llu, %llu);\n", docid, impact);
							docids_in_impact++;
							}
						}

					if (static_data_compressed)
						{
						uint8_t *data, *byte;
						uint32_t data_length_in_bytes;

						fprintf(postings_dot_c, "uint32_t doc, sum;\nstatic unsigned char doclist[] = {\n");
						data = remember_compress(&data_length_in_bytes);
						for (byte = data; byte < data + data_length_in_bytes; byte++)
							{
							fprintf(postings_dot_c, "0x%02X, ", *byte);
							if (*byte & 0x80)
								fprintf(postings_dot_c, "\n");
							}

						fprintf(postings_dot_c, "};\n");
						fprintf(postings_dot_c, "sum = 0;\n");
						fprintf(postings_dot_c, "for (uint8_t *i = doclist; i < doclist + %u;)\n", data_length_in_bytes);
						fprintf(postings_dot_c, "	{\n");
						fprintf(postings_dot_c, "	if (*i & 0x80)\n");
						fprintf(postings_dot_c, "		doc = *i++ & 0x7F;\n");
						fprintf(postings_dot_c, "	else\n");
						fprintf(postings_dot_c, "		{\n");
						fprintf(postings_dot_c, "		doc = *i++;\n");
						fprintf(postings_dot_c, "		while (!(*i & 0x80))\n");
						fprintf(postings_dot_c, "		   doc = (doc << 7) | *i++;\n");
						fprintf(postings_dot_c, "		doc = (doc << 7) | (*i++ & 0x7F);\n");
						fprintf(postings_dot_c, "		}\n");
						fprintf(postings_dot_c, "	sum += doc;\n");
						fprintf(postings_dot_c, "	add_rsv(sum, %llu);\n", previous_impact);
						fprintf(postings_dot_c, "	}\n");
						}
					else if (static_data)
						{
						fprintf(postings_dot_c, "};\n");
						fprintf(postings_dot_c, "for (uint32_t *i = doclist; i < doclist + %u; i++)\n", docids_in_impact);
						fprintf(postings_dot_c, "\tadd_rsv(*i, %llu);\n", previous_impact);
						}

					fprintf(postings_dot_c, "}\n\n");
					fprintf(postings_dot_c, "%s{0,0}\n};\n\n", term_method_list.str().c_str());

					fprintf(postings_dot_c, "void *CIt_ip_%s[] = \n{\n", buffer);
					for (which_impact = 0; which_impact <= impacts_for_this_term; which_impact++)
						fprintf(postings_dot_c, "CIt_i_%s + %llu,\n", buffer, which_impact);
					fprintf(postings_dot_c, "};\n");

					*vocab_in_current_file << "{\"" << buffer << "\", CIt_ip_" << buffer << ", " << impacts_for_this_term << "},\n";
					if (seperate_files)
						close_postings_dot_c(postings_dot_c);
					}
				}
			if (include_postings)
				unique_terms_in_index++;
			}
		}
	}

fprintf(vocab_dot_c, "CI_vocab CI_dictionary[%llu];\n", unique_terms_in_index);
fprintf(vocab_dot_c, "uint32_t CI_unique_terms = %llu;\n", unique_terms_in_index);
fprintf(vocab_dot_c, "uint32_t CI_unique_documents = %llu;\n", max_docid + 1);			// +1 because we count from zero
fprintf(vocab_dot_c, "uint32_t CI_max_q = %llu;\n", max_q);

fclose(vocab_dot_c);
fclose(fp);

fclose(makefile);
#ifdef _MSC_VER
	fprintf(makefile_include, "\n\tlib CIt_*.obj /OUT:..\\CIpostings.lib\n\n");
#else
	fprintf(makefile_include, "\n\ttouch ../CIpostings.o\n\n");
#endif
fclose(makefile_include);

if (!seperate_files)
	if (postings_dot_c != NULL)
		close_postings_dot_c(postings_dot_c);

return 0;
}
