/* An @-completion snapshot, owned only while the popup is in use. No watcher,
   helper process or fixed-size path slots. Explicit budgets bound the scan. */
enum {
	PROJECT_FILE_LIMIT = 8192,
	PROJECT_PATH_BYTES = 2 * 1024 * 1024,
	PROJECT_ENTRY_LIMIT = 65536,
	PROJECT_DEPTH_LIMIT = 12
};

static char **project_files;
static size_t project_file_count, project_file_capacity, project_file_bytes;
static size_t project_scan_entries;
static int project_files_valid, project_files_limited, project_scan_stopped;
static dev_t project_device;
static ino_t project_inode;

static void project_files_clear(void) {

	for (size_t i = 0; i < project_file_count; i++) {
		free(project_files[i]);
	}
	free(project_files);
	project_files = NULL;
	project_file_count = project_file_capacity = project_file_bytes = 0;
	project_scan_entries = 0;
	project_files_valid = project_files_limited = project_scan_stopped = 0;
}

/* Take ownership only on success. Keep the old vector intact on allocation
   failure, so the already collected part of the snapshot stays usable. */
static int project_file_add(char *path) {

	size_t bytes = strlen(path) + 1;
	if (project_file_count == PROJECT_FILE_LIMIT ||
		bytes > PROJECT_PATH_BYTES - project_file_bytes) {
		return 0;
	}
	if (project_file_count == project_file_capacity) {
		size_t capacity = project_file_capacity ? project_file_capacity * 2 : 64;
		char **next = realloc(project_files, capacity * sizeof(*next));
		if (!next) {
			return 0;
		}
		project_files = next;
		project_file_capacity = capacity;
	}
	project_files[project_file_count++] = path;
	project_file_bytes += bytes;

	return 1;
}

/* Descriptor-relative traversal avoids truncating an absolute pathname or
   following a directory symlink if an entry changes during the scan. */
static void scan_files(int fd, const char *relative, unsigned depth) {

	DIR *directory = fdopendir(fd);
	if (!directory) {
		close(fd);
		project_files_limited = 1;
		return;
	}
	while (!project_scan_stopped) {
		errno = 0;
		struct dirent *entry = readdir(directory);
		if (!entry) {
			if (errno) {
				project_files_limited = 1;
			}
			break;
		}
		if (++project_scan_entries > PROJECT_ENTRY_LIMIT) {
			project_scan_stopped = project_files_limited = 1;
			break;
		}
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "node_modules")) {
			continue;
		}
		struct stat info;
		if (fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) {
			project_files_limited = 1;
			continue;
		}
		if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) {
			continue;
		}
		size_t prefix = strlen(relative), name = strlen(entry->d_name);
		char *path = malloc(prefix + (prefix != 0) + name + 1);
		if (!path) {
			project_scan_stopped = project_files_limited = 1;
			break;
		}
		memcpy(path, relative, prefix);
		size_t at = prefix;
		if (prefix) {
			path[at++] = '/';
		}
		memcpy(path + at, entry->d_name, name + 1);
		if (S_ISREG(info.st_mode)) {
			if (project_file_add(path)) {
				continue;
			}
			project_scan_stopped = project_files_limited = 1;
		} else if (depth == PROJECT_DEPTH_LIMIT) {
			project_files_limited = 1;
		} else {
			int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (child < 0) {
				project_files_limited = 1;
			} else {
				scan_files(child, path, depth + 1);
			}
		}
		free(path);
	}
	closedir(directory);
}

static int project_file_compare(const void *a, const void *b) {

	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void project_files_load(void) {

	struct stat info;
	if (stat(".", &info)) {
		project_files_clear();
		project_files_limited = 1;
		return;
	}
	if (project_files_valid && info.st_dev == project_device && info.st_ino == project_inode) {
		return;
	}
	project_files_clear();
	int fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &info)) {
		if (fd >= 0) {
			close(fd);
		}
		project_files_limited = 1;
		return;
	}
	project_device = info.st_dev;
	project_inode = info.st_ino;
	scan_files(fd, "", 0);
	if (project_file_count > 1) {
		qsort(project_files, project_file_count, sizeof(*project_files), project_file_compare);
	}
	project_files_valid = 1;
}

/* Compare characters, not individual UTF-8 bytes. Unrepresentable/control
   characters cannot be inserted safely into the draft, so omit those names. */
static int fuzzy_file(const char *path, const wchar_t *query, size_t room) {

	mbstate_t state = {0};
	size_t len = 0;
	while (*path) {
		wchar_t ch;
		size_t bytes = mbrtowc(&ch, path, MB_CUR_MAX, &state);
		if (bytes == (size_t)-1 || bytes == (size_t)-2 || !bytes || wcwidth(ch) < 0) {
			return 0;
		}
		if (++len > room) {
			return 0;
		}
		if (*query && towlower(ch) == towlower(*query)) {
			query++;
		}
		path += bytes;
	}

	return !*query;
}
