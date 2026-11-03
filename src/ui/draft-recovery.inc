/* Each running UI owns a locked recovery slot. Atomic replacement protects
   the previous snapshot from partial writes; recovery is scoped to cwd. */
#include <sys/file.h>
static int recovery_dir = -1, recovery_lock = -1, recovery_failed;
static char recovery_path[4096];

static int recovery_write(int directory, const char *name, const char *text) {

	int fd =
		openat(directory, "pending", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return 0;
	}
	size_t n = strlen(text), at = 0;
	while (at < n) {
		ssize_t w = write(fd, text + at, n - at);
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w <= 0) {
			break;
		}
		at += (size_t)w;
	}
	int ok = at == n;
	if (close(fd)) {
		ok = 0;
	}
	if (ok) {
		ok = !renameat(directory, "pending", directory, name);
	}
	if (!ok) {
		unlinkat(directory, "pending", 0);
	}

	return ok;
}

static int recovery_read(int directory, const char *name, char *text, size_t capacity) {

	int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		return 0;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_size < 0 ||
		(size_t)st.st_size >= capacity) {
		close(fd);
		return 0;
	}
	size_t at = 0;
	while (at < (size_t)st.st_size) {
		ssize_t n = read(fd, text + at, (size_t)st.st_size - at);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		at += (size_t)n;
	}
	close(fd);
	text[at] = 0;

	return at == (size_t)st.st_size && !memchr(text, 0, at);
}

static char *recovery_text(int directory) {

	int fd = openat(directory, "text", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		return NULL;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_size < 0 ||
		(uintmax_t)st.st_size >= SIZE_MAX) {
		close(fd);
		return NULL;
	}
	size_t size = (size_t)st.st_size, at = 0;
	char *text = malloc(size + 1);
	if (!text) {
		close(fd);
		return NULL;
	}
	while (at < size) {
		ssize_t n = read(fd, text + at, size - at);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		at += (size_t)n;
	}
	close(fd);
	if (at != size || memchr(text, 0, at)) {
		free(text);
		return NULL;
	}
	text[at] = 0;

	return text;
}

static void recovery_save(void) {

	if (recovery_dir < 0) {
		return;
	}
	size_t n = wcstombs(NULL, input, 0);
	char *text = n == (size_t)-1 ? NULL : malloc(n + 1);
	if (!text) {
		recovery_failed = 1;
		return;
	}
	wcstombs(text, input, n + 1);
	recovery_failed = !recovery_write(recovery_dir, "text", text);
	free(text);
}

static int recovery_open(void) {

	if (getenv("PHI_DRAFT_RECOVERY") && !strcmp(getenv("PHI_DRAFT_RECOVERY"), "0")) {
		return 0;
	}
	char root[4096], cwd[4096];
	if (!getcwd(cwd, sizeof(cwd))) {
		return 0;
	}
	if (!phi_xdg_path(root, sizeof(root), "XDG_STATE_HOME", ".local/state", "phi/drafts") ||
		!phi_make_directories(root)) {
		return 0;
	}
	int rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (rootfd < 0) {
		return 0;
	}
	struct stat root_stat;
	if (fstat(rootfd, &root_stat) || root_stat.st_uid != geteuid() || (root_stat.st_mode & 0077)) {
		close(rootfd);
		return 0;
	}
	DIR *dir = fdopendir(rootfd);
	if (!dir) {
		close(rootfd);
		return 0;
	}
	struct dirent *entry;
	struct timespec newest = {0};
	while ((entry = readdir(dir))) {
		size_t root_len = strlen(root), name_len = strlen(entry->d_name);
		if (strncmp(entry->d_name, "draft-", 6) ||
			root_len + name_len + 2 > sizeof(recovery_path)) {
			continue;
		}
		int fd = openat(rootfd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			continue;
		}
		struct stat st;
		char saved_cwd[4096];
		if (fstat(fd, &st) || st.st_uid != geteuid() ||
			!recovery_read(fd, "cwd", saved_cwd, sizeof(saved_cwd)) || strcmp(cwd, saved_cwd)) {
			close(fd);
			continue;
		}
		int lock = openat(fd, "lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
		if (lock < 0) {
			close(fd);
			continue;
		}
		if (fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
			flock(lock, LOCK_EX | LOCK_NB)) {
			close(lock);
			close(fd);
			continue;
		}
		if (fstatat(fd, "text", &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) ||
			st.st_size <= 0 || st.st_mtim.tv_sec < newest.tv_sec ||
			(st.st_mtim.tv_sec == newest.tv_sec && st.st_mtim.tv_nsec <= newest.tv_nsec)) {
			close(lock);
			close(fd);
			continue;
		}
		/* A newer corrupt draft must not hide an older good one.
           Validate under the slot's lock before choosing it. */
		char *text = recovery_text(fd);
		size_t count;
		wchar_t *next = text ? wide_decode(text, &count) : NULL;
		free(text);
		if (!next || !input_reserve(count)) {
			free(next);
			close(lock);
			close(fd);
			continue;
		}
		if (recovery_lock >= 0) {
			close(recovery_lock);
		}
		if (recovery_dir >= 0) {
			close(recovery_dir);
		}
		recovery_dir = fd;
		recovery_lock = lock;
		newest = st.st_mtim;
		memcpy(input, next, (count + 1) * sizeof(*input));
		free(next);
		length = cursor = count;
		memcpy(recovery_path, root, root_len);
		recovery_path[root_len] = '/';
		memcpy(recovery_path + root_len + 1, entry->d_name, name_len + 1);
	}
	closedir(dir);
	if (recovery_dir >= 0) {
		return length > 0;
	}
	if (strlen(root) + 20 >= sizeof(recovery_path)) {
		return 0;
	}
	size_t root_len = strlen(root);
	memcpy(recovery_path, root, root_len);
	memcpy(recovery_path + root_len, "/draft-XXXXXX", sizeof("/draft-XXXXXX"));
	if (!mkdtemp(recovery_path)) {
		return 0;
	}
	recovery_dir = open(recovery_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (recovery_dir < 0) {
		return 0;
	}
	recovery_lock =
		openat(recovery_dir, "lock", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (recovery_lock < 0 || flock(recovery_lock, LOCK_EX | LOCK_NB) ||
		!recovery_write(recovery_dir, "cwd", cwd)) {
		if (recovery_lock >= 0) {
			close(recovery_lock);
		}
		close(recovery_dir);
		recovery_lock = recovery_dir = -1;
	}

	return 0;
}

static void recovery_close(void) {

	recovery_save();
	if (recovery_dir >= 0) {
		if (!length) {
			unlinkat(recovery_dir, "text", 0);
			unlinkat(recovery_dir, "pending", 0);
			unlinkat(recovery_dir, "cwd", 0);
			unlinkat(recovery_dir, "lock", 0);
			rmdir(recovery_path);
		}
		close(recovery_dir);
	}
	if (recovery_lock >= 0) {
		close(recovery_lock);
	}
	recovery_dir = recovery_lock = -1;
}
