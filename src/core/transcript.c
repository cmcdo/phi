#define _GNU_SOURCE
#include "transcript.h"
#include "../tools.h"
#include "fsutil.h"
#include "session-name.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static void failed(struct transcript *t) {

	if (t->failed) {
		return;
	}
	t->failed = 1;
	tool_report(stderr, "\nTranscript saving failed: %s. Conversation can continue.\n",
		strerror(errno ? errno : EIO));
}

static int private_directory(const char *path) {

	struct stat st;
	if (lstat(path, &st)) {
		return 0;
	}
	if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
		errno = EACCES;
		return 0;
	}

	return !chmod(path, 0700);
}

static int write_event(struct transcript *t, const char *type, json_object *data, uint64_t id,
	uint64_t parent, int node);

/* Write buffered reply text as one record, keeping it ahead of later events. */
static void flush_text(struct transcript *t) {

	if (!t->text_used) {
		return;
	}
	t->text_used = 0; /* before writing: write_event calls back here */
	json_object *value = json_object_new_string(t->text);
	if (value) {
		write_event(t, "assistant_delta", value, 0, 0, 0);
	}
	json_object_put(value);
}

/* Another process appended to this file since our last write: adopt the
   node ids it used, so ours stay unique and in file order. */
static int catch_up(struct transcript *t, int fd) {

	struct stat st;
	if (fstat(fd, &st)) {
		return 0;
	}
	if (st.st_size < t->offset) {
		errno = ESTALE;
		return 0;
	}
	if (st.st_size == t->offset) {
		return 1;
	}
	size_t size = (size_t)(st.st_size - t->offset);
	char *text = malloc(size + 1);
	if (!text) {
		return 0;
	}
	size_t got = 0;
	while (got < size) {
		ssize_t n = pread(fd, text + got, size - got, (off_t)t->offset + (off_t)got);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			int error = n < 0 ? errno : EIO;
			free(text);
			errno = error;
			return 0;
		}
		got += (size_t)n;
	}
	{
		text[got] = 0;
		for (char *line = text, *end; *line; line = end + 1) {
			end = strchr(line, '\n');
			if (!end) {
				break;
			}
			*end = 0;
			json_object *event = json_tokener_parse(line);
			json_object *id = json_object_object_get(event, "id");
			if (json_object_is_type(id, json_type_int) &&
				(uint64_t)json_object_get_int64(id) >= t->next_id) {
				t->next_id = (uint64_t)json_object_get_int64(id) + 1;
			}
			if (event) {
				json_object_put(event);
			}
		}
	}
	free(text);

	return 1;
}

static int write_event(struct transcript *t, const char *type, json_object *data, uint64_t id,
	uint64_t parent, int node) {

	if (!t->file || t->failed) {
		return 0;
	}
	flush_text(t);
	if (t->failed) {
		return 0;
	}
	int fd = fileno(t->file);
	if (!phi_lock_exclusive(fd)) {
		failed(t);
		return 0;
	}
	if (!catch_up(t, fd)) {
		int error = errno;
		flock(fd, LOCK_UN);
		errno = error;
		failed(t);
		return 0;
	}
	if (node && id < t->next_id) {
		id = t->next_id; /* another writer took it */
	}
	json_object *event = json_object_new_object();
	if (!event) {
		flock(fd, LOCK_UN);
		errno = ENOMEM;
		failed(t);
		return 0;
	}
	json_object_object_add(event, "type", json_object_new_string(type));
	json_object_object_add(event, "time", json_object_new_int64((int64_t)time(NULL)));
	if (node) {
		json_object_object_add(event, "id", json_object_new_int64((int64_t)id));
		json_object_object_add(event, "parent", json_object_new_int64((int64_t)parent));
	}
	json_object_object_add(event, "data", json_object_get(data));
	off_t name_offset = !strcmp(type, "name") ? lseek(fd, 0, SEEK_END) : -1;
	int ok = fprintf(t->file, "%s\n",
				 json_object_to_json_string_ext(event, JSON_C_TO_STRING_PLAIN)) >= 0 &&
		!fflush(t->file);
	json_object_put(event);
	struct stat st;
	if (!fstat(fd, &st)) {
		t->offset = st.st_size;
	}
	if (ok && name_offset >= 0 && !session_name_index_write(t->path, fd, name_offset)) {
		tool_report(
			stderr, "\nSession name saved, but its discovery index could not be updated.\n");
	}
	if (ok && node) {
		t->head = id;
		t->next_id = id + 1;
	}
	flock(fd, LOCK_UN);
	if (!ok) {
		failed(t);
	}

	return ok;
}

