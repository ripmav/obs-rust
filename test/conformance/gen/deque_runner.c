/*
 * Conformance runner for deque (spec S-005). Bytes are pushed as ASCII text so
 * that the ring contents are readable. "dump" prints every field of struct deque
 * and the raw backing buffer, because plugins' inline code manipulates those
 * fields directly.
 */
#include "util/deque.h"
#include "harness.h"

static void dump(const struct deque *dq)
{
	printf("dump size=%zu capacity=%zu start=%zu end=%zu data=", dq->size, dq->capacity, dq->start_pos,
	       dq->end_pos);
	if (!dq->capacity || !dq->data) {
		printf("-\n");
		return;
	}
	/* Bytes outside the live ring are not defined by the data structure (they may be
	 * uninitialised heap memory), so they are printed as ".." to keep vectors
	 * deterministic. */
	for (size_t i = 0; i < dq->capacity; i++) {
		size_t rel = (i + dq->capacity - dq->start_pos) % dq->capacity;
		if (rel < dq->size)
			printf("%02x", ((unsigned char *)dq->data)[i]);
		else
			printf("..");
	}
	printf("\n");
}

static void print_bytes(const char *label, const uint8_t *buf, size_t n)
{
	printf("%s ", label);
	script_print_hex(buf, n);
	printf("\n");
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	struct deque dq;
	deque_init(&dq);

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "init")) {
			deque_init(&dq);
		} else if (!strcmp(c, "free")) {
			deque_free(&dq);
		} else if (!strcmp(c, "reserve")) {
			script_need(&sl, 2);
			deque_reserve(&dq, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "push_back") || !strcmp(c, "push_front")) {
			script_need(&sl, 2);
			size_t n;
			uint8_t *buf = script_parse_hex(sl.argv[1], &n);
			if (c[5] == 'b')
				deque_push_back(&dq, buf, n);
			else
				deque_push_front(&dq, buf, n);
			free(buf);
		} else if (!strcmp(c, "push_back_zero")) {
			script_need(&sl, 2);
			deque_push_back_zero(&dq, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "push_front_zero")) {
			script_need(&sl, 2);
			deque_push_front_zero(&dq, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "place")) {
			script_need(&sl, 3);
			size_t n;
			uint8_t *buf = script_parse_hex(sl.argv[2], &n);
			deque_place(&dq, (size_t)strtoull(sl.argv[1], NULL, 10), buf, n);
			free(buf);
		} else if (!strcmp(c, "upsize")) {
			script_need(&sl, 2);
			deque_upsize(&dq, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "peek_front") || !strcmp(c, "peek_back") || !strcmp(c, "pop_front") ||
			   !strcmp(c, "pop_back")) {
			script_need(&sl, 2);
			size_t n = (size_t)strtoull(sl.argv[1], NULL, 10);
			uint8_t *buf = calloc(1, n ? n : 1);
			if (!strcmp(c, "peek_front"))
				deque_peek_front(&dq, buf, n);
			else if (!strcmp(c, "peek_back"))
				deque_peek_back(&dq, buf, n);
			else if (!strcmp(c, "pop_front"))
				deque_pop_front(&dq, buf, n);
			else
				deque_pop_back(&dq, buf, n);
			print_bytes(c, buf, n);
			free(buf);
		} else if (!strcmp(c, "pop_front_discard")) {
			script_need(&sl, 2);
			deque_pop_front(&dq, NULL, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "pop_back_discard")) {
			script_need(&sl, 2);
			deque_pop_back(&dq, NULL, (size_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "dump")) {
			dump(&dq);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	deque_free(&dq);
	fclose(f);
	return 0;
}
