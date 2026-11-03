#define _GNU_SOURCE
#include "sessions.h"
#include "json.h"
#include "history.h"
#include "fsutil.h"
#include "session-name.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <wchar.h>
#include <wctype.h>

#define SESSION_MAX (512 * 1024 * 1024)
#define SESSION_PREVIEW_BYTES (64 * 1024)
#define SESSION_LIST_MAX 4096

static int transcript_directory(char *path, size_t size) {

	return phi_xdg_path(path, size, "XDG_STATE_HOME", ".local/state", "phi/transcripts");
}

/* Reuse one parser, but reset it before each independent JSONL record. */
static json_object *parse_record(struct json_tokener *parser, const char *line, size_t bytes) {

	json_tokener_reset(parser);
	if (memchr(line, 0, bytes)) {
		return NULL;
	}
	json_object *event = json_tokener_parse_ex(parser, line, (int)bytes);
	size_t end = json_tokener_get_parse_end(parser);
	while (end < bytes && isspace((unsigned char)line[end])) {
		end++;
	}
	if (json_tokener_get_error(parser) != json_tokener_success || end != bytes ||
		!json_object_is_type(event, json_type_object)) {
		if (event) {
			json_object_put(event);
		}
		return NULL;
	}

	return event;
}

static int string_field(json_object *object, const char *name) {

	json_object *value = json_object_object_get(object, name);

	return json_object_is_type(value, json_type_string) &&
		strlen(json_object_get_string(value)) == (size_t)json_object_get_string_len(value);
}

static int read_metadata(json_object *event, struct session_entry *entry) {

	json_object *data = json_object_object_get(event, "data");
	int version = json_object_get_int(json_object_object_get(data, "version"));
	if (strcmp(field(event, "type"), "session") ||
		strcmp(field(data, "format"), "phi-transcript") || (version != 1 && version != 2)) {
		return 0;
	}
	const char *cwd = field(data, "cwd");
	if (!string_field(data, "cwd") || *cwd != '/' || strlen(cwd) >= sizeof(entry->cwd)) {
		return 0;
	}
	snprintf(entry->cwd, sizeof(entry->cwd), "%s", cwd);
	entry->version = version;

	return 1;
}

static size_t copy_prefix(char *destination, size_t capacity, const char *source) {

	size_t copied = strnlen(source, capacity - 1);
	memcpy(destination, source, copied);
	destination[copied] = 0;

	return copied;
}

void session_item_text(json_object *item, char *text, size_t size) {

	if (!size) {
		return;
	}
	size_t used = 0;
	*text = 0;
	json_object *content = json_object_object_get(item, "content");
	if (json_object_is_type(content, json_type_string)) {
		used = copy_prefix(text, size, json_object_get_string(content));
	} else if (json_object_is_type(content, json_type_array)) {
		size_t count = json_object_array_length(content);
		for (size_t i = 0; i < count && used + 1 < size; i++) {
			json_object *part = json_object_array_get_idx(content, i);
			const char *kind = field(part, "type");
			if (strcmp(kind, "input_text") && strcmp(kind, "output_text")) {
				continue;
			}
			if (used) {
				text[used++] = '\n';
			}
			used += copy_prefix(text + used, size - used, field(part, "text"));
		}
	}
	size_t start = used;
	while (start && ((unsigned char)text[start - 1] & 0xc0) == 0x80) {
		start--;
	}
	if (start) {
		unsigned char lead = (unsigned char)text[start - 1];
		size_t wanted = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
		if (used - (start - 1) < wanted) {
			text[start - 1] = 0;
		}
	}
}

static void clean_title(char *title, size_t size) {

	for (char *p = title; *p; p++) {
		if ((unsigned char)*p < 32 || *p == 127) {
			*p = ' ';
		}
	}
	if (!*title) {
		snprintf(title, size, "Untitled session");
	}
}

static void update_preview(json_object *event, struct session_entry *entry) {

	json_object *data = json_object_object_get(event, "data");
	const char *type = field(event, "type");
	if (!strcmp(type, "cwd") && json_object_is_type(data, json_type_string)) {
		const char *cwd = json_object_get_string(data);
		if (*cwd == '/' && strlen(cwd) < sizeof(entry->cwd)) {
			snprintf(entry->cwd, sizeof(entry->cwd), "%s", cwd);
		}
	} else if (!strcmp(type, "name") && json_object_is_type(data, json_type_string)) {
		snprintf(entry->title, sizeof(entry->title), "%s", json_object_get_string(data));
		entry->named = 1;
		clean_title(entry->title, sizeof(entry->title));
	} else if (!strcmp(type, "item") && !strcmp(field(data, "role"), "user") &&
		!history_is_hidden(data)) {
		/* The opening prompt is a stable fallback, not a rolling title. */
		if (!entry->named && !*entry->title) {
			session_item_text(data, entry->title, sizeof(entry->title));
			if (*entry->title) {
				clean_title(entry->title, sizeof(entry->title));
			}
		}
	}
}

/* Stars are local sidecar markers, not conversation events. They neither
   touch transcript mtime (--continue ordering) nor alter imported history. */
static int star_path(const struct session_entry *entry, char *path, size_t size) {

	int n = snprintf(path, size, "%s.phi-star", entry->path);

	return n >= 0 && (size_t)n < size;
}

static int is_starred(const struct session_entry *entry) {

	char path[4128];
	struct stat st;

	return star_path(entry, path, sizeof(path)) && !lstat(path, &st) && S_ISREG(st.st_mode) &&
		st.st_uid == geteuid() && st.st_size == 0;
}

