/*
 * Conformance runner for dstr and the string helpers (spec S-006).
 * Two dstrs: a (default) and b (prefix "b:"). A command that takes a second
 * dstr uses the other one. Printf-style commands take the format then one int
 * and one string argument ("%d" and "%s" conversions only).
 */
#include <inttypes.h>
#include "util/dstr.h"
#include "harness.h"

static void dump(const char *name, const struct dstr *d)
{
	printf("%s len=%zu cap=%zu str=", name, d->len, d->capacity);
	if (d->array)
		script_print_quoted(d->array);
	else
		printf("@null");
	printf("\n");
}
static const char *arg(const struct script_line *sl, int i)
{
	return (i < sl->argc && strcmp(sl->argv[i], "@null")) ? sl->argv[i] : NULL;
}
static size_t num(const struct script_line *sl, int i)
{
	return (size_t)strtoull(sl->argv[i], NULL, 10);
}
static int sign(int v)
{
	return v < 0 ? -1 : v > 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	struct dstr A, B;
	dstr_init(&A);
	dstr_init(&B);

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		struct dstr *d = &A, *o = &B;
		const char *nm = "a";
		if (!strncmp(c, "b:", 2)) {
			d = &B;
			o = &A;
			nm = "b";
			c += 2;
		}
		if (!strcmp(c, "init"))
			dstr_init(d);
		else if (!strcmp(c, "free"))
			dstr_free(d);
		else if (!strcmp(c, "copy"))
			dstr_copy(d, arg(&sl, 1));
		else if (!strcmp(c, "copy_other"))
			dstr_copy_dstr(d, o);
		else if (!strcmp(c, "ncopy"))
			dstr_ncopy(d, arg(&sl, 1), num(&sl, 2));
		else if (!strcmp(c, "ncopy_other"))
			dstr_ncopy_dstr(d, o, num(&sl, 1));
		else if (!strcmp(c, "cat"))
			dstr_cat(d, arg(&sl, 1));
		else if (!strcmp(c, "cat_other"))
			dstr_cat_dstr(d, o);
		else if (!strcmp(c, "ncat"))
			dstr_ncat(d, arg(&sl, 1), num(&sl, 2));
		else if (!strcmp(c, "ncat_other"))
			dstr_ncat_dstr(d, o, num(&sl, 1));
		else if (!strcmp(c, "cat_ch"))
			dstr_cat_ch(d, sl.argv[1][0]);
		else if (!strcmp(c, "insert"))
			dstr_insert(d, num(&sl, 1), arg(&sl, 2));
		else if (!strcmp(c, "insert_other"))
			dstr_insert_dstr(d, num(&sl, 1), o);
		else if (!strcmp(c, "insert_ch"))
			dstr_insert_ch(d, num(&sl, 1), sl.argv[2][0]);
		else if (!strcmp(c, "remove"))
			dstr_remove(d, num(&sl, 1), num(&sl, 2));
		else if (!strcmp(c, "replace"))
			dstr_replace(d, sl.argv[1], arg(&sl, 2));
		else if (!strcmp(c, "depad"))
			dstr_depad(d);
		else if (!strcmp(c, "resize"))
			dstr_resize(d, num(&sl, 1));
		else if (!strcmp(c, "reserve"))
			dstr_reserve(d, num(&sl, 1));
		else if (!strcmp(c, "ensure"))
			dstr_ensure_capacity(d, num(&sl, 1));
		else if (!strcmp(c, "left"))
			dstr_left(d, o, num(&sl, 1));
		else if (!strcmp(c, "mid"))
			dstr_mid(d, o, num(&sl, 1), num(&sl, 2));
		else if (!strcmp(c, "right"))
			dstr_right(d, o, num(&sl, 1));
		else if (!strcmp(c, "upper"))
			dstr_to_upper(d);
		else if (!strcmp(c, "lower"))
			dstr_to_lower(d);
		else if (!strcmp(c, "printf"))
			dstr_printf(d, sl.argv[1], (int)strtol(sl.argv[2], NULL, 10), sl.argv[3]);
		else if (!strcmp(c, "catf"))
			dstr_catf(d, sl.argv[1], (int)strtol(sl.argv[2], NULL, 10), sl.argv[3]);
		else if (!strcmp(c, "safe_printf"))
			dstr_safe_printf(d, sl.argv[1], arg(&sl, 2), arg(&sl, 3), arg(&sl, 4), arg(&sl, 5));
		else if (!strcmp(c, "find") || !strcmp(c, "find_i")) {
			const char *r = c[4] == '_' ? dstr_find_i(d, sl.argv[1]) : dstr_find(d, sl.argv[1]);
			if (r)
				printf("%s %s offset=%td\n", c, nm, r - d->array);
			else
				printf("%s %s none\n", c, nm);
		} else if (!strcmp(c, "cmp"))
			printf("cmp %s %d\n", nm, sign(dstr_cmp(d, arg(&sl, 1))));
		else if (!strcmp(c, "cmpi"))
			printf("cmpi %s %d\n", nm, sign(dstr_cmpi(d, arg(&sl, 1))));
		else if (!strcmp(c, "ncmp"))
			printf("ncmp %s %d\n", nm, sign(dstr_ncmp(d, arg(&sl, 1), num(&sl, 2))));
		else if (!strcmp(c, "ncmpi"))
			printf("ncmpi %s %d\n", nm, sign(dstr_ncmpi(d, arg(&sl, 1), num(&sl, 2))));
		else if (!strcmp(c, "is_empty"))
			printf("is_empty %s %d\n", nm, dstr_is_empty(d) ? 1 : 0);
		else if (!strcmp(c, "end"))
			printf("end %s %d\n", nm, (int)dstr_end(d));
		else if (!strcmp(c, "astrcmpi"))
			printf("astrcmpi %d\n", sign(astrcmpi(arg(&sl, 1), arg(&sl, 2))));
		else if (!strcmp(c, "astrcmp_n"))
			printf("astrcmp_n %d\n", sign(astrcmp_n(arg(&sl, 1), arg(&sl, 2), num(&sl, 3))));
		else if (!strcmp(c, "astrcmpi_n"))
			printf("astrcmpi_n %d\n", sign(astrcmpi_n(arg(&sl, 1), arg(&sl, 2), num(&sl, 3))));
		else if (!strcmp(c, "astrstri")) {
			const char *r = astrstri(arg(&sl, 1), arg(&sl, 2));
			if (r)
				printf("astrstri offset=%td\n", r - sl.argv[1]);
			else
				printf("astrstri none\n");
		} else if (!strcmp(c, "strdepad")) {
			char buf[256];
			snprintf(buf, sizeof buf, "%s", sl.argv[1]);
			strdepad(buf);
			printf("strdepad ");
			script_print_quoted(buf);
			printf("\n");
		} else if (!strcmp(c, "strlist_split")) {
			char **l = strlist_split(arg(&sl, 1), sl.argv[2][0], strtol(sl.argv[3], NULL, 10) != 0);
			if (!l)
				printf("strlist @null\n");
			else {
				printf("strlist");
				for (char **p = l; *p; p++) {
					printf(" ");
					script_print_quoted(*p);
				}
				printf("\n");
				strlist_free(l);
			}
		} else if (!strcmp(c, "dump"))
			dump(nm, d);
		else if (!strcmp(c, "info"))
			printf("%s len=%zu cap=%zu\n", nm, d->len, d->capacity);
		else if (!strcmp(c, "terminated"))
			printf("%s terminated=%d\n", nm, d->array ? d->array[d->len] == 0 : -1);
		else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	dstr_free(&A);
	dstr_free(&B);
	fclose(f);
	return 0;
}
