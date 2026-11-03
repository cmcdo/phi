#ifndef PHI_BRIDGE_TAIL_H
#define PHI_BRIDGE_TAIL_H
#include "json.h"
#include <stdio.h>
#include <string.h>

/* A summary is one logical line, even for multiline errors or control bytes.
   The JSON service retains the original result; this is plain CLI formatting. */
static inline void bridge_inline(FILE *out, const char *text, size_t limit) {

	size_t line = strcspn(text, "\r\n"), n = line < limit ? line : limit;
	while (n && ((unsigned char)text[n] & 0xc0) == 0x80) {
		n--;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c < 32 || c == 127) {
			fprintf(out, "\\x%02x", c);
		} else {
			fputc(c, out);
		}
	}
	if (n < line) {
		fputs("…", out);
	}
}

static inline void bridge_print_tool(FILE *out, json_object *entry) {

	fputs("tool ", out);
	bridge_inline(out, field(entry, "name"), 96);
	const char *summary = field(entry, "summary");
	if (*summary) {
		fputs(": ", out);
		bridge_inline(out, summary, 1024);
	}
	fputs(" [", out);
	json_object *result = NULL;
	int present = json_object_object_get_ex(entry, "result", &result);
	if (!present) {
		fputs("pending", out);
	} else if (!json_object_is_type(result, json_type_object)) {
		fputs("completed", out);
	} else {
		json_object *exit = json_object_object_get(result, "exit_code");
		int has_exit = json_object_is_type(exit, json_type_int), state = 1;
		const char *error = field(result, "error");
		if (*error) {
			fputs("error: ", out);
			bridge_inline(out, error, 160);
		} else if (json_object_get_boolean(json_object_object_get(result, "cancelled"))) {
			fputs("cancelled", out);
		} else if (json_object_get_boolean(json_object_object_get(result, "timed_out"))) {
			fputs("timed out", out);
		} else if (json_object_get_boolean(json_object_object_get(result, "killed"))) {
			fputs("killed", out);
		} else if (json_object_get_boolean(json_object_object_get(result, "isError")) ||
			json_object_get_boolean(json_object_object_get(result, "error"))) {
			fputs("error", out);
		} else if (json_object_get_boolean(json_object_object_get(result, "running")) ||
			!strcmp(field(result, "status"), "running")) {
			fputs("running", out);
		} else if (!has_exit) {
			fputs("ok", out);
		} else {
			state = 0;
		}
		if (has_exit) {
			fprintf(out, "%sexit %lld", state ? ", " : "", (long long)json_object_get_int64(exit));
		}
	}
	fputs("]\n", out);
}
#endif