int session_star(struct session_entry *entry, int starred, char *error, size_t size) {

	char path[4128];
	struct stat st;
	*error = 0;
	if (lstat(entry->path, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		st.st_dev != entry->device || st.st_ino != entry->inode) {
		snprintf(error, size, "Transcript changed or is unavailable.");
		return 0;
	}
	if (!star_path(entry, path, sizeof(path))) {
		snprintf(error, size, "Transcript path is too long.");
		return 0;
	}
	int exists = is_starred(entry);
	if (starred && !exists) {
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0) {
			snprintf(error, size, "Cannot star transcript: %s", strerror(errno));
			return 0;
		}
		close(fd);
	} else if (!starred && exists && unlink(path)) {
		snprintf(error, size, "Cannot unstar transcript: %s", strerror(errno));
		return 0;
	}
	entry->starred = !!starred;

	return 1;
}

static char *complete_record_start(int fd, char *text, off_t offset) {

	if (!offset) {
		return text;
	}
	char before;
	if (pread(fd, &before, 1, offset - 1) == 1 && before == '\n') {
		return text;
	}
	char *newline = strchr(text, '\n');

	return newline ? newline + 1 : NULL;
}

static int read_preview(int directory_fd, const char *directory, const char *name,
	struct json_tokener *parser, struct session_entry *entry) {

	int fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		return 0;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		st.st_size > SESSION_MAX) {
		close(fd);
		return 0;
	}
	char head[SESSION_PREVIEW_BYTES + 1];
	ssize_t got = read(fd, head, sizeof(head) - 1);
	if (got <= 0) {
		close(fd);
		return 0;
	}
	head[got] = 0;

	*entry =
		(struct session_entry){.modified = st.st_mtime, .device = st.st_dev, .inode = st.st_ino};
	int length = snprintf(entry->path, sizeof(entry->path), "%s/%s", directory, name);
	if (length < 0 || (size_t)length >= sizeof(entry->path)) {
		close(fd);
		return 0;
	}
	char *line = head;
	char *end = strchr(line, '\n');
	if (!end) {
		close(fd);
		return 0;
	}
	json_object *event = parse_record(parser, line, (size_t)(end + 1 - line));
	int valid = event && read_metadata(event, entry);
	if (event) {
		json_object_put(event);
	}
	if (!valid) {
		close(fd);
		return 0;
	}

	entry->starred = is_starred(entry);
	entry->title[0] = 0;
	line = end + 1;
	while ((end = strchr(line, '\n'))) {
		event = parse_record(parser, line, (size_t)(end + 1 - line));
		if (event) {
			update_preview(event, entry);
			json_object_put(event);
		}
		line = end + 1;
	}
	clean_title(entry->title, sizeof(entry->title));
	/* Explicit names may be far outside both discovery windows. The index
       points into this same owned inode; never trust cached title text. */
	off_t name_offset = session_name_index_read(entry->path, &st);
	if (name_offset >= 0) {
		char record[SESSION_PREVIEW_BYTES + 1];
		ssize_t bytes = pread(fd, record, sizeof(record) - 1, name_offset);
		if (bytes > 0) {
			record[bytes] = 0;
			char *newline = strchr(record, '\n');
			event = newline ? parse_record(parser, record, (size_t)(newline + 1 - record)) : NULL;
			if (event) {
				if (!strcmp(field(event, "type"), "name") && string_field(event, "data")) {
					update_preview(event, entry);
				}
				json_object_put(event);
			}
		}
	}
	/* The head owns the stable fallback title; the tail supplies recent
       explicit names/cwd changes without scanning a whole conversation. */
	if (st.st_size > got) {
		off_t offset = st.st_size > SESSION_PREVIEW_BYTES ? st.st_size - SESSION_PREVIEW_BYTES : 0;
		ssize_t tail = pread(fd, head, SESSION_PREVIEW_BYTES, offset);
		if (tail > 0) {
			head[tail] = 0;
			line = complete_record_start(fd, head, offset);
			while (line && (end = strchr(line, '\n'))) {
				event = parse_record(parser, line, (size_t)(end + 1 - line));
				if (event) {
					update_preview(event, entry);
					json_object_put(event);
				}
				line = end + 1;
			}
		}
	}
	close(fd);

	return 1;
}

int session_from_path(const char *path, struct session_entry *entry, char *error, size_t size) {

	char resolved[4096];
	*error = 0;
	if (!realpath(path, resolved)) {
		snprintf(error, size, "Cannot open transcript: %s", strerror(errno));
		return 0;
	}
	char *slash = strrchr(resolved, '/');
	if (!slash || !slash[1]) {
		snprintf(error, size, "Expected a transcript file.");
		return 0;
	}
	char name[4096];
	snprintf(name, sizeof(name), "%s", slash + 1);
	if (slash == resolved) {
		slash[1] = 0;
	} else {
		*slash = 0;
	}
	int fd = open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct json_tokener *parser = json_tokener_new();
	int ok = fd >= 0 && parser && read_preview(fd, resolved, name, parser, entry);
	if (fd >= 0) {
		close(fd);
	}
	if (parser) {
		json_tokener_free(parser);
	}
	if (!ok) {
		snprintf(error, size, "Not a readable, owned Phi transcript (JSONL).");
	}

	return ok;
}

