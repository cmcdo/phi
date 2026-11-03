#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "extension_platform.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void *phi_dynlib_open(const char *path, char *error, size_t size) {

	dlerror();
	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		snprintf(error, size, "%s", dlerror());
	}

	return handle;
}

void *phi_dynlib_symbol(void *handle, const char *name, char *error, size_t size) {

	dlerror();
	void *symbol = dlsym(handle, name);
	const char *problem = dlerror();
	if (problem) {
		snprintf(error, size, "%s", problem);
		return NULL;
	}

	return symbol;
}

void phi_dynlib_close(void *handle) {

	if (handle) {
		dlclose(handle);
	}
}

const char *phi_dynlib_suffix(void) {

#ifdef __APPLE__
	return ".dylib";
#else

	return ".so";
#endif
}

/* json-c is linked into every extension: Phi already loads it, and parsing
   tool arguments or API replies without it means hand-writing a parser.
   build.sh passes the host's pkg-config flags; these are fallbacks. */
#ifndef PHI_EXTENSION_JSON_CFLAGS
#define PHI_EXTENSION_JSON_CFLAGS "-I/usr/include/json-c"
#endif
#ifndef PHI_EXTENSION_JSON_LIBS
#define PHI_EXTENSION_JSON_LIBS "-ljson-c"
#endif

const char *phi_extension_build_id(void) {

#ifdef __APPLE__
	return "phi-ext-v1:posix:darwin:c11:O2:fPIC:hidden:dynamiclib:json-c:recipe2";
#else

	return "phi-ext-v1:posix:elf:c11:O2:fPIC:hidden:shared:json-c:recipe2";
#endif
}

/* Split a space-separated flag string into argv slots (no quoting). */
static size_t split_flags(char *text, char **argv, size_t capacity) {

	size_t count = 0;
	for (char *token = strtok(text, " "); token && count < capacity; token = strtok(NULL, " ")) {
		argv[count++] = token;
	}

	return count;
}

static volatile sig_atomic_t compiler_pid;

void phi_abort_extension_compile(void) {

	if (compiler_pid > 0) {
		kill(-(pid_t)compiler_pid, SIGKILL);
	}
}

static void close_compiler_fds(long maximum) {

#ifdef SYS_close_range
	if (!syscall(SYS_close_range, 3u, UINT_MAX, 0)) {
		return;
	}
#endif
	for (long fd = 3; fd < maximum; fd++) {
		close((int)fd);
	}
}

static double seconds(void) {

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	return now.tv_sec + now.tv_nsec / 1e9;
}

static int pipe_cloexec(int pair[2]) {

	if (pipe(pair)) {
		return -1;
	}
	if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) || fcntl(pair[1], F_SETFD, FD_CLOEXEC)) {
		int error = errno;
		close(pair[0]);
		close(pair[1]);
		pair[0] = pair[1] = -1;
		errno = error;
		return -1;
	}

	return 0;
}

static int lower_limit(int resource, rlim_t maximum) {

	struct rlimit limit;
	if (getrlimit(resource, &limit)) {
		return -1;
	}
	if (limit.rlim_cur > maximum) {
		limit.rlim_cur = maximum;
	}

	return setrlimit(resource, &limit);
}

