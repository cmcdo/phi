/* phi-host: keeps a program running on its own terminal so any terminal can
   attach to it, detach, and reattach later (like dtach, or Claude Code's
   pty host), in about a megabyte.

     phi-host new SOCKET ROWS COLS -- PROGRAM [ARGS...]
         start PROGRAM on a new pseudo-terminal in the background and serve
         it on SOCKET; returns once the socket is ready
     phi-host attach SOCKET [NAME]
         show the program in this terminal; Ctrl+\ (or the program's detach
         request) detaches, leaving it running
     phi-host send SOCKET TEXT
         type TEXT into the program as a paste, then Enter

   Clients send frames: one type byte, a 16-bit big-endian length, data.
     'i' input bytes   'w' window size (rows, cols, pixel width, pixel height:
                           16-bit values; the pixel sizes are optional)
     'p' paste TEXT then Enter
   The host sends the program's output as it is. When no one is attached
   the output is read and dropped, so the program never blocks.

   A program switches terminal modes (alternate screen, mouse, bracketed
   paste, kitty keyboard) once, at startup. The host remembers them and
   replays them to each terminal that attaches, then makes the program
   redraw by resizing its terminal. Detaching resets them, so the shell
   left behind is usable. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "../core/host_attach.h"
#include "../core/proctitle.h"

#define MAX_CLIENTS 8

/* ---- terminal mode tracking ---------------------------------------------- */

static const int tracked[] = {
	1049, 1047, 47, 1, 25, 1000, 1002, 1003, 1005, 1006, 1015, 1004, 2004};
#define TRACKED (sizeof(tracked) / sizeof(*tracked))
static signed char mode_state[TRACKED]; /* 0 unknown, 1 set, -1 reset */
static int keypad, kitty_depth, kitty_flags[8];
static char pending[64];
static size_t pending_used;

static void note_mode(int mode, int set) {

	for (size_t i = 0; i < TRACKED; i++) {
		if (tracked[i] == mode) {
			mode_state[i] = set ? 1 : -1;
		}
	}
}

/* Output is untrusted and not NUL-terminated. Never use atoi here: a
   negative/overflowing pop count can grow kitty_depth past its array. */
static int mode_number(const char *s, size_t end, size_t *at, int *value) {

	size_t start = *at;
	int number = 0;
	while (*at < end && s[*at] >= '0' && s[*at] <= '9') {
		int digit = s[(*at)++] - '0';
		if (number > (INT_MAX - digit) / 10) {
			return 0;
		}
		number = number * 10 + digit;
	}
	*value = number;

	return *at > start;
}

/* One complete escape sequence. */
static void note_sequence(const char *s, size_t n) {

	if (n == 2 && (s[1] == '=' || s[1] == '>')) {
		keypad = s[1] == '=';
		return;
	}
	if (n < 3 || s[1] != '[') {
		return;
	}
	char final = s[n - 1];
	if (s[2] == '?' && (final == 'h' || final == 'l')) {
		for (size_t i = 3; i < n - 1;) {
			int mode;
			if (!mode_number(s, n - 1, &i, &mode)) {
				return;
			}
			note_mode(mode, final == 'h');
			if (i < n - 1 && s[i] == ';') {
				i++;
			} else {
				break;
			}
		}
	} else if ((s[2] == '>' || s[2] == '<') && final == 'u') {
		size_t at = 3;
		int value = s[2] == '<' ? 1 : 0;
		if (at < n - 1 && !mode_number(s, n - 1, &at, &value)) {
			return;
		}
		if (at != n - 1) {
			return;
		}
		if (s[2] == '>') {
			if (kitty_depth < 8) {
				kitty_flags[kitty_depth++] = value;
			}
		} else {
			if (!value) {
				value = 1;
			}
			kitty_depth = value >= kitty_depth ? 0 : kitty_depth - value;
		}
	}
}

/* Follow ESC sequences in program output, across read boundaries. */
static void track(const char *data, size_t size) {

	for (size_t i = 0; i < size; i++) {
		char c = data[i];
		if (!pending_used) {
			if (c == '\033') {
				pending[pending_used++] = c;
			}
			continue;
		}
		if (pending_used == sizeof(pending)) {
			pending_used = 0;
			continue;
		}
		pending[pending_used++] = c;
		if (pending_used == 2) {
			if (c == '=' || c == '>') {
				note_sequence(pending, 2);
				pending_used = 0;
			} else if (c != '[') {
				pending_used = 0;
			}
		} else if (c >= 0x40 && c <= 0x7e) {
			note_sequence(pending, pending_used);
			pending_used = 0;
		}
	}
}

