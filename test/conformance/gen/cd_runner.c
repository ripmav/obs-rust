/*
 * Conformance runner for calldata (spec S-002). Executes a script, printing one
 * result line per query command. See test/conformance/README.md.
 */
#include <inttypes.h>

#include "callback/calldata.h"
#include "harness.h"

static bool is_null(const char *s)
{
	return strcmp(s, "@null") == 0;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	calldata_t cd;
	uint8_t *fixed_buf = NULL;
	memset(&cd, 0, sizeof(cd));

	while (script_next(f, &sl, &lineno)) {
		const char *cmd = sl.argv[0];

		if (!strcmp(cmd, "init")) {
			calldata_init(&cd);
		} else if (!strcmp(cmd, "init_fixed")) {
			script_need(&sl, 2);
			size_t n = (size_t)strtoull(sl.argv[1], NULL, 10);
			fixed_buf = calloc(1, n);
			calldata_init_fixed(&cd, fixed_buf, n);
		} else if (!strcmp(cmd, "clear")) {
			calldata_clear(&cd);
		} else if (!strcmp(cmd, "free")) {
			calldata_free(&cd);
			free(fixed_buf);
			fixed_buf = NULL;
			memset(&cd, 0, sizeof(cd));
		} else if (!strcmp(cmd, "set_int")) {
			script_need(&sl, 3);
			calldata_set_int(&cd, sl.argv[1], strtoll(sl.argv[2], NULL, 0));
		} else if (!strcmp(cmd, "set_float")) {
			script_need(&sl, 3);
			calldata_set_float(&cd, sl.argv[1], strtod(sl.argv[2], NULL));
		} else if (!strcmp(cmd, "set_bool")) {
			script_need(&sl, 3);
			calldata_set_bool(&cd, sl.argv[1], strtol(sl.argv[2], NULL, 0) != 0);
		} else if (!strcmp(cmd, "set_ptr")) {
			script_need(&sl, 3);
			calldata_set_ptr(&cd, sl.argv[1], (void *)(uintptr_t)strtoull(sl.argv[2], NULL, 0));
		} else if (!strcmp(cmd, "set_string")) {
			script_need(&sl, 3);
			calldata_set_string(&cd, sl.argv[1], is_null(sl.argv[2]) ? NULL : sl.argv[2]);
		} else if (!strcmp(cmd, "set_data")) {
			script_need(&sl, 3);
			size_t n;
			uint8_t *buf = script_parse_hex(sl.argv[2], &n);
			calldata_set_data(&cd, sl.argv[1], n ? buf : NULL, n);
			free(buf);
		} else if (!strcmp(cmd, "get_int")) {
			script_need(&sl, 2);
			long long v = 0x5a5a5a5a5a5a5a5aLL;
			if (calldata_get_int(&cd, sl.argv[1], &v))
				printf("get_int %s ok %lld\n", sl.argv[1], v);
			else
				printf("get_int %s fail\n", sl.argv[1]);
		} else if (!strcmp(cmd, "get_float")) {
			script_need(&sl, 2);
			double v = 0;
			if (calldata_get_float(&cd, sl.argv[1], &v))
				printf("get_float %s ok %.17g\n", sl.argv[1], v);
			else
				printf("get_float %s fail\n", sl.argv[1]);
		} else if (!strcmp(cmd, "get_bool")) {
			script_need(&sl, 2);
			bool v = false;
			if (calldata_get_bool(&cd, sl.argv[1], &v))
				printf("get_bool %s ok %d\n", sl.argv[1], v ? 1 : 0);
			else
				printf("get_bool %s fail\n", sl.argv[1]);
		} else if (!strcmp(cmd, "get_ptr")) {
			script_need(&sl, 2);
			void *v = NULL;
			if (calldata_get_ptr(&cd, sl.argv[1], &v))
				printf("get_ptr %s ok 0x%" PRIxPTR "\n", sl.argv[1], (uintptr_t)v);
			else
				printf("get_ptr %s fail\n", sl.argv[1]);
		} else if (!strcmp(cmd, "get_string")) {
			script_need(&sl, 2);
			const char *v = "unset";
			if (calldata_get_string(&cd, sl.argv[1], &v)) {
				if (v) {
					printf("get_string %s ok ", sl.argv[1]);
					script_print_quoted(v);
					printf("\n");
				} else
					printf("get_string %s ok @null\n", sl.argv[1]);
			} else {
				printf("get_string %s fail\n", sl.argv[1]);
			}
		} else if (!strcmp(cmd, "get_data")) {
			script_need(&sl, 3);
			size_t n = (size_t)strtoull(sl.argv[2], NULL, 10);
			uint8_t *buf = calloc(1, n ? n : 1);
			if (calldata_get_data(&cd, sl.argv[1], buf, n)) {
				printf("get_data %s %zu ok ", sl.argv[1], n);
				script_print_hex(buf, n);
				printf("\n");
			} else {
				printf("get_data %s %zu fail\n", sl.argv[1], n);
			}
			free(buf);
		} else if (!strcmp(cmd, "val_int")) {
			script_need(&sl, 2);
			printf("val_int %s %lld\n", sl.argv[1], calldata_int(&cd, sl.argv[1]));
		} else if (!strcmp(cmd, "val_string")) {
			script_need(&sl, 2);
			const char *v = calldata_string(&cd, sl.argv[1]);
			if (v) {
				printf("val_string %s ", sl.argv[1]);
				script_print_quoted(v);
				printf("\n");
			} else
				printf("val_string %s @null\n", sl.argv[1]);
		} else if (!strcmp(cmd, "dump")) {
			printf("dump size=%zu bytes=", cd.size);
			script_print_hex(cd.stack, cd.stack ? cd.size : 0);
			printf("\n");
		} else if (!strcmp(cmd, "capacity")) {
			printf("capacity %zu\n", cd.capacity);
		} else if (!strcmp(cmd, "echo")) {
			script_need(&sl, 2);
			printf("echo %s\n", sl.argv[1]);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, cmd);
			return 2;
		}
	}
	calldata_free(&cd);
	free(fixed_buf);
	fclose(f);
	return 0;
}
