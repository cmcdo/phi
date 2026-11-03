#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "internal.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/resource.h>

#define BASH_OUTPUT_BYTES (50 * 1024)
#define BASH_OUTPUT_LINES 2000
#define BASH_SPILL_BYTES (64 * 1024 * 1024)
#define BASH_CHUNK_BYTES 4096

static char *build_shell_command(const char *command, int pty, int persist) {

	if (!pty) {
		return strdup(command);
	}

	const char *prefix = getenv("NO_COLOR") ? ""
											: "shopt -s expand_aliases\n"
											  "alias ls='ls --color=auto'\n";
	const char *suffix = persist ? "\n_phi_status=$?\n"
								   "pwd -P >&3\n"
								   "exit $_phi_status"
								 : "";

	size_t size = strlen(prefix) + strlen(command) + strlen(suffix) + 1;
	char *result = malloc(size);
	if (result) {
		snprintf(result, size, "%s%s%s", prefix, command, suffix);
	}

	return result;
}

static const char *open_output_channel(int pty, int channel[2]) {

	if (!pty) {
		return pipe(channel) ? "Cannot create pipe" : NULL;
	}

	return tool_open_pty(channel) ? "Cannot create pseudo-terminal" : NULL;
}

static _Noreturn void exec_child(
	const char *command, int pty_columns, int channel[2], int persist, int cwd_channel[2]) {

	setpgid(0, 0);
	if (pty_columns > 0) {
		struct winsize size = {.ws_col = (unsigned short)pty_columns, .ws_row = 24};
		ioctl(channel[1], TIOCSWINSZ, &size);
		setenv("TERM", "xterm-256color", 1);
		char columns[16];
		snprintf(columns, sizeof(columns), "%d", pty_columns);
		setenv("COLUMNS", columns, 1);
	}

	int null = open("/dev/null", O_RDONLY);
	if (null < 0 || dup2(null, STDIN_FILENO) < 0 || dup2(channel[1], STDOUT_FILENO) < 0 ||
		dup2(channel[1], STDERR_FILENO) < 0) {
		_exit(126);
	}
	close(null);
	close(channel[0]);
	close(channel[1]);

	if (persist) {
		close(cwd_channel[0]);
		if (dup2(cwd_channel[1], 3) < 0) {
			_exit(126);
		}
		if (cwd_channel[1] != 3) {
			close(cwd_channel[1]);
		}
	}

	char *argv[] = {"/bin/bash", "-c", (char *)command, NULL};
	tool_exec_child(NULL, argv, NULL, persist ? 3 : -1, -1);
}

static void display_segment(const char *text, size_t size, const struct tool_ansi *ansi) {

	char shown[BASH_CHUNK_BYTES + 1];
	memcpy(shown, text, size);
	shown[size] = '\0';
	tool_display_styled(shown, ansi->color, ansi->bold);
}

struct output_capture {
	char tail[BASH_OUTPUT_BYTES];
	size_t used;
	size_t newlines;
	size_t saved;
	FILE *spill;
	int spill_attempted;
	int spill_failed;
	int truncated;
	char spill_path[sizeof("/tmp/phi-bash-XXXXXX")];
};

static void start_spill(struct output_capture *capture) {

	if (capture->spill_attempted) {
		return;
	}
	capture->spill_attempted = 1;
	memcpy(capture->spill_path, "/tmp/phi-bash-XXXXXX", sizeof(capture->spill_path));

	int fd = mkstemp(capture->spill_path);
	capture->spill = fd >= 0 ? fdopen(fd, "wb") : NULL;
	if (!capture->spill) {
		if (fd >= 0) {
			close(fd);
			unlink(capture->spill_path);
		}
		capture->spill_failed = 1;
		return;
	}
	if (capture->used && fwrite(capture->tail, 1, capture->used, capture->spill) != capture->used) {
		capture->spill_failed = 1;
	} else {
		capture->saved = capture->used;
	}
}

static void spill_chunk(struct output_capture *capture, const char *chunk, size_t size) {

	if (!capture->spill || capture->spill_failed) {
		return;
	}
	if (size > BASH_SPILL_BYTES - capture->saved ||
		fwrite(chunk, 1, size, capture->spill) != size) {
		capture->spill_failed = 1;
		return;
	}
	capture->saved += size;
}

