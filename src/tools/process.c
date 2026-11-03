#define _GNU_SOURCE
#include "internal.h"
#include <limits.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define CAPTURE_LIMIT (256 * 1024)
#define CAPTURE_CHUNK 4096

static int set_environment(const struct tool_env *env) {

	for (; env && env->name; env++) {
		int result = env->value ? setenv(env->name, env->value, 1) : unsetenv(env->name);
		if (result) {
			return -1;
		}
	}

	return 0;
}

int tool_lower_limit(int resource, rlim_t maximum) {

	struct rlimit limit;
	if (getrlimit(resource, &limit)) {
		return -1;
	}
	if (limit.rlim_cur > maximum) {
		limit.rlim_cur = maximum;
	}
	if (limit.rlim_max > maximum) {
		limit.rlim_max = maximum;
	}

	return setrlimit(resource, &limit);
}

static void close_range_from(unsigned first, unsigned last) {

	if (first > last) {
		return;
	}
#ifdef SYS_close_range
	if (!syscall(SYS_close_range, first, last, 0)) {
		return;
	}
#endif
	long maximum = sysconf(_SC_OPEN_MAX);
	if (maximum < 0) {
		maximum = 65536;
	}
	if (last >= (unsigned long)maximum) {
		last = (unsigned)maximum - 1;
	}
	for (unsigned fd = first; fd <= last; fd++) {
		close((int)fd);
	}
}

/* Child-only. Do not leak transcript files, sockets or credentials through FDs. */
void tool_close_child_fds(int keep, int error_fd) {

	unsigned first = 3;
	for (int i = 0; i < 2; i++) {
		int next = keep < 3 ? error_fd : error_fd < 3 ? keep : keep < error_fd ? keep : error_fd;
		if (next < 3) {
			break;
		}
		close_range_from(first, (unsigned)next - 1);
		first = (unsigned)next + 1;
		if (next == keep) {
			keep = -1;
		}
		if (next == error_fd) {
			error_fd = -1;
		}
	}
	close_range_from(first, UINT_MAX);
}

static _Noreturn void exec_failed(int error_fd, int error) {

	if (error_fd >= 0) {
		tool_write_all(error_fd, &error, sizeof(error));
	} else {
		dprintf(STDERR_FILENO, "Cannot start tool: %s\n", strerror(error));
	}
	_exit(error == ENOENT ? 127 : 126);
}

/* One sandbox entry point for Bash AND Git/patch helpers. Only the child gets
   environment changes. The calling process's cwd defines the workspace. */
_Noreturn void tool_exec_child(
	const char *cwd, char *const command[], const struct tool_env *env, int keep_fd, int error_fd) {

	tool_close_child_fds(keep_fd, error_fd);
	if (!tools_sandbox_enabled()) {
		/* Provider/search credentials belong to Phi, not model-generated tools. */
		if ((cwd && chdir(cwd)) || unsetenv("TAVILY_API_KEY") || unsetenv("PHI_TAVILY_API_KEY") ||
			set_environment(env)) {
			exec_failed(error_fd, errno);
		}
		execvp(command[0], command);
		exec_failed(error_fd, errno);
	}

	const char *bwrap = !access("/usr/bin/bwrap", X_OK) ? "/usr/bin/bwrap"
		: !access("/bin/bwrap", X_OK)                   ? "/bin/bwrap"
														: NULL;
	if (!bwrap) {
		exec_failed(error_fd, ENOENT);
	}

	char workspace[PATH_MAX];
	if (!getcwd(workspace, sizeof(workspace))) {
		exec_failed(error_fd, errno);
	}
	if (!strcmp(workspace, "/") || (cwd && !tool_path_allowed(cwd))) {
		exec_failed(error_fd, EPERM);
	}

	/* Do not carry API keys, agent sockets, BASH_ENV, LD_PRELOAD or a
       workspace-controlled PATH into a restricted subprocess. */
	char term[128] = "xterm-256color";
	const char *old_term = getenv("TERM");
	if (old_term && strlen(old_term) < sizeof(term)) {
		strcpy(term, old_term);
	}
	if (clearenv() || setenv("PATH", "/usr/bin:/bin", 1) || setenv("HOME", "/tmp", 1) ||
		setenv("TMPDIR", "/tmp", 1) || setenv("XDG_CACHE_HOME", "/tmp/.cache", 1) ||
		setenv("XDG_CONFIG_HOME", "/tmp/.config", 1) || setenv("LANG", "C.UTF-8", 1) ||
		setenv("TERM", term, 1) || set_environment(env)) {
		exec_failed(error_fd, errno);
	}

	int strict = tools_sandbox_mode() == SANDBOX_STRICT;
	if (strict &&
		(tool_lower_limit(RLIMIT_CORE, 0) || tool_lower_limit(RLIMIT_NOFILE, 1024) ||
			tool_lower_limit(RLIMIT_NPROC, 256))) {
		exec_failed(error_fd, errno);
	}

	/* Size follows from the fixed bind list plus the command's argument count. */
	const char *system_paths[] = {"/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc/ld.so.cache",
		"/etc/ld.so.conf", "/etc/ld.so.conf.d", "/etc/alternatives", "/etc/passwd", "/etc/group",
		"/etc/nsswitch.conf", "/etc/resolv.conf", "/etc/hosts", "/etc/gai.conf", "/etc/localtime",
		"/etc/ssl", "/etc/ca-certificates", "/etc/pki"};
	size_t commands = 0;
	while (command[commands]) {
		commands++;
	}
	size_t capacity = 40 + 3 * (sizeof(system_paths) / sizeof(*system_paths)) + commands;
	char **argv = calloc(capacity, sizeof(*argv));
	if (!argv) {
		exec_failed(error_fd, ENOMEM);
	}
	size_t n = 0;
#define ARG(value) argv[n++] = (char *)(value)
	ARG(bwrap);
	ARG("--die-with-parent");
	ARG("--new-session");
	ARG("--unshare-all");
	ARG("--cap-drop");
	ARG("ALL");
	if (!strict) {
		ARG("--share-net");
	}
	ARG("--proc");
	ARG("/proc");
	ARG("--dev");
	ARG("/dev");
	ARG("--tmpfs");
	ARG("/tmp");
	for (size_t i = 0; i < sizeof(system_paths) / sizeof(*system_paths); i++) {
		ARG("--ro-bind-try");
		ARG(system_paths[i]);
		ARG(system_paths[i]);
	}
	ARG("--bind");
	ARG(workspace);
	ARG(workspace);
	ARG("--chdir");
	ARG(cwd ? cwd : workspace);
	ARG("--");
	for (size_t i = 0; i < commands; i++) {
		ARG(command[i]);
	}
#undef ARG
	execv(bwrap, argv); /* Absolute path: never execute a project-local fake bwrap. */
	exec_failed(error_fd, errno);
}

