#include "compaction.h"
#include "codex.h"
#include "json.h"
#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TOOL_RESULT_MAX 2000

static const char checkpoint_instructions[] =
	"Create a concise structured context checkpoint using exactly: ## Goal, "
	"## Constraints & Preferences, ## Progress (Done/In Progress/Blocked), "
	"## Key Decisions, ## Next Steps, ## Critical Context. Preserve exact paths, "
	"names and errors. Do not continue the conversation.";

static size_t item_chars(json_object *item) {

	const char *type = field(item, "type");
	const char *role = field(item, "role");
	if (!strcmp(type, "function_call_output")) {
		return strlen(field(item, "output"));
	}
	if (!strcmp(type, "function_call")) {
		return strlen(field(item, "name")) + strlen(field(item, "arguments"));
	}
	if (*role) {
		json_object *content = json_object_object_get(item, "content");
		if (json_object_is_type(content, json_type_string)) {
			return strlen(json_object_get_string(content));
		}
		if (json_object_is_type(content, json_type_array)) {
			size_t chars = 0;
			for (size_t i = 0; i < json_object_array_length(content); i++) {
				json_object *part = json_object_array_get_idx(content, i);
				const char *kind = field(part, "type");
				if (!strcmp(kind, "input_text") || !strcmp(kind, "output_text") ||
					!strcmp(kind, "text")) {
					chars += strlen(field(part, "text"));
				} else {
					chars += 16384; /* rough attachment allowance, never base64 text tokens */
				}
			}
			return chars;
		}
	}

	return strlen(json_object_to_json_string_ext(item, JSON_C_TO_STRING_PLAIN));
}

int64_t compaction_estimate(json_object *history) {

	size_t chars = 0;
	size_t count = json_object_array_length(history);
	for (size_t i = 0; i < count; i++) {
		chars += item_chars(json_object_array_get_idx(history, i));
	}

	return (int64_t)((chars + 3) / 4);
}

/* Find the old prefix to summarize, leaving the recent context unchanged. */
static size_t cut_point(json_object *history) {

	size_t count = json_object_array_length(history);
	size_t cut = 0;
	int64_t tokens = 0;
	for (size_t i = count; i > 0; i--) {
		json_object *item = json_object_array_get_idx(history, i - 1);
		tokens += (int64_t)(item_chars(item) + 3) / 4;
		const char *role = field(item, "role");
		const char *type = field(item, "type");
		if (tokens >= PHI_COMPACTION_KEEP &&
			((!strcmp(role, "user") && !history_is_hidden(item)) || !strcmp(role, "assistant") ||
				!strcmp(type, "function_call"))) {
			cut = i - 1;
			break;
		}
	}
	/* Prefer a user boundary only within twice the keep budget. Autonomous
       turns can span most of the context window; retaining them wholesale
       would leave compaction unable to shrink history. `tokens` already
       includes the item at cut and everything after it. */
	for (size_t i = cut + 1; i > 0; i--) {
		json_object *item = json_object_array_get_idx(history, i - 1);
		if (i - 1 < cut) {
			tokens += (int64_t)(item_chars(item) + 3) / 4;
		}
		if (tokens > 2 * PHI_COMPACTION_KEEP) {
			break;
		}
		if (!strcmp(field(item, "role"), "user") && !history_is_hidden(item)) {
			cut = i - 1;
			break;
		}
	}
	/* A new checkpoint resets replay's known tool calls. If the retained
       tail contains a result, its matching call must also survive. This can
       happen even when a user boundary precedes a late tool result. Move
       the cut back rather than discarding results or inventing calls. */
	for (;;) {
		size_t earliest = cut;
		for (size_t i = cut; i < count; i++) {
			json_object *item = json_object_array_get_idx(history, i);
			if (strcmp(field(item, "type"), "function_call_output")) {
				continue;
			}
			const char *id = field(item, "call_id");
			if (!*id) {
				continue;
			}
			int found = 0;
			for (size_t j = cut; j < i; j++) {
				json_object *call = json_object_array_get_idx(history, j);
				if (!strcmp(field(call, "type"), "function_call") &&
					!strcmp(field(call, "call_id"), id)) {
					found = 1;
					break;
				}
			}
			if (found) {
				continue;
			}
			for (size_t j = 0; j < cut; j++) {
				json_object *call = json_object_array_get_idx(history, j);
				if (!strcmp(field(call, "type"), "function_call") &&
					!strcmp(field(call, "call_id"), id) && j < earliest) {
					earliest = j;
				}
			}
		}
		if (earliest == cut) {
			break;
		}
		cut = earliest;
	}

	return cut;
}

