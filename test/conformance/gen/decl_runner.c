/*
 * Conformance runner for the declaration parser (spec S-003).
 *
 *   parse <decl>    parse one declaration string and print the result
 */
#include <sys/wait.h>
#include <unistd.h>

#include "callback/decl.h"
#include "harness.h"

static const char *type_name(enum call_param_type t)
{
	switch (t) {
	case CALL_PARAM_TYPE_VOID:
		return "void";
	case CALL_PARAM_TYPE_INT:
		return "int";
	case CALL_PARAM_TYPE_FLOAT:
		return "float";
	case CALL_PARAM_TYPE_BOOL:
		return "bool";
	case CALL_PARAM_TYPE_PTR:
		return "ptr";
	case CALL_PARAM_TYPE_STRING:
		return "string";
	}
	return "?";
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;

	while (script_next(f, &sl, &lineno)) {
		if (!strcmp(sl.argv[0], "parse")) {
			script_need(&sl, 2);
			printf("decl ");
			script_print_quoted(sl.argv[1]);
			printf("\n");
			fflush(stdout);

			/* Each parse runs in a child so that a crash inside the reference
			 * implementation is recorded as "parse crash" instead of aborting
			 * the whole script. A conforming implementation must report
			 * "parse fail" wherever the reference crashes (see README). */
			pid_t pid = fork();
			if (pid == 0) {
				struct decl_info decl = {0};
				if (parse_decl_string(&decl, sl.argv[1])) {
					printf("parse ok name=%s params=%zu\n", decl.name, decl.params.num);
					for (size_t i = 0; i < decl.params.num; i++) {
						struct decl_param *p = decl.params.array + i;
						printf("  param %zu name=%s type=%s in=%d out=%d\n", i, p->name,
						       type_name(p->type), (p->flags & CALL_PARAM_IN) ? 1 : 0,
						       (p->flags & CALL_PARAM_OUT) ? 1 : 0);
					}
					decl_info_free(&decl);
				} else {
					printf("parse fail\n");
				}
				fflush(stdout);
				_exit(0);
			}
			int status = 0;
			waitpid(pid, &status, 0);
			if (!WIFEXITED(status))
				printf("parse crash\n");
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, sl.argv[0]);
			return 2;
		}
	}
	fclose(f);
	return 0;
}