static int record(struct transcript *t, const char *type, json_object *data) {

	return write_event(t, type, data, 0, 0, 0);
}

static int record_node(struct transcript *t, const char *type, json_object *data) {

	return write_event(t, type, data, t->next_id ? t->next_id : 1, t->head, 1);
}

static int saving_disabled;

void transcript_disable(int disabled) {

	saving_disabled = disabled;
}

int transcript_disabled(void) {

	return saving_disabled;
}

void transcript_begin(struct transcript *t, const char *model, const char *effort) {

	if (t->file || t->failed || saving_disabled) {
		return;
	}
	char directory[4096];
	if (!phi_xdg_path(directory, sizeof(directory) - sizeof("/transcripts"), "XDG_STATE_HOME",
			".local/state", "phi")) {
		errno = EINVAL;
		failed(t);
		return;
	}
	if (!phi_make_directories(directory) || !private_directory(directory)) {
		failed(t);
		return;
	}
	strcat(directory, "/transcripts");
	if (!phi_make_directories(directory) || !private_directory(directory)) {
		failed(t);
		return;
	}
	char stamp[32];
	time_t clock = time(NULL);
	struct tm utc;
	if (!gmtime_r(&clock, &utc) || !strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc)) {
		strcpy(stamp, "session");
	}
	int n = snprintf(t->path, sizeof(t->path), "%s/%s-XXXXXX.jsonl", directory, stamp);
	if (n < 0 || (size_t)n >= sizeof(t->path)) {
		t->path[0] = 0;
		errno = ENAMETOOLONG;
		failed(t);
		return;
	}
	int fd = mkstemps(t->path, 6); /* unique, O_EXCL, mode 0600 */
	if (fd < 0) {
		t->path[0] = 0;
		failed(t);
		return;
	}
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fchmod(fd, 0600)) {
		int error = errno;
		close(fd);
		unlink(t->path);
		t->path[0] = 0;
		errno = error;
		failed(t);
		return;
	}
	t->file = fcntl(fd, F_SETFL, O_APPEND) ? NULL : fdopen(fd, "a");
	if (!t->file) {
		int error = errno;
		close(fd);
		unlink(t->path);
		t->path[0] = 0;
		errno = error;
		failed(t);
		return;
	}
	t->head = 0;
	t->next_id = 1;
	json_object *meta = json_object_new_object();
	json_object_object_add(meta, "format", json_object_new_string("phi-transcript"));
	json_object_object_add(meta, "version", json_object_new_int(2));
	json_object_object_add(meta, "model", json_object_new_string(model));
	json_object_object_add(meta, "effort", json_object_new_string(effort));
	char *cwd = getcwd(NULL, 0);
	if (cwd) {
		json_object_object_add(meta, "cwd", json_object_new_string(cwd));
		free(cwd);
	}
	record(t, "session", meta);
	json_object_put(meta);
}

void transcript_sync(struct transcript *t, json_object *history) {

	while (t->saved < json_object_array_length(history)) {
		if (!record_node(t, "item", json_object_array_get_idx(history, t->saved))) {
			break;
		}
		t->saved++;
	}
}

void transcript_compact(struct transcript *t, json_object *history) {

	if (record_node(t, "compaction", history)) {
		t->saved = json_object_array_length(history);
	}
}

int transcript_import_node(
	struct transcript *t, const char *type, json_object *data, uint64_t id, uint64_t parent) {

	uint64_t expected = t->next_id ? t->next_id : 1;
	if (id != expected || parent >= id || (strcmp(type, "item") && strcmp(type, "compaction"))) {
		return 0;
	}

	return write_event(t, type, data, id, parent, 1);
}

int transcript_reopen(
	struct transcript *t, const char *path, uint64_t head, uint64_t next_id, size_t saved) {

	if (t->file || saving_disabled) {
		return 0;
	}
	int fd =
		open(path, O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC); /* read: catch-up and tail repair */
	struct stat st;
	if (fd < 0) {
		return 0;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		!(t->file = fdopen(fd, "a"))) {
		close(fd);
		errno = EACCES;
		return 0;
	}
	/* A crash can leave a partial last line, which loading ignores. Drop it
       before appending, or the next record would join it and be lost. */
	if (!phi_lock_exclusive(fd)) {
		int error = errno;
		fclose(t->file);
		t->file = NULL;
		errno = error;
		return 0;
	}
	if (!fstat(fd, &st) && st.st_size > 0) {
		char byte = 0;
		off_t end = st.st_size;
		if (pread(fd, &byte, 1, end - 1) == 1 && byte != '\n') {
			while (end > 0 && pread(fd, &byte, 1, end - 1) == 1 && byte != '\n') {
				end--;
			}
			if (ftruncate(fd, end) == 0) {
				st.st_size = end;
			}
		}
	}
	flock(fd, LOCK_UN);
	snprintf(t->path, sizeof(t->path), "%s", path);
	t->head = head;
	t->next_id = next_id ? next_id : 1;
	t->saved = saved;
	t->offset = st.st_size;

	return 1;
}

