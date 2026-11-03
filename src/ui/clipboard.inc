/* Clipboard helper for Wayland/X11. No shell, no image decoder, no model call. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int clipboard_fetch(const char *mime, int fd, size_t cap) {

	if (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0) {
		return 0;
	}
	pid_t pid = fork();
	if (pid < 0) {
		return 0;
	}
	if (!pid) {
		struct rlimit limit = {cap, cap};
		/* Text has no application cap; retain any inherited OS limit. */
		if (cap != RLIM_INFINITY && setrlimit(RLIMIT_FSIZE, &limit)) {
			_exit(126);
		}
		int nullfd = open("/dev/null", O_RDWR);
		if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 || dup2(nullfd, STDERR_FILENO) < 0 ||
			dup2(fd, STDOUT_FILENO) < 0) {
			_exit(126);
		}
		if (nullfd > 2) {
			close(nullfd);
		}
		if (fd > 2) {
			close(fd);
		}
		if (getenv("WAYLAND_DISPLAY")) {
			execlp("wl-paste", "wl-paste", "--no-newline", "--type", mime, (char *)NULL);
		} else {
			execlp(
				"xclip", "xclip", "-selection", "clipboard", "-out", "-target", mime, (char *)NULL);
		}
		_exit(127);
	}
	double deadline = now() + 2;
	int status = 0;
	for (;;) {
		pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == pid) {
			break;
		}
		if (result < 0 && errno != EINTR) {
			return 0;
		}
		if (now() >= deadline) {
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			}
			return 0;
		}
		struct timespec pause = {0, 10000000};
		nanosleep(&pause, NULL);
	}
	struct stat st;

	return WIFEXITED(status) && WEXITSTATUS(status) == 0 && !fstat(fd, &st) && st.st_size > 0 &&
		(size_t)st.st_size <= cap;
}

/* Returns allocated UTF-8 text or an image path. Image files remain for reuse. */
static char *clipboard_paste(void) {

	char directory[] = "/tmp/phi-clipboard-XXXXXX";
	if (!mkdtemp(directory)) {
		return NULL;
	}
	char path[128];
	const char *types[] = {"image/png", "image/jpeg", "image/webp"};
	const char *extensions[] = {"png", "jpg", "webp"};
	for (size_t i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%s/image.%s", directory, extensions[i]);
		int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
		if (fd < 0) {
			break;
		}
		int ok = clipboard_fetch(types[i], fd, 16 * 1024 * 1024);
		close(fd);
		if (ok) {
			char *result = strdup(path);
			if (result) {
				return result;
			}
		}
		unlink(path);
	}
	snprintf(path, sizeof(path), "%s/text", directory);
	int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
	char *result = NULL;
	if (fd >= 0) {
		if (clipboard_fetch(
				getenv("WAYLAND_DISPLAY") ? "text" : "UTF8_STRING", fd, RLIM_INFINITY)) {
			struct stat st;
			if (!fstat(fd, &st) && st.st_size >= 0 && (uintmax_t)st.st_size < SIZE_MAX &&
				lseek(fd, 0, SEEK_SET) >= 0) {
				result = malloc((size_t)st.st_size + 1);
				if (result) {
					size_t filled = 0;
					while (filled < (size_t)st.st_size) {
						ssize_t n = read(fd, result + filled, (size_t)st.st_size - filled);
						if (n < 0 && errno == EINTR) {
							continue;
						}
						if (n <= 0) {
							break;
						}
						filled += (size_t)n;
					}
					if (filled != (size_t)st.st_size || memchr(result, 0, filled)) {
						free(result);
						result = NULL;
					} else {
						result[filled] = 0;
					}
				}
			}
		}
		close(fd);
		unlink(path);
	}
	rmdir(directory);

	return result;
}

/* Store UTF-8 text in the system clipboard without invoking a shell. */
static int clipboard_copy(const char *text, size_t len) {

	if (!text) {
		return 0;
	}
	int pipefd[2];
	if (pipe(pipefd)) {
		return 0;
	}
	fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return 0;
	}
	if (!pid) {
		int nullfd = open("/dev/null", O_RDWR);
		if (nullfd < 0 || dup2(pipefd[0], STDIN_FILENO) < 0 || dup2(nullfd, STDOUT_FILENO) < 0 ||
			dup2(nullfd, STDERR_FILENO) < 0) {
			_exit(126);
		}
		if (pipefd[0] > 2) {
			close(pipefd[0]);
		}
		close(pipefd[1]);
		if (nullfd > 2) {
			close(nullfd);
		}
		if (getenv("WAYLAND_DISPLAY")) {
			execlp("wl-copy", "wl-copy", "--type", "text/plain;charset=utf-8", (char *)NULL);
		} else {
			execlp("xclip", "xclip", "-selection", "clipboard", "-in", "-target", "UTF8_STRING",
				(char *)NULL);
		}
		_exit(127);
	}
	close(pipefd[0]);
	size_t written = 0;
	int ok = 1;
	struct sigaction ignored = {0}, previous = {0};
	ignored.sa_handler = SIG_IGN;
	sigemptyset(&ignored.sa_mask);
	sigaction(SIGPIPE, &ignored, &previous);
	while (written < len) {
		ssize_t n = write(pipefd[1], text + written, len - written);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			ok = 0;
			break;
		}
		written += (size_t)n;
	}
	close(pipefd[1]);
	sigaction(SIGPIPE, &previous, NULL);
	double deadline = now() + 2;
	int status = 0;
	for (;;) {
		pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == pid) {
			break;
		}
		if (result < 0 && errno != EINTR) {
			ok = 0;
			break;
		}
		if (now() >= deadline) {
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			}
			return 0;
		}
		struct timespec pause = {0, 10000000};
		nanosleep(&pause, NULL);
	}

	return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
