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

return postings_dot_c;
}

/*
	CLOSE_POSTINGS_DOT_C()
	----------------------
*/
void close_postings_dot_c(FILE *fp)
{
fclose(fp);
}

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
char *end_of_term, *buffer_address = (char *)buffer;
uint64_t line = 0;
uint64_t cf, df, docid, impact, first_time = true, max_docid = 0, max_q = 0;
FILE *fp, *vocab_dot_c, *postings_dot_c, *postings_dot_h, *doclist, *doclist_dot_c, *makefile, *makefile_include;
uint32_t include_postings;
uint64_t positings_file_number = 0;
uint64_t previous_impact, impacts_for_this_term, which_impact, unique_terms_in_index = 0;

if (argc != 3 && argc != 4)
	exit(printf("Usage: %s <index.dump> <docid.aspt> [<topicfile>]\nGenerate index.dump with atire_dictionary > index.dump\nGeneratedocid.aspt with atire_doclist\nGenerate <topicfile> with trec2query <trectopicfile> t\n", argv[0]));

if (argc == 4)
	{
	/*
		If the user gives me a <topicfile> then put each term into a seperate file.
	*/
	seperate_files = true;
	load_topic_file(argv[3]);
	}
else
	seperate_files = false;

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
fprintf(vocab_dot_c, "CI_vocab CI_dictionary[] =\n{\n");

postings_dot_c = NULL;
mkdir("CIpostings", S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
if ((makefile = fopen("CIpostings/makefile", "wb")) == NULL)
	exit(printf("Cannot open 'CIpostings/makefile' output file\n"));
#ifdef _MSC_VER
	fprintf(makefile, "include makefile.include\n\nCI_FLAGS = -c /Ot /Tp\n\n");
#else
	fprintf(makefile, "include makefile.include\n\nCI_FLAGS = -x c++ -c\n\n");
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

first_time = true;
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
							fprintf(makefile, "CIt_%s.o : CIt_%s.c\n\t $(CXX) $(CXXFLAGS) $(CI_FLAGS) CIt_%s.c\n\n", buffer, buffer, buffer);
							fprintf(makefile_include, " CIt_%s.o", buffer);
						#endif
						}
					else if (((line - 1) % TERMS_PER_SOURCE_CODE_FILE) == 0)
						{
						char filename[1024];

						sprintf(filename, "%llu", positings_file_number);
						if (postings_dot_c != NULL)
							close_postings_dot_c(postings_dot_c);
						postings_dot_c = open_postings_dot_c(filename);

						#ifdef _MSC_VER
							fprintf(makefile, "CIt_%llu.obj : CIt_%llu.c\n\t $(CXX) $(CXXFLAGS) $(CI_FLAGS)  CIt_%llu.c\n\n", positings_file_number, positings_file_number, positings_file_number);
							fprintf(makefile_include, " CIt_%llu.obj", positings_file_number);
						#else
							fprintf(makefile, "CIt_%llu.o : CIt_%llu.c\n\t $(CXX) $(CXXFLAGS) $(CI_FLAGS) CIt_%llu.c\n\n", positings_file_number, positings_file_number, positings_file_number);
							fprintf(makefile_include, " CIt_%llu.o", positings_file_number);
						#endif
						positings_file_number++;
						}

					previous_impact = ULONG_MAX;
					ostringstream term_method_list;

					while (end_of_term != NULL)
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
									fprintf(postings_dot_c, "}\n\n");

								fprintf(postings_dot_c, "static void CIt_%s_i_%llu(CI_globals *g)\n{\n", buffer, impact);
								term_method_list << "{" << impact << ", CIt_" << buffer << "_i_" << impact << "},\n";
								previous_impact = impact;
								impacts_for_this_term++;
								}
							fprintf(postings_dot_c, "add_rsv(g, %llu, %llu);\n", docid, impact);
							}
					fprintf(postings_dot_c, "}\n\n");
					fprintf(postings_dot_c, "%s{0,0}\n};\n", term_method_list.str().c_str());

					fprintf(postings_dot_c, "void *CIt_ip_%s[] = \n{\n", buffer);
					for (which_impact = 0; which_impact <= impacts_for_this_term; which_impact++)
						fprintf(postings_dot_c, "CIt_i_%s + %llu,\n", buffer, which_impact);
					fprintf(postings_dot_c, "};\n");

					if (seperate_files)
						close_postings_dot_c(postings_dot_c);
					}
				}
			if (!first_time)
				if (include_postings)
					fprintf(vocab_dot_c, ",\n");			// add to the vocab c file

			if (include_postings)
				{
				fprintf(vocab_dot_c, "{\"%s\", CIt_ip_%s, %llu}", buffer, buffer, impacts_for_this_term);			// add to the vocab c file
				first_time = false;
				unique_terms_in_index++;
				}
			else
				{
//				fprintf(vocab_dot_c, "{\"%s\", 0, %llu}", buffer, impacts_for_this_term);			// add to the vocab c file
//				first_time = false;
				}
			}
		}
	}

fprintf(vocab_dot_c, "\n};\n\n");
fprintf(vocab_dot_c, "uint32_t CI_unique_terms = %llu;\n", unique_terms_in_index);
fprintf(vocab_dot_c, "uint32_t CI_unique_documents = %llu;\n", max_docid + 1);			// +1 because we count from zero
fprintf(vocab_dot_c, "uint32_t CI_max_q = %llu;\n", max_q);

fclose(vocab_dot_c);
fclose(fp);

fclose(makefile);
#ifdef _MSC_VER
	fprintf(makefile_include, "\n\tlib CIt_*.obj /OUT:..\\CIpostings.lib\n\n");
#else
	fprintf(makefile_include, "\n\tld -r CIt_*.o -o ../CIpostings.o\n\n");
#endif
fclose(makefile_include);

if (!seperate_files)
	if (postings_dot_c != NULL)
		close_postings_dot_c(postings_dot_c);

return 0;
}