int sessions_latest_in(const char *directory, struct session_entry *out, char *error, size_t size) {

	struct session_entry *entries = NULL;
	size_t count = 0;
	if (!sessions_list(&entries, &count, error, size)) {
		free(entries);
		return -1;
	}
	char here[PATH_MAX], there[PATH_MAX];
	int found = 0;
	if (realpath(directory, here)) {
		for (size_t i = 0; i < count && !found; i++) { /* newest first */
			if (!strcmp(entries[i].cwd, directory) ||
				(realpath(entries[i].cwd, there) && !strcmp(there, here))) {
				*out = entries[i];
				found = 1;
			}
		}
	}
	free(entries);

	return found;
}

static int newest_first(const void *a, const void *b) {

	const struct session_entry *x = a, *y = b;
	if (x->modified != y->modified) {
		return x->modified > y->modified ? -1 : 1;
	}

	return strcmp(x->path, y->path);
}

int sessions_list(struct session_entry **entries, size_t *count, char *error, size_t size) {

	*entries = NULL;
	*count = 0;
	*error = 0;
	char path[4096];
	if (!transcript_directory(path, sizeof(path))) {
		snprintf(error, size, "Cannot locate saved transcripts.");
		return 0;
	}
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		if (errno == ENOENT) {
			return 1;
		}
		snprintf(error, size, "Cannot open transcripts: %s", strerror(errno));
		return 0;
	}
	DIR *directory = fdopendir(fd);
	if (!directory) {
		close(fd);
		snprintf(error, size, "Cannot read transcript directory.");
		return 0;
	}
	struct json_tokener *parser = json_tokener_new();
	if (!parser) {
		goto out_of_memory;
	}
	size_t capacity = 0;
	struct dirent *file;
	while ((file = readdir(directory))) {
		size_t length = strlen(file->d_name);
		if (length < 7 || strcmp(file->d_name + length - 6, ".jsonl")) {
			continue;
		}
		struct session_entry entry;
		if (!read_preview(fd, path, file->d_name, parser, &entry)) {
			continue;
		}
		if (*count >= SESSION_LIST_MAX) {
			snprintf(error, size, "Showing at most 4096 saved sessions.");
			break;
		}
		if (*count == capacity) {
			size_t next_capacity = capacity ? capacity * 2 : 8;
			struct session_entry *next = realloc(*entries, next_capacity * sizeof(**entries));
			if (!next) {
				goto out_of_memory;
			}
			*entries = next;
			capacity = next_capacity;
		}
		(*entries)[(*count)++] = entry;
	}
	json_tokener_free(parser);
	closedir(directory);
	if (*count) {
		qsort(*entries, *count, sizeof(**entries), newest_first);
		if (*count < capacity) {
			struct session_entry *trimmed = realloc(*entries, *count * sizeof(**entries));
			if (trimmed) {
				*entries = trimmed;
			}
		}
	}
	return 1;

out_of_memory:
	if (parser) {
		json_tokener_free(parser);
	}
	closedir(directory);
	free(*entries);
	*entries = NULL;
	*count = 0;
	snprintf(error, size, "Not enough memory for session list.");

	return 0;
}

struct replay {
	json_object *history, *calls, *answers;
};

enum record_origin {
	SAVED_ITEM,
	CHECKPOINT_ITEM
};

static int hidden_item_valid(json_object *item) {

	json_object *hidden = NULL;
	if (!json_object_object_get_ex(item, "_phi_hidden", &hidden)) {
		return 1;
	}
	if (!json_object_is_type(hidden, json_type_boolean) || !json_object_get_boolean(hidden) ||
		strcmp(field(item, "role"), "user") || !string_field(item, "content") ||
		!string_field(item, "_phi_extension") || !*field(item, "_phi_extension") ||
		strlen(field(item, "_phi_extension")) > 48 || !string_field(item, "_phi_custom_type") ||
		!*field(item, "_phi_custom_type") || strlen(field(item, "_phi_custom_type")) > 48) {
		return 0;
	}

	return 1;
}

/* Hold only a short scalar identifier, never the owning history object.
   Scalar identity also prevents a reused item address acquiring an old time.
   This sidecar does not interfere with native extensions' JSON userdata. */
static struct saved_time {
	json_object *item, *anchor;
	const char *key;
	time_t time;
} *saved_times;

static size_t saved_count, saved_capacity;

static void sessions_forget_times(void) {

	for (size_t i = 0; i < saved_count; i++) {
		json_object_put(saved_times[i].anchor);
	}
	saved_count = 0;
}

void sessions_set_item_time(json_object *item, time_t time) {

	if (!item || !time) {
		return;
	}
	static const char *keys[] = {"role", "call_id", "type"};
	json_object *anchor = NULL;
	const char *key = NULL;
	for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
		json_object *value = json_object_object_get(item, keys[i]);
		if (json_object_is_type(value, json_type_string) &&
			json_object_get_string_len(value) <= 1024) {
			anchor = value;
			key = keys[i];
			break;
		}
	}
	if (!anchor) {
		return;
	}
	if (saved_count == saved_capacity) {
		size_t capacity = saved_capacity ? saved_capacity * 2 : 256;
		struct saved_time *next = realloc(saved_times, capacity * sizeof(*next));
		if (!next) {
			return;
		}
		saved_times = next;
		saved_capacity = capacity;
	}
	saved_times[saved_count++] = (struct saved_time){item, json_object_get(anchor), key, time};
}

static void sessions_mark_time(json_object *item, json_object *event) {

	json_object *time_field = json_object_object_get(event, "time");
	if (json_object_is_type(item, json_type_object) &&
		json_object_is_type(time_field, json_type_int)) {
		sessions_set_item_time(item, (time_t)json_object_get_int64(time_field));
	}
}

