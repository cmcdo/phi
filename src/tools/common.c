#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <limits.h>

static struct tool_host host = {.banners = 1};
static void (*update_hook)(const char *text, void *context);
static void *update_context;

void tools_set_update_hook(void (*hook)(const char *text, void *context), void *context) {

	update_hook = hook;
	update_context = context;
}

void tools_get_update_hook(void (**hook)(const char *text, void *context), void **context) {

	*hook = update_hook;
	*context = update_context;
}

static int pty_columns, persist_cwd, sandbox_mode;

void tools_user_pty(int columns) {

	persist_cwd = columns < 0;
	pty_columns = columns < 0 ? -columns : columns;
}

int tool_pty_columns(void) {

	return pty_columns;
}

int tool_persist_cwd(void) {

	return persist_cwd;
}

void tools_set_sandbox(int mode) {

	/* Invalid nonzero values must never weaken a requested sandbox. */
	sandbox_mode = mode >= SANDBOX_OFF && mode <= SANDBOX_STRICT ? mode : SANDBOX_STRICT;
}

int tools_sandbox_mode(void) {

	return sandbox_mode;
}

int tools_sandbox_enabled(void) {

	return sandbox_mode != 0;
}

void tools_get_host(struct tool_host *value) {

	if (value) {
		*value = host;
	}
}

void tools_set_host(const struct tool_host *value) {

	host = value ? *value : (struct tool_host){.banners = 1};
}

int tool_show_banners(void) {

	return host.banners;
}

int tool_check_cancel(void) {

	return host.cancel ? host.cancel() : 0;
}

int tool_has_diff(void) {

	return host.diff != NULL;
}

void tool_diff(char kind, size_t line, const char *text, size_t size, size_t first, size_t end) {

	if (host.diff) {
		host.diff(kind, line, text, size, first, end);
	}
}

void tool_write_preview(const char *text) {

	if (host.write_preview) {
		host.write_preview(text);
	}
}

void tool_read_preview(const char *text) {

	if (host.read_preview) {
		host.read_preview(text);
	}
}

void tool_display_styled(const char *text, int color, int bold) {

	if (update_hook) {
		update_hook(text, update_context);
	}
	if (host.styled) {
		host.styled(text, color, bold);
	} else {
		tool_display(text, stderr);
	}
}

void tool_display(const char *text, FILE *destination) {

	if (update_hook) {
		update_hook(text, update_context);
	}
	if (host.display) {
		host.display(text, destination);
	} else {
		fputs(text, destination);
		fflush(destination);
	}
}

int tool_report(FILE *destination, const char *format, ...) {

	char local[512];
	va_list args, retry;
	va_start(args, format);
	va_copy(retry, args);
	int size = vsnprintf(local, sizeof(local), format, args);
	va_end(args);

	if (size >= 0 && (size_t)size < sizeof(local)) {
		tool_display(local, destination);
	} else if (size >= 0) {
		char *text = malloc((size_t)size + 1);
		if (text) {
			vsnprintf(text, (size_t)size + 1, format, retry);
			tool_display(text, destination);
			free(text);
		}
	}
	va_end(retry);

	return size;
}

json_object *tool_error(const char *message) {

	json_object *answer = json_object_new_object();
	if (!answer) {
		return NULL;
	}
	json_object_object_add(answer, "error", json_object_new_string(message));

	return answer;
}

int tool_write_all(int fd, const void *data, size_t size) {

	const char *bytes = data;
	while (size) {
		ssize_t written = write(fd, bytes, size);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			if (!written) {
				errno = EIO;
			}
			return -1;
		}
		bytes += (size_t)written;
		size -= (size_t)written;
	}

	return 0;
}

void tool_finish_process(pid_t pid, int *status, int *exited, int terminate_group) {

	if (terminate_group) {
		kill(-pid, SIGKILL);
	}
	if (*exited) {
		return;
	}
	if (terminate_group) {
		kill(pid, SIGKILL);
	}
	while (waitpid(pid, status, 0) < 0 && errno == EINTR) {
	}
	*exited = 1;
}

/* Expand only ~/; never interpret shell expressions in model-supplied paths. */
char *expand_path(const char *path) {

	if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
		const char *home = getenv("HOME");
		if (!home) {
			return NULL;
		}
		size_t length = strlen(home) + strlen(path + 1) + 1;
		char *expanded = malloc(length);
		if (expanded) {
			snprintf(expanded, length, "%s%s", home, path + 1);
		}
		return expanded;
	}

	return strdup(path);
}
