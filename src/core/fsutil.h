#ifndef PHI_FSUTIL_H
#define PHI_FSUTIL_H
/* Small file helpers shared by settings, credentials, instructions, themes
   and caches. Header-only so each user stays a single translation unit. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

/* An interrupted blocking lock is retried; all other failures stop the write. */
static inline int phi_lock_exclusive(int fd) {

	int result;
	do {
		result = flock(fd, LOCK_EX);
	} while (result && errno == EINTR);

	return result == 0;
}

/* $VARIABLE/leaf when VARIABLE is an absolute path, else $HOME/fallback/leaf.
   Example: phi_xdg_path(p, n, "XDG_CONFIG_HOME", ".config", "phi/theme.json"). */
static inline int phi_xdg_path(
	char *out, size_t size, const char *variable, const char *fallback, const char *leaf) {

	const char *base = getenv(variable), *home = getenv("HOME");
	int n = base && *base == '/' ? snprintf(out, size, "%s/%s", base, leaf)
		: home && *home == '/'   ? snprintf(out, size, "%s/%s/%s", home, fallback, leaf)
								 : -1;
	if (n < 0 || (size_t)n >= size) {
		errno = ENAMETOOLONG;
		return 0;
	}

	return 1;
}

/* mkdir -p with mode 0700 for every missing component of path. */
static inline int phi_make_directories(const char *path) {

	char copy[4096];
	int n = snprintf(copy, sizeof(copy), "%s", path);
	if (n < 0 || (size_t)n >= sizeof(copy)) {
		errno = ENAMETOOLONG;
		return 0;
	}
	for (char *p = copy + 1;; p++) {
		if (*p && *p != '/') {
			continue;
		}
		char saved = *p;
		*p = 0;
		int ok = !mkdir(copy, 0700) || errno == EEXIST;
		*p = saved;
		if (!ok) {
			return 0;
		}
		if (!saved) {
			return 1;
		}
	}
}

enum {
	PHI_READ_OWNED = 1,
	PHI_READ_FOLLOW = 2
};

/* Read a whole regular file of at most limit bytes into a NUL-terminated
   buffer the caller frees. Symlinks are followed only with PHI_READ_FOLLOW;
   with PHI_READ_OWNED the file must belong to this user. On failure returns
   NULL with errno: ENOENT (no file), EINVAL (not a regular file, wrong owner
   or contains NUL), EFBIG (too large), or the open/read error. */
static inline char *phi_read_file(const char *path, size_t limit, int flags, size_t *length) {

	int fd =
		open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | (flags & PHI_READ_FOLLOW ? 0 : O_NOFOLLOW));
	if (fd < 0) {
		return NULL;
	}
	struct stat st;
	char *text = NULL;
	int error = 0;
	if (fstat(fd, &st)) {
		error = errno;
	} else if (!S_ISREG(st.st_mode) || ((flags & PHI_READ_OWNED) && st.st_uid != geteuid())) {
		error = EINVAL;
	} else if (st.st_size < 0 || (size_t)st.st_size > limit) {
		error = EFBIG;
	} else if (!(text = malloc((size_t)st.st_size + 1))) {
		error = ENOMEM;
	}
	size_t used = 0;
	while (!error && used < (size_t)st.st_size) {
		ssize_t n = read(fd, text + used, (size_t)st.st_size - used);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0) {
			error = errno;
		} else if (!n) {
			break; /* shrank while reading: use what was there */
		} else {
			used += (size_t)n;
		}
	}
	close(fd);
	if (!error && memchr(text, 0, used)) {
		error = EINVAL;
	}
	if (error) {
		free(text);
		errno = error;
		return NULL;
	}
	text[used] = 0;
	if (length) {
		*length = used;
	}

	return text;
}

/* Replace path atomically with a private (0600) file: write a temporary file
   in the same directory, fsync it, rename it over path, then fsync the
   directory. Readers see the old or new file, never a partial one. */
static inline int phi_write_file_atomic(const char *path, const char *data, size_t size) {

	char temporary[4200], directory[4096];
	int n = snprintf(temporary, sizeof(temporary), "%s.tmp-XXXXXX", path);
	if (n < 0 || (size_t)n >= sizeof(temporary)) {
		errno = ENAMETOOLONG;
		return 0;
	}
	size_t path_length = strlen(path);
	if (path_length >= sizeof(directory)) {
		errno = ENAMETOOLONG;
		return 0;
	}
	memcpy(directory, path, path_length + 1);
	char *slash = strrchr(directory, '/');
	if (slash) {
		*(slash == directory ? slash + 1 : slash) = 0;
	} else {
		snprintf(directory, sizeof(directory), ".");
	}
	int fd = mkstemp(temporary);
	if (fd < 0) {
		return 0;
	}
	int ok = !fchmod(fd, 0600) && !fcntl(fd, F_SETFD, FD_CLOEXEC);
	for (size_t at = 0; ok && at < size;) {
		ssize_t written = write(fd, data + at, size - at);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			if (!written) {
				errno = EIO;
			}
			ok = 0;
			break;
		}
		at += (size_t)written;
	}
	if (ok && fsync(fd)) {
		ok = 0;
	}
	int error = errno;
	if (close(fd) && ok) {
		ok = 0;
		error = errno;
	}
	if (ok && rename(temporary, path)) {
		ok = 0;
		error = errno;
	}
	if (!ok) {
		unlink(temporary);
		errno = error;
		return 0;
	}
	int d = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (d >= 0) {
		fsync(d);
		close(d);
	}

	return 1;
}
#endif
