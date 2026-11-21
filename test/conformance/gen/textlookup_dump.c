/*
 * text-lookup dumper (S-010 §5.7, P1-08.4): dump every key of a locale
 * .ini file through the C text_lookup and print one line per key:
 *
 *   kv <key> <value>          (resolved)
 *   kv <key> <missing>        (key not found)
 *
 * Key extraction: for every line, strip leading whitespace, skip
 * comments (#) and blanks, cut at the first '=' and trim. This matches
 * the en-US.ini format (Name="Value"); the Rust test uses the same
 * extraction, and both sides must resolve every such line (the dumper
 * exits 1 if any line comes back <missing>).
 *
 * Build from test/conformance:
 *
 *   cc -std=gnu11 -O2 -Wall -I../../libobs gen/textlookup_dump.c gen/stubs.c \
 *     ../../libobs/util/text-lookup.c ../../libobs/util/lexer.c \
 *     ../../libobs/util/dstr.c ../../libobs/util/bmem.c \
 *     ../../libobs/util/base.c -o textlookup_dump
 *
 * Run: ./textlookup_dump <path-to-en-US.ini>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "util/text-lookup.h"

static char *read_whole_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(n + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (n > 0 && fread(buf, 1, n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[n] = 0;
	fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <file.ini>\n", argv[0]);
		return 2;
	}
	char *data = read_whole_file(argv[1]);
	if (!data) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	lookup_t *lookup = text_lookup_create(argv[1]);
	if (!lookup) {
		fprintf(stderr, "text_lookup_create failed\n");
		free(data);
		return 2;
	}

	int missing = 0;
	char *saveptr = NULL;
	for (char *line = strtok_r(data, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
		while (isspace((unsigned char)*line))
			line++;
		if (*line == 0 || *line == '#')
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *end = eq;
		while (end > line && isspace((unsigned char)end[-1]))
			end--;
		*end = 0;
		const char *value;
		if (!text_lookup_getstr(lookup, line, &value)) {
			printf("kv %s <missing>\n", line);
			missing++;
		} else {
			printf("kv %s %s\n", line, value);
		}
	}
	text_lookup_destroy(lookup);
	free(data);
	if (missing) {
		fprintf(stderr, "%d keys missing\n", missing);
		return 1;
	}
	return 0;
}
