/* The viewer side of a hosted chat: connect a terminal to phi-host's socket
   and pass keys and screen through until the chat asks to detach or ends.
   Shared by phi-host (phi-host attach) and Phi's own viewer, which attaches
   in-process rather than starting another program. */
#define _GNU_SOURCE
#include "host_attach.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

int phi_host_write_all(int fd, const void *data, size_t size) {

	const char *bytes = data;
	while (size) {
		ssize_t n = write(fd, bytes, size);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 && errno == EAGAIN) {
			struct pollfd wait = {fd, POLLOUT, 0};
			if (poll(&wait, 1, 2000) <= 0) {
				return 0; /* a stuck viewer is dropped */
			}
			continue;
		}
		if (n <= 0) {
			return 0;
		}
		bytes += n;
		size -= (size_t)n;
	}

	return 1;
}

int phi_host_send_frame(int fd, char type, const void *data, size_t size) {

	if (size > PHI_HOST_FRAME_MAX) {
		errno = EMSGSIZE;
		return 0;
	}
	unsigned char header[3] = {
		(unsigned char)type, (unsigned char)(size >> 8), (unsigned char)size};

	return phi_host_write_all(fd, header, 3) && phi_host_write_all(fd, data, size);
}

int phi_host_connect_socket(const char *path) {

	struct sockaddr_un address = {.sun_family = AF_UNIX};
	if (strlen(path) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(address.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof(address))) {
		close(fd);
		return -1;
	}

	return fd;
}

static volatile sig_atomic_t window_changed = 1;

static void on_winch(int signal_number) {

	(void)signal_number;
	window_changed = 1;
}

static int attach_view(const char *path, const char *name, int backgrounded) {

	int fd = phi_host_connect_socket(path);
	if (fd < 0) {
		fprintf(stderr, "phi: cannot attach to %s: %s\n", path, strerror(errno));
		return 1;
	}
	struct termios saved, raw;
	int terminal = isatty(STDIN_FILENO) && !tcgetattr(STDIN_FILENO, &saved);
	if (terminal) {
		raw = saved;
		cfmakeraw(&raw);
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}
	/* Also run inside Phi's own viewer: keep its handlers to put back. */
	struct sigaction action = {.sa_handler = on_winch}, old_winch, old_pipe,
					 ignore = {.sa_handler = SIG_IGN};
	sigaction(SIGWINCH, &action, &old_winch);
	sigaction(SIGPIPE, &ignore, &old_pipe);
	window_changed = 1; /* the first size frame makes the program repaint */
	char buffer[16384], input[PHI_HOST_FRAME_MAX];
	size_t tail = 0; /* bytes held back: a possible start of DETACH_REQUEST */
	int detached = 0, ended = 0, closing = 0;
	while (!detached && !ended) {
		if (window_changed) {
			window_changed = 0;
			struct winsize size = {.ws_row = 24, .ws_col = 80};
			ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
			unsigned char data[8] = {size.ws_row >> 8, size.ws_row & 255, size.ws_col >> 8,
				size.ws_col & 255, size.ws_xpixel >> 8, size.ws_xpixel & 255, size.ws_ypixel >> 8,
				size.ws_ypixel & 255};
			if (!phi_host_send_frame(fd, 'w', data, 8)) {
				ended = 1;
				break;
			}
			/* The size frame registers the viewer for output. Only then ask
               the worker to repaint: output emitted before it is dropped. */
			if (backgrounded) {
				backgrounded = 0;
				if (!phi_host_send_frame(fd, 'b', NULL, 0)) {
					ended = 1;
					break;
				}
			}
		}
		/* A detach/close request is a handoff boundary, not ordinary screen
           output. Process it before forwarding any more keyboard input to
           the old chat. A split request must finish before input resumes. */
		struct pollfd fds[2] = {{tail ? -1 : STDIN_FILENO, POLLIN, 0}, {fd, POLLIN, 0}};
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(fd, buffer + tail, sizeof(buffer) - tail);
			if (n <= 0) {
				ended = 1;
				break;
			}
			size_t size = tail + (size_t)n;
			char *request = memmem(buffer, size, DETACH_REQUEST, sizeof(DETACH_REQUEST) - 1);
			char *close_request = memmem(buffer, size, CLOSE_REQUEST, sizeof(CLOSE_REQUEST) - 1);
			if (close_request && (!request || close_request < request)) {
				request = close_request;
				closing = 1;
			}
			if (request) {
				phi_host_write_all(STDOUT_FILENO, buffer, (size_t)(request - buffer));
				detached = 1;
				break;
			}
			/* Hold back a trailing partial request until the next read. */
			tail = 0;
			for (size_t keep = sizeof(DETACH_REQUEST) - 2; keep > 0; keep--) {
				if (size >= keep &&
					(!memcmp(buffer + size - keep, DETACH_REQUEST, keep) ||
						(keep < sizeof(CLOSE_REQUEST) &&
							!memcmp(buffer + size - keep, CLOSE_REQUEST, keep)))) {
					tail = keep;
					break;
				}
			}
			phi_host_write_all(STDOUT_FILENO, buffer, size - tail);
			memmove(buffer, buffer + size - tail, tail);
		}
		if (!tail && (fds[0].revents & (POLLIN | POLLHUP))) {
			/* Keep a partial output control sequence intact while forwarding
               input. Its bytes are buffered separately until the next read. */
			ssize_t n = read(STDIN_FILENO, input, sizeof(input));
			if (n <= 0) {
				ended = 1;
				break;
			}
			/* Ctrl+\ detaches, plain or in kitty keyboard form. */
			if (memchr(input, 0x1c, (size_t)n) || memmem(input, (size_t)n, "\033[92;5u", 7)) {
				detached = 1;
				break;
			}
			if (!phi_host_send_frame(fd, 'i', input, (size_t)n)) {
				ended = 1;
				break;
			}
		}
	}
	close(fd);
	if (detached) {
		/* Phi's exact-theme and soft-diff slots must not leak into the board
           or shell. Leave the user's ANSI palette alone. */
		for (int slot = 232; slot < 256; slot++) {
			char reset[32];
			int n = snprintf(reset, sizeof(reset), "\033]104;%d\007", slot);
			phi_host_write_all(STDOUT_FILENO, reset, (size_t)n);
		}
		phi_host_write_all(STDOUT_FILENO, RESET_MODES "\r\n", sizeof(RESET_MODES) + 1);
	}
	if (terminal) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	}
	sigaction(SIGWINCH, &old_winch, NULL);
	sigaction(SIGPIPE, &old_pipe, NULL);
	if (detached && name && !closing) {
		printf("Detached; %s keeps running. phi attach %s returns to it.\n", name, name);
	}
	/* 3: detached, the program is still running; 4: detached as it closes */

	return closing ? 4 : detached ? 3 : 0;
}

int phi_host_attach(const char *path, const char *name) {

	return attach_view(path, name, 0);
}

int phi_host_attach_backgrounded(const char *path, const char *name) {

	return attach_view(path, name, 1);
}