int tool_nonblocking(int fd) {

	int flags = fcntl(fd, F_GETFL);

	return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Bounded capture with a staged input file avoids pipe deadlocks on large
   patches. The read loop also yields to cancellation under continuous output. */
int tool_run_capture_env_at(const char *cwd, char *const argv[], const struct tool_env *env,
	const char *input, size_t input_size, int timeout_seconds, char **output, size_t *output_size,
	int *exit_code) {

	*output = NULL;
	*exit_code = -1;
	if (output_size) {
		*output_size = 0;
	}
	if (!argv || !argv[0] || (input_size && !input)) {
		errno = EINVAL;
		return -1;
	}
	if (tool_check_cancel()) {
		errno = ECANCELED;
		return -1;
	}

	int result = -1, saved_error = 0;
	int channel[2] = {-1, -1}, errors[2] = {-1, -1};
	int source_fd = -1, exited = 0, status = 0, eof = 0;
	FILE *source = NULL;
	pid_t pid = -1;
	char *buffer = NULL;
	size_t used = 0, capacity = CAPTURE_CHUNK;

	buffer = malloc(capacity + 1);
	if (!buffer) {
		goto done;
	}
	if (input_size) {
		source = tmpfile();
		if (!source) {
			goto done;
		}
		source_fd = fileno(source);
		if (tool_write_all(source_fd, input, input_size) || lseek(source_fd, 0, SEEK_SET) < 0) {
			goto done;
		}
	} else {
		source_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (source_fd < 0) {
			goto done;
		}
	}
	if (pipe2(channel, O_CLOEXEC) || pipe2(errors, O_CLOEXEC) || tool_nonblocking(channel[0])) {
		goto done;
	}

	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		goto done;
	}
	if (!pid) {
		if (setpgid(0, 0) || dup2(source_fd, STDIN_FILENO) < 0 ||
			dup2(channel[1], STDOUT_FILENO) < 0 || dup2(channel[1], STDERR_FILENO) < 0) {
			exec_failed(errors[1], errno);
		}
		tool_exec_child(cwd, argv, env, -1, errors[1]);
	}
	setpgid(pid, pid); /* Close the cancellation race with the child's setpgid. */
	close(channel[1]);
	channel[1] = -1;
	close(errors[1]);
	errors[1] = -1;
	if (source) {
		fclose(source);
		source = NULL;
	} else {
		close(source_fd);
	}
	source_fd = -1;
	double deadline = tool_monotonic_seconds() + (timeout_seconds > 0 ? timeout_seconds : 15);

	while (!exited || !eof) {
		if (tool_check_cancel()) {
			errno = ECANCELED;
			goto done;
		}
		if (tool_monotonic_seconds() >= deadline) {
			errno = ETIMEDOUT;
			goto done;
		}
		if (!eof) {
			char chunk[CAPTURE_CHUNK];
			ssize_t count = read(channel[0], chunk, sizeof(chunk));
			if (count > 0) {
				size_t size = (size_t)count;
				if (size > CAPTURE_LIMIT - used) {
					errno = EOVERFLOW;
					goto done;
				}
				if (used + size > capacity) {
					size_t next_size = capacity * 2;
					if (next_size < used + size) {
						next_size = used + size;
					}
					char *next = realloc(buffer, next_size + 1);
					if (!next) {
						goto done;
					}
					buffer = next;
					capacity = next_size;
				}
				memcpy(buffer + used, chunk, size);
				used += size;
			} else if (!count) {
				eof = 1;
			} else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				goto done;
			}
		}
		if (!exited) {
			pid_t waited = waitpid(pid, &status, WNOHANG);
			if (waited == pid) {
				exited = 1;
			} else if (waited < 0 && errno != EINTR) {
				goto done;
			}
		}
		if (exited && eof) {
			break;
		}
		struct pollfd poller = {.fd = eof ? -1 : channel[0], .events = POLLIN};
		if (poll(&poller, 1, 20) < 0 && errno != EINTR) {
			goto done;
		}
	}
	int exec_error = 0;
	ssize_t count;
	do {
		count = read(errors[0], &exec_error, sizeof(exec_error));
	} while (count < 0 && errno == EINTR);
	if (count > 0) {
		errno = exec_error;
		goto done;
	}
	if (count < 0) {
		goto done;
	}
	buffer[used] = '\0';
	*exit_code = tool_exit_code(status);
	*output = buffer;
	buffer = NULL;
	if (output_size) {
		*output_size = used;
	}
	result = 0;

