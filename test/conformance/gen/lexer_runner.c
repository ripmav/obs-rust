/*
 * Conformance runner for the lexer family (spec S-011): base lexer (lexer.c),
 * C-like token layer + preprocessor (cf-lexer.c) and parser helpers (cf-parser.h).
 * The command list is documented in test/conformance/README.md ("lexer runner").
 *
 * Output conventions: text is printed with script_print_quoted(); file names that start
 * with $CONFORMANCE_TMPDIR are printed as "@tmp<rest>" and scripts write "@tmp/..." in
 * file arguments.  "@null" is NULL.
 */
#include <sys/wait.h>
#include <unistd.h>

#include "util/cf-parser.h"
#include "harness.h"

static struct lexer L;
static struct error_data ED;
static struct cf_parser P;
static bool used;
static struct cf_lexer *side[64];
static int nside;
static char tmpdir[400];

static const char *s(const struct script_line *sl, int i)
{
	return strcmp(sl->argv[i], "@null") ? sl->argv[i] : NULL;
}
static const char *opt(int argc, char **argv, int i)
{
	return i < argc ? (strcmp(argv[i], "@null") ? argv[i] : NULL) : NULL;
}

/* print n bytes of p, stopping at NUL */
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
	size_t n = strlen(tmpdir);
	if (!strncmp(f, tmpdir, n)) {
		char b[1024];
		snprintf(b, sizeof b, "@tmp%s", f + n);
		script_print_quoted(b);
	} else
		script_print_quoted(f);
}

static char *expand_file(const char *f)
{
	if (!f)
		return NULL;
	char b[1024];
	if (!strncmp(f, "@tmp", 4))
		snprintf(b, sizeof b, "%s%s", tmpdir, f + 4);
	else
		snprintf(b, sizeof b, "%s", f);
	return strdup(b);
}

static const char *btype(enum base_token_type t)
{
	switch (t) {
	case BASETOKEN_NONE:
		return "NONE";
	case BASETOKEN_ALPHA:
		return "ALPHA";
	case BASETOKEN_DIGIT:
		return "DIGIT";
	case BASETOKEN_WHITESPACE:
		return "WHITESPACE";
	case BASETOKEN_OTHER:
		return "OTHER";
	}
	return "?";
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

static int parse_ctype(const char *n)
{
	for (int i = 0; i <= CFTOKEN_OTHER; i++)
		if (!strcmp(n, ctype((enum cf_token_type)i)))
			return i;
	fprintf(stderr, "script: bad token type %s\n", n);
	exit(2);
}

static const char *pcode(int c)
{
	switch (c) {
	case PARSE_SUCCESS:
		return "SUCCESS";
	case PARSE_CONTINUE:
		return "CONTINUE";
	case PARSE_BREAK:
		return "BREAK";
	case PARSE_UNEXPECTED_CONTINUE:
		return "UNEXPECTED_CONTINUE";
	case PARSE_UNEXPECTED_BREAK:
		return "UNEXPECTED_BREAK";
	case PARSE_EOF:
		return "EOF";
	}
	return "?";
}

static void print_base(size_t i, const struct base_token *t)
{
	printf("base %zu type=%s off=%ld len=%zu text=", i, btype(t->type), (long)(t->text.array - L.text),
	       t->text.len);
	pseg(t->text.array, t->text.len);
	printf(" pws=%d\n", t->passed_whitespace);
}

/* The owner of a token is found by address range of its unmerged text; t->lex is never
 * dereferenced because the reference leaves it dangling for tokens of included files (D-4). */
static const struct cf_lexer *owner_of(const char *u)
{
	const struct cf_lexer *c[80];
	int n = 0;
	c[n++] = &P.lex;
	for (size_t i = 0; i < P.pp.dependencies.num; i++)
		c[n++] = P.pp.dependencies.array + i;
	for (int i = 0; i < nside && n < 80; i++)
		c[n++] = side[i];
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
		if (lx != &P.lex) {
			printf(" f=");
			pfile(lx->file);
		}
	} else {
		printf(" src=-");
	}
}

