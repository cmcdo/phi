#define _GNU_SOURCE
#include "internal.h"
#include <limits.h>
#include <sys/random.h>
#include <sys/xattr.h>

/* Resolve the existing prefix before creating anything. In sandbox mode we
   reject parent traversal even when it follows a not-yet-created directory. */
static int has_parent_component(const char *path) {

	for (const char *p = path; *p;) {
		while (*p == '/') {
			p++;
		}
		size_t size = strcspn(p, "/");
		if (size == 2 && p[0] == '.' && p[1] == '.') {
			return 1;
		}
		p += size;
	}

	return 0;
}

static int inside_workspace(const char *path) {

	char root[PATH_MAX];
	if (!getcwd(root, sizeof(root))) {
		return 0;
	}
	size_t size = strlen(root);
	if (size == 1) {
		errno = EPERM;
		return 0;
	}
	if (!strncmp(path, root, size) && (!path[size] || path[size] == '/')) {
		return 1;
	}
	errno = EPERM;

	return 0;
}

static char *canonical_path(const char *path) {

	if (!path || !*path) {
		errno = EINVAL;
		return NULL;
	}
	if (tools_sandbox_enabled() && has_parent_component(path)) {
		errno = EPERM;
		return NULL;
	}
	char *resolved = realpath(path, NULL);
	if (!resolved && errno == ENOENT) {
		struct stat status;
		if (!lstat(path, &status) && S_ISLNK(status.st_mode)) {
			errno = ENOENT; /* Do not replace a dangling final symlink. */
			return NULL;
		}
		char *absolute = NULL;
		if (path[0] == '/') {
			absolute = strdup(path);
		} else {
			char *cwd = getcwd(NULL, 0);
			if (!cwd) {
				return NULL;
			}
			if (asprintf(&absolute, "%s/%s", cwd, path) < 0) {
				absolute = NULL;
			}
			free(cwd);
		}
		if (!absolute) {
			return NULL;
		}
		char *probe = strdup(absolute);
		if (!probe) {
			free(absolute);
			return NULL;
		}
		size_t suffix;
		for (;;) {
			char *slash = strrchr(probe, '/');
			if (!slash) {
				errno = EINVAL;
				break;
			}
			suffix = (size_t)(slash - probe) + 1;
			if (slash == probe) {
				probe[1] = '\0';
			} else {
				*slash = '\0';
			}
			char *parent = realpath(probe, NULL);
			if (parent) {
				if (asprintf(&resolved, "%s%s%s", parent, !strcmp(parent, "/") ? "" : "/",
						absolute + suffix) < 0) {
					resolved = NULL;
				}
				free(parent);
				break;
			}
			if (errno != ENOENT || slash == probe) {
				break;
			}
		}
		int saved = errno;
		free(probe);
		free(absolute);
		errno = saved;
	}
	if (resolved && tools_sandbox_enabled() && !inside_workspace(resolved)) {
		free(resolved);
		resolved = NULL;
	}

	return resolved;
}

int tool_path_allowed(const char *path) {

	if (!tools_sandbox_enabled()) {
		return 1;
	}
	char *resolved = canonical_path(path);
	int allowed = resolved != NULL;
	free(resolved);

	return allowed;
}

struct destination {
	int directory;
	char *path;
	char *name; /* borrowed suffix of path */
};

static void destination_close(struct destination *destination) {

	if (destination->directory >= 0) {
		close(destination->directory);
	}
	free(destination->path);
	*destination = (struct destination){.directory = -1};
}

/* Walk the canonical path with directory descriptors and O_NOFOLLOW. A parent
   replaced by a symlink after validation is rejected, not followed. */
