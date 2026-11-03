#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "read-image.inc"

#define READ_MAX_BYTES (50 * 1024)
#define READ_MAX_LINES 2000

/* Bounded-memory, line-based text reads. Long lines are never silently split. */
static json_object *run(json_object *args) {

	const char *error = "Expected a path string.";
	const char *path = NULL;
	int64_t offset = 1, limit = READ_MAX_LINES;
	json_object *value = NULL;

	if (!args || !json_object_object_get_ex(args, "path", &value) ||
		!(path = tool_plain_string(value, NULL))) {
		return tool_error(error);
	}
	if (!tool_integer_arg(args, "offset", 1, 1, INT64_MAX, &offset) ||
		!tool_integer_arg(args, "limit", READ_MAX_LINES, 1, INT64_MAX, &limit)) {
		return tool_error("offset and limit must be positive integers "
						  "(offset is a 1-based line number).");
	}
	if (limit > READ_MAX_LINES) {
		limit = READ_MAX_LINES;
	}

	char *name = expand_path(path);
	if (!name) {
		return tool_error("Cannot open a regular file.");
	}
	if (!tool_path_allowed(name)) {
		free(name);
		return tool_error("Sandbox blocks paths outside the working tree.");
	}
	if (tool_show_banners()) {
		tool_report(stderr, "[read: %s, line: %lld]\n", name, (long long)offset);
	}

	int fd = tool_open_read(name);
	free(name);
	if (fd < 0) {
		return tool_error("Cannot open a regular file.");
	}

	struct stat status;
	FILE *file = NULL;
	if (fstat(fd, &status) || !S_ISREG(status.st_mode) || !(file = fdopen(fd, "r"))) {
		close(fd);
		return tool_error("Cannot open a regular file.");
	}

	unsigned char signature[12];
	size_t signature_size = fread(signature, 1, sizeof(signature), file);
	const char *mime = image_mime(signature, signature_size);
	if (ferror(file) || fseek(file, 0, SEEK_SET)) {
		fclose(file);
		return tool_error("Cannot rewind file after reading its signature.");
	}
	if (mime) {
		json_object *image = read_image(file, status.st_size, mime);
		fclose(file);
		return image;
	}

	char output[READ_MAX_BYTES];
	char line[READ_MAX_BYTES + 1];
	size_t used = 0;
	int64_t current = 1, lines = 0;
	int truncated = 0, at_eof = 0, c;

	while (current < offset && (c = fgetc(file)) != EOF) {
		if (c == '\n') {
			current++;
		}
	}
	if (current < offset) {
		error = ferror(file) ? "File read failed." : "Offset is beyond the end of the file.";
		goto done;
	}

	while (lines < limit) {
		size_t line_size = 0;
		while ((c = fgetc(file)) != EOF) {
			if (c == 0) {
				error = "Binary files are not supported.";
				goto done;
			}
			if (line_size == sizeof(line)) {
				break;
			}
			line[line_size++] = (char)c;
			if (c == '\n') {
				break;
			}
		}
		at_eof = c == EOF;
		if (!line_size) {
			break;
		}

		char prefix[32];
		size_t prefix_size =
			(size_t)snprintf(prefix, sizeof(prefix), "%lld | ", (long long)(offset + lines));
		if (prefix_size + line_size > sizeof(output) - used) {
			if (!lines) {
				error = "The first requested line, including its display prefix, "
						"exceeds 50 KiB; use bash to inspect a portion.";
				goto done;
			}
			truncated = 1;
			break;
		}
		memcpy(output + used, prefix, prefix_size);
		used += prefix_size;
		memcpy(output + used, line, line_size);
		used += line_size;
		lines++;
		if (at_eof) {
			break;
		}
	}

	if (!truncated && !at_eof) {
		truncated = fgetc(file) != EOF;
	}
	if (ferror(file)) {
		error = "File read failed.";
		goto done;
	}
	error = NULL;

done:
	fclose(file);
	if (error) {
		return tool_error(error);
	}

	json_object *result = json_object_new_object();
	json_object_object_add(result, "text", json_object_new_string_len(output, (int)used));
	json_object_object_add(result, "lines", json_object_new_int64(lines));
	json_object_object_add(result, "start_line", json_object_new_int64(offset));
	json_object_object_add(result, "line_numbers", json_object_new_boolean(1));
	json_object_object_add(result, "note",
		json_object_new_string(
			"N | prefixes are display-only. Exclude them from edit oldText/newText; "
			"preserve the file's indentation."));
	json_object_object_add(result, "truncated", json_object_new_boolean(truncated));
	if (truncated) {
		json_object_object_add(result, "next_offset", json_object_new_int64(offset + lines));
	}
	tool_read_preview(field(result, "text"));

	return result;
}

const struct phi_tool phi_tool_read = {.name = "read",
	.schema =
		"{\"type\":\"function\",\"name\":\"read\","
		"\"description\":\"Read text or view PNG/JPEG/WebP/GIF images (up to 16 MiB). Images are attached for visual inspection; offset/limit are ignored for images. Text has N | display-only line prefixes; exclude these prefixes from edits. At most 2000 lines or 50 KiB including prefixes. offset is a 1-based LINE number, not bytes. Null offset/limit use defaults 1/2000. Continue with next_offset when truncated. Supports ~/.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\"},"
		"\"offset\":{\"type\":[\"integer\",\"null\"],\"minimum\":1},"
		"\"limit\":{\"type\":[\"integer\",\"null\"],\"minimum\":1}},"
		"\"required\":[\"path\",\"offset\",\"limit\"],"
		"\"additionalProperties\":false},\"strict\":true}",
	.detail_key = "path",
	.run = run};