int phi_compile_extension(const char *directory, const char *compiler, int timeout_seconds,
	int (*cancel)(void), struct phi_compile_result *result) {

	*result = (struct phi_compile_result){.exit_code = -1};
	if (!directory || !compiler || compiler[0] != '/' || timeout_seconds < 1) {
		snprintf(result->text, sizeof(result->text), "Invalid compiler configuration");
		return 0;
	}
	int channel[2] = {-1, -1}, status = 0, exited = 0, eof = 0;
	pid_t pid = -1;
	sigset_t blocked, previous_mask;
	int signals_blocked = 0;
	size_t used = 0;
	if (pipe_cloexec(channel)) {
		goto system_error;
	}
	int flags = fcntl(channel[0], F_GETFL);
	if (flags < 0 || fcntl(channel[0], F_SETFL, flags | O_NONBLOCK)) {
		goto system_error;
	}
	long max_fds = sysconf(_SC_OPEN_MAX);
	if (max_fds < 0) {
		max_fds = 65536;
	}
	fflush(NULL);
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &blocked, &previous_mask)) {
		goto system_error;
	}
	signals_blocked = 1;
	pid = fork();
	if (pid < 0) {
		goto system_error;
	}
	if (!pid) {
		if (setpgid(0, 0) || chdir(directory) || dup2(channel[1], STDOUT_FILENO) < 0 ||
			dup2(channel[1], STDERR_FILENO) < 0) {
			_exit(126);
		}
		int input = open("/dev/null", O_RDONLY);
		if (input < 0 || dup2(input, STDIN_FILENO) < 0) {
			_exit(126);
		}
		/* POSIX fallback, intentionally outside the rest of the extension code. */
		close_compiler_fds(max_fds);
		sigprocmask(SIG_SETMASK, &previous_mask, NULL);
		if (lower_limit(RLIMIT_CORE, 0) || lower_limit(RLIMIT_FSIZE, 16 * 1024 * 1024) ||
			lower_limit(RLIMIT_CPU, (rlim_t)timeout_seconds)) {
			_exit(126);
		}
#ifdef __linux__
		if (lower_limit(RLIMIT_AS, 1024u * 1024u * 1024u)) {
			_exit(126);
		}
#endif
		char *env[] = {"PATH=/usr/bin:/bin", "HOME=.", "TMPDIR=.", "LC_ALL=C", NULL};
		char json_cflags[] = PHI_EXTENSION_JSON_CFLAGS, json_libs[] = PHI_EXTENSION_JSON_LIBS;
		char *argv[48] = {(char *)compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-fPIC",
			"-fvisibility=hidden", "-fdiagnostics-color=never",
#ifdef __APPLE__
			"-dynamiclib", "-Wl,-undefined,error",
#else
			"-shared", "-Wl,-z,defs", "-Wl,-z,noexecstack",
#endif
			"-I."};
		size_t n = 0;
		while (argv[n]) {
			n++;
		}
		n += split_flags(json_cflags, argv + n, 8);
		argv[n++] = "extension.c";
		n += split_flags(json_libs, argv + n, 8);
		argv[n++] = "-o";
		argv[n++] = "module";
		argv[n] = NULL;
		execve(compiler, argv, env);
		dprintf(STDERR_FILENO, "Cannot execute compiler: %s\n", strerror(errno));
		_exit(127);
	}
	setpgid(pid, pid);
	compiler_pid = (sig_atomic_t)pid;
	sigprocmask(SIG_SETMASK, &previous_mask, NULL);
	signals_blocked = 0;
	close(channel[1]);
	channel[1] = -1;
	double deadline = seconds() + timeout_seconds;
	while (!exited || !eof) {
		if (cancel && cancel()) {
			result->cancelled = 1;
			break;
		}
		if (seconds() >= deadline) {
			result->timed_out = 1;
			break;
		}
		/* Bound each drain pass too: a noisy compiler cannot starve cancellation. */
		char chunk[8192];
		ssize_t count = eof ? -1 : read(channel[0], chunk, sizeof(chunk));
		if (count > 0) {
			size_t n = (size_t)count;
			if (n > PHI_EXTENSION_DIAGNOSTICS - used) {
				n = PHI_EXTENSION_DIAGNOSTICS - used;
				result->truncated = 1;
			}
			for (size_t i = 0; i < n; i++) {
				unsigned char ch = (unsigned char)chunk[i];
				result->text[used++] = ch == '\n' || ch == '\t' || ch >= 32 ? (char)ch : '?';
			}
		} else if (!count) {
			eof = 1;
		} else if (!eof && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			goto system_error;
		}
		if (!exited) {
			pid_t done = waitpid(pid, &status, WNOHANG);
			if (done == pid) {
				exited = 1;
			} else if (done < 0 && errno != EINTR) {
				goto system_error;
			}
		}
		if (exited && eof) {
			break;
		}
		struct pollfd wait = {.fd = eof ? -1 : channel[0], .events = POLLIN};
		if (poll(&wait, 1, 20) < 0 && errno != EINTR) {
			goto system_error;
		}
	}
	result->text[used] = 0;
	/* Also clean up descendants if a compiler/wrapper exits before its children. */
	kill(-pid, SIGKILL);
	if (!exited) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
	}
	compiler_pid = 0;
	close(channel[0]);
	result->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
		: WIFSIGNALED(status)             ? 128 + WTERMSIG(status)
										  : 1;
	return !result->cancelled && !result->timed_out && !result->exit_code;

system_error: {
	int error = errno;
	if (pid > 0) {
		kill(-pid, SIGKILL);
		if (!exited) {
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			}
		}
	}
	compiler_pid = 0;
	if (signals_blocked) {
		sigprocmask(SIG_SETMASK, &previous_mask, NULL);
	}
	if (channel[0] >= 0) {
		close(channel[0]);
	}
	if (channel[1] >= 0) {
		close(channel[1]);
	}
	snprintf(result->text, sizeof(result->text), "Compiler process error: %s", strerror(error));
	return 0;
}
}
