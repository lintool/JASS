/*
	ATIRE_TO_COMPILED.C
	-------------------
	atire dump format:
	term cf df <docid,impact>...<docid,impact>
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

uint32_t seperate_files = true;									// set this to false to get all the postings into a single file
static char buffer[1024 * 1024 * 1024];

char *termlist[1024 * 1024];
uint32_t termlist_length = 0;

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

if (term == NULL)
	sprintf(buffer, "CIpostings.c");
else
	sprintf(buffer, "CIpostings/CIt_%s.c", term);
if ((postings_dot_c = fopen(buffer, "wb")) == NULL)
	exit(printf("Cannot open '%s' output file\n", buffer));

fprintf(postings_dot_c, "#include <stdint.h>\n");

if (term == NULL)
	fprintf(postings_dot_c, "#include \"CI.h\"\n\n");
else
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

if (argc != 3 && argc != 4)
	exit(printf("Usage: %s <index.dump> <docid.aspt> [<topicfile>]\nGenerate index.dump with atire_dictionary > index.dump\nGeneratedocid.aspt with atire_doclist\nGenerate <topicfile> with trec2query <trectopicfile> t\n", argv[0]));

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
if ((vocab_dot_c = fopen("CIvocab.c", "wb")) == NULL)
	exit(printf("Cannot open CIvocab.c output file\n"));

fprintf(vocab_dot_c, "#include <stdint.h>\n");
fprintf(vocab_dot_c, "#include \"CI.h\"\n");
fprintf(vocab_dot_c, "#include \"CIpostings.h\"\n\n");
fprintf(vocab_dot_c, "CI_vocab CI_dictionary[] =\n{\n");

if (seperate_files)
	{
	mkdir("CIpostings", S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

	if ((makefile = fopen("CIpostings/makefile", "wb")) == NULL)
		exit(printf("Cannot open 'CIpostings/makefile' output file\n"));
	fprintf(makefile, "include makefile.include\n\nCI_FLAGS = -c\n\n");

	if ((makefile_include = fopen("CIpostings/makefile.include", "wb")) == NULL)
		exit(printf("Cannot open 'CIpostings/makefile.include' output file\n"));
	fprintf(makefile_include, "../CIpostings.o : ");
	}
else
	postings_dot_c = open_postings_dot_c(NULL);

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

			if (first_time)
				{
				if (include_postings)
					fprintf(vocab_dot_c, "{\"%s\", CIt_%s, %lld, %lld}", buffer, buffer, cf, df);			// add to the vocab c file
				else
					fprintf(vocab_dot_c, "{\"%s\", 0, %lld, %lld}", buffer, cf, df);			// add to the vocab c file
				first_time = false;
				}
			else
				if (include_postings)
					fprintf(vocab_dot_c, ",\n{\"%s\", CIt_%s, %lld, %lld}", buffer, buffer, cf, df);			// add to the vocab c file
				else
					fprintf(vocab_dot_c, ",\n{\"%s\", 0, %lld, %lld}", buffer, cf, df);			// add to the vocab c file

			if (include_postings)
				{
				if ((end_of_term = strchr(end_of_term + 1, '<')) != NULL)
					{
					if (seperate_files)
						{
						postings_dot_c = open_postings_dot_c(buffer);
						fprintf(makefile, "CIt_%s.o : CIt_%s.c\n\t $(CXX) $(CXXFLAGS) $(CI_FLAGS) CIt_%s.c\n\n", buffer, buffer, buffer);
						fprintf(makefile_include, " CIt_%s.o", buffer);
						}

					fprintf(postings_dot_h, "void CIt_%s(void);\n", buffer);
					fprintf(postings_dot_c, "void CIt_%s(void)\n{\n", buffer);
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

							fprintf(postings_dot_c, "add_rsv(%lld, %lld);\n", docid, impact);
							}
					fprintf(postings_dot_c, "}\n\n");

					if (seperate_files)
						close_postings_dot_c(postings_dot_c);
					}
				}
			}
		}
	}

fprintf(vocab_dot_c, "\n};\n\n");
fprintf(vocab_dot_c, "uint32_t CI_unique_terms = %llu;\n", line);
fprintf(vocab_dot_c, "uint32_t CI_unique_documents = %llu;\n", max_docid + 1);			// +1 because we count from zero
fprintf(vocab_dot_c, "uint32_t CI_max_q = %llu;\n", max_q);

fclose(vocab_dot_c);
fclose(fp);

if (seperate_files)
	{
	fclose(makefile);

	fprintf(makefile_include, "\n\tld -r CIt_*.o -o ../CIpostings.o\n\n");

	fclose(makefile_include);
	}
else
	close_postings_dot_c(postings_dot_c);

return 0;
}
