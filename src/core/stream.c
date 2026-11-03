#define _POSIX_C_SOURCE 200809L
#include "stream.h"
#include "images.h"
#include "json.h"
#include "../tools.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>

static char last_usage[160];

const char *stream_take_usage(void) {

	static char taken[sizeof(last_usage)];
	memcpy(taken, last_usage, sizeof(taken));
	*last_usage = 0;

	return taken;
}

/* Bounded, display-only argument prefixes. Completed output remains authoritative.
   Retain enough for the UI's expanded live-write view, not just its short tail. */
#define STREAM_PENDING_LIMIT (128 * 1024)

static void pending_item(struct stream *s, json_object *item) {

	if (!s->sink.tool_pending || strcmp(field(item, "type"), "function_call")) {
		return;
	}
	const char *id = field(item, "id");
	if (!*id) {
		s->sink.tool_pending(item);
		return;
	}
	if (!s->pending) {
		s->pending = json_object_new_object();
	}
	if (!s->pending || json_object_object_length(s->pending) >= 128) {
		return;
	}
	json_object *copy = json_object_new_object();
	if (!copy) {
		return;
	}
	const char *keys[] = {"id", "call_id", "name", "type", "arguments"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
		const char *text = field(item, keys[i]);
		size_t limit = !strcmp(keys[i], "arguments") ? STREAM_PENDING_LIMIT : 8192;
		size_t length = strlen(text);
		if (length > limit) {
			length = limit;
		}
		json_object_object_add(copy, keys[i], json_object_new_string_len(text, (int)length));
	}
	json_object_object_add(copy, "arguments_truncated",
		json_object_new_boolean(strlen(field(item, "arguments")) > STREAM_PENDING_LIMIT));
	json_object_object_add(s->pending, id, copy);
	s->sink.tool_pending(copy);
}

static void pending_arguments(struct stream *s, json_object *event, int complete) {

	if (!s->sink.tool_pending || !s->pending) {
		return;
	}
	json_object *item = json_object_object_get(s->pending, field(event, "item_id"));
	if (!item) {
		return;
	}
	const char *old = complete ? "" : field(item, "arguments");
	const char *delta = field(event, complete ? "arguments" : "delta");
	char text[STREAM_PENDING_LIMIT + 1];
	size_t length = strlen(old), extra = strlen(delta);
	if (length > STREAM_PENDING_LIMIT) {
		length = STREAM_PENDING_LIMIT;
	}
	int truncated =
		(!complete &&
			json_object_get_boolean(json_object_object_get(item, "arguments_truncated"))) ||
		extra > STREAM_PENDING_LIMIT - length;
	if (extra > STREAM_PENDING_LIMIT - length) {
		extra = STREAM_PENDING_LIMIT - length;
	}
	memcpy(text, old, length);
	memcpy(text + length, delta, extra);
	text[length + extra] = 0;
	json_object_object_add(
		item, "arguments", json_object_new_string_len(text, (int)(length + extra)));
	json_object_object_add(item, "arguments_truncated", json_object_new_boolean(truncated));
	s->sink.tool_pending(item);
}

