/*
 * Conformance runner for config-file (spec S-007). One config object `c`.
 * The temp file is <CONFORMANCE_TMPDIR>/cfg.ini (default "."). Item values are
 * only observable through the getters and through the saved file.
 */
#include <inttypes.h>
#include "util/config-file.h"
#include "harness.h"

static char path[512], path2[512];

static void print_file(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (!f) {
		printf("nofile\n");
		return;
	}
	char buf[65536];
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	script_print_quoted(buf);
	printf("\n");
}
static const char *s(const struct script_line *sl, int i)
{
	return strcmp(sl->argv[i], "@null") ? sl->argv[i] : NULL;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	config_t *c = NULL;
	const char *dir = getenv("CONFORMANCE_TMPDIR");
	snprintf(path, sizeof path, "%s/cfg.ini", dir ? dir : ".");
	snprintf(path2, sizeof path2, "%s/cfg.ini.new", dir ? dir : ".");
	remove(path);
	remove(path2);
	snprintf(path2, sizeof path2, "%s/cfg.ini.bak", dir ? dir : ".");
	remove(path2);

	while (script_next(f, &sl, &lineno)) {
		const char *cmd = sl.argv[0];
		if (!strcmp(cmd, "write_file")) {
			FILE *o = fopen(path, "wb");
			fwrite(sl.argv[1], 1, strlen(sl.argv[1]), o);
			fclose(o);
		} else if (!strcmp(cmd, "open_file")) {
			config_close(c);
			c = NULL;
			int rc = config_open(&c, path, !strcmp(sl.argv[1], "always") ? CONFIG_OPEN_ALWAYS : CONFIG_OPEN_EXISTING);
			printf("open_file rc=%d\n", rc);
		} else if (!strcmp(cmd, "open_string")) {
			config_close(c);
			c = NULL;
			printf("open_string rc=%d\n", config_open_string(&c, sl.argv[1]));
		} else if (!strcmp(cmd, "create")) {
			config_close(c);
			c = config_create(path);
			printf("create %s\n", c ? "ok" : "fail");
		} else if (!strcmp(cmd, "open_defaults")) {
			printf("open_defaults rc=%d\n", config_open_defaults(c, path));
		} else if (!strcmp(cmd, "close")) {
			config_close(c);
			c = NULL;
		} else if (!strcmp(cmd, "save")) {
			printf("save rc=%d\n", config_save(c));
		} else if (!strcmp(cmd, "save_safe")) {
			printf("save_safe rc=%d\n", config_save_safe(c, s(&sl, 1), s(&sl, 2)));
		} else if (!strcmp(cmd, "file")) {
			print_file(sl.argc > 1 ? (snprintf(path2, sizeof path2, "%s%s", path, sl.argv[1]), path2) : path);
		} else if (!strcmp(cmd, "sections")) {
			size_t n = config_num_sections(c);
			printf("sections %zu", n);
			for (size_t i = 0; i < n; i++)
				printf(" [%s]", config_get_section(c, i));
			printf("\n");
		} else if (!strcmp(cmd, "section")) {
			const char *n = config_get_section(c, (size_t)strtoull(sl.argv[1], NULL, 10));
			printf("section %s %s\n", sl.argv[1], n ? n : "@null");
		} else if (!strcmp(cmd, "set_string"))
			config_set_string(c, sl.argv[1], sl.argv[2], s(&sl, 3));
		else if (!strcmp(cmd, "set_int"))
			config_set_int(c, sl.argv[1], sl.argv[2], strtoll(sl.argv[3], NULL, 0));
		else if (!strcmp(cmd, "set_uint"))
			config_set_uint(c, sl.argv[1], sl.argv[2], strtoull(sl.argv[3], NULL, 0));
		else if (!strcmp(cmd, "set_bool"))
			config_set_bool(c, sl.argv[1], sl.argv[2], strtol(sl.argv[3], NULL, 0) != 0);
		else if (!strcmp(cmd, "set_double"))
			config_set_double(c, sl.argv[1], sl.argv[2], strtod(sl.argv[3], NULL));
		else if (!strcmp(cmd, "set_default_string"))
			config_set_default_string(c, sl.argv[1], sl.argv[2], s(&sl, 3));
		else if (!strcmp(cmd, "set_default_int"))
			config_set_default_int(c, sl.argv[1], sl.argv[2], strtoll(sl.argv[3], NULL, 0));
		else if (!strcmp(cmd, "set_default_uint"))
			config_set_default_uint(c, sl.argv[1], sl.argv[2], strtoull(sl.argv[3], NULL, 0));
		else if (!strcmp(cmd, "set_default_bool"))
			config_set_default_bool(c, sl.argv[1], sl.argv[2], strtol(sl.argv[3], NULL, 0) != 0);
		else if (!strcmp(cmd, "set_default_double"))
			config_set_default_double(c, sl.argv[1], sl.argv[2], strtod(sl.argv[3], NULL));
		else if (!strncmp(cmd, "get_", 4) || !strncmp(cmd, "default_", 8)) {
			bool def = cmd[0] == 'd';
			const char *kind = cmd + (def ? 8 : 4);
			printf("%s %s %s ", cmd, sl.argv[1], sl.argv[2]);
			if (!strcmp(kind, "string")) {
				const char *v = def ? config_get_default_string(c, sl.argv[1], sl.argv[2])
						    : config_get_string(c, sl.argv[1], sl.argv[2]);
				if (v)
					script_print_quoted(v);
				else
					printf("@null");
				printf("\n");
			} else if (!strcmp(kind, "int"))
				printf("%" PRId64 "\n", def ? config_get_default_int(c, sl.argv[1], sl.argv[2])
							: config_get_int(c, sl.argv[1], sl.argv[2]));
			else if (!strcmp(kind, "uint"))
				printf("%" PRIu64 "\n", def ? config_get_default_uint(c, sl.argv[1], sl.argv[2])
							 : config_get_uint(c, sl.argv[1], sl.argv[2]));
			else if (!strcmp(kind, "bool"))
				printf("%d\n", (def ? config_get_default_bool(c, sl.argv[1], sl.argv[2])
					     : config_get_bool(c, sl.argv[1], sl.argv[2])) ? 1 : 0);
			else if (!strcmp(kind, "double"))
				printf("%.17g\n", def ? config_get_default_double(c, sl.argv[1], sl.argv[2])
						      : config_get_double(c, sl.argv[1], sl.argv[2]));
			else {
				fprintf(stderr, "script: line %d: bad getter\n", sl.lineno);
				return 2;
			}
		} else if (!strcmp(cmd, "has_user"))
			printf("has_user %s %s %d\n", sl.argv[1], sl.argv[2], config_has_user_value(c, sl.argv[1], sl.argv[2]));
		else if (!strcmp(cmd, "has_default"))
			printf("has_default %s %s %d\n", sl.argv[1], sl.argv[2],
			       config_has_default_value(c, sl.argv[1], sl.argv[2]));
		else if (!strcmp(cmd, "remove"))
			printf("remove %s %s %d\n", sl.argv[1], sl.argv[2], config_remove_value(c, sl.argv[1], sl.argv[2]));
		else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, cmd);
			return 2;
		}
	}
	config_close(c);
	fclose(f);
	return 0;
}
