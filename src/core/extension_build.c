#define _POSIX_C_SOURCE 200809L
#include "extension_build.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

const char phi_extension_sdk[] = {
#include "extension-sdk.inc"
};

const char phi_extension_guide[] = {
#include "extension-guide.inc"
};

static int problem(char *error, size_t size, const char *operation) {

	snprintf(error, size, "%s: %s", operation, strerror(errno));

	return 0;
}

int extension_name_valid(const char *name) {

	if (!name || !*name || strlen(name) > 48) {
		return 0;
	}
	for (size_t i = 0; name[i]; i++) {
		unsigned char c = (unsigned char)name[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
			continue;
		}
		if (i && ((c >= '0' && c <= '9') || c == '-')) {
			continue;
		}
		return 0;
	}

	return 1;
}

static char *join(const char *a, const char *b) {

	size_t n = strlen(a) + strlen(b) + 2;
	char *out = malloc(n);
	if (out) {
		snprintf(out, n, "%s/%s", a, b);
	}

	return out;
}

int extension_paths(char **sources, char **cache, char *error, size_t size) {

	*sources = *cache = NULL;
	const char *home = getenv("HOME"), *config = getenv("XDG_CONFIG_HOME");
	const char *base = getenv("XDG_CACHE_HOME");
	if (!home || home[0] != '/') {
		errno = EINVAL;
		return problem(error, size, "HOME must be absolute");
	}
	char *fallback_config = NULL, *fallback_cache = NULL;
	if (!config || config[0] != '/') {
		config = fallback_config = join(home, ".config");
	}
	if (!base || base[0] != '/') {
		base = fallback_cache = join(home, ".cache");
	}
	if (config && base) {
		*sources = join(config, "phi/extensions");
		*cache = join(base, "phi/extensions");
	}
	free(fallback_config);
	free(fallback_cache);
	if (*sources && *cache) {
		return 1;
	}
	free(*sources);
	free(*cache);
	*sources = *cache = NULL;
	errno = ENOMEM;

	return problem(error, size, "Extension paths");
}

int extension_state_directory(char **state, char *error, size_t size) {

	*state = NULL;
	const char *home = getenv("HOME"), *base = getenv("XDG_STATE_HOME");
	if (!home || home[0] != '/') {
		errno = EINVAL;
		return problem(error, size, "HOME must be absolute");
	}
	char *fallback = NULL;
	if (!base || base[0] != '/') {
		base = fallback = join(home, ".local/state");
	}
	if (base) {
		*state = join(base, "phi/extensions");
	}
	free(fallback);
	if (*state) {
		return 1;
	}
	errno = ENOMEM;

	return problem(error, size, "Extension state path");
}

/* Existing ancestors follow the user's XDG layout; the final directory must
   not be a symlink or writable by another uid. Cache files are private. */
int extension_directory(const char *path, int create, int private_mode, char *error, size_t size) {

	if (create) {
		char *copy = strdup(path);
		if (!copy) {
			return problem(error, size, "Directory allocation");
		}
		for (char *p = copy + 1;; p++) {
			if (*p && *p != '/') {
				continue;
			}
			char saved = *p;
			*p = 0;
			if (mkdir(copy, 0700) && errno != EEXIST) {
				free(copy);
				return problem(error, size, "Create extension directory");
			}
			*p = saved;
			if (!saved) {
				break;
			}
		}
		free(copy);
	}
	struct stat info;
	if (lstat(path, &info)) {
		return problem(error, size, "Extension directory");
	}
	if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
		(info.st_mode & (private_mode ? 077 : 022))) {
		errno = EPERM;
		return problem(error, size, "Unsafe extension directory ownership/permissions");
	}

	return 1;
}

static char *read_regular(const char *path, size_t maximum, size_t *length) {

	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		return NULL;
	}
	struct stat info;
	char *buffer = NULL;
	if (fstat(fd, &info)) {
		goto done;
	}
	if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 022) ||
		info.st_size < 0 || (uintmax_t)info.st_size > maximum) {
		errno = EPERM;
		goto done;
	}
	size_t expected = (size_t)info.st_size, used = 0;
	buffer = malloc(expected + 1);
	if (!buffer) {
		goto done;
	}
	while (used < expected) {
		ssize_t n = read(fd, buffer + used, expected - used);
		if (n > 0) {
			used += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			free(buffer);
			buffer = NULL;
			errno = EIO;
			goto done;
		}
	}
	char extra;
	ssize_t n;
	do {
		n = read(fd, &extra, 1);
	} while (n < 0 && errno == EINTR);
	if (n != 0) {
		free(buffer);
		buffer = NULL;
		errno = EAGAIN;
		goto done;
	}
	buffer[used] = 0;
	*length = used;
done: {
	int saved = errno;
	close(fd);
	errno = saved;
	return buffer;
}
}

static void hex(const unsigned char digest[32], char out[65]) {

	static const char digits[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		out[2 * i] = digits[digest[i] >> 4];
		out[2 * i + 1] = digits[digest[i] & 15];
	}
	out[64] = 0;
}

