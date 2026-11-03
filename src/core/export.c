#define _POSIX_C_SOURCE 200809L
#include "export.h"
#include "history.h"
#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int escaped(FILE *file, const char *text) {

	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		const char *entity = *p == '&' ? "&amp;"
			: *p == '<'                ? "&lt;"
			: *p == '>'                ? "&gt;"
			: *p == '"'                ? "&quot;"
									   : NULL;
		if (entity ? fputs(entity, file) == EOF : fputc(*p, file) == EOF) {
			return 0;
		}
	}

	return 1;
}

static int section(FILE *file, const char *label, const char *text) {

	return fputs("<section><h2>", file) != EOF && escaped(file, label) &&
		fputs("</h2><pre>", file) != EOF && escaped(file, text) &&
		fputs("</pre></section>\n", file) != EOF;
}

static int content(FILE *file, const char *label, json_object *item) {

	json_object *value = json_object_object_get(item, "content");
	if (json_object_is_type(value, json_type_string)) {
		return section(file, label, json_object_get_string(value));
	}
	if (!json_object_is_type(value, json_type_array)) {
		return 1;
	}
	for (size_t i = 0; i < json_object_array_length(value); i++) {
		json_object *part = json_object_array_get_idx(value, i);
		const char *type = field(part, "type");
		if (!strcmp(type, "output_text") || !strcmp(type, "input_text")) {
			if (!section(file, label, field(part, "text"))) {
				return 0;
			}
		} else if (!strcmp(type, "input_image") || !strcmp(type, "image_url")) {
			/* Never embed base64 image data or external URLs in an export. */
			if (!section(file, label, "[Image omitted from export]")) {
				return 0;
			}
		}
	}

	return 1;
}

static int render(FILE *file, json_object *history) {

	if (fputs("<!doctype html>\n<html lang=\"en\"><meta charset=\"utf-8\">"
			  "<meta name=\"viewport\" content=\"width=device-width\">"
			  "<title>Phi session</title><style>body{max-width:52rem;margin:2rem auto;"
			  "padding:0 1rem;background:#171b1a;color:#e4e9e5;font:16px/1.5 system-ui}"
			  "section{background:#252b29;padding:1rem;margin:1rem 0;border-radius:.5rem}"
			  "h2{font-size:1rem;margin:0 0 .5rem;color:#a7dbbd}"
			  "pre{white-space:pre-wrap;overflow-wrap:anywhere;font:inherit;margin:0}"
			  "</style><h1>Phi session</h1>\n",
			file) == EOF) {
		return 0;
	}
	for (size_t i = 0; i < json_object_array_length(history); i++) {
		json_object *item = json_object_array_get_idx(history, i);
		const char *role = field(item, "role"), *type = field(item, "type");
		/* Hidden extension messages are not user conversation. */
		if (history_is_hidden(item)) {
			continue;
		}
		if (!strcmp(role, "user") || !strcmp(role, "assistant")) {
			if (!content(file, !strcmp(role, "user") ? "User" : "Assistant", item)) {
				return 0;
			}
		} else if (!strcmp(type, "function_call")) {
			if (fputs("<section><h2>Tool: ", file) == EOF || !escaped(file, field(item, "name")) ||
				fputs("</h2><pre>", file) == EOF || !escaped(file, field(item, "arguments")) ||
				fputs("</pre></section>\n", file) == EOF) {
				return 0;
			}
		} else if (!strcmp(type, "function_call_output")) {
			if (!section(file, "Tool result", field(item, "output"))) {
				return 0;
			}
		}
	}

	return fputs("</html>\n", file) != EOF;
}

int phi_export_html(json_object *history, const char *path, char *error, size_t error_size) {

	if (!history || !json_object_is_type(history, json_type_array) ||
		!json_object_array_length(history) || !path || !*path) {
		snprintf(error, error_size, "Nothing to export or no path supplied.");
		return 0;
	}
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		snprintf(error, error_size, "Cannot create export: %s", strerror(errno));
		return 0;
	}
	FILE *file = fdopen(fd, "w");
	if (!file) {
		int saved = errno;
		close(fd);
		unlink(path);
		snprintf(error, error_size, "Cannot open export: %s", strerror(saved));
		return 0;
	}
	int ok = render(file, history);
	if (ok) {
		ok = fflush(file) == 0 && fsync(fd) == 0;
	}
	int saved = errno;
	if (fclose(file) != 0) {
		ok = 0;
		saved = errno;
	}
	if (!ok) {
		unlink(path);
		snprintf(error, error_size, "Cannot finish export: %s", strerror(saved));
	}

	return ok;
}
