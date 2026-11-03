#ifndef PHI_TOOL_INTERNAL_H
#define PHI_TOOL_INTERNAL_H
#include "../tools.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <stdint.h>
#include <stdarg.h>

#include "../core/json.h"

static inline const char *tool_plain_string(json_object *value, size_t *size) {

	if (!value || !json_object_is_type(value, json_type_string)) {
		return NULL;
	}
	const char *text = json_object_get_string(value);
	size_t length = (size_t)json_object_get_string_len(value);
	if (strlen(text) != length) {
		return NULL;
	}
	if (size) {
		*size = length;
	}

	return text;
}

static inline double tool_monotonic_seconds(void) {

	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);

	return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

/* Nullable integer arguments use their default; present values must be in range. */
static inline int tool_integer_arg(json_object *args, const char *name, int64_t fallback,
	int64_t minimum, int64_t maximum, int64_t *result) {

	json_object *value = NULL;
	*result = fallback;
	if (!json_object_object_get_ex(args, name, &value) || !value) {
		return 1;
	}
	if (!json_object_is_type(value, json_type_int)) {
		return 0;
	}
	int64_t parsed = json_object_get_int64(value);
	if (parsed < minimum || parsed > maximum) {
		return 0;
	}
	*result = parsed;

	return 1;
}

char *expand_path(const char *path);
json_object *tool_error(const char *message);
int tool_write_all(int fd, const void *data, size_t size);
/* -1: unchanged; 0: committed and synced; 1: committed, directory sync failed. */
int tool_atomic_replace(const char *path, const void *data, size_t size,
	const struct stat *expected, int create_parents);
int tool_open_read(const char *path);

struct tool_env {
	const char *name;
	const char *value;
};

void tool_close_child_fds(int keep_fd, int error_fd);
_Noreturn void tool_exec_child(
	const char *cwd, char *const argv[], const struct tool_env *env, int keep_fd, int error_fd);
int tool_run_capture_env_at(const char *cwd, char *const argv[], const struct tool_env *env,
	const char *input, size_t input_size, int timeout_seconds, char **output, size_t *output_size,
	int *exit_code);
int tool_run_capture_at(const char *cwd, char *const argv[], const char *input, size_t input_size,
	int timeout_seconds, char **output, int *exit_code);
int tool_git_run(const char *cwd, char *const arguments[], const struct tool_env *extra,
	const char *input, size_t input_size, int timeout, char **output, size_t *output_size,
	int *code);
const char *tool_process_error(void);
int tool_exit_code(int status); /* wait status -> shell-style exit code */
int tool_nonblocking(int fd);
int tool_open_pty(int channel[2]); /* 0 on success; both ends close-on-exec */
int tool_sandbox_unavailable(void); /* sandbox requested but bwrap missing */
#include <sys/resource.h>
int tool_lower_limit(int resource, rlim_t maximum);

struct tool_ansi {
	int state, color, bold, value;
}; /* zero-init, then color = -1 */

size_t tool_clean_output(char *chunk, size_t size, struct tool_ansi *ansi,
	void (*segment)(const char *text, size_t size, const struct tool_ansi *ansi));
void tool_finish_process(pid_t pid, int *status, int *exited, int terminate_group);
void tool_display(const char *text, FILE *destination);
void tool_display_styled(const char *text, int color, int bold);
int tool_check_cancel(void);
int tool_show_banners(void);
int tool_has_diff(void);
int tool_pty_columns(void);
int tool_persist_cwd(void);
int tool_path_allowed(const char *path);
void tool_diff(char kind, size_t line, const char *text, size_t size, size_t first, size_t end);
void tool_write_preview(const char *text);
void tool_read_preview(const char *text);
#endif
