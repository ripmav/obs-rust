/*
 * Conformance runner for darray (spec S-005). Elements are int32_t so that the
 * element size is visible. Every command that mutates prints nothing; "dump"
 * prints the state. Preconditions guarded by assert() in darray.h must be
 * respected by scripts.
 */
#include "util/darray.h"
#include "harness.h"

static void dump(const struct darray *da)
{
	printf("dump num=%zu capacity=%zu items=", da->num, da->capacity);
	if (!da->num)
		printf("-");
	for (size_t i = 0; i < da->num; i++)
		printf("%s%d", i ? "," : "", ((int32_t *)da->array)[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	struct darray a, b;
	darray_init(&a);
	darray_init(&b);
	const size_t es = sizeof(int32_t);

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		struct darray *d = &a;
		/* optional leading "b:" selects the second array */
		if (!strncmp(c, "b:", 2)) {
			d = &b;
			c += 2;
		}
		if (!strcmp(c, "init")) {
			darray_init(d);
		} else if (!strcmp(c, "free")) {
			darray_free(d);
		} else if (!strcmp(c, "clear")) {
			darray_clear(d);
		} else if (!strcmp(c, "reserve")) {
			script_need(&sl, 2);
			darray_reserve(es, d, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "ensure")) {
			script_need(&sl, 2);
			darray_ensure_capacity(es, d, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "resize")) {
			script_need(&sl, 2);
			darray_resize(es, d, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "push")) {
			script_need(&sl, 2);
			int32_t v = (int32_t)strtol(sl.argv[1], NULL, 10);
			size_t idx = darray_push_back(es, d, &v);
			printf("push idx=%zu\n", idx);
		} else if (!strcmp(c, "push_new")) {
			int32_t *p = darray_push_back_new(es, d);
			printf("push_new value=%d\n", *p);
		} else if (!strcmp(c, "push_array")) {
			int32_t tmp[16];
			size_t n = 0;
			for (int i = 1; i < sl.argc && n < 16; i++)
				tmp[n++] = (int32_t)strtol(sl.argv[i], NULL, 10);
			size_t idx = darray_push_back_array(es, d, tmp, n);
			printf("push_array idx=%zu\n", idx);
		} else if (!strcmp(c, "insert")) {
			script_need(&sl, 3);
			int32_t v = (int32_t)strtol(sl.argv[2], NULL, 10);
			darray_insert(es, d, (size_t)strtoull(sl.argv[1], NULL, 10), &v);
		} else if (!strcmp(c, "insert_new")) {
			script_need(&sl, 2);
			int32_t *p = darray_insert_new(es, d, (size_t)strtoull(sl.argv[1], NULL, 10));
			printf("insert_new value=%d\n", *p);
		} else if (!strcmp(c, "insert_array")) {
			script_need(&sl, 3);
			int32_t tmp[16];
			size_t n = 0;
			for (int i = 2; i < sl.argc && n < 16; i++)
				tmp[n++] = (int32_t)strtol(sl.argv[i], NULL, 10);
			darray_insert_array(es, d, (size_t)strtoull(sl.argv[1], NULL, 10), tmp, n);
		} else if (!strcmp(c, "erase")) {
			script_need(&sl, 2);
			darray_erase(es, d, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "erase_item")) {
			script_need(&sl, 2);
			int32_t v = (int32_t)strtol(sl.argv[1], NULL, 10);
			darray_erase_item(es, d, &v);
		} else if (!strcmp(c, "erase_range")) {
			script_need(&sl, 3);
			darray_erase_range(es, d, (size_t)strtoull(sl.argv[1], NULL, 10),
					   (size_t)strtoull(sl.argv[2], NULL, 10));
		} else if (!strcmp(c, "find")) {
			script_need(&sl, 3);
			int32_t v = (int32_t)strtol(sl.argv[1], NULL, 10);
			size_t idx = darray_find(es, d, &v, (size_t)strtoull(sl.argv[2], NULL, 10));
			if (idx == DARRAY_INVALID)
				printf("find %d invalid\n", v);
			else
				printf("find %d idx=%zu\n", v, idx);
		} else if (!strcmp(c, "copy_from_other")) {
			darray_copy(es, d, d == &a ? &b : &a);
		} else if (!strcmp(c, "move_from_other")) {
			darray_move(d, d == &a ? &b : &a);
		} else if (!strcmp(c, "push_darray_from_other")) {
			size_t idx = darray_push_back_darray(es, d, d == &a ? &b : &a);
			printf("push_darray idx=%zu\n", idx);
		} else if (!strcmp(c, "dump")) {
			printf("%s", d == &a ? "a " : "b ");
			dump(d);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	darray_free(&a);
	darray_free(&b);
	fclose(f);
	return 0;
}