static int digest(const void *data, size_t length, char out[65]) {

	unsigned char hash[32];
	unsigned n = 0;
	if (!EVP_Digest(data, length, hash, &n, EVP_sha256(), NULL) || n != 32) {
		return 0;
	}
	hex(hash, out);

	return 1;
}

int extension_source_read(const char *directory, const char *name, struct extension_source *source,
	char *error, size_t size) {

	*source = (struct extension_source){0};
	if (!extension_name_valid(name)) {
		errno = EINVAL;
		return problem(error, size, "Invalid extension name");
	}
	char leaf[64];
	snprintf(leaf, sizeof(leaf), "%s.c", name);
	char *path = join(directory, leaf);
	if (!path) {
		return problem(error, size, "Source path allocation");
	}
	source->text = read_regular(path, PHI_EXTENSION_SOURCE_LIMIT, &source->length);
	free(path);
	if (!source->text) {
		return problem(error, size, "Read extension source");
	}
	if (memchr(source->text, 0, source->length)) {
		free(source->text);
		source->text = NULL;
		errno = EINVAL;
		return problem(error, size, "NUL in C source");
	}
	struct utsname platform;
	EVP_MD_CTX *hash = EVP_MD_CTX_new();
	unsigned char output[32];
	unsigned n = 0;
	const char *build = phi_extension_build_id();
	size_t pointer_size = sizeof(void *);
	int ok = !uname(&platform) && hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) &&
		EVP_DigestUpdate(hash, source->text, source->length) &&
		EVP_DigestUpdate(hash, phi_extension_sdk, sizeof(phi_extension_sdk)) &&
		EVP_DigestUpdate(hash, build, strlen(build) + 1) &&
		EVP_DigestUpdate(hash, platform.machine, strlen(platform.machine) + 1) &&
		EVP_DigestUpdate(hash, &pointer_size, sizeof(pointer_size)) &&
		EVP_DigestFinal_ex(hash, output, &n) && n == 32;
	EVP_MD_CTX_free(hash);
	if (!ok) {
		free(source->text);
		source->text = NULL;
		errno = EIO;
		return problem(error, size, "Hash extension source");
	}
	hex(output, source->key);

	return 1;
}

int extension_source_matches(const char *directory, const char *name, const char *key) {

	struct extension_source source;
	char error[256];
	if (!extension_source_read(directory, name, &source, error, sizeof(error))) {
		return 0;
	}
	int same = !strcmp(source.key, key);
	free(source.text);

	return same;
}

static char *cache_path(const char *cache, const char *name, const char *key, const char *binary) {

	char leaf[256];
	if (binary) {
		snprintf(leaf, sizeof(leaf), "%s-%s-%s%s", name, key, binary, phi_dynlib_suffix());
	} else {
		snprintf(leaf, sizeof(leaf), "%s-%s.trust", name, key);
	}

	return join(cache, leaf);
}

static int binary_valid(const char *path, const char *expected) {

	size_t length;
	char *data = read_regular(path, 16u * 1024u * 1024u, &length);
	char found[65];
	int ok = data && length && digest(data, length, found) && !strcmp(found, expected);
	free(data);

	return ok;
}

int extension_cached(const char *cache, const char *name, const char *key, char **artifact,
	char *error, size_t size) {

	*artifact = NULL;
	if (!extension_directory(cache, 0, 1, error, size)) {
		return 0;
	}
	char *record = cache_path(cache, name, key, NULL);
	if (!record) {
		return problem(error, size, "Cache path allocation");
	}
	size_t n = 0;
	char *text = read_regular(record, 65, &n);
	free(record);
	if (!text) {
		return problem(error, size, "No usable approval record");
	}
	int valid = n == 65 && text[64] == '\n' && strspn(text, "0123456789abcdef") == 64;
	if (valid) {
		text[64] = 0;
	}
	char *path = valid ? cache_path(cache, name, key, text) : NULL;
	valid = path && binary_valid(path, text);
	free(text);
	if (!valid) {
		free(path);
		errno = EINVAL;
		return problem(error, size, "Cached binary/approval is invalid; reload requires approval");
	}
	*artifact = path;

	return 1;
}

static int write_new(const char *path, const void *data, size_t length) {

	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		return 0;
	}
	const char *p = data;
	size_t used = 0;
	while (used < length) {
		ssize_t n = write(fd, p + used, length - used);
		if (n > 0) {
			used += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	int ok = used == length && !fsync(fd);
	if (close(fd)) {
		ok = 0;
	}

	return ok;
}

static void remove_scratch_fd(int fd, unsigned depth) {

	DIR *directory = fdopendir(fd);
	if (!directory) {
		close(fd);
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}
		int child = depth < 32
			? openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
			: -1;
		if (child >= 0) {
			remove_scratch_fd(child, depth + 1);
			unlinkat(fd, entry->d_name, AT_REMOVEDIR);
		} else {
			unlinkat(fd, entry->d_name, 0);
		}
	}
	closedir(directory);
}