static void print_cursor(void)
{
	if (!P.cur_token) {
		printf("  at @null\n");
		return;
	}
	printf("  at #%ld ", (long)(P.cur_token - P.pp.tokens.array));
	print_tok(P.cur_token);
	printf("\n");
}

static void print_tokens(const struct cf_token *a, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		printf("tok %zu ", i);
		print_tok(a + i);
		printf("\n");
	}
}

static void print_errors(struct error_data *ed)
{
	printf("errors %zu (errors=%zu warnings=%zu has_errors=%d)\n", ed->errors.num,
	       error_data_type_count(ed, LEX_ERROR), error_data_type_count(ed, LEX_WARNING), error_data_has_errors(ed));
	for (size_t i = 0; i < ed->errors.num; i++) {
		const struct error_item *it = error_data_item(ed, i);
		printf("err %zu %s file=", i, it->level == LEX_ERROR ? "ERROR" : it->level == LEX_WARNING ? "WARNING" : "LEVEL?");
		pfile(it->file);
		printf(" row=%u col=%u msg=", it->row, it->column);
		script_print_quoted(it->error);
		printf("\n");
	}
}

static void need_fresh(const struct script_line *sl)
{
	if (used) {
		fprintf(stderr, "script: line %d: 'reset' required before a second lex/parse\n", sl->lineno);
		exit(2);
	}
	used = true;
}

static void do_reset(void)
{
	cf_parser_free(&P);
	for (int i = 0; i < nside; i++) {
		cf_lexer_free(side[i]);
		free(side[i]);
	}
	nside = 0;
	cf_parser_init(&P);
	used = false;
}

static int exec(int argc, char **argv, int lineno);

static int exec1(struct script_line *sl)
{
	return exec(sl->argc, sl->argv, sl->lineno);
}

