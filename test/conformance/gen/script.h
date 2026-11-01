/*
 * Conformance harness: tiny line-oriented script reader shared by the runners.
 *
 * Script syntax (see test/conformance/README.md):
 *   - one command per line; tokens separated by spaces or tabs
 *   - '#' starts a comment (only at the start of a token that begins a line, or
 *     after whitespace)
 *   - a token may be written "like this" with C escapes: \n \t \r \\ \" \xHH
 *   - the bare token -  means "no value" (NULL / empty)
 *
 * This file is test tooling, not part of libobs.
 */
#pragma once

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_MAX_TOKENS 32
#define SCRIPT_MAX_LINE 8192

struct script_line {
	int argc;
	char *argv[SCRIPT_MAX_TOKENS];
	char storage[SCRIPT_MAX_LINE];
	int lineno;
};

static inline int script_hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Tokenises one raw line into sl. Returns false if the line is empty/comment. */
static inline bool script_tokenize(struct script_line *sl, const char *raw)
{
	char *out = sl->storage;
	const char *p = raw;
	sl->argc = 0;

	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (!*p || *p == '#')
			break;
		if (sl->argc >= SCRIPT_MAX_TOKENS) {
			fprintf(stderr, "script: too many tokens at line %d\n", sl->lineno);
			exit(2);
		}
		sl->argv[sl->argc++] = out;
		if (*p == '"') {
			p++;
			while (*p && *p != '"') {
				if (*p == '\\' && p[1]) {
					p++;
					switch (*p) {
					case 'n':
						*out++ = '\n';
						break;
					case 't':
						*out++ = '\t';
						break;
					case 'r':
						*out++ = '\r';
						break;
					case 'x': {
						int h = script_hexval(p[1]), l = h >= 0 ? script_hexval(p[2]) : -1;
						if (h < 0 || l < 0) {
							fprintf(stderr, "script: bad \\x at line %d\n", sl->lineno);
							exit(2);
						}
						*out++ = (char)(h * 16 + l);
						p += 2;
						break;
					}
					default:
						*out++ = *p;
					}
					p++;
				} else {
					*out++ = *p++;
				}
			}
			if (*p != '"') {
				fprintf(stderr, "script: unterminated string at line %d\n", sl->lineno);
				exit(2);
			}
			p++;
		} else {
			while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
				*out++ = *p++;
		}
		*out++ = 0;
	}
	return sl->argc > 0;
}

/* Reads the next command. Returns false at EOF. */
static inline bool script_next(FILE *f, struct script_line *sl, int *lineno)
{
	char raw[SCRIPT_MAX_LINE];
	while (fgets(raw, sizeof(raw), f)) {
		(*lineno)++;
		sl->lineno = *lineno;
		if (script_tokenize(sl, raw))
			return true;
	}
	return false;
}

static inline void script_print_hex(const uint8_t *data, size_t len)
{
	if (!len) {
		fputs("-", stdout);
		return;
	}
	for (size_t i = 0; i < len; i++)
		printf("%02x", data[i]);
}

/* Prints s between double quotes, escaping non-printable bytes as \xHH and
 * the characters \ " \n \t \r. Stops at the first NUL. */
static inline void script_print_quoted(const char *s)
{
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '\\')
			fputs("\\\\", stdout);
		else if (*p == '"')
			fputs("\\\"", stdout);
		else if (*p == '\n')
			fputs("\\n", stdout);
		else if (*p == '\t')
			fputs("\\t", stdout);
		else if (*p == '\r')
			fputs("\\r", stdout);
		else if (*p < 0x20 || *p >= 0x7f)
			printf("\\x%02x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

/* Parses a hex string ("-" = empty). Returns malloc'd buffer (never NULL). */
static inline uint8_t *script_parse_hex(const char *s, size_t *len)
{
	size_t n = strcmp(s, "-") == 0 ? 0 : strlen(s) / 2;
	uint8_t *buf = malloc(n ? n : 1);
	for (size_t i = 0; i < n; i++)
		buf[i] = (uint8_t)(script_hexval(s[2 * i]) * 16 + script_hexval(s[2 * i + 1]));
	*len = n;
	return buf;
}

static inline void script_need(const struct script_line *sl, int n)
{
	if (sl->argc < n) {
		fprintf(stderr, "script: line %d: '%s' needs %d arguments\n", sl->lineno, sl->argv[0], n - 1);
		exit(2);
	}
}