time_t sessions_item_time(json_object *item) {

	for (size_t i = saved_count; item && i; i--) {
		const struct saved_time *entry = saved_times + i - 1;
		if (entry->item == item && json_object_object_get(item, entry->key) == entry->anchor) {
			return entry->time;
		}
	}

	return 0;
}

static int replay_item(struct replay *state, json_object *item, enum record_origin origin) {

	if (!json_object_is_type(item, json_type_object)) {
		return 0;
	}
	const char *type = field(item, "type");
	const char *role = field(item, "role");
	const char *id = field(item, "call_id");
	int strict = origin == SAVED_ITEM;
	if (!*type || !strcmp(type, "message")) {
		if (strcmp(role, "user") && strcmp(role, "assistant")) {
			return 0;
		}
		json_object *content = json_object_object_get(item, "content");
		if (strict &&
			(!string_field(item, "role") ||
				!(json_object_is_type(content, json_type_string) ||
					json_object_is_type(content, json_type_array)) ||
				!hidden_item_valid(item))) {
			return 0;
		}
	} else if (!strcmp(type, "function_call")) {
		if (!*id || !*field(item, "name") || json_object_object_get(state->calls, id)) {
			return 0;
		}
		if (strict &&
			(*role || !string_field(item, "call_id") || strlen(id) > 1024 ||
				!string_field(item, "name") || !string_field(item, "arguments"))) {
			return 0;
		}
		json_object_object_add(state->calls, id, json_object_get(item));
	} else if (!strcmp(type, "function_call_output")) {
		if (!*id || !json_object_object_get(state->calls, id)) {
			return 0;
		}
		if (strict &&
			(*role || !string_field(item, "call_id") || !string_field(item, "output") ||
				json_object_object_get(state->answers, id))) {
			return 0;
		}
		json_object_object_add(state->answers, id, json_object_new_boolean(1));
	} else {
		if (strict && *role) {
			return 0;
		}
		if (strcmp(type, "reasoning") && strcmp(type, "image_generation_call")) {
			return 0;
		}
	}
	json_object_array_add(state->history, json_object_get(item));

	return 1;
}

static int replay_reset(struct replay *state) {

	json_object_array_del_idx(state->history, 0, json_object_array_length(state->history));
	json_object_put(state->calls);
	json_object_put(state->answers);
	state->calls = json_object_new_object();
	state->answers = json_object_new_object();

	return state->calls && state->answers;
}

static int replay_checkpoint(struct replay *state, json_object *items) {

	if (!json_object_is_type(items, json_type_array) || !json_object_array_length(items) ||
		!replay_reset(state)) {
		return 0;
	}
	sessions_forget_times();
	for (size_t i = 0; i < json_object_array_length(items); i++) {
		json_object *item = json_object_array_get_idx(items, i);
		const char *id = field(item, "call_id");
		if (!strcmp(field(item, "type"), "function_call_output") && *id &&
			!json_object_object_get(state->calls, id)) {
			/* Older compaction checkpoints could retain a result while
               summarizing away its call. The saved result is inert history;
               drop only the orphan rather than rejecting the whole session.
               Never synthesize or replay the missing tool call. */
			continue;
		}
		if (!replay_item(state, item, CHECKPOINT_ITEM)) {
			return 0;
		}
	}

	return 1;
}

static void recover_unanswered_calls(struct replay *state) {

	json_object_object_foreach(state->calls, id, call) {
		(void)call;
		if (json_object_object_get(state->answers, id)) {
			continue;
		}
		json_object *answer = json_object_new_object();
		json_object_object_add(answer, "type", json_object_new_string("function_call_output"));
		json_object_object_add(answer, "call_id", json_object_new_string(id));
		json_object_object_add(answer, "output",
			json_object_new_string(
				"{\"error\":\"No saved result: session ended before this tool result was recorded. "
				"The action may or may not have run; it has NOT been replayed.\"}"));
		json_object_array_add(state->history, answer);
	}
}

/* Read at most the indexed snapshot, even if another writer appends an
   unfinished giant record while we scan. stdio still buffers disk reads. */
static ssize_t snapshot_record(FILE *file, char **text, size_t *capacity, size_t limit) {

	size_t used = 0;
	while (used < limit) {
		int ch = getc_unlocked(file);
		if (ch == EOF) {
			if (ferror(file) && errno == EINTR) {
				clearerr(file);
				continue;
			}
			if (ferror(file)) {
				return -1;
			}
			break;
		}
		if (used + 1 >= *capacity) {
			size_t next = *capacity ? *capacity * 2 : 4096;
			if (next > limit + 1) {
				next = limit + 1;
			}
			char *grown = realloc(*text, next);
			if (!grown) {
				return -1;
			}
			*text = grown;
			*capacity = next;
		}
		(*text)[used++] = (char)ch;
		if (ch == '\n') {
			break;
		}
	}
	if (used) {
		(*text)[used] = 0;
	}

	return (ssize_t)used;
}

static int open_session_fd(
	const struct session_entry *entry, struct stat *info, char *error, size_t size) {

	int fd = open(entry->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		st.st_dev != entry->device || st.st_ino != entry->inode || st.st_size <= 0 ||
		st.st_size > SESSION_MAX) {
		if (fd >= 0) {
			close(fd);
		}
		snprintf(error, size, "Transcript unavailable, replaced, or too large.");
		return -1;
	}
	*info = st;

	return fd;
}

/* Bounded, read-only discovery: no history reconstruction or tool replay.
   Ignore a cut first JSONL record and any crash-truncated final record. */