static int exec(int argc, char **argv, int lineno)
{
	struct script_line tmp;
	struct script_line *sl = &tmp;
	tmp.argc = argc;
	tmp.lineno = lineno;
	for (int i = 0; i < argc; i++)
		tmp.argv[i] = argv[i];
	const char *c = argv[0];

	/* ---------------- base lexer ---------------- */
	if (!strcmp(c, "lstart")) {
		script_need(sl, 2);
		lexer_start(&L, s(sl, 1));
	} else if (!strcmp(c, "lstart_move")) {
		script_need(sl, 2);
		lexer_start_move(&L, bstrdup(s(sl, 1)));
	} else if (!strcmp(c, "lreset")) {
		lexer_reset(&L);
	} else if (!strcmp(c, "lfree")) {
		lexer_free(&L);
	} else if (!strcmp(c, "lnext") || !strcmp(c, "lall")) {
		enum ignore_whitespace iws = (argc > 1 && !strcmp(argv[1], "ignore")) ? IGNORE_WHITESPACE : PARSE_WHITESPACE;
		bool all = !strcmp(c, "lall");
		size_t n = 0;
		do {
			struct base_token t;
			base_token_clear(&t);
			bool r = lexer_getbasetoken(&L, &t, iws);
			if (r)
				print_base(n++, &t);
			else
				printf("base none\n");
			if (L.offset)
				printf("  offset=%ld\n", (long)(L.offset - L.text));
			else
				printf("  offset=null\n");
			if (!r)
				break;
		} while (all);
	} else if (!strcmp(c, "stroffset")) {
		/* stroffset TEXT IDX|@null : row/col of text+IDX */
		script_need(sl, 3);
		struct lexer lx;
		lexer_init(&lx);
		lexer_start(&lx, argv[1]);
		uint32_t row = 999, col = 999;
		lexer_getstroffset(&lx, s(sl, 2) ? lx.text + strtol(argv[2], NULL, 0) : NULL, &row, &col);
		printf("stroffset row=%u col=%u\n", row, col);
		lexer_free(&lx);
	} else if (!strcmp(c, "newline_size")) {
		script_need(sl, 2);
		printf("newline_size %d\n", newline_size(argv[1]));
	} else if (!strcmp(c, "chars")) {
		/* chars TEXT: classification helpers for every byte of TEXT */
		script_need(sl, 2);
		for (const char *p = argv[1]; *p; p++)
			printf("char %02x ws=%d nl=%d st=%d\n", (unsigned char)*p, is_whitespace(*p), is_newline(*p),
			       is_space_or_tab(*p));
	} else if (!strcmp(c, "is_newline_pair")) {
		script_need(sl, 3);
		printf("is_newline_pair %d\n", is_newline_pair(argv[1][0], argv[2][0]));

		/* ---------------- strref ---------------- */
	} else if (!strcmp(c, "cmp") || !strcmp(c, "cmpi")) {
		/* cmp TEXT LEN STR2|@null */
		script_need(sl, 4);
		struct strref r;
		strref_set(&r, s(sl, 1), (size_t)strtoull(argv[2], NULL, 0));
		int v = !strcmp(c, "cmp") ? strref_cmp(&r, s(sl, 3)) : strref_cmpi(&r, s(sl, 3));
		printf("%s %d\n", c, v < 0 ? -1 : v > 0 ? 1 : 0);
	} else if (!strcmp(c, "cmp_strref") || !strcmp(c, "cmpi_strref")) {
		/* cmp_strref TEXT1 LEN1 TEXT2 LEN2 */
		script_need(sl, 5);
		struct strref a, b;
		strref_set(&a, s(sl, 1), (size_t)strtoull(argv[2], NULL, 0));
		strref_set(&b, s(sl, 3), (size_t)strtoull(argv[4], NULL, 0));
		int v = !strcmp(c, "cmp_strref") ? strref_cmp_strref(&a, &b) : strref_cmpi_strref(&a, &b);
		printf("%s %d\n", c, v < 0 ? -1 : v > 0 ? 1 : 0);
	} else if (!strcmp(c, "is_empty")) {
		/* is_empty TEXT|@null LEN */
		script_need(sl, 3);
		struct strref r;
		strref_set(&r, s(sl, 1), (size_t)strtoull(argv[2], NULL, 0));
		printf("is_empty %d\n", strref_is_empty(&r));
	} else if (!strcmp(c, "is_empty_null")) {
		printf("is_empty_null %d\n", strref_is_empty(NULL));
	} else if (!strcmp(c, "ref_add")) {
		/* ref_add TEXT OFF1 LEN1 OFF2 LEN2 : dst=(TEXT+OFF1,LEN1) += (TEXT+OFF2,LEN2); OFF1=@null: dst empty */
		script_need(sl, 6);
		struct strref d, t;
		if (s(sl, 2))
			strref_set(&d, argv[1] + strtol(argv[2], NULL, 0), (size_t)strtoull(argv[3], NULL, 0));
		else
			strref_clear(&d);
		strref_set(&t, argv[1] + strtol(argv[4], NULL, 0), (size_t)strtoull(argv[5], NULL, 0));
		strref_add(&d, &t);
		printf("ref_add off=%ld len=%zu\n", d.array ? (long)(d.array - argv[1]) : -1L, d.len);
	} else if (!strcmp(c, "valid_int") || !strcmp(c, "valid_float")) {
		/* valid_int TEXT|@null N */
		script_need(sl, 3);
		size_t n = (size_t)strtoull(argv[2], NULL, 0);
		bool v = !strcmp(c, "valid_int") ? valid_int_str(s(sl, 1), n) : valid_float_str(s(sl, 1), n);
		printf("%s %d\n", c, v);

		/* ---------------- error_data ---------------- */
	} else if (!strcmp(c, "ed_add")) {
		/* ed_add FILE ROW COL LEVEL MSG */
		script_need(sl, 6);
		static char files[16][64];
		static int nfiles;
		const char *f = s(sl, 1);
		if (f) {
			snprintf(files[nfiles % 16], 64, "%s", f);
			f = files[nfiles++ % 16];
		}
		error_data_add(&ED, f, (uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0), argv[5],
			       atoi(argv[4]));
	} else if (!strcmp(c, "ed_add_null")) {
		error_data_add(NULL, "f", 1, 1, "x", LEX_ERROR);
		printf("ed_add_null done\n");
	} else if (!strcmp(c, "ed_dump")) {
		print_errors(&ED);
		char *b = error_data_buildstring(&ED);
		printf("buildstring ");
		if (b)
			script_print_quoted(b);
		else
			printf("@null");
		printf("\n");
		bfree(b);
	} else if (!strcmp(c, "ed_free")) {
		error_data_free(&ED);
		error_data_init(&ED);

		/* ---------------- cf lexer ---------------- */
	} else if (!strcmp(c, "reset")) {
		do_reset();
	} else if (!strcmp(c, "writefile")) {
		/* writefile NAME TEXT : writes $CONFORMANCE_TMPDIR/NAME */
		script_need(sl, 3);
		char p[1024];
		snprintf(p, sizeof p, "%s/%s", tmpdir, argv[1]);
		FILE *f = fopen(p, "wb");
		if (!f) {
			perror(p);
			exit(2);
		}
		fwrite(argv[2], 1, strlen(argv[2]), f);
		fclose(f);
	} else if (!strcmp(c, "lex") || !strcmp(c, "parse")) {
		/* lex|parse TEXT [FILE] */
		script_need(sl, 2);
		need_fresh(sl);
		char *file = expand_file(opt(argc, argv, 2));
		bool r;
		if (!strcmp(c, "lex")) {
			r = cf_lexer_lex(&P.lex, s(sl, 1), file);
			printf("lex %s tokens=%zu unexpected_eof=%d\n", r ? "true" : "false", P.lex.tokens.num,
			       P.lex.unexpected_eof);
			if (P.lex.reformatted) {
				printf("reformatted ");
				script_print_quoted(P.lex.reformatted);
				printf("\n");
			}
		} else {
			r = cf_parser_parse(&P, s(sl, 1), file);
			printf("parse %s\n", r ? "true" : "false");
		}
		free(file);
	} else if (!strcmp(c, "pp")) {
		bool r = cf_preprocess(&P.pp, &P.lex, &P.error_list);
		if (r)
			P.cur_token = cf_preprocessor_get_tokens(&P.pp);
		printf("pp %s\n", r ? "true" : "false");
	} else if (!strcmp(c, "tokens")) {
		/* tokens lex|pp */
		script_need(sl, 2);
		if (!strcmp(argv[1], "lex")) {
			printf("tokens lex %zu\n", P.lex.tokens.num);
			print_tokens(P.lex.tokens.array, P.lex.tokens.num);
		} else {
			printf("tokens pp %zu\n", P.pp.tokens.num);
			print_tokens(P.pp.tokens.array, P.pp.tokens.num);
		}
	} else if (!strcmp(c, "errors")) {
		print_errors(&P.error_list);
	} else if (!strcmp(c, "errstr")) {
		char *b = error_data_buildstring(&P.error_list);
		printf("errstr ");
		if (b)
			script_print_quoted(b);
		else
			printf("@null");
		printf("\n");
		bfree(b);
	} else if (!strcmp(c, "def")) {
		/* def NAME [BODY] : cf_preprocessor_add_def with name/body tokens obtained from the cf lexer.
		 * BODY tokens are terminated by a NONE token as the preprocessor expects; no BODY = empty list. */
		script_need(sl, 2);
		struct cf_def def;
		cf_def_init(&def);
		struct cf_lexer *ln = calloc(1, sizeof *ln);
		cf_lexer_init(ln);
		cf_lexer_lex(ln, argv[1], "def-name");
		side[nside++] = ln;
		cf_token_copy(&def.name, ln->tokens.array);
		if (argc > 2) {
			struct cf_lexer *lb = calloc(1, sizeof *lb);
			cf_lexer_init(lb);
			cf_lexer_lex(lb, argv[2], "def-body");
			side[nside++] = lb;
			for (size_t i = 0; i < lb->tokens.num; i++)
				cf_def_addtoken(&def, lb->tokens.array + i);
		}
		cf_preprocessor_add_def(&P.pp, &def);
		printf("def added defines=%zu\n", P.pp.defines.num);
	} else if (!strcmp(c, "undef")) {
		script_need(sl, 2);
		cf_preprocessor_remove_def(&P.pp, argv[1]);
		printf("undef defines=%zu\n", P.pp.defines.num);
	} else if (!strcmp(c, "literal")) {
		/* literal TEXT [COUNT] */
		script_need(sl, 2);
		size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 0) : 0;
		/* exact-size heap copy so that sanitizers see reads past the terminator */
		char *heap = malloc(strlen(argv[1]) + 1);
		strcpy(heap, argv[1]);
		char *r = cf_literal_to_str(heap, n);
		free(heap);
		printf("literal ");
		if (r)
			script_print_quoted(r);
		else
			printf("@null");
		printf("\n");
		bfree(r);

		/* ---------------- parser helpers ---------------- */
	} else if (!strcmp(c, "cur")) {
		print_cursor();
	} else if (!strcmp(c, "setcur")) {
		script_need(sl, 2);
		P.cur_token = P.pp.tokens.array + strtol(argv[1], NULL, 0);
		print_cursor();
	} else if (!strcmp(c, "next") || !strcmp(c, "next_valid")) {
		bool r = !strcmp(c, "next") ? cf_next_token(&P) : cf_next_valid_token(&P);
		printf("%s %d\n", c, r);
		print_cursor();
	} else if (!strcmp(c, "peek") || !strcmp(c, "peek_valid")) {
		struct cf_token pk;
		bool r = !strcmp(c, "peek") ? cf_peek_token(&P, &pk) : cf_peek_valid_token(&P, &pk);
		printf("%s %d ", c, r);
		print_tok(&pk);
		printf("\n");
		print_cursor();
	} else if (!strcmp(c, "is")) {
		script_need(sl, 2);
		printf("is %d\n", cf_token_is(&P, s(sl, 1)));
	} else if (!strcmp(c, "is_type")) {
		/* is_type TYPE EXPECTED [GOTO] */
		script_need(sl, 3);
		int r = cf_token_is_type(&P, (enum cf_token_type)parse_ctype(argv[1]), argv[2], opt(argc, argv, 3));
		printf("is_type %s\n", pcode(r));
		print_cursor();
	} else if (!strcmp(c, "should_be") || !strcmp(c, "next_should_be")) {
		/* should_be STR [G1 [G2]] */
		script_need(sl, 2);
		int r = !strcmp(c, "should_be") ? cf_token_should_be(&P, argv[1], opt(argc, argv, 2), opt(argc, argv, 3))
						: cf_next_token_should_be(&P, argv[1], opt(argc, argv, 2),
									  opt(argc, argv, 3));
		printf("%s %s\n", c, pcode(r));
		print_cursor();
	} else if (!strcmp(c, "goto") || !strcmp(c, "goto_valid")) {
		/* goto S1 [S2] */
		script_need(sl, 2);
		bool r = !strcmp(c, "goto") ? cf_go_to_token(&P, s(sl, 1), opt(argc, argv, 2))
					    : cf_go_to_valid_token(&P, s(sl, 1), opt(argc, argv, 2));
		printf("%s %d\n", c, r);
		print_cursor();
	} else if (!strcmp(c, "goto_type")) {
		script_need(sl, 2);
		printf("goto_type %d\n", cf_go_to_token_type(&P, (enum cf_token_type)parse_ctype(argv[1])));
		print_cursor();
	} else if (!strcmp(c, "pass_pair")) {
		script_need(sl, 3);
		printf("pass_pair %d\n", cf_pass_pair(&P, argv[1][0], argv[2][0]));
		print_cursor();
	} else if (!strcmp(c, "get_name") || !strcmp(c, "next_name")) {
		/* get_name EXPECTED [GOTO] */
		script_need(sl, 2);
		char *dst = NULL;
		int r = !strcmp(c, "get_name") ? cf_get_name(&P, &dst, argv[1], opt(argc, argv, 2))
					       : cf_next_name(&P, &dst, argv[1], opt(argc, argv, 2));
		printf("%s %s name=", c, pcode(r));
		if (dst)
			script_print_quoted(dst);
		else
			printf("@null");
		printf("\n");
		bfree(dst);
		print_cursor();
	} else if (!strcmp(c, "get_name_ref") || !strcmp(c, "next_name_ref")) {
		script_need(sl, 2);
		struct strref ref;
		strref_clear(&ref);
		int r = !strcmp(c, "get_name_ref") ? cf_get_name_ref(&P, &ref, argv[1], opt(argc, argv, 2))
						   : cf_next_name_ref(&P, &ref, argv[1], opt(argc, argv, 2));
		printf("%s %s ref=", c, pcode(r));
		if (ref.array)
			pseg(ref.array, ref.len);
		else
			printf("@null");
		printf("\n");
		print_cursor();
	} else if (!strcmp(c, "next_copy") || !strcmp(c, "copy")) {
		char *dst = NULL;
		int r = PARSE_SUCCESS;
		if (!strcmp(c, "copy"))
			cf_copy_token(&P, &dst);
		else
			r = cf_next_token_copy(&P, &dst);
		printf("%s %s copy=", c, pcode(r));
		if (dst)
			script_print_quoted(dst);
		else
			printf("@null");
		printf("\n");
		bfree(dst);
		print_cursor();
	} else if (!strcmp(c, "adderror")) {
		/* adderror MSG LEVEL V1 V2 V3 */
		script_need(sl, 3);
		cf_adderror(&P, argv[1], atoi(argv[2]), opt(argc, argv, 3), opt(argc, argv, 4), opt(argc, argv, 5));
		print_errors(&P.error_list);
	} else if (!strcmp(c, "expecting")) {
		script_need(sl, 2);
		cf_adderror_expecting(&P, argv[1]);
	} else if (!strcmp(c, "eof_error")) {
		cf_adderror_unexpected_eof(&P);
	} else if (!strcmp(c, "syntax_error")) {
		cf_adderror_syntax_error(&P);
	} else {
		fprintf(stderr, "script: line %d: unknown command '%s'\n", lineno, c);
		exit(2);
	}
	return 0;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	const char *d = getenv("CONFORMANCE_TMPDIR");
	snprintf(tmpdir, sizeof tmpdir, "%s", d ? d : ".");
	error_data_init(&ED);
	cf_parser_init(&P);

	while (script_next(f, &sl, &lineno)) {
		if (!strcmp(sl.argv[0], "try")) {
			/* try CMD args ; CMD args ... : run in a child; state changes are discarded.
			 * Prints try-begin / output / try-end ok|crash. */
			printf("try-begin\n");
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				int start = 1;
				for (int i = 1; i <= sl.argc; i++) {
					if (i == sl.argc || !strcmp(sl.argv[i], ";")) {
						if (i > start)
							exec(i - start, sl.argv + start, sl.lineno);
						start = i + 1;
					}
				}
				fflush(stdout);
				_exit(0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			printf("try-end %s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "ok" : "crash");
		} else {
			exec1(&sl);
		}
	}
	fclose(f);
	return 0;
}
