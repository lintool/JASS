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

static char buffer[1024 * 1024 * 1024];

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
char *end_of_term;
uint64_t line = 0;
uint64_t cf, df, docid, impact, first_time = true, max_docid = 0;
FILE *fp, *vocab_dot_c, *postings_dot_c, *postings_dot_h;

if (argc != 2)
	exit(printf("Usage: %s <infile.txt>\n", argv[0]));

if ((fp = fopen(argv[1], "rb")) == NULL)
	exit(printf("Cannot open input file '%s'\n", argv[1]));

if ((vocab_dot_c = fopen("CIvocab.c", "wb")) == NULL)
	exit(printf("Cannot open CIvocab.c output file\n"));

fprintf(vocab_dot_c, "#include <stdint.h>\n");
fprintf(vocab_dot_c, "#include \"CI.h\"\n");
fprintf(vocab_dot_c, "#include \"CIpostings.h\"\n\n");
fprintf(vocab_dot_c, "CI_vocab CI_dictionary[] =\n{\n");

if ((postings_dot_c = fopen("CIpostings.c", "wb")) == NULL)
	exit(printf("Cannot open CIpostings.c output file\n"));

fprintf(postings_dot_c, "#include <stdint.h>\n\n");

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
			if (first_time)
				{
				fprintf(vocab_dot_c, "{\"%s\", CIt_%s, %lld, %lld}", buffer, buffer, cf, df);			// add to the vocab c file
				first_time = false;
				}
			else
				fprintf(vocab_dot_c, ",\n{\"%s\", CIt_%s, %lld, %lld}", buffer, buffer, cf, df);			// add to the vocab c file

			if ((end_of_term = strchr(end_of_term + 1, '<')) != NULL)
				{
				fprintf(postings_dot_h, "void CIt_%s(uint16_t *a);\n", buffer);
				fprintf(postings_dot_c, "void CIt_%s(uint16_t *a)\n{\n", buffer);
				while (end_of_term != NULL)
					if ((end_of_term = strchr(end_of_term, '<')) != NULL)
						{
						docid = atoll(end_of_term + 1);
						if  (docid > max_docid)
							max_docid = docid;
						end_of_term = strchr(end_of_term + 1, ',');
						impact = atoll(end_of_term + 1);
						fprintf(postings_dot_c, "a[%lld] += %lld;\n", docid, impact);
						}
				fprintf(postings_dot_c, "}\n\n");
				}
			}
		}
	}

fprintf(vocab_dot_c, "\n};\n\n");
fprintf(vocab_dot_c, "uint64_t CI_unique_terms = %llu;\n", line);
fprintf(vocab_dot_c, "uint64_t CI_unique_documents = %llu;\n", max_docid + 1);			// +1 because we count from zero

fclose(vocab_dot_c);
fclose(fp);
fclose(postings_dot_c);
}