/* Use the normal locked, unbounded JSON writer for an ended chat too.
   Do not append session_end: this operation never opened a conversation. */
int transcript_name(const char *path, const char *name) {

	if (!path || !*path || !name || !*name || strpbrk(name, "\r\n")) {
		errno = EINVAL;
		return 0;
	}
	if (saving_disabled) {
		errno = EPERM;
		return 0;
	}
	struct transcript t = {0};
	if (!transcript_reopen(&t, path, 0, 1, 0)) {
		return 0;
	}
	json_object *value = json_object_new_string(name);
	int ok = 0;
	if (value) {
		ok = record(&t, "name", value);
	} else {
		errno = ENOMEM;
	}
	json_object_put(value);
	int error = errno;
	if (fclose(t.file) && ok) {
		ok = 0;
		error = errno;
	}
	errno = error;

	return ok;
}

int transcript_select(struct transcript *t, uint64_t head, size_t saved) {

	if (!t->file || t->failed || head >= t->next_id) {
		return 0;
	}
	json_object *value = json_object_new_int64((int64_t)head);
	if (!value) {
		errno = ENOMEM;
		failed(t);
		return 0;
	}
	int ok = record(t, "head", value);
	json_object_put(value);
	if (ok) {
		t->head = head;
		t->saved = saved;
	}

	return ok;
}

void transcript_link(struct transcript *t, const char *kind, const char *path, uint64_t entry) {

	if (!t->file || t->failed || !path || !*path) {
		return;
	}
	json_object *data = json_object_new_object();
	if (!data) {
		errno = ENOMEM;
		failed(t);
		return;
	}
	json_object_object_add(data, "kind", json_object_new_string(kind ? kind : "branch"));
	json_object_object_add(data, "path", json_object_new_string(path));
	json_object_object_add(data, "entry", json_object_new_int64((int64_t)entry));
	record(t, "parent_session", data);
	json_object_put(data);
}

int transcript_annotate(struct transcript *t, uint64_t id, const char *label, int bookmarked) {

	if (!t->file || t->failed || !id || id >= t->next_id || !label || strlen(label) >= 128) {
		return 0;
	}
	json_object *data = json_object_new_object();
	if (!data) {
		errno = ENOMEM;
		failed(t);
		return 0;
	}
	json_object_object_add(data, "entry", json_object_new_int64((int64_t)id));
	json_object_object_add(data, "label", json_object_new_string(label));
	json_object_object_add(data, "bookmarked", json_object_new_boolean(bookmarked));
	int ok = record(t, "annotation", data);
	json_object_put(data);

	return ok;
}

void transcript_note(struct transcript *t, const char *type, const char *text) {

	if (!t->file || t->failed) {
		return;
	}
	json_object *value = json_object_new_string(text);
	record(t, type, value);
	json_object_put(value);
}

void transcript_text(const char *text, void *context) {

	/* Separate from committed history: retain visible partial replies even
       when a response is cancelled or the connection fails. Deltas arrive a
       token at a time, so gather them and write one record per ~4 KiB or
       before the next event, rather than one JSON line per token. */
	struct transcript *t = context;
	if (!t->file || t->failed || !text || !*text) {
		return;
	}
	size_t n = strlen(text);
	if (t->text_used + n + 1 > t->text_capacity) {
		size_t capacity = t->text_capacity ? t->text_capacity : 4096;
		while (capacity < t->text_used + n + 1) {
			capacity *= 2;
		}
		char *grown = realloc(t->text, capacity);
		if (!grown) {
			flush_text(t);
			transcript_note(t, "assistant_delta", text);
			return;
		}
		t->text = grown;
		t->text_capacity = capacity;
	}
	memcpy(t->text + t->text_used, text, n + 1);
	t->text_used += n;
	if (t->text_used >= 4096) {
		flush_text(t);
	}
}

void transcript_close(struct transcript *t) {

	transcript_note(t, "session_end", "closed");
	if (t->file && fclose(t->file)) {
		failed(t);
	}
	free(t->text);
	*t = (struct transcript){0};
}

int transcript_entry(struct transcript *t, const char *type, json_object *data) {

	return record(t, type, data);
}