/* Local growable string: failed appends must never silently drop history. */
struct text_buffer {
	char *data;
	size_t used;
	size_t capacity;
};

static int append_bytes(struct text_buffer *buffer, const char *text, size_t length) {

	if (length > SIZE_MAX - buffer->used - 1) {
		return 0;
	}
	size_t needed = buffer->used + length + 1;
	if (needed > buffer->capacity) {
		size_t capacity = buffer->capacity ? buffer->capacity : 256;
		while (capacity < needed) {
			if (capacity > SIZE_MAX / 2) {
				capacity = needed;
				break;
			}
			capacity *= 2;
		}
		char *grown = realloc(buffer->data, capacity);
		if (!grown) {
			return 0;
		}
		buffer->data = grown;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->used, text, length);
	buffer->used += length;
	buffer->data[buffer->used] = 0;

	return 1;
}

static int append_text(struct text_buffer *buffer, const char *text) {

	return text && append_bytes(buffer, text, strlen(text));
}

static int append_item(struct text_buffer *buffer, json_object *item) {

	const char *type = field(item, "type");
	const char *role = field(item, "role");
	const char *text;
	const char *label;
	char hidden_label[160];
	size_t shown;
	size_t length;

	if (!strcmp(type, "function_call")) {
		/* Preserve the existing 255-byte name prefix in the summary prompt. */
		char name[256];
		snprintf(name, sizeof(name), "%s ", field(item, "name"));
		return append_text(buffer, "[Assistant tool call]: ") && append_text(buffer, name) &&
			append_text(buffer, field(item, "arguments")) && append_text(buffer, "\n\n");
	}
	if (!strcmp(type, "function_call_output")) {
		label = "[Tool result]: ";
		text = field(item, "output");
		length = strlen(text);
		shown = length > TOOL_RESULT_MAX ? TOOL_RESULT_MAX : length;
		/* Cut between characters: half a UTF-8 character makes the whole
           summary request invalid, and the server rejects it as a bare 400. */
		while (shown < length && shown && ((unsigned char)text[shown] & 0xc0) == 0x80) {
			shown--;
		}
	} else if (!strcmp(role, "user") || !strcmp(role, "assistant")) {
		if (!strcmp(role, "user") && history_is_hidden(item)) {
			snprintf(hidden_label, sizeof(hidden_label),
				"[Hidden extension message %s/%s]: ", history_hidden_extension(item),
				history_hidden_type(item));
			label = hidden_label;
		} else {
			label = !strcmp(role, "user") ? "[User]: " : "[Assistant]: ";
		}
		json_object *content = json_object_object_get(item, "content");
		if (json_object_is_type(content, json_type_array)) {
			if (!append_text(buffer, label)) {
				return 0;
			}
			for (size_t i = 0; i < json_object_array_length(content); i++) {
				json_object *part = json_object_array_get_idx(content, i);
				const char *kind = field(part, "type");
				if (!strcmp(kind, "input_text") || !strcmp(kind, "output_text") ||
					!strcmp(kind, "text")) {
					if (!append_text(buffer, field(part, "text")) || !append_text(buffer, "\n")) {
						return 0;
					}
				} else if (
					!append_text(buffer,
						"[Non-text attachment omitted from summary input; consult original transcript/files.]\n")) {
					return 0;
				}
			}
			return append_text(buffer, "\n");
		}
		text = json_object_is_type(content, json_type_string) ? json_object_get_string(content)
															  : "[Unsupported content omitted]";
		if (!text) {
			return 0;
		}
		shown = length = strlen(text);
	} else {
		return 1; /* Reasoning and other non-conversation records are omitted. */
	}
	if (!append_text(buffer, label) || !append_bytes(buffer, text, shown)) {
		return 0;
	}
	if (shown < length) {
		char notice[96];
		snprintf(notice, sizeof(notice), "\n[... %zu more characters truncated]", length - shown);
		if (!append_text(buffer, notice)) {
			return 0;
		}
	}

	return append_text(buffer, "\n\n");
}

/* Build the final prompt directly, without a second full conversation copy. */
static char *summary_prompt(json_object *history, size_t end, const char *focus) {

	struct text_buffer buffer = {0};
	if (!append_text(&buffer, "<conversation>\n")) {
		goto failed;
	}
	for (size_t i = 0; i < end; i++) {
		if (!append_item(&buffer, json_object_array_get_idx(history, i))) {
			goto failed;
		}
	}
	if (!append_text(&buffer, "</conversation>\n\n") ||
		!append_text(&buffer, checkpoint_instructions)) {
		goto failed;
	}
	if (*focus && (!append_text(&buffer, "\nAdditional focus: ") || !append_text(&buffer, focus))) {
		goto failed;
	}
	return buffer.data;

failed:
	free(buffer.data);

	return NULL;
}