int session_recent_text(
	const struct session_entry *entry, char *text, size_t size, char *error, size_t error_size) {

	if (error_size) {
		*error = 0;
	}
	if (!size) {
		return 0;
	}
	*text = 0;
	struct stat st;
	int fd = open_session_fd(entry, &st, error, error_size);
	if (fd < 0) {
		return 0;
	}
	char tail[SESSION_PREVIEW_BYTES + 1];
	off_t offset = st.st_size > SESSION_PREVIEW_BYTES ? st.st_size - SESSION_PREVIEW_BYTES : 0;
	ssize_t got = pread(fd, tail, SESSION_PREVIEW_BYTES, offset);
	if (got < 0) {
		int saved = errno;
		close(fd);
		snprintf(error, error_size, "Cannot read recent messages: %s", strerror(saved));
		return 0;
	}
	tail[got] = 0;
	char *line = complete_record_start(fd, tail, offset), *end;
	close(fd);
	struct json_tokener *parser = json_tokener_new();
	if (!parser) {
		snprintf(error, error_size, "Out of memory.");
		return 0;
	}
	while (line && (end = strchr(line, '\n'))) {
		json_object *event = parse_record(parser, line, (size_t)(end + 1 - line));
		json_object *item = event ? json_object_object_get(event, "data") : NULL;
		const char *role = field(item, "role");
		if (event && !strcmp(field(event, "type"), "item") && !history_is_hidden(item) &&
			(!strcmp(role, "user") || !strcmp(role, "assistant"))) {
			char message[SESSION_PREVIEW_BYTES + 1];
			session_item_text(item, message, sizeof(message));
			size_t n = strlen(message), used = strlen(text), extra = strlen(role) + 3;
			if (n && n + extra < size) {
				/* Evict complete old lines, never split UTF-8. */
				while (used + n + extra >= size && used) {
					char *next = strchr(text, '\n');
					size_t drop = next ? (size_t)(next + 1 - text) : used;
					memmove(text, text + drop, used - drop + 1);
					used -= drop;
				}
				snprintf(text + used, size - used, "%s\n%s\n\n", role, message);
			}
		}
		if (event) {
			json_object_put(event);
		}
		line = end + 1;
	}
	json_tokener_free(parser);

	return 1;
}

static int open_session(
	const struct session_entry *entry, char **text, size_t *bytes, char *error, size_t size) {

	*text = NULL;
	*bytes = 0;
	struct stat st;
	int fd = open_session_fd(entry, &st, error, size);
	if (fd < 0) {
		return 0;
	}
	char *buffer = malloc((size_t)st.st_size + 1);
	if (!buffer) {
		close(fd);
		snprintf(error, size, "Not enough memory to load transcript.");
		return 0;
	}
	size_t got = 0;
	while (got < (size_t)st.st_size) {
		ssize_t n = read(fd, buffer + got, (size_t)st.st_size - got);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		got += (size_t)n;
	}
	close(fd);
	buffer[got] = 0;
	if (got != (size_t)st.st_size) {
		free(buffer);
		snprintf(error, size, "Transcript could not be read completely.");
		return 0;
	}
	*text = buffer;
	*bytes = got;

	return 1;
}

static int node_shape(json_object *event, size_t expected, uint64_t *id, uint64_t *parent) {

	json_object *id_value = json_object_object_get(event, "id");
	json_object *parent_value = json_object_object_get(event, "parent");
	if (!json_object_is_type(id_value, json_type_int) ||
		!json_object_is_type(parent_value, json_type_int)) {
		return 0;
	}
	int64_t raw_id = json_object_get_int64(id_value),
			raw_parent = json_object_get_int64(parent_value);
	if (raw_id <= 0 || raw_parent < 0 || (uint64_t)raw_id != expected + 1 ||
		(uint64_t)raw_parent > expected) {
		return 0;
	}
	*id = (uint64_t)raw_id;
	*parent = (uint64_t)raw_parent;

	return 1;
}

void session_tree_free(struct session_tree *tree) {

	if (!tree) {
		return;
	}
	for (size_t i = 0; i < tree->count; i++) {
		if (tree->nodes[i].data) {
			json_object_put(tree->nodes[i].data);
		}
	}
	if (tree->source) {
		fclose(tree->source);
	}
	free(tree->nodes);
	*tree = (struct session_tree){0};
}

static unsigned session_display_flags(json_object *data) {

	const char *role = field(data, "role"), *type = field(data, "type");
	unsigned flags = !strcmp(role, "assistant") ? 2u
		: !strcmp(role, "user")                 ? 1u |
			(json_object_is_type(json_object_object_get(data, "content"), json_type_string) ? 4u
																							: 0u)
								: 0u;
	if (!strcmp(type, "function_call") || !strcmp(type, "function_call_output")) {
		flags |= 8u;
	}
	if (!strcmp(type, "reasoning")) {
		flags |= 16u;
		json_object *summary = json_object_object_get(data, "summary");
		for (size_t i = 0;
			json_object_is_type(summary, json_type_array) && i < json_object_array_length(summary);
			i++) {
			json_object *part = json_object_array_get_idx(summary, i);
			if (!strcmp(field(part, "type"), "summary_text") && *field(part, "text")) {
				flags |= 32u;
				break;
			}
		}
	}

	return flags;
}