done:
	saved_error = errno;
	if (pid > 0 && result) {
		tool_finish_process(pid, &status, &exited, 1);
	}
	if (source) {
		fclose(source);
	} else if (source_fd >= 0) {
		close(source_fd);
	}
	for (int i = 0; i < 2; i++) {
		if (channel[i] >= 0) {
			close(channel[i]);
		}
		if (errors[i] >= 0) {
			close(errors[i]);
		}
	}
	free(buffer);
	errno = saved_error;

	return result;
}

int tool_run_capture_at(const char *cwd, char *const argv[], const char *input, size_t input_size,
	int timeout_seconds, char **output, int *exit_code) {

	return tool_run_capture_env_at(
		cwd, argv, NULL, input, input_size, timeout_seconds, output, NULL, exit_code);
}

/* Message for errno after a failed helper run (Git/patch wording for ENOENT). */
const char *tool_process_error(void) {

	switch (errno) {
	case ECANCELED:
		return "Tool cancelled.";
	case ETIMEDOUT:
		return "Tool timed out.";
	case EOVERFLOW:
		return "Helper output exceeds 256 KiB.";
	case ENOENT:
		return tools_sandbox_enabled()
			? "Cannot start helper: install Git and bubblewrap for sandboxed Git/patch tools."
			: "Cannot start Git; install git or check PATH.";
	default:
		return strerror(errno);
	}
}

int tool_exit_code(int status) {

	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)) {
		return 128 + WTERMSIG(status);
	}

	return 1;
}

int tool_sandbox_unavailable(void) {

	return tools_sandbox_enabled() && access("/usr/bin/bwrap", X_OK) && access("/bin/bwrap", X_OK);
}