/* Returns an owned array containing one user message. */
static json_object *user_history(const char *text) {

	json_object *history = json_object_new_array();
	json_object *message = json_object_new_object();
	json_object *role = json_object_new_string("user");
	json_object *content = json_object_new_string(text);
	if (!history || !message || !role || !content) {
		goto failed;
	}
	if (json_object_object_add(message, "role", role) < 0) {
		goto failed;
	}
	role = NULL; /* The message now owns this reference. */
	if (json_object_object_add(message, "content", content) < 0) {
		goto failed;
	}
	content = NULL;
	if (json_object_array_add(history, message) < 0) {
		goto failed;
	}
	return history;

failed:
	if (role) {
		json_object_put(role);
	}
	if (content) {
		json_object_put(content);
	}
	if (message) {
		json_object_put(message);
	}
	if (history) {
		json_object_put(history);
	}

	return NULL;
}

static const char *summary_text(json_object *output) {

	if (!json_object_is_type(output, json_type_array)) {
		return "";
	}
	for (size_t i = 0; i < json_object_array_length(output); i++) {
		json_object *item = json_object_array_get_idx(output, i);
		json_object *content = json_object_object_get(item, "content");
		if (strcmp(field(item, "role"), "assistant") ||
			!json_object_is_type(content, json_type_array)) {
			continue;
		}
		for (size_t j = 0; j < json_object_array_length(content); j++) {
			json_object *part = json_object_array_get_idx(content, j);
			if (!strcmp(field(part, "type"), "output_text")) {
				return field(part, "text");
			}
		}
	}

	return "";
}

static json_object *checkpoint_history(json_object *history, size_t cut, const char *summary) {

	struct text_buffer buffer = {0};
	int ok = append_text(&buffer, "<context-checkpoint>\n") && append_text(&buffer, summary) &&
		append_text(&buffer, "\n</context-checkpoint>");
	json_object *next = ok ? user_history(buffer.data) : NULL;
	free(buffer.data);
	if (!next) {
		return NULL;
	}

	/* Retain references to the untouched tail, rather than copying its JSON. */
	for (size_t i = cut; i < json_object_array_length(history); i++) {
		json_object *item = json_object_get(json_object_array_get_idx(history, i));
		if (json_object_array_add(next, item) < 0) {
			json_object_put(item);
			json_object_put(next);
			return NULL;
		}
	}

	return next;
}

size_t compaction_boundary(json_object *history) {

	size_t cut = cut_point(history);
	/* One oversized turn has no older turn boundary. Summarize that whole
       turn instead of refusing recovery. */
	if (!cut && compaction_estimate(history) >= PHI_COMPACTION_KEEP) {
		cut = json_object_array_length(history);
	}

	return cut;
}

int compaction_replace(json_object **history, size_t cut, const char *summary) {

	json_object *next = checkpoint_history(*history, cut, summary);
	if (!next) {
		return 0;
	}
	json_object_put(*history);
	*history = next;

	return 1;
}

int compaction_run(struct codex *client, json_object **history, const char *model,
	const char *effort, const char *focus, struct response_sink sink, char *error,
	size_t error_size) {

	size_t cut = compaction_boundary(*history);
	if (!cut) {
		snprintf(error, error_size, "Not enough history to compact.");
		return 0;
	}
	char *prompt = summary_prompt(*history, cut, focus);
	if (!prompt) {
		goto out_of_memory;
	}
	/* Serialize once (text only, capped tool results), then summarize the
       older history in one request. Do not repeatedly summarize byte chunks. */
	if (sink.cancel && sink.cancel()) {
		free(prompt);
		snprintf(error, error_size, "Compaction cancelled; history unchanged.");
		return 0;
	}
	json_object *input = user_history(prompt);
	free(prompt);
	if (!input) {
		goto out_of_memory;
	}
	json_object *output = codex_respond_summary(client, input, model, effort, sink);
	json_object_put(input);
	const char *summary = summary_text(output);
	if (!*summary || (sink.cancel && sink.cancel())) {
		if (output) {
			json_object_put(output);
		}
		snprintf(error, error_size, "Compaction summary failed or cancelled; history unchanged.");
		return 0;
	}
	json_object *next = checkpoint_history(*history, cut, summary);
	json_object_put(output);
	if (!next) {
		goto out_of_memory;
	}

	/* Commit only after the whole replacement exists. Failure keeps history. */
	json_object_put(*history);
	*history = next;
	return 1;

out_of_memory:
	snprintf(error, error_size, "Out of memory.");

	return 0;
}
