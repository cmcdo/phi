#define _POSIX_C_SOURCE 200809L
#include "internal.h"

/* Keep repository selection and temporary indexes local to each child. Read
   actions cannot invoke pagers, hooks, fsmonitor or external diff helpers.
   Git content filters remain normal Git behaviour for checkpoint/restore;
   those operations therefore require approval in dangerous mode. */
int tool_git_run(const char *cwd, char *const arguments[], const struct tool_env *extra,
	const char *input, size_t input_size, int timeout, char **output, size_t *output_size,
	int *code) {

	const char *prefix[] = {"git", "--no-pager", "-c", "core.hooksPath=/dev/null", "-c",
		"core.fsmonitor=false", "-c", "commit.gpgSign=false", "-c", "gc.auto=0", "-c",
		"maintenance.auto=false", "-c", "submodule.recurse=false", "-c", "color.ui=false"};
	const struct tool_env clean[] = {{"GIT_DIR", NULL}, {"GIT_COMMON_DIR", NULL},
		{"GIT_WORK_TREE", NULL}, {"GIT_INDEX_FILE", NULL}, {"GIT_OBJECT_DIRECTORY", NULL},
		{"GIT_ALTERNATE_OBJECT_DIRECTORIES", NULL}, {"GIT_NAMESPACE", NULL}, {"GIT_PREFIX", NULL},
		{"GIT_CONFIG", NULL}, {"GIT_CONFIG_COUNT", "0"}, {"GIT_CONFIG_PARAMETERS", NULL},
		{"GIT_ATTR_SOURCE", NULL}, {"GIT_CEILING_DIRECTORIES", NULL},
		{"GIT_DISCOVERY_ACROSS_FILESYSTEM", NULL}, {"GIT_LITERAL_PATHSPECS", "1"},
		{"GIT_TERMINAL_PROMPT", "0"}, {"GIT_PAGER", "cat"}, {"GIT_OPTIONAL_LOCKS", "0"}};
	size_t count = 0, extra_count = 0;
	while (arguments[count]) {
		count++;
	}
	while (extra && extra[extra_count].name) {
		extra_count++;
	}
	size_t prefix_count = sizeof(prefix) / sizeof(*prefix);
	size_t clean_count = sizeof(clean) / sizeof(*clean);
	char **argv = calloc(prefix_count + count + 1, sizeof(*argv));
	struct tool_env *env = calloc(clean_count + extra_count + 1, sizeof(*env));
	if (!argv || !env) {
		free(argv);
		free(env);
		*output = NULL;
		*code = -1;
		if (output_size) {
			*output_size = 0;
		}
		errno = ENOMEM;
		return -1;
	}
	for (size_t i = 0; i < prefix_count; i++) {
		argv[i] = (char *)prefix[i];
	}
	for (size_t i = 0; i < count; i++) {
		argv[prefix_count + i] = arguments[i];
	}
	memcpy(env, clean, sizeof(clean));
	if (extra_count) {
		memcpy(env + clean_count, extra, extra_count * sizeof(*extra));
	}
	int result = tool_run_capture_env_at(
		cwd, argv, env, input, input_size, timeout, output, output_size, code);
	int saved = errno;
	free(argv);
	free(env);
	errno = saved;

	return result;
}
