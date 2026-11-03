#define _POSIX_C_SOURCE 200809L
#include "chat_start.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOCK_ENV "PHI_CHAT_START_LOCK_FD"

int chat_start_lock(const char *directory, const char *file, char *error, size_t size) {

	struct stat session, lock;
	if (stat(file, &session) || !S_ISREG(session.st_mode)) {
		snprintf(error, size, "Cannot find the saved conversation.");
		return -1;
	}
	char path[4352];
	int n = snprintf(path, sizeof(path), "%s/resume-%jx-%jx.lock", directory,
		(uintmax_t)session.st_dev, (uintmax_t)session.st_ino);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		snprintf(error, size, "Conversation lock path is too long.");
		return -1;
	}
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0 || fstat(fd, &lock) || !S_ISREG(lock.st_mode) || lock.st_uid != getuid()) {
		if (fd >= 0) {
			close(fd);
		}
		snprintf(error, size, "Cannot lock conversation startup.");
		return -1;
	}
	/* Bound the wait: slow authentication or extension startup must not hang
       another terminal indefinitely. Never bypass a busy startup lock. */
	for (int attempt = 0; attempt < 200; attempt++) {
		if (!flock(fd, LOCK_EX | LOCK_NB)) {
			return fd;
		}
		if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
			break;
		}
		struct timespec pause = {.tv_nsec = 25000000};
		nanosleep(&pause, NULL);
	}
	close(fd);
	snprintf(
		error, size, "This conversation is still starting in another terminal; retry shortly.");

	return -1;
}

int chat_start_inherit(int fd) {

	if (fd < 0) {
		return 1;
	}
	char text[32];
	snprintf(text, sizeof(text), "%d", fd);

	return !fcntl(fd, F_SETFD, 0) && !setenv(LOCK_ENV, text, 1);
}

int chat_start_receive(void) {

	const char *text = getenv(LOCK_ENV);
	if (!text) {
		return -1;
	}
	char *end;
	errno = 0;
	long value = strtol(text, &end, 10);
	int valid = !errno && *text && !*end && value >= 3 && value <= INT_MAX;
	unsetenv(LOCK_ENV);
	if (!valid) {
		return -1;
	}
	int fd = (int)value;
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
		fcntl(fd, F_SETFD, FD_CLOEXEC)) {
		return -1;
	}

	return fd;
}

void chat_start_release(int fd) {

	if (fd < 0) {
		return;
	}
	flock(fd, LOCK_UN);
	close(fd);
}