static void capture_output(struct output_capture *capture, const char *chunk, size_t size) {

	size_t chunk_newlines = 0;
	for (size_t i = 0; i < size; i++) {
		if (chunk[i] == '\n') {
			chunk_newlines++;
		}
	}

	int byte_overflow = size > sizeof(capture->tail) - capture->used;
	if (!capture->spill_attempted &&
		(byte_overflow || capture->newlines + chunk_newlines >= BASH_OUTPUT_LINES)) {
		start_spill(capture);
	}
	spill_chunk(capture, chunk, size);
	capture->newlines += chunk_newlines;

	/* Input arrives in BASH_CHUNK_BYTES blocks, smaller than the tail buffer. */
	if (byte_overflow) {
		size_t drop = capture->used + size - sizeof(capture->tail);
		memmove(capture->tail, capture->tail + drop, capture->used - drop);
		capture->used -= drop;
		capture->truncated = 1;
	}
	memcpy(capture->tail + capture->used, chunk, size);
	capture->used += size;
}

static size_t trim_to_line_limit(struct output_capture *capture) {

	size_t start = 0, lines = 0;
	for (size_t i = capture->used; i > 0; i--) {
		if (capture->tail[i - 1] == '\n' && i != capture->used && ++lines == BASH_OUTPUT_LINES) {
			start = i;
			capture->truncated = 1;
			break;
		}
	}
	if (capture->truncated) {
		while (start < capture->used && ((unsigned char)capture->tail[start] & 0xc0) == 0x80) {
			start++;
		}
	}

	return start;
}

static void update_working_directory(int fd, json_object *result) {

	char path[4096];
	/* A background descendant can retain the directory pipe after Bash exits.
       No directory report is better than blocking the entire agent forever. */
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
		close(fd);
		return;
	}
	ssize_t size = read(fd, path, sizeof(path) - 1);
	close(fd);
	if (size <= 0) {
		return;
	}

	while (size && (path[size - 1] == '\n' || path[size - 1] == '\r')) {
		size--;
	}
	path[size] = '\0';
	if (*path && path[0] == '/' && !chdir(path)) {
		json_object_object_add(result, "working_directory", json_object_new_string(path));
	}
}

