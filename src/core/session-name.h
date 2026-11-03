#ifndef PHI_SESSION_NAME_H
#define PHI_SESSION_NAME_H
#include "fsutil.h"
#include <inttypes.h>

/* A tiny index, not a second source of title text. The transcript remains
   authoritative; readers validate its identity and parse the indexed record.
   Writers update this while holding the transcript's append lock. */
static inline int session_name_index_write(const char *path, int fd, off_t offset) {

	char index[4128], text[128];
	struct stat st;
	int n = snprintf(index, sizeof(index), "%s.phi-name", path);
	if (n < 0 || (size_t)n >= sizeof(index) || offset < 0 || fstat(fd, &st)) {
		return 0;
	}
	n = snprintf(text, sizeof(text), "%ju %ju %jd\n", (uintmax_t)st.st_dev, (uintmax_t)st.st_ino,
		(intmax_t)offset);

	return n > 0 && (size_t)n < sizeof(text) && phi_write_file_atomic(index, text, (size_t)n);
}

static inline off_t session_name_index_read(const char *path, const struct stat *st) {

	char index[4128], extra;
	uintmax_t device, inode;
	intmax_t offset;
	int n = snprintf(index, sizeof(index), "%s.phi-name", path);
	if (n < 0 || (size_t)n >= sizeof(index)) {
		return -1;
	}
	char *text = phi_read_file(index, 128, PHI_READ_OWNED, NULL);
	if (!text) {
		return -1;
	}
	int fields = sscanf(text, "%ju %ju %jd %c", &device, &inode, &offset, &extra);
	free(text);

	return fields == 3 && device == (uintmax_t)st->st_dev && inode == (uintmax_t)st->st_ino &&
			offset >= 0 && (uintmax_t)offset < (uintmax_t)st->st_size
		? (off_t)offset
		: -1;
}
#endif