static void replay_modes(int fd) {

	char text[512];
	size_t used = 0;
	for (size_t i = 0; i < TRACKED; i++) {
		if (mode_state[i]) {
			used += (size_t)snprintf(text + used, sizeof(text) - used, "\033[?%d%c", tracked[i],
				mode_state[i] > 0 ? 'h' : 'l');
		}
	}
	for (int i = 0; i < kitty_depth; i++) {
		used += (size_t)snprintf(text + used, sizeof(text) - used, "\033[>%du", kitty_flags[i]);
	}
	if (keypad) {
		used += (size_t)snprintf(text + used, sizeof(text) - used, "\033=");
	}
	phi_host_write_all(fd, text, used);
}

/* ---- host ---------------------------------------------------------------- */

struct client {
	int fd;
	unsigned char buffer[PHI_HOST_FRAME_MAX + 3];
	size_t used;
};

static int pixel_width, pixel_height; /* the latest viewer's, for sixel images */

static void resize(int master, int rows, int cols) {

	struct winsize size = {.ws_row = (unsigned short)rows,
		.ws_col = (unsigned short)cols,
		.ws_xpixel = (unsigned short)pixel_width,
		.ws_ypixel = (unsigned short)pixel_height};
	ioctl(master, TIOCSWINSZ, &size);
}

/* Make the program repaint: a size change sends it SIGWINCH. */
static void redraw(int master, int rows, int cols) {

	struct winsize now;
	if (!ioctl(master, TIOCGWINSZ, &now) && now.ws_row == rows && now.ws_col == cols && rows > 1) {
		resize(master, rows - 1, cols);
		usleep(20000);
	}
	resize(master, rows, cols);
}

static void drop(struct client *clients, int *count, int index) {

	close(clients[index].fd);
	clients[index] = clients[--*count];
}

/* Returns 0 to drop the client. */
static int client_input(struct client *client, int master, int *attached) {

	ssize_t n =
		read(client->fd, client->buffer + client->used, sizeof(client->buffer) - client->used);
	if (n <= 0) {
		return n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
	}
	client->used += (size_t)n;
	size_t at = 0;
	while (client->used - at >= 3) {
		unsigned char type = client->buffer[at];
		size_t length = ((size_t)client->buffer[at + 1] << 8) | client->buffer[at + 2];
		if (length > sizeof(client->buffer) - 3) {
			return 0;
		}
		if (client->used - at < 3 + length) {
			break;
		}
		unsigned char *data = client->buffer + at + 3;
		if (type == 'i') {
			if (!phi_host_write_all(master, data, length)) {
				return 0;
			}
		} else if (type == 'b' && !length) {
			if (!phi_host_write_all(master, BACKGROUNDED_NOTICE, sizeof(BACKGROUNDED_NOTICE) - 1)) {
				return 0;
			}
		} else if (type == 'w' && (length == 4 || length == 8)) {
			int rows = data[0] << 8 | data[1], cols = data[2] << 8 | data[3];
			if (length == 8) {
				pixel_width = data[4] << 8 | data[5];
				pixel_height = data[6] << 8 | data[7];
			}
			if (!*attached) {
				replay_modes(client->fd);
				redraw(master, rows, cols);
				*attached = 1;
			} else {
				resize(master, rows, cols);
			}
		} else if (type == 'p') {
			if (!phi_host_write_all(master, "\033[200~", 6) ||
				!phi_host_write_all(master, data, length) ||
				!phi_host_write_all(master, "\033[201~\r", 7)) {
				return 0;
			}
		}
		at += 3 + length;
	}
	memmove(client->buffer, client->buffer + at, client->used - at);
	client->used -= at;

	return 1;
}

