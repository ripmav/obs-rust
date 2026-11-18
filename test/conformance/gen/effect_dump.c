/* Effect-file golden dumper (W21d).
 *
 * Dumps the cf-lexer + preprocessor output for one .effect file in the
 * conformance runner's print format (parse / tokens pp / errors). File
 * names under DATA_DIR are printed as "@data<rest>" so the digests are
 * location-independent. DATA_DIR must have no trailing slash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/cf-parser.h"

static const char *data_dir;

static void script_print_quoted(const char *s)
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

static void pseg(const char *p, size_t n)
{
	char buf[8192];
	size_t k = 0;
	if (p)
		while (k < n && k < sizeof(buf) - 1 && p[k])
			k++;
	if (k)
		memcpy(buf, p, k);
	buf[k] = 0;
	script_print_quoted(buf);
}

static void pfile(const char *f)
{
	if (!f) {
		fputs("@null", stdout);
		return;
	}
	size_t n = strlen(data_dir);
	if (!strncmp(f, data_dir, n) && f[n] == '/') {
		char b[1024];
		snprintf(b, sizeof b, "@data%s", f + n);
		script_print_quoted(b);
	} else
		script_print_quoted(f);
}

static const char *ctype(enum cf_token_type t)
{
	switch (t) {
	case CFTOKEN_NONE:
		return "NONE";
	case CFTOKEN_NAME:
		return "NAME";
	case CFTOKEN_NUM:
		return "NUM";
	case CFTOKEN_SPACETAB:
		return "SPACETAB";
	case CFTOKEN_NEWLINE:
		return "NEWLINE";
	case CFTOKEN_STRING:
		return "STRING";
	case CFTOKEN_OTHER:
		return "OTHER";
	}
	return "?";
}

static const struct cf_lexer *P_lex;
static struct cf_preprocessor *P_pp;

static const struct cf_lexer *owner_of(const char *u)
{
	const struct cf_lexer *c[80];
	int n = 0;
	c[n++] = P_lex;
	for (size_t i = 0; i < P_pp->dependencies.num; i++)
		c[n++] = P_pp->dependencies.array + i;
	for (int i = 0; i < n; i++) {
		const char *txt = c[i]->base_lexer.text;
		if (txt && u && u >= txt && u <= txt + strlen(txt))
			return c[i];
	}
	return NULL;
}

static void print_tok(const struct cf_token *t)
{
	printf("type=%s str=", ctype(t->type));
	pseg(t->str.array, t->str.len);
	const char *u = t->unmerged_str.array;
	const struct cf_lexer *lx = owner_of(u);
	if (lx) {
		const char *txt = lx->base_lexer.text;
		uint32_t r = 0, c = 0;
		lexer_getstroffset(&lx->base_lexer, u, &r, &c);
		printf(" src=%ld+%zu @%u:%u", (long)(u - txt), t->unmerged_str.len, r, c);
		if (t->unmerged_str.len != t->str.len || (t->str.array && strncmp(t->str.array, u, t->str.len))) {
			printf(" u=");
			pseg(u, t->unmerged_str.len);
		}
		if (lx != P_lex) {
			printf(" f=");
			pfile(lx->file);
		}
	} else {
		printf(" src=-");
	}
}

static void print_errors(struct error_data *ed)
{
	printf("errors %zu (errors=%zu warnings=%zu has_errors=%d)\n", ed->errors.num,
	       error_data_type_count(ed, LEX_ERROR), error_data_type_count(ed, LEX_WARNING),
	       error_data_has_errors(ed));
	for (size_t i = 0; i < ed->errors.num; i++) {
		const struct error_item *it = error_data_item(ed, i);
		printf("err %zu %s file=", i,
		       it->level == LEX_ERROR ? "ERROR" : it->level == LEX_WARNING ? "WARNING" : "LEVEL?");
		pfile(it->file);
		printf(" row=%u col=%u msg=", it->row, it->column);
		script_print_quoted(it->error);
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <effect-file> <data-dir>\n", argv[0]);
		return 2;
	}
	data_dir = argv[2];
	const char *path = argv[1];

	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	long sz = 0;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(sz + 1);
	if (sz && fread(buf, 1, sz, f) != (size_t)sz) {
		fprintf(stderr, "read error\n");
		return 1;
	}
	fclose(f);
	buf[sz] = 0;

	struct cf_lexer lex;
	cf_lexer_init(&lex);
	P_lex = &lex;
	bool r = cf_lexer_lex(&lex, buf, path);

	struct cf_preprocessor pp;
	cf_preprocessor_init(&pp);
	P_pp = &pp;
	struct error_data ed;
	error_data_init(&ed);
	bool pr = r && cf_preprocess(&pp, &lex, &ed);

	printf("parse %s\n", pr ? "true" : "false");
	printf("tokens pp %zu\n", pp.tokens.num);
	for (size_t i = 0; i < pp.tokens.num; i++) {
		printf("tok %zu ", i);
		print_tok(pp.tokens.array + i);
		printf("\n");
	}
	print_errors(&ed);
	return 0;
}