void stream_event(struct stream *s, const char *line) {

	if (strncmp(line, "data:", 5)) {
		/* Error responses are plain JSON, not SSE. Keep a bounded copy so the
           failure message can say what the server objected to. */
		if (*line && strncmp(line, "event:", 6) && *line != ':') {
			size_t n = strlen(line), room = sizeof(s->body) - 1 - s->body_used;
			if (n > room) {
				n = room;
			}
			memcpy(s->body + s->body_used, line, n);
			s->body_used += n;
			s->body[s->body_used] = 0;
		}
		return;
	}
	line += 5;
	if (*line == ' ') {
		line++;
	}
	json_object *event = json_tokener_parse(line);
	if (!event) {
		return;
	}
	if (s->observe_event) {
		s->observe_event(event, s->event_context);
	}
	const char *type = field(event, "type");
	if (!strcmp(type, "response.output_text.delta")) {
		s->delivered = 1;
		if (s->sink.text) {
			s->sink.text(field(event, "delta"));
		}
		if (s->sink.observe_text) {
			s->sink.observe_text(field(event, "delta"), s->sink.observer);
		}
	} else if (!strcmp(type, "response.reasoning_summary_text.delta")) {
		if (s->sink.thinking) {
			s->delivered = s->thinking_seen = 1;
			s->sink.thinking(field(event, "delta"), 0);
		}
	} else if (!strcmp(type, "response.reasoning_summary_text.done")) {
		if (s->sink.thinking) {
			if (!s->thinking_seen && *field(event, "text")) {
				s->delivered = s->thinking_seen = 1;
				s->sink.thinking(field(event, "text"), 0);
			}
			if (s->thinking_seen) {
				s->sink.thinking("", 1);
			}
		}
	} else if (!strcmp(type, "response.output_item.added")) {
		s->delivered = 1;
		json_object *item = json_object_object_get(event, "item");
		if (json_object_is_type(item, json_type_object)) {
			pending_item(s, item);
		}
	} else if (!strcmp(type, "response.function_call_arguments.delta")) {
		pending_arguments(s, event, 0);
	} else if (!strcmp(type, "response.function_call_arguments.done")) {
		pending_arguments(s, event, 1);
	} else if (!strcmp(type, "response.image_generation_call.generating")) {
		tool_report(stderr, "[image: generating…]\n");
	} else if (!strcmp(type, "response.output_item.done")) {
		json_object *item = NULL;
		if (json_object_object_get_ex(event, "item", &item) &&
			json_object_is_type(item, json_type_object)) {
			if (!s->output) {
				s->output = json_object_new_array();
			}
			json_object_array_add(s->output, retain_output_item(s, item));
			if (!strcmp(field(item, "type"), "reasoning") && s->sink.thinking) {
				json_object *summary = json_object_object_get(item, "summary");
				if (!s->thinking_seen && json_object_is_type(summary, json_type_array)) {
					for (size_t i = 0; i < json_object_array_length(summary); i++) {
						json_object *part = json_object_array_get_idx(summary, i);
						if (!strcmp(field(part, "type"), "summary_text") && *field(part, "text")) {
							s->delivered = 1;
							s->sink.thinking(field(part, "text"), 0);
							s->sink.thinking("", 1);
						}
					}
				}
				s->sink.thinking("", 1);
				s->thinking_seen = 0;
			}
			pending_item(s, item);
		} else {
			s->failed = 1;
		}
	} else if (!strcmp(type, "response.completed") || !strcmp(type, "response.done")) {
		s->completed = 1;
		if (s->thinking_seen && s->sink.thinking) {
			s->sink.thinking("", 1);
			s->thinking_seen = 0;
		}
		json_object *response = NULL, *output = NULL;
		json_object_object_get_ex(event, "response", &response);
		if (response && json_object_object_get_ex(response, "output", &output)) {
			if (!json_object_is_type(output, json_type_array)) {
				s->failed = 1;
			}
			/* Codex may send completed items individually, then finish with
               output: []. That empty summary must not erase tool calls (or
               assistant messages) already received in output_item.done. */
			else if (json_object_array_length(output) || !s->output) {
				if (s->output) {
					json_object_put(s->output);
				}
				s->output = json_object_new_array();
				for (size_t i = 0; i < json_object_array_length(output); i++) {
					json_object *item = json_object_array_get_idx(output, i);
					if (!json_object_is_type(item, json_type_object)) {
						s->failed = 1;
						break;
					}
					json_object_array_add(s->output, retain_output_item(s, item));
				}
			}
		}
		if (!s->failed && response && s->sink.usage) {
			json_object *usage = json_object_object_get(response, "usage");
			json_object *input = usage ? json_object_object_get(usage, "input_tokens") : NULL;
			json_object *output_tokens =
				usage ? json_object_object_get(usage, "output_tokens") : NULL;
			if (json_object_is_type(input, json_type_int) &&
				json_object_is_type(output_tokens, json_type_int)) {
				int64_t in = json_object_get_int64(input),
						out = json_object_get_int64(output_tokens);
				/* input_tokens already includes cached tokens. Do not subtract them. */
				if (in >= 0 && out >= 0 && in <= INT64_MAX - out) {
					s->sink.usage(in, out);
				}
				json_object *details = json_object_object_get(usage, "input_tokens_details");
				json_object *cached =
					details ? json_object_object_get(details, "cached_tokens") : NULL;
				json_object *odetails = json_object_object_get(usage, "output_tokens_details");
				json_object *reasoning =
					odetails ? json_object_object_get(odetails, "reasoning_tokens") : NULL;
				snprintf(last_usage, sizeof(last_usage),
					"input=%lld cached=%lld output=%lld reasoning=%lld", (long long)in,
					(long long)(cached ? json_object_get_int64(cached) : -1), (long long)out,
					(long long)(reasoning ? json_object_get_int64(reasoning) : -1));
			}
		}
	} else if (!strcmp(type, "error") || !strcmp(type, "response.failed") ||
		!strcmp(type, "response.incomplete")) {
		/* A reason is kept for the failure report; anything else is shown as is. */
		if (!stream_error_text(event, s->error, sizeof(s->error))) {
			tool_report(stderr, "\n%s\n", json_object_to_json_string(event));
		}
		s->failed = 1;
	}
	json_object_put(event);
}