static int serve(const char *path, int rows, int cols, char **program, int ready) {

	/* In ps/btop: "phi host ID", beside its "phi chat ID". */
	const char *chat = getenv("PHI_CHAT_ID"), *slash = strrchr(path, '/');
	char title[64];
	snprintf(title, sizeof(title), "phi host %.16s",
		chat && *chat ? chat
			: slash   ? slash + 1
					  : path);
	proctitle_set("phi host", title);
	/* Bind before executing anything, and never unlink an existing path:
       it might be a live host socket, a symlink, or an unrelated file. */
	int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	mode_t old = umask(0077);
	int bound = listener >= 0 && !bind(listener, (struct sockaddr *)&address, sizeof(address));
	umask(old);
	if (!bound || listen(listener, 8)) {
		if (bound) {
			unlink(path);
		}
		if (listener >= 0) {
			close(listener);
		}
		phi_host_write_all(ready, "n", 1);
		close(ready);
		return 1;
	}
	struct winsize size = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
	int master;
	pid_t child = forkpty(&master, NULL, NULL, &size);
	if (child < 0) {
		close(listener);
		unlink(path);
		phi_host_write_all(ready, "n", 1);
		close(ready);
		return 1;
	}
	if (!child) {
		execvp(program[0], program);
		_exit(127);
	}
	fcntl(master, F_SETFD, FD_CLOEXEC);
	fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
	phi_host_write_all(ready, "y", 1);
	close(ready);

	struct client clients[MAX_CLIENTS];
	int count = 0, attached[MAX_CLIENTS] = {0};
	char output[16384];
	for (;;) {
		struct pollfd fds[MAX_CLIENTS + 2] = {{master, POLLIN, 0}, {listener, POLLIN, 0}};
		for (int i = 0; i < count; i++) {
			fds[i + 2] = (struct pollfd){clients[i].fd, POLLIN, 0};
		}
		if (poll(fds, (nfds_t)count + 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (fds[0].revents) {
			ssize_t n = read(master, output, sizeof(output));
			if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
				continue;
			}
			if (n <= 0) {
				break; /* the program exited */
			}
			track(output, (size_t)n);
			for (int i = count - 1; i >= 0; i--) {
				if (attached[i] && !phi_host_write_all(clients[i].fd, output, (size_t)n)) {
					attached[i] = attached[count - 1];
					drop(clients, &count, i);
				}
			}
		}
		if (fds[1].revents & POLLIN) {
			/* Bound a stalled viewer's writes instead of freezing every chat
               client on a blocking socket once its receive buffer fills. */
			int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
			struct ucred peer;
			socklen_t length = sizeof(peer);
			if (fd >= 0 &&
				(count == MAX_CLIENTS || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) ||
					peer.uid != getuid())) {
				close(fd);
				fd = -1;
			}
			if (fd >= 0) {
				clients[count].fd = fd;
				clients[count].used = 0;
				attached[count++] = 0;
			}
		}
		for (int i = count - 1; i >= 0; i--) {
			if (i + 2 < (int)(sizeof(fds) / sizeof(*fds)) && fds[i + 2].fd == clients[i].fd &&
				fds[i + 2].revents && !client_input(&clients[i], master, &attached[i])) {
				attached[i] = attached[count - 1];
				drop(clients, &count, i);
			}
		}
	}
	unlink(path);
	for (int i = 0; i < count; i++) {
		close(clients[i].fd);
	}
	int status_code = 0;
	waitpid(child, &status_code, 0);

	return 0;
}

static int start(const char *path, int rows, int cols, char **program) {

	if (!*path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path) || rows < 1 ||
		rows > USHRT_MAX || cols < 1 || cols > USHRT_MAX) {
		fprintf(stderr, "phi-host: invalid socket path or terminal dimensions\n");
		return 1;
	}
	int ready[2];
	if (pipe2(ready, O_CLOEXEC)) {
		return 1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		return 1;
	}
	if (!pid) {
		/* Detach from the starting terminal: its closing must not end us. */
		close(ready[0]);
		setsid();
		signal(SIGHUP, SIG_IGN);
		signal(SIGPIPE, SIG_IGN);
		if (fork()) {
			_exit(0);
		}
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
			if (null > 2) {
				close(null);
			}
		}
		_exit(serve(path, rows, cols, program, ready[1]));
	}
	close(ready[1]);
	waitpid(pid, NULL, 0);
	char status = 'n';
	ssize_t n;
	while ((n = read(ready[0], &status, 1)) < 0 && errno == EINTR) {
	}
	close(ready[0]);

	return n == 1 && status == 'y' ? 0 : 1;
}

static int terminal_dimension(const char *text) {

	size_t at = 0, length = strlen(text);
	int value;

	return mode_number(text, length, &at, &value) && at == length && value > 0 && value <= USHRT_MAX
		? value
		: 0;
}

int main(int argc, char **argv) {

	argv = proctitle_init(argc, argv);
	if (argc >= 7 && !strcmp(argv[1], "new") && !strcmp(argv[5], "--")) {
		return start(argv[2], terminal_dimension(argv[3]), terminal_dimension(argv[4]), argv + 6);
	}
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "attach")) {
		return phi_host_attach(argv[2], argc == 4 ? argv[3] : NULL);
	}
	if (argc == 4 && !strcmp(argv[1], "send")) {
		int fd = phi_host_connect_socket(argv[2]);
		if (fd < 0) {
			fprintf(stderr, "phi-host: cannot reach %s: %s\n", argv[2], strerror(errno));
			return 1;
		}
		size_t length = strlen(argv[3]);
		int ok = length < 4000 && phi_host_send_frame(fd, 'p', argv[3], length);
		close(fd);
		return !ok;
	}
	fprintf(stderr,
		"usage: phi-host new SOCKET ROWS COLS -- PROGRAM [ARGS...]\n"
		"       phi-host attach SOCKET [NAME]\n       phi-host send SOCKET TEXT\n");

	return 2;
}