/* channel[0] is the controlling side kept by Phi, channel[1] the child's terminal. */
int tool_open_pty(int channel[2]) {

	channel[0] = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (channel[0] < 0 || grantpt(channel[0]) || unlockpt(channel[0])) {
		goto error;
	}
	char *name = ptsname(channel[0]);
	channel[1] = name ? open(name, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
	if (channel[1] < 0) {
		goto error;
	}
	return 0;
error:
	if (channel[0] >= 0) {
		close(channel[0]);
	}
	channel[0] = channel[1] = -1;

	return -1;
}

static void apply_sgr(struct tool_ansi *ansi) {

	if (ansi->value == 0) {
		ansi->color = -1;
		ansi->bold = 0;
	} else if (ansi->value == 1) {
		ansi->bold = 1;
	} else if (ansi->value >= 30 && ansi->value <= 37) {
		ansi->color = ansi->value - 30;
	} else if (ansi->value == 39) {
		ansi->color = -1;
	}
	ansi->value = 0;
}

/* Strip terminal controls in place, keeping text, newlines and tabs. CSI and
   OSC sequences may span chunks; the state carries over. SGR colour/bold is
   tracked, and segment() (optional) receives each run of text with the style
   in effect, so live output can keep basic colours. */
size_t tool_clean_output(char *chunk, size_t size, struct tool_ansi *ansi,
	void (*segment)(const char *text, size_t size, const struct tool_ansi *ansi)) {

	enum {
		TEXT,
		ESCAPE,
		CSI,
		OSC,
		OSC_ESCAPE
	};

	size_t kept = 0, start = 0;
	for (size_t i = 0; i < size; i++) {
		unsigned char c = (unsigned char)chunk[i];
		switch (ansi->state) {
		case TEXT:
			if (c == 27) {
				if (segment && kept > start) {
					segment(chunk + start, kept - start, ansi);
				}
				start = kept;
				ansi->state = ESCAPE;
			} else if (c == '\r' && i + 1 < size && chunk[i + 1] == '\n') {
				/* CRLF becomes LF. */
			} else if (c == '\n' || c == '\t' || (c >= 32 && c != 127)) {
				chunk[kept++] = (char)c;
			}
			break;
		case ESCAPE:
			ansi->state = c == '[' ? CSI : c == ']' ? OSC : TEXT;
			ansi->value = 0;
			break;
		case CSI:
			if (c >= '0' && c <= '9') {
				/* Malformed sequences must not overflow a signed int. */
				if (ansi->value < 1000) {
					ansi->value = ansi->value * 10 + c - '0';
				}
			} else if (c == ';') {
				apply_sgr(ansi);
			} else if (c >= 64 && c <= 126) {
				if (c == 'm') {
					apply_sgr(ansi);
				}
				ansi->value = 0;
				ansi->state = TEXT;
			}
			break;
		case OSC:
			if (c == 7) {
				ansi->state = TEXT;
			} else if (c == 27) {
				ansi->state = OSC_ESCAPE;
			}
			break;
		case OSC_ESCAPE:
			ansi->state = c == '\\' ? TEXT : OSC;
			break;
		}
	}
	if (segment && kept > start) {
		segment(chunk + start, kept - start, ansi);
	}

	return kept;
}

/* Host service for extensions: run argv (or a bash command) with the same
   sandbox, environment scrubbing, timeout and bounded capture as tools. */
json_object *tools_exec_request(json_object *request) {

	json_object *argv_value = json_object_object_get(request, "argv");
	const char *command = tool_plain_string(json_object_object_get(request, "command"), NULL);
	if (!!argv_value == !!command) {
		return tool_error("Give exactly one of argv (array of strings) or command (string)");
	}
	char *argv[258];
	size_t count = 0;
	if (command) {
		argv[count++] = "/bin/bash";
		argv[count++] = "-c";
		argv[count++] = (char *)command;
	} else {
		if (!json_object_is_type(argv_value, json_type_array) ||
			!json_object_array_length(argv_value) || json_object_array_length(argv_value) > 256) {
			return tool_error("argv must be an array of 1-256 strings");
		}
		for (size_t i = 0; i < json_object_array_length(argv_value); i++) {
			const char *item = tool_plain_string(json_object_array_get_idx(argv_value, i), NULL);
			if (!item) {
				return tool_error("argv must contain only strings without NUL bytes");
			}
			argv[count++] = (char *)item;
		}
	}
	argv[count] = NULL;
	json_object *cwd_value = json_object_object_get(request, "cwd");
	const char *cwd = cwd_value ? tool_plain_string(cwd_value, NULL) : NULL;
	if (cwd_value && !cwd) {
		return tool_error("cwd must be a string");
	}
	json_object *stdin_value = json_object_object_get(request, "stdin");
	if (stdin_value && !json_object_is_type(stdin_value, json_type_string)) {
		return tool_error("stdin must be a string");
	}
	const char *input = stdin_value ? json_object_get_string(stdin_value) : NULL;
	size_t input_size = stdin_value ? (size_t)json_object_get_string_len(stdin_value) : 0;
	int64_t timeout;
	if (!tool_integer_arg(request, "timeout", 30, 1, 600, &timeout)) {
		return tool_error("timeout must be 1-600 seconds");
	}
	if (tool_sandbox_unavailable()) {
		return tool_error("Sandbox is enabled but bubblewrap (bwrap) is not installed.");
	}

	char *output = NULL;
	size_t size = 0;
	int code = -1;
	if (tool_run_capture_env_at(
			cwd, argv, NULL, input, input_size, (int)timeout, &output, &size, &code)) {
		if (errno == ENOENT) {
			return tool_error("Cannot start program: not found");
		}
		if (errno == EPERM) {
			return tool_error("Sandbox blocks this working directory");
		}
		return tool_error(tool_process_error());
	}
	json_object *result = json_object_new_object();
	if (result) {
		json_object_object_add(result, "exit_code", json_object_new_int(code));
		json_object_object_add(result, "output", json_object_new_string_len(output, (int)size));
	}
	free(output);

	return result;
}