static int tree_add(struct session_tree *tree, size_t *capacity, unsigned *root_children,
	json_object *event, enum session_node_kind kind, off_t offset, size_t bytes) {

	uint64_t id, parent;
	if (!node_shape(event, tree->count, &id, &parent)) {
		return 0;
	}
	json_object *data = json_object_object_get(event, "data");
	if ((kind == SESSION_ITEM && !json_object_is_type(data, json_type_object)) ||
		(kind == SESSION_COMPACTION && !json_object_is_type(data, json_type_array))) {
		return 0;
	}
	if (tree->count == *capacity) {
		size_t next_capacity = *capacity ? *capacity * 2 : 32;
		struct session_node *next = realloc(tree->nodes, next_capacity * sizeof(*next));
		if (!next) {
			return -1;
		}
		tree->nodes = next;
		*capacity = next_capacity;
	}
	unsigned depth;
	if (parent) {
		struct session_node *parent_node = tree->nodes + parent - 1;
		depth = parent_node->depth + (parent_node->children ? 1u : 0u);
		parent_node->children++;
	} else {
		depth = *root_children ? 1u : 0u;
		(*root_children)++;
	}
	tree->nodes[tree->count++] = (struct session_node){.id = id,
		.parent = parent,
		.time = (time_t)json_object_get_int64(json_object_object_get(event, "time")),
		.depth = depth,
		.kind = kind,
		.offset = offset,
		.bytes = bytes,
		.hidden = kind == SESSION_ITEM && history_is_hidden(data),
		.display = kind == SESSION_ITEM ? session_display_flags(data) : 0u};
	tree->head = id;

	return 1;
}

int session_tree_index(
	struct session_entry *entry, struct session_tree *tree, char *error, size_t size) {

	*tree = (struct session_tree){0};
	*error = 0;
	struct stat info;
	int fd = open_session_fd(entry, &info, error, size);
	if (fd < 0) {
		return 0;
	}
	tree->source = fdopen(fd, "r");
	if (!tree->source) {
		close(fd);
		snprintf(error, size, "Cannot read transcript.");
		return 0;
	}
	tree->source_bytes = info.st_size;
	size_t bytes = (size_t)info.st_size;
	char *text = NULL;
	size_t text_capacity = 0;
	struct json_tokener *parser = json_tokener_new();
	size_t offset = 0, records = 0, capacity = 0;
	unsigned root_children = 0;
	int valid = parser != NULL, version = 0;
	while (valid && offset < bytes) {
		ssize_t got = snapshot_record(tree->source, &text, &text_capacity, bytes - offset);
		if (got <= 0) {
			valid = 0;
			break;
		}
		size_t record_bytes = (size_t)got, record_offset = offset;
		if (record_bytes > bytes - offset || text[record_bytes - 1] != '\n') {
			break; /* unfinished snapshot tail */
		}
		json_object *event = parse_record(parser, text, record_bytes);
		offset += record_bytes;
		if (!event) {
			valid = 0;
			break;
		}
		if (!records++) {
			struct session_entry metadata = {0};
			valid = read_metadata(event, &metadata);
			version = metadata.version;
			if (valid) {
				entry->version = metadata.version;
				snprintf(entry->cwd, sizeof(entry->cwd), "%s", metadata.cwd);
			}
		} else if (version == 2) {
			const char *type = field(event, "type");
			json_object *data = json_object_object_get(event, "data");
			int added = 1;
			if (!strcmp(type, "item")) {
				added = tree_add(tree, &capacity, &root_children, event, SESSION_ITEM,
					(off_t)record_offset, record_bytes);
			} else if (!strcmp(type, "compaction")) {
				added = tree_add(tree, &capacity, &root_children, event, SESSION_COMPACTION,
					(off_t)record_offset, record_bytes);
			} else if (!strcmp(type, "head")) {
				int64_t head = json_object_get_int64(data);
				if (!json_object_is_type(data, json_type_int) || head < 0 ||
					(uint64_t)head > tree->count) {
					added = 0;
				} else {
					tree->head = (uint64_t)head;
				}
			} else if (!strcmp(type, "annotation")) {
				json_object *id = json_object_object_get(data, "entry");
				json_object *mark = json_object_object_get(data, "bookmarked");
				int64_t n = json_object_get_int64(id);
				const char *label = field(data, "label");
				if (!json_object_is_type(id, json_type_int) || n <= 0 ||
					(uint64_t)n > tree->count || !string_field(data, "label") ||
					strlen(label) >= sizeof(tree->nodes[0].label) ||
					!json_object_is_type(mark, json_type_boolean)) {
					added = 0;
				} else {
					struct session_node *node = tree->nodes + n - 1;
					snprintf(node->label, sizeof(node->label), "%s", label);
					for (char *p = node->label; *p; p++) {
						if ((unsigned char)*p < 32 || *p == 127) {
							*p = ' ';
						}
					}
					node->bookmarked = json_object_get_boolean(mark);
				}
			} else if (!strcmp(type, "cwd")) {
				const char *cwd =
					json_object_is_type(data, json_type_string) ? json_object_get_string(data) : "";
				if (!string_field(event, "data") || *cwd != '/' ||
					strlen(cwd) >= sizeof(entry->cwd)) {
					added = 0;
				} else {
					snprintf(entry->cwd, sizeof(entry->cwd), "%s", cwd);
				}
			} else if (!strcmp(type, "name")) {
				if (!string_field(event, "data")) {
					added = 0;
				} else {
					snprintf(
						entry->title, sizeof(entry->title), "%s", json_object_get_string(data));
					entry->named = 1;
					clean_title(entry->title, sizeof(entry->title));
				}
			}
			if (added <= 0) {
				valid = 0;
			}
		}
		json_object_put(event);
	}
	free(text);
	if (parser) {
		json_tokener_free(parser);
	}
	if (fstat(fileno(tree->source), &info) || info.st_size < tree->source_bytes) {
		valid = 0;
	}
	if (!valid || !records || version != 2) {
		session_tree_free(tree);
		snprintf(error, size,
			version == 1 ? "This session predates tree history; resume it once to upgrade."
						 : "Transcript tree is corrupt or unsupported.");
		return 0;
	}
	for (uint64_t id = tree->head; id; id = tree->nodes[id - 1].parent) {
		tree->nodes[id - 1].active = 1;
	}

	return 1;
}

