#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#define EDIT_MAX_BYTES (4 * 1024 * 1024)
#define EDIT_MAX_REPLACEMENTS 256

/* Exact replacements against the original file. */
#include "edit-diff.inc"

static void display_edit_diff(char kind, size_t line, const char *text, size_t size, size_t first,
	size_t end, void *context) {

	(void)context;
	tool_diff(kind, line, text, size, first, end);
}

static int replacement_order(const void *left, const void *right) {

	const struct replacement *a = left, *b = right;

	return (a->start > b->start) - (a->start < b->start);
}

static int read_exactly(int fd, char *buffer, size_t size) {

	size_t used = 0;
	while (used < size) {
		ssize_t count = read(fd, buffer + used, size - used);
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return -1;
		}
		used += (size_t)count;
	}

	return 0;
}

static const char *load_replacements(json_object *edits, const char *original,
	struct replacement *changes, size_t count, size_t *updated_size) {

	for (size_t i = 0; i < count; i++) {
		json_object *entry = json_object_array_get_idx(edits, i);
		json_object *old_value = NULL, *new_value = NULL;
		if (!json_object_object_get_ex(entry, "oldText", &old_value) ||
			!json_object_object_get_ex(entry, "newText", &new_value)) {
			return "Each edit needs nonempty oldText and a newText string, "
				   "without NUL bytes.";
		}

		size_t old_size = 0, new_size = 0;
		const char *old_text = tool_plain_string(old_value, &old_size);
		const char *new_text = tool_plain_string(new_value, &new_size);
		if (!old_text || !new_text || !old_size) {
			return "Each edit needs nonempty oldText and a newText string, "
				   "without NUL bytes.";
		}

		char *match = strstr(original, old_text);
		if (!match) {
			return "oldText was not found exactly in the original file.";
		}
		if (strstr(match + 1, old_text)) {
			return "oldText matches more than once; supply more surrounding text.";
		}
		changes[i] = (struct replacement){.start = (size_t)(match - original),
			.length = old_size,
			.text = new_text,
			.size = new_size};
	}

	qsort(changes, count, sizeof(*changes), replacement_order);
	for (size_t i = 0; i < count; i++) {
		if (i && changes[i].start < changes[i - 1].start + changes[i - 1].length) {
			return "Edits overlap in the original file; merge them into one replacement.";
		}
		*updated_size -= changes[i].length;
		if (changes[i].size > EDIT_MAX_BYTES - *updated_size) {
			return "Edited file would exceed 4 MiB.";
		}
		*updated_size += changes[i].size;
	}

	return NULL;
}

static void apply_replacements(char *updated, const char *original, size_t original_size,
	const struct replacement *changes, size_t count) {

	size_t source = 0, destination = 0;
	for (size_t i = 0; i < count; i++) {
		size_t unchanged = changes[i].start - source;
		memcpy(updated + destination, original + source, unchanged);
		destination += unchanged;
		memcpy(updated + destination, changes[i].text, changes[i].size);
		destination += changes[i].size;
		source = changes[i].start + changes[i].length;
	}
	memcpy(updated + destination, original + source, original_size - source);
}

static json_object *run(json_object *args) {

	const char *error = "Expected path and a nonempty edits array.";
	json_object *path_value = NULL, *edits = NULL;
	const char *path = NULL;
	char *name = NULL, *original = NULL, *updated = NULL;
	struct replacement *changes = NULL;
	int fd = -1, replacement_status = 0;
	size_t count = 0, original_size = 0, updated_size = 0;

	if (!args || !json_object_object_get_ex(args, "path", &path_value) ||
		!(path = tool_plain_string(path_value, NULL)) ||
		!json_object_object_get_ex(args, "edits", &edits) ||
		!json_object_is_type(edits, json_type_array) ||
		!(count = json_object_array_length(edits)) || count > EDIT_MAX_REPLACEMENTS) {
		goto done;
	}

	name = expand_path(path);
	if (name && !tool_path_allowed(name)) {
		error = "Sandbox blocks paths outside the working tree.";
		goto done;
	}
	error = "Cannot open an existing regular file for editing.";
	if (!name || (fd = tool_open_read(name)) < 0) {
		goto done;
	}

	struct stat status;
	if (fstat(fd, &status) || !S_ISREG(status.st_mode)) {
		goto done;
	}
	error = "Edit supports files up to 4 MiB.";
	if (status.st_size < 0 || status.st_size > EDIT_MAX_BYTES) {
		goto done;
	}
	original_size = (size_t)status.st_size;
	updated_size = original_size;

	error = "Out of memory.";
	original = malloc(original_size + 1);
	changes = calloc(count, sizeof(*changes));
	if (!original || !changes) {
		goto done;
	}

	error = "Could not read the entire file.";
	if (read_exactly(fd, original, original_size)) {
		goto done;
	}
	original[original_size] = '\0';
	error = "Binary files are not supported.";
	if (memchr(original, '\0', original_size)) {
		goto done;
	}

	error = load_replacements(edits, original, changes, count, &updated_size);
	if (error) {
		goto done;
	}

	error = "Out of memory.";
	updated = malloc(updated_size + 1);
	if (!updated) {
		goto done;
	}
	apply_replacements(updated, original, original_size, changes, count);
	updated[updated_size] = '\0';

	if (tool_show_banners()) {
		tool_report(stderr, "[edit: %s, %zu replacements]\n", name, count);
	}

	/* Close the read descriptor before replacing the file. The replacement is
       written and synced under a same-directory temporary name, then renamed. */
	if (close(fd)) {
		fd = -1;
		error = "Could not close file before atomic edit.";
		goto done;
	}
	fd = -1;
	error = "Atomic edit failed before replacement.";
	replacement_status = tool_atomic_replace(name, updated, updated_size, &status, 0);
	if (replacement_status < 0) {
		if (errno == ESTALE) {
			error = "File changed during editing; read it again before retrying.";
		}
		goto done;
	}

	error = NULL;
	if (tool_has_diff() &&
		edit_diff(original, original_size, updated, updated_size, changes, count, display_edit_diff,
			NULL)) {
		tool_display("[Diff preview truncated; file changes were applied.]\n", stderr);
	}

done:
	if (fd >= 0) {
		close(fd);
	}
	free(name);
	free(original);
	free(updated);
	free(changes);

	if (error) {
		return tool_error(error);
	}
	json_object *result = json_object_new_object();
	json_object_object_add(result, "replacements", json_object_new_int64((int64_t)count));
	if (replacement_status > 0) {
		json_object_object_add(result, "warning",
			json_object_new_string(
				"File replaced, but directory durability could not be confirmed."));
	}

	return result;
}

const struct phi_tool phi_tool_edit = {.name = "edit",
	.schema =
		"{\"type\":\"function\",\"name\":\"edit\","
		"\"description\":\"Exact text replacements in an existing file (up to 4 MiB). Each oldText must be nonempty, unique, and non-overlapping in the ORIGINAL file. Supports ~/ paths.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\"},"
		"\"edits\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
		"\"properties\":{\"oldText\":{\"type\":\"string\"},"
		"\"newText\":{\"type\":\"string\"}},"
		"\"required\":[\"oldText\",\"newText\"],"
		"\"additionalProperties\":false}}},"
		"\"required\":[\"path\",\"edits\"],"
		"\"additionalProperties\":false},\"strict\":true}",
	.detail_key = "path",
	.run = run};
