#define _POSIX_C_SOURCE 200809L
#include "internal.h"

static json_object *run(json_object *args) {

	json_object *path_value = NULL, *content_value = NULL;
	if (!args || !json_object_object_get_ex(args, "path", &path_value) ||
		!json_object_object_get_ex(args, "content", &content_value)) {
		return tool_error("Expected path and content strings.");
	}

	const char *path = tool_plain_string(path_value, NULL);
	if (!path || !json_object_is_type(content_value, json_type_string)) {
		return tool_error("Expected path and content strings.");
	}

	char *name = expand_path(path);
	if (!name || !*name) {
		free(name);
		return tool_error("Invalid path or HOME unavailable");
	}
	if (!tool_path_allowed(name)) {
		free(name);
		return tool_error("Sandbox blocks paths outside the working tree.");
	}

	if (tool_show_banners()) {
		tool_report(stderr, "[write: %s]\n", name);
	}
	const char *data = json_object_get_string(content_value);
	size_t size = (size_t)json_object_get_string_len(content_value);
	int status = tool_atomic_replace(name, data, size, NULL, 1);
	if (status < 0) {
		free(name);
		char message[256];
		snprintf(message, sizeof(message), "Atomic write failed before replacement: %s",
			strerror(errno));
		return tool_error(message);
	}
	free(name);

	tool_write_preview(data);
	json_object *result = json_object_new_object();
	json_object_object_add(result, "bytes_written", json_object_new_int64((int64_t)size));
	if (status > 0) {
		json_object_object_add(result, "warning",
			json_object_new_string(
				"File replaced, but directory durability could not be confirmed."));
	}

	return result;
}

const struct phi_tool phi_tool_write = {.name = "write",
	.schema =
		"{\"type\":\"function\",\"name\":\"write\","
		"\"description\":\"Create or atomically overwrite an entire regular file. Creates parent directories and expands ~/ to the home directory.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\"},"
		"\"content\":{\"type\":\"string\"}},"
		"\"required\":[\"path\",\"content\"],"
		"\"additionalProperties\":false},\"strict\":true}",
	.detail_key = "path",
	.run = run};