static int destination_open(const char *path, int create, struct destination *out) {

	*out = (struct destination){.directory = -1};
	out->path = canonical_path(path);
	if (!out->path) {
		return -1;
	}
	char *relative = out->path + 1;
	if (tools_sandbox_enabled()) {
		char root[PATH_MAX];
		if (!getcwd(root, sizeof(root))) {
			goto failed;
		}
		relative = out->path + strlen(root);
		if (*relative == '/') {
			relative++;
		}
		out->directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	} else {
		out->directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	}
	if (out->directory < 0) {
		goto failed;
	}

	for (;;) {
		char *slash = strchr(relative, '/');
		if (!slash) {
			break;
		}
		*slash = '\0';
		if (*relative && strcmp(relative, ".")) {
			int next =
				openat(out->directory, relative, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (next < 0 && errno == ENOENT && create) {
				if (mkdirat(out->directory, relative, 0777) && errno != EEXIST) {
					goto failed;
				}
				next = openat(
					out->directory, relative, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			}
			if (next < 0) {
				goto failed;
			}
			close(out->directory);
			out->directory = next;
		}
		relative = slash + 1;
	}
	if (!*relative || !strcmp(relative, ".") || !strcmp(relative, "..")) {
		errno = EINVAL;
		goto failed;
	}
	out->name = relative;
	return 0;
failed: {
	int saved = errno;
	destination_close(out);
	errno = saved;
	return -1;
}
}

int tool_open_read(const char *path) {

	struct destination destination;
	if (destination_open(path, 0, &destination)) {
		return -1;
	}
	int fd = openat(
		destination.directory, destination.name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	int saved = errno;
	destination_close(&destination);
	errno = saved;

	return fd;
}

static int same_version(const struct stat *a, const struct stat *b) {

	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
		a->st_mode == b->st_mode && a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
		a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
		a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int copy_attributes(int source, int destination) {

	ssize_t size = flistxattr(source, NULL, 0);
	if (size < 0) {
		return errno == ENOTSUP ? 0 : -1;
	}
	if (size > 65536) {
		errno = EOVERFLOW;
		return -1;
	}
	char *names = malloc((size_t)size + 1);
	if (!names) {
		return -1;
	}
	if (size) {
		ssize_t actual = flistxattr(source, names, (size_t)size);
		if (actual != size) {
			if (actual >= 0) {
				errno = ESTALE;
			}
			free(names);
			return -1;
		}
	}
	int result = 0, has_acl = 0;
	for (ssize_t at = 0; at < size;) {
		const char *name = names + at;
		size_t length = strnlen(name, (size_t)(size - at));
		if (length == (size_t)(size - at)) {
			errno = EIO;
			result = -1;
			break;
		}
		at += (ssize_t)length + 1;
		if (!strcmp(name, "security.capability")) {
			continue; /* New code must not inherit file capabilities. */
		}
		if (!strcmp(name, "system.posix_acl_access")) {
			has_acl = 1;
		}
		ssize_t count = fgetxattr(source, name, NULL, 0);
		if (count < 0 || count > 65536) {
			if (count > 65536) {
				errno = EOVERFLOW;
			}
			result = -1;
			break;
		}
		void *value = malloc((size_t)count + 1);
		if (!value) {
			result = -1;
			break;
		}
		ssize_t actual = fgetxattr(source, name, value, (size_t)count);
		if (actual != count) {
			if (actual >= 0) {
				errno = ESTALE;
			}
			result = -1;
		} else if (fsetxattr(destination, name, value, (size_t)count, 0)) {
			result = -1;
		}
		free(value);
		if (result) {
			break;
		}
	}
	if (!result && !has_acl && fremovexattr(destination, "system.posix_acl_access") &&
		errno != ENODATA && errno != ENOTSUP) {
		result = -1;
	}
	int saved = errno;
	free(names);
	errno = saved;

	return result;
}

static int create_temporary(int directory, char name[40]) {

	static const char hex[] = "0123456789abcdef";
	for (int attempt = 0; attempt < 16; attempt++) {
		unsigned char random[12];
		size_t used = 0;
		while (used < sizeof(random)) {
			ssize_t count = getrandom(random + used, sizeof(random) - used, 0);
			if (count < 0 && errno == EINTR) {
				continue;
			}
			if (count <= 0) {
				if (!count) {
					errno = EIO;
				}
				return -1;
			}
			used += (size_t)count;
		}
		memcpy(name, ".phi-tmp-", 9);
		for (size_t i = 0; i < sizeof(random); i++) {
			name[9 + 2 * i] = hex[random[i] >> 4];
			name[10 + 2 * i] = hex[random[i] & 15];
		}
		name[33] = '\0';
		int fd =
			openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
		if (fd >= 0 || errno != EEXIST) {
			return fd;
		}
	}
	errno = EEXIST;

	return -1;
}

/* Same-directory rename is the commit point. Never report "unchanged" after
   that point: a later directory fsync failure is a durability warning. */
int tool_atomic_replace(const char *path, const void *data, size_t size,
	const struct stat *expected, int create_parents) {

	struct destination destination;
	if (destination_open(path, create_parents, &destination)) {
		return -1;
	}
	int source = -1, temporary = -1, result = -1;
	int existing = 0, saved_error;
	char temporary_name[40] = {0};
	struct stat before, created, current;
	source = openat(
		destination.directory, destination.name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	if (source >= 0) {
		if (fstat(source, &before)) {
			goto done;
		}
		if (!S_ISREG(before.st_mode)) {
			errno = EINVAL;
			goto done;
		}
		existing = 1;
	} else if (errno != ENOENT) {
		goto done;
	}
	if (expected && (!existing || !same_version(expected, &before))) {
		errno = ESTALE;
		goto done;
	}

	temporary = create_temporary(destination.directory, temporary_name);
	if (temporary < 0) {
		temporary_name[0] = '\0';
		goto done;
	}
	if (fstat(temporary, &created) || fchmod(temporary, 0600)) {
		goto done;
	}
	if (tool_write_all(temporary, data, size)) {
		goto done;
	}
	if (existing) {
		if ((created.st_uid != before.st_uid || created.st_gid != before.st_gid) &&
			fchown(temporary, before.st_uid, before.st_gid)) {
			goto done;
		}
		if (copy_attributes(source, temporary)) {
			goto done;
		}
	}
	/* Keep ordinary permissions, but drop set-ID bits when replacing content. */
	mode_t mode = (existing ? before.st_mode : created.st_mode) & 0777;
	if (fchmod(temporary, mode) || fsync(temporary)) {
		goto done;
	}
	if (close(temporary)) {
		temporary = -1;
		goto done;
	}
	temporary = -1;
	if (tool_check_cancel()) {
		errno = ECANCELED;
		goto done;
	}
	if (existing) {
		if (fstatat(destination.directory, destination.name, &current, AT_SYMLINK_NOFOLLOW)) {
			goto done;
		}
		if (!same_version(&before, &current)) {
			errno = ESTALE;
			goto done;
		}
		if (renameat(
				destination.directory, temporary_name, destination.directory, destination.name)) {
			goto done;
		}
	} else {
		/* linkat is an atomic no-clobber install for a new regular file. */
		if (linkat(destination.directory, temporary_name, destination.directory, destination.name,
				0)) {
			goto done;
		}
		if (unlinkat(destination.directory, temporary_name, 0)) {
			result = 1;
			goto done;
		}
	}
	temporary_name[0] = '\0';
	result = fsync(destination.directory) ? 1 : 0;
done:
	saved_error = errno;
	if (temporary >= 0) {
		close(temporary);
	}
	if (source >= 0) {
		close(source);
	}
	if (*temporary_name) {
		unlinkat(destination.directory, temporary_name, 0);
	}
	destination_close(&destination);
	errno = saved_error;

	return result;
}