json_object *session_tree_item(
	const struct session_tree *tree, size_t index, char *error, size_t size) {

	*error = 0;
	if (index >= tree->count) {
		goto unavailable;
	}
	const struct session_node *node = tree->nodes + index;
	if (node->data) {
		return json_object_get(node->data);
	}
	struct stat info;
	if (!tree->source || fstat(fileno(tree->source), &info) || info.st_size < tree->source_bytes ||
		node->offset < 0 || !node->bytes || node->bytes > SESSION_MAX ||
		node->offset > tree->source_bytes ||
		node->bytes > (size_t)(tree->source_bytes - node->offset)) {
		goto unavailable;
	}
	char *text = malloc(node->bytes + 1);
	if (!text) {
		goto unavailable;
	}
	size_t used = 0;
	while (used < node->bytes) {
		ssize_t got = pread(
			fileno(tree->source), text + used, node->bytes - used, node->offset + (off_t)used);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			break;
		}
		used += (size_t)got;
	}
	text[used] = 0;
	struct json_tokener *parser = json_tokener_new();
	json_object *event = used == node->bytes && parser ? parse_record(parser, text, used) : NULL;
	free(text);
	if (parser) {
		json_tokener_free(parser);
	}
	uint64_t id, parent;
	const char *type = node->kind == SESSION_ITEM ? "item" : "compaction";
	json_object *data = event ? json_object_object_get(event, "data") : NULL;
	int valid = event && node_shape(event, index, &id, &parent) && id == node->id &&
		parent == node->parent && !strcmp(field(event, "type"), type) &&
		json_object_is_type(data, node->kind == SESSION_ITEM ? json_type_object : json_type_array);
	json_object *result = valid ? json_object_get(data) : NULL;
	if (event) {
		json_object_put(event);
	}
	if (result) {
		return result;
	}
unavailable:
	snprintf(error, size, "Saved history record is unavailable, changed, or truncated.");

	return NULL;
}

/* Compatibility for consumers which explicitly request all payloads (exports
   and the branch picker). Agent resume uses the disk-backed index directly. */
int session_tree_load(
	struct session_entry *entry, struct session_tree *tree, char *error, size_t size) {

	if (!session_tree_index(entry, tree, error, size)) {
		return 0;
	}
	for (size_t i = 0; i < tree->count; i++) {
		tree->nodes[i].data = session_tree_item(tree, i, error, size);
		if (!tree->nodes[i].data) {
			session_tree_free(tree);
			return 0;
		}
	}

	return 1;
}

json_object *session_tree_history(
	const struct session_tree *tree, uint64_t head, char *error, size_t size) {

	*error = 0;
	sessions_forget_times();
	if (head > tree->count) {
		snprintf(error, size, "Tree selection no longer exists.");
		return NULL;
	}
	struct replay state = {.history = json_object_new_array(),
		.calls = json_object_new_object(),
		.answers = json_object_new_object()};
	if (!state.history || !state.calls || !state.answers) {
		goto invalid;
	}
	uint64_t *chain = malloc((tree->count + 1) * sizeof(*chain));
	if (!chain) {
		goto invalid;
	}
	size_t count = 0;
	for (uint64_t id = head; id; id = tree->nodes[id - 1].parent) {
		chain[count++] = id;
	}
	int valid = 1;
	while (count && valid) {
		const struct session_node *node = &tree->nodes[chain[--count] - 1];
		json_object *data = session_tree_item(tree, (size_t)(node - tree->nodes), error, size);
		if (!data) {
			valid = 0;
			break;
		}
		if (node->kind == SESSION_ITEM) {
			sessions_set_item_time(data, node->time);
		}
		valid = node->kind == SESSION_COMPACTION ? replay_checkpoint(&state, data)
												 : replay_item(&state, data, SAVED_ITEM);
		json_object_put(data);
	}
	free(chain);
	if (!valid) {
		goto invalid;
	}
	recover_unanswered_calls(&state);
	json_object_put(state.calls);
	json_object_put(state.answers);
	return state.history;

invalid:
	if (state.history) {
		json_object_put(state.history);
	}
	if (state.calls) {
		json_object_put(state.calls);
	}
	if (state.answers) {
		json_object_put(state.answers);
	}
	snprintf(error, size, "Selected branch contains unsupported or inconsistent history.");

	return NULL;
}

