#define _POSIX_C_SOURCE 200809L
#include "images.h"
#include "stream.h"
#include "json.h"
#include "../tools.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* Native Responses image_generation results; no Codex process or API key. */
static int image_base64_value(unsigned char c) {

	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+') {
		return 62;
	}
	if (c == '/') {
		return 63;
	}

	return -1;
}

int save_generated_png(
	const char *encoded, size_t size, char *path, size_t path_size, int (*cancel)(void)) {

	if (!size || size % 4 || size > ((16 * 1024 * 1024 + 2) / 3) * 4) {
		return 0;
	}
	char directory[] = "/tmp/phi-generated-XXXXXX";
	if (!mkdtemp(directory)) {
		return 0;
	}
	snprintf(path, path_size, "%s/image.png", directory);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	FILE *out = fd >= 0 ? fdopen(fd, "wb") : NULL;
	if (!out) {
		if (fd >= 0) {
			close(fd);
		}
		unlink(path);
		rmdir(directory);
		return 0;
	}
	int ok = 1;
	unsigned char signature[8];
	size_t written = 0;
	for (size_t i = 0; i < size && ok; i += 4) {
		if (i % 16384 == 0 && cancel && cancel()) {
			ok = 0;
			break;
		}
		int a = image_base64_value(encoded[i]), b = image_base64_value(encoded[i + 1]);
		int c = encoded[i + 2] == '=' ? 0 : image_base64_value(encoded[i + 2]);
		int d = encoded[i + 3] == '=' ? 0 : image_base64_value(encoded[i + 3]);
		int padding = (encoded[i + 2] == '=') + (encoded[i + 3] == '=');
		if (a < 0 || b < 0 || c < 0 || d < 0 || (padding && i + 4 != size) ||
			(encoded[i + 2] == '=' && encoded[i + 3] != '=') || (padding == 2 && (b & 15)) ||
			(padding == 1 && (c & 3))) {
			ok = 0;
			break;
		}
		unsigned char bytes[3] = {(unsigned char)((a << 2) | (b >> 4)),
			(unsigned char)((b << 4) | (c >> 2)), (unsigned char)((c << 6) | d)};
		size_t count = (size_t)(3 - padding);
		if (written + count > 16 * 1024 * 1024) {
			ok = 0;
			break;
		}
		for (size_t j = 0; j < count && written + j < sizeof(signature); j++) {
			signature[written + j] = bytes[j];
		}
		if (fwrite(bytes, 1, count, out) != count) {
			ok = 0;
		}
		written += count;
	}
	if (fclose(out)) {
		ok = 0;
	}
	if (written < sizeof(signature) || memcmp(signature, "\211PNG\r\n\032\n", 8)) {
		ok = 0;
	}
	if (!ok) {
		unlink(path);
		rmdir(directory);
		path[0] = 0;
	}

	return ok;
}

/* Replace bulky image output with a saved-path note. Cache by item ID so
   output_item.done and response.completed never save the same result twice. */
static json_object *generated_image_item(struct stream *s, json_object *item) {

	const char *id = field(item, "id");
	if (!s->images) {
		s->images = json_object_new_object();
	}
	json_object *note = NULL;
	if (*id && json_object_object_get_ex(s->images, id, &note)) {
		return json_object_get(note);
	}
	char path[160] = {0}, text[320];
	json_object *encoded = NULL;
	int ok = *id && json_object_object_length(s->images) < 8 &&
		json_object_object_get_ex(item, "result", &encoded) &&
		json_object_is_type(encoded, json_type_string) &&
		save_generated_png(json_object_get_string(encoded),
			(size_t)json_object_get_string_len(encoded), path, sizeof(path), s->sink.cancel);
	if (ok) {
		snprintf(text, sizeof(text),
			"Generated image saved locally: %s. Use read on this path to inspect it.", path);
		tool_report(stderr, "[image saved: %s]\n", path);
	} else {
		snprintf(text, sizeof(text),
			"Image generation returned no savable PNG (missing/invalid data, over 16 MiB, too many images, or a file-write failure).");
		tool_report(stderr, "[image: %s]\n", text);
	}
	note = json_object_new_object();
	json_object *content = json_object_new_array(), *part = json_object_new_object();
	json_object_object_add(part, "type", json_object_new_string("output_text"));
	json_object_object_add(part, "text", json_object_new_string(text));
	json_object_object_add(part, "annotations", json_object_new_array());
	json_object_array_add(content, part);
	json_object_object_add(note, "type", json_object_new_string("message"));
	json_object_object_add(note, "role", json_object_new_string("assistant"));
	json_object_object_add(note, "content", content);
	if (*id) {
		json_object_object_add(s->images, id, json_object_get(note));
	}

	return note;
}

json_object *retain_output_item(struct stream *s, json_object *item) {

	return !strcmp(field(item, "type"), "image_generation_call") ? generated_image_item(s, item)
																 : json_object_get(item);
}