static json_object *run(json_object *args) {

	json_object *command_value = NULL;
	const char *raw_command = NULL;
	if (!args || !json_object_object_get_ex(args, "command", &command_value) ||
		!(raw_command = tool_plain_string(command_value, NULL))) {
		return tool_error("Expected a command string");
	}

	int64_t timeout = 30;
	if (!tool_integer_arg(args, "timeout", 30, 1, 3600, &timeout)) {
		return tool_error("timeout must be 1..3600 seconds or null for 30");
	}

	if (tool_sandbox_unavailable()) {
		return tool_error("Sandbox is enabled but bubblewrap (bwrap) is not installed.");
	}

	int pty_columns = tool_pty_columns();
	int pty = pty_columns > 0;
	int persist = tool_persist_cwd();
	char *command = build_shell_command(raw_command, pty, persist);
	if (!command) {
		return tool_error("Not enough memory");
	}

	int cwd_channel[2] = {-1, -1};
	if (persist && pipe(cwd_channel)) {
		free(command);
		return tool_error("Cannot track working directory");
	}

	int channel[2];
	const char *channel_error = open_output_channel(pty, channel);
	if (channel_error) {
		if (persist) {
			close(cwd_channel[0]);
			close(cwd_channel[1]);
		}
		free(command);
		return tool_error(channel_error);
	}

	fflush(NULL);
	pid_t pid = fork();
	if (pid == 0) {
		exec_child(command, pty_columns, channel, persist, cwd_channel);
	}

	close(channel[1]);
	if (persist) {
		close(cwd_channel[1]);
	}
	free(command);
	if (pid < 0) {
		close(channel[0]);
		if (persist) {
			close(cwd_channel[0]);
		}
		return tool_error("Cannot start bash");
	}
	setpgid(pid, pid);

	if (tool_show_banners()) {
		tool_report(stderr, "[bash: %s]\n", raw_command);
	}

	struct output_capture capture = {0};
	struct tool_ansi ansi = {.color = -1};
	int status = 0, exited = 0, eof = 0;
	int cancelled = 0, timed_out = 0, detached = 0;
	double deadline = tool_monotonic_seconds() + (double)timeout;
	/* `cmd &` leaves a descendant holding the output open after Bash exits.
       Drain briefly, then stop rather than waiting for the full timeout. */
	double settle = 0, settle_limit = 0;

	while (!exited || !eof) {
		if (tool_check_cancel()) {
			cancelled = 1;
			break;
		}
		if (tool_monotonic_seconds() >= deadline) {
			timed_out = 1;
			break;
		}

		struct pollfd pollfd = {channel[0], POLLIN, 0};
		int ready = poll(&pollfd, 1, 100);
		if (ready > 0 && (pollfd.revents & (POLLIN | POLLHUP))) {
			char chunk[BASH_CHUNK_BYTES];
			ssize_t count = read(channel[0], chunk, sizeof(chunk));
			if (count == 0) {
				eof = 1;
			} else if (count > 0) {
				if (exited) {
					settle = tool_monotonic_seconds() + 0.5;
				}
				size_t clean_size =
					tool_clean_output(chunk, (size_t)count, &ansi, pty ? display_segment : NULL);
				if (!pty && clean_size) {
					char shown[BASH_CHUNK_BYTES + 1];
					memcpy(shown, chunk, clean_size);
					shown[clean_size] = '\0';
					tool_display(shown, stderr);
				}
				capture_output(&capture, chunk, clean_size);
			} else if (errno != EINTR) {
				break;
			}
		} else if (ready < 0 && errno != EINTR) {
			break;
		}

		if (!exited && waitpid(pid, &status, WNOHANG) == pid) {
			exited = 1;
			settle = tool_monotonic_seconds() + 0.5;
			settle_limit = tool_monotonic_seconds() + 2.0;
		}
		if (exited && !eof) {
			double now = tool_monotonic_seconds();
			if (now >= settle || now >= settle_limit) {
				detached = 1;
				break;
			}
		}
		if (eof && !exited) {
			struct timespec pause = {0, 100000000};
			nanosleep(&pause, NULL);
		}
	}

	close(channel[0]);
	tool_finish_process(pid, &status, &exited, cancelled || timed_out || !eof);
	size_t output_start = trim_to_line_limit(&capture);

	if (capture.spill && fclose(capture.spill)) {
		capture.spill_failed = 1;
	}
	if (!capture.truncated && capture.spill_attempted && capture.spill) {
		unlink(capture.spill_path);
	}

	json_object *result = json_object_new_object();
	if (capture.truncated && capture.spill) {
		json_object_object_add(result, "output_file", json_object_new_string(capture.spill_path));
		json_object_object_add(
			result, "output_file_complete", json_object_new_boolean(!capture.spill_failed));
	}
	if (persist) {
		update_working_directory(cwd_channel[0], result);
	}
	json_object_object_add(result, "output",
		json_object_new_string_len(
			capture.tail + output_start, (int)(capture.used - output_start)));
	json_object_object_add(result, "exit_code", json_object_new_int(tool_exit_code(status)));
	json_object_object_add(result, "cancelled", json_object_new_boolean(cancelled));
	json_object_object_add(result, "timed_out", json_object_new_boolean(timed_out));
	json_object_object_add(result, "truncated", json_object_new_boolean(capture.truncated));
	if (detached) {
		json_object_object_add(result, "note",
			json_object_new_string(
				"Background processes started by this command kept its output open and were stopped. "
				"Use bash_start for long-running or background commands."));
	}

	return result;
}

const struct phi_tool phi_tool_bash = {.name = "bash",
	.schema =
		"{\"type\":\"function\",\"name\":\"bash\","
		"\"description\":\"Run Bash without interactive stdin. Returns last 2000 lines or 50 KiB of combined output, exit_code and timed_out. Truncated output is saved to output_file (up to 64 MiB; check output_file_complete). timeout is seconds, null defaults to 30.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\"},"
		"\"timeout\":{\"type\":[\"integer\",\"null\"],"
		"\"minimum\":1,\"maximum\":3600}},"
		"\"required\":[\"command\",\"timeout\"],"
		"\"additionalProperties\":false},\"strict\":true}",
	.detail_key = "command",
	.run = run};