static json_object *load_v1(
	struct session_entry *entry, char *text, size_t bytes, char *error, size_t size) {

	sessions_forget_times();
	struct replay state = {.history = json_object_new_array(),
		.calls = json_object_new_object(),
		.answers = json_object_new_object()};
	struct json_tokener *parser = json_tokener_new();
	int valid = state.history && state.calls && state.answers && parser;
	size_t offset = 0, records = 0;
	while (valid && offset < bytes) {
		char *end = memchr(text + offset, '\n', bytes - offset);
		if (!end) {
			break;
		}
		size_t record_bytes = (size_t)(end + 1 - (text + offset));
		json_object *event = parse_record(parser, text + offset, record_bytes);
		offset += record_bytes;
		if (!event) {
			valid = 0;
			break;
		}
		json_object *data = json_object_object_get(event, "data");
		const char *type = field(event, "type");
		if (!records++) {
			struct session_entry metadata = {0};
			valid = read_metadata(event, &metadata) && metadata.version == 1;
		} else if (!strcmp(type, "cwd")) {
			if (!json_object_is_type(data, json_type_string) ||
				*json_object_get_string(data) != '/' ||
				strlen(json_object_get_string(data)) >= sizeof(entry->cwd)) {
				valid = 0;
			} else {
				snprintf(entry->cwd, sizeof(entry->cwd), "%s", json_object_get_string(data));
			}
		} else if (!strcmp(type, "compaction")) {
			valid = replay_checkpoint(&state, data);
		} else if (!strcmp(type, "item")) {
			sessions_mark_time(data, event);
			valid = replay_item(&state, data, SAVED_ITEM);
		}
		json_object_put(event);
	}
	if (parser) {
		json_tokener_free(parser);
	}
	valid = valid && records && json_object_array_length(state.history);
	if (valid) {
		recover_unanswered_calls(&state);
	}
	if (state.calls) {
		json_object_put(state.calls);
	}
	if (state.answers) {
		json_object_put(state.answers);
	}
	if (!valid) {
		if (state.history) {
			json_object_put(state.history);
		}
		snprintf(error, size, "Transcript is empty, corrupt, or contains unsupported history.");
		return NULL;
	}

	return state.history;
}

json_object *session_load_ex(
	struct session_entry *entry, uint64_t *head, char *error, size_t size) {

	*error = 0;
	if (head) {
		*head = 0;
	}
	struct stat info;
	int fd = open_session_fd(entry, &info, error, size);
	if (fd < 0) {
		return NULL;
	}
	FILE *file = fdopen(fd, "r");
	if (!file) {
		close(fd);
		snprintf(error, size, "Cannot read transcript.");
		return NULL;
	}
	/* Format detection must not allocate the whole saved conversation. */
	char *text = NULL;
	size_t capacity = 0;
	ssize_t bytes = snapshot_record(file, &text, &capacity, (size_t)info.st_size);
	fclose(file);
	struct json_tokener *parser = json_tokener_new();
	json_object *first = bytes > 0 && bytes <= info.st_size && text[bytes - 1] == '\n' && parser
		? parse_record(parser, text, (size_t)bytes)
		: NULL;
	struct session_entry metadata = {0};
	int valid = first && read_metadata(first, &metadata);
	if (first) {
		json_object_put(first);
	}
	if (parser) {
		json_tokener_free(parser);
	}
	if (!valid) {
		free(text);
		snprintf(error, size, "Transcript metadata is corrupt or unsupported.");
		return NULL;
	}
	entry->version = metadata.version;
	free(text);
	if (metadata.version == 1) {
		size_t legacy_bytes;
		if (!open_session(entry, &text, &legacy_bytes, error, size)) {
			return NULL;
		}
		json_object *history = load_v1(entry, text, legacy_bytes, error, size);
		free(text);
		return history;
	}
	struct session_tree tree;
	if (!session_tree_index(entry, &tree, error, size)) {
		return NULL;
	}
	json_object *history = session_tree_history(&tree, tree.head, error, size);
	if (history && head) {
		*head = tree.head;
	}
	session_tree_free(&tree);

	return history;
}

json_object *session_load(struct session_entry *entry, char *error, size_t size) {

	return session_load_ex(entry, NULL, error, size);
}

static int score_text(const char *joined, size_t title_length, const char *query) {

	wchar_t haystack[SESSION_PREVIEW_BYTES + 1], needle[256];
	size_t length = mbstowcs(haystack, joined, sizeof(haystack) / sizeof(*haystack));
	size_t query_length = mbstowcs(needle, query, sizeof(needle) / sizeof(*needle));
	if (length == (size_t)-1 || query_length == (size_t)-1 || query_length >= 256) {
		return -1;
	}
	int score = 0, consecutive = 0;
	size_t at = 0;
	for (size_t i = 0; i < query_length; i++) {
		if (iswspace(needle[i])) {
			at = 0;
			consecutive = 0;
			continue;
		}
		wint_t wanted = towlower(needle[i]);
		while (at < length && towlower(haystack[at]) != wanted) {
			at++;
			consecutive = 0;
		}
		if (at == length) {
			return -1;
		}
		score += 10 + (consecutive ? 15 : 0) + (!at || !iswalnum(haystack[at - 1]) ? 12 : 0);
		if (at < title_length) {
			score += 3;
		}
		at++;
		consecutive = 1;
	}

	return score;
}

int session_text_score(const char *text, const char *query) {

	return score_text(text, 0, query);
}

int session_score(const struct session_entry *entry, const char *query) {

	char joined[4608];
	snprintf(joined, sizeof(joined), "%s %s", entry->title, entry->cwd);
	int score = score_text(joined, mbstowcs(NULL, entry->title, 0), query);
	if (score >= 0) {
		return *query ? score + 1000 : score;
	}
	char recent[SESSION_PREVIEW_BYTES + 1], error[256];
	if (!session_recent_text(entry, recent, sizeof(recent), error, sizeof(error))) {
		return -1;
	}
	score = score_text(recent, 0, query);

	return score > 999 ? 999 : score;
}