/* The server's own words for a failure: error.message (at the top or
   inside "response"; else its code), or why a response stopped early.
   0 when there are none. */
int stream_error_text(json_object *root, char *out, size_t size) {

	json_object *response = json_object_object_get(root, "response");
	json_object *error = json_object_object_get(root, "error");
	if (!error && response) {
		error = json_object_object_get(response, "error");
	}
	const char *message = NULL, *code = NULL;
	if (json_object_is_type(error, json_type_string)) {
		message = json_object_get_string(error);
	} else if (json_object_is_type(error, json_type_object)) {
		json_object *value = json_object_object_get(error, "message");
		if (json_object_is_type(value, json_type_string)) {
			message = json_object_get_string(value);
		}
		value = json_object_object_get(error, "code");
		if (!json_object_is_type(value, json_type_string)) {
			value = json_object_object_get(error, "type");
		}
		if (json_object_is_type(value, json_type_string)) {
			code = json_object_get_string(value);
		}
	}
	json_object *detail = json_object_object_get(root, "detail");
	if (!message && json_object_is_type(detail, json_type_string)) {
		message = json_object_get_string(detail);
	}
	if (!message && response) {
		json_object *incomplete = json_object_object_get(response, "incomplete_details"),
					*why = NULL;
		if (incomplete) {
			why = json_object_object_get(incomplete, "reason");
		}
		if (json_object_is_type(why, json_type_string)) {
			snprintf(out, size, "The response stopped early: %s", json_object_get_string(why));
			return 1;
		}
	}
	if ((!message || !*message) && (!code || !*code)) {
		return 0;
	}
	snprintf(out, size, "%s", message && *message ? message : code);

	return 1;
}

size_t stream_receive(char *data, size_t size, size_t count, void *userdata) {

	struct stream *s = userdata;
	if (s->sink.cancel && s->sink.cancel()) {
		return 0;
	}
	if (count && size > SIZE_MAX / count) {
		return 0;
	}
	size_t bytes = size * count;
	for (size_t i = 0; i < bytes; i++) {
		/* Bound one SSE line, not the entire conversation. */
		if (s->length >= 32 * 1024 * 1024) {
			return 0;
		}
		if (s->length + 2 > s->capacity) {
			size_t capacity = s->capacity ? s->capacity * 2 : 4096;
			char *next = realloc(s->line, capacity);
			if (!next) {
				return 0;
			}
			s->line = next;
			s->capacity = capacity;
		}
		if (data[i] == '\n') {
			s->line[s->length] = '\0';
			stream_event(s, s->line);
			s->length = 0;
		} else {
			s->line[s->length++] = data[i];
		}
	}

	return bytes;
}

/* Process a final line that arrived without a terminating newline. */
void stream_finish(struct stream *s) {

	if (!s->length || !s->line) {
		return;
	}
	s->line[s->length] = '\0';
	s->length = 0;
	stream_event(s, s->line);
}

/* Remember Retry-After so a 429/503 retry waits as long as the server asks. */
size_t stream_header(char *data, size_t size, size_t count, void *userdata) {

	struct stream *s = userdata;
	size_t bytes = size * count;
	if (bytes > 12 && !strncasecmp(data, "retry-after:", 12)) {
		char value[32] = "";
		size_t n = bytes - 12 < sizeof(value) - 1 ? bytes - 12 : sizeof(value) - 1;
		memcpy(value, data + 12, n);
		long seconds = strtol(value, NULL, 10);
		s->retry_after = seconds > 0 && seconds <= 3600 ? seconds : 0;
	}

	return bytes;
}

void stream_clear(struct stream *s) {

	if (s->output) {
		json_object_put(s->output);
	}
	if (s->images) {
		json_object_put(s->images);
	}
	if (s->pending) {
		json_object_put(s->pending);
	}
	free(s->line);
	*s = (struct stream){0};
}