static void remove_scratch(const char *path) {

	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd >= 0) {
		remove_scratch_fd(fd, 0);
	}
	rmdir(path);
}

int extension_build(const char *cache, const char *name, const struct extension_source *source,
	const char *compiler, int (*cancel)(void), char **artifact, char binary_hash[65],
	struct phi_compile_result *result, char *error, size_t size) {

	*artifact = NULL;
	if (!extension_directory(cache, 1, 1, error, size)) {
		return 0;
	}
	char *scratch = join(cache, ".build-XXXXXX");
	if (!scratch || !mkdtemp(scratch)) {
		free(scratch);
		return problem(error, size, "Private compiler directory");
	}
	char *input = join(scratch, "extension.c"), *header = join(scratch, "phi_extension.h");
	char *module = join(scratch, "module");
	char *data = NULL, *target = NULL;
	int ok = 0;
	size_t length = 0;
	if (!input || !header || !module) {
		errno = ENOMEM;
		goto done;
	}
	if (!write_new(input, source->text, source->length) ||
		!write_new(header, phi_extension_sdk, sizeof(phi_extension_sdk) - 1) ||
		!write_new(module, "", 0)) {
		goto done;
	}
	if (!phi_compile_extension(scratch, compiler, 30, cancel, result)) {
		snprintf(error, size, "%s",
			result->cancelled       ? "Extension compilation cancelled"
				: result->timed_out ? "Extension compilation timed out"
									: "Extension compilation failed");
		goto done;
	}
	data = read_regular(module, 16u * 1024u * 1024u, &length);
	if (!data || !length || !digest(data, length, binary_hash)) {
		errno = EINVAL;
		goto done;
	}
	free(data);
	data = NULL;
	target = cache_path(cache, name, source->key, binary_hash);
	if (!target) {
		goto done;
	}
	int fd = open(module, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		goto done;
	}
	int synced = !fchmod(fd, 0500) && !fsync(fd);
	if (close(fd)) {
		synced = 0;
	}
	if (!synced) {
		goto done;
	}
	/* Never overwrite a loaded DSO. Both source key and binary digest are in
       the name, so recompilation cannot alias a live dlopen handle. */
	if (link(module, target) && (errno != EEXIST || !binary_valid(target, binary_hash))) {
		goto done;
	}
	*artifact = target;
	target = NULL;
	ok = 1;
done:
	if (!ok && !*error) {
		problem(error, size, "Build extension artifact");
	}
	free(data);
	free(target);
	free(input);
	free(header);
	free(module);
	remove_scratch(scratch);
	free(scratch);

	return ok;
}

static int atomic_text(const char *cache, const char *target, const char *text) {

	char *temporary = join(cache, ".trust-XXXXXX");
	if (!temporary) {
		return 0;
	}
	int fd = mkstemp(temporary), ok = 0;
	if (fd >= 0) {
		size_t length = strlen(text), used = 0;
		while (used < length) {
			ssize_t n = write(fd, text + used, length - used);
			if (n > 0) {
				used += (size_t)n;
			} else if (n < 0 && errno == EINTR) {
				continue;
			} else {
				break;
			}
		}
		ok = used == length && !fsync(fd);
		if (close(fd)) {
			ok = 0;
		}
		if (ok) {
			ok = !rename(temporary, target);
		}
		if (ok) {
			int directory = open(cache, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			ok = directory >= 0 && !fsync(directory);
			if (directory >= 0) {
				close(directory);
			}
		}
	}
	unlink(temporary);
	free(temporary);

	return ok;
}

int extension_approve(const char *cache, const char *name, const char *key, const char *binary_hash,
	char *error, size_t size) {

	char *path = cache_path(cache, name, key, NULL);
	char text[66];
	snprintf(text, sizeof(text), "%s\n", binary_hash);
	int ok = path && atomic_text(cache, path, text);
	free(path);

	return ok ? 1 : problem(error, size, "Save native-code approval");
}

static char *disabled_path(const char *cache, const char *name) {

	char leaf[80];
	snprintf(leaf, sizeof(leaf), "%s.disabled", name);

	return join(cache, leaf);
}

int extension_disabled(const char *cache, const char *name) {

	char *path = disabled_path(cache, name);
	if (!path) {
		return 1;
	}
	struct stat info;
	int disabled = !lstat(path, &info) || errno != ENOENT;
	free(path);

	return disabled;
}

int extension_set_disabled(
	const char *cache, const char *name, int disabled, char *error, size_t size) {

	if (!extension_directory(cache, 1, 1, error, size)) {
		return 0;
	}
	char *path = disabled_path(cache, name);
	if (!path) {
		return problem(error, size, "Disable path allocation");
	}
	int ok = disabled ? atomic_text(cache, path, "disabled\n") : !unlink(path) || errno == ENOENT;
	free(path);

	return ok ? 1 : problem(error, size, "Update extension disabled state");
}
