#define _POSIX_C_SOURCE 200809L
#include "modes.h"
#include "agent.h"
#include "commands.h"
#include "compaction.h"
#include "context.h"
#include "events.h"
#include "export.h"
#include "extensions.h"
#include "history.h"
#include "host.h"
#include "json.h"
#include "models.h"
#include "resources.h"
#include "sessions.h"
#include "settings.h"
#include "terminal.h"
#include "../tools.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static enum phi_mode mode;
static struct agent *agent;
static struct settings *settings;
static int stdin_closed;
static char last_status[16] = "completed";

/* ---- output -------------------------------------------------------------- */

/* stdout carries only protocol records, one JSON object per line. */
static void write_record(json_object *record) {

	if (!record) {
		return;
	}
	fputs(json_object_to_json_string_ext(
			  record, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE),
		stdout);
	fputc('\n', stdout);
	fflush(stdout);
	json_object_put(record);
}

static json_object *record(const char *type) {

	json_object *value = json_object_new_object();
	json_object_object_add(value, "type", json_object_new_string(type));

	return value;
}

static void copy_field(
	json_object *to, const char *to_key, json_object *from, const char *from_key) {

	json_object *value = json_object_object_get(from, from_key);
	if (value) {
		json_object_object_add(to, to_key, json_object_get(value));
	}
}

/* Translate core lifecycle events into the JSON event stream (Pi's shapes,
   with Phi's Responses-API items as message bodies). */
static void observe(const char *event, json_object *payload) {

	if (!strcmp(event, "agent_end")) {
		snprintf(last_status, sizeof(last_status), "%s", field(payload, "status"));
	}
	if (mode == MODE_PRINT) {
		return;
	}
	json_object *out = NULL;
	if (!strcmp(event, "agent_start")) {
		write_record(record("agent_start"));
		const char *prompt = field(payload, "prompt");
		if (*prompt) {
			json_object *message = json_object_new_object();
			json_object_object_add(message, "role", json_object_new_string("user"));
			json_object_object_add(message, "content", json_object_new_string(prompt));
			out = record("message_start");
			json_object_object_add(out, "message", json_object_get(message));
			write_record(out);
			out = record("message_end");
			json_object_object_add(out, "message", message);
		}
	} else if (!strcmp(event, "turn_start")) {
		out = record("turn_start");
		copy_field(out, "turn", payload, "turn");
	} else if (!strcmp(event, "message_start")) {
		out = record("message_start");
		json_object *message = json_object_new_object();
		json_object_object_add(message, "role", json_object_new_string("assistant"));
		json_object_object_add(message, "content", json_object_new_array());
		json_object_object_add(out, "message", message);
	} else if (!strcmp(event, "message_update")) {
		out = record("message_update");
		json_object *update = json_object_new_object();
		json_object_object_add(update, "type", json_object_new_string("text_delta"));
		json_object_object_add(update, "contentIndex", json_object_new_int(0));
		copy_field(update, "delta", payload, "delta");
		json_object_object_add(out, "assistantMessageEvent", update);
	} else if (!strcmp(event, "message_end")) {
		out = record("message_end");
		copy_field(out, "message", payload, "message");
	} else if (!strncmp(event, "tool_execution_", 15)) {
		out = record(event);
		copy_field(out, "toolCallId", payload, "call_id");
		copy_field(out, "toolName", payload, "name");
		if (!strcmp(event, "tool_execution_start")) {
			copy_field(out, "args", payload, "arguments");
		}
		if (!strcmp(event, "tool_execution_update")) {
			json_object *partial = json_object_new_object();
			copy_field(partial, "text", payload, "text");
			json_object_object_add(out, "partialResult", partial);
		}
		if (!strcmp(event, "tool_execution_end")) {
			copy_field(out, "result", payload, "result");
			copy_field(out, "isError", payload, "is_error");
		}
	} else if (!strcmp(event, "turn_end")) {
		out = record("turn_end");
		copy_field(out, "turn", payload, "turn");
		copy_field(out, "toolCalls", payload, "tool_calls");
		copy_field(out, "status", payload, "status");
	} else if (!strcmp(event, "agent_end") || !strcmp(event, "agent_settled")) {
		out = record(event);
		copy_field(out, "status", payload, "status");
	} else if (!strcmp(event, "session_before_compact")) {
		out = record("compaction_start");
		json_object_object_add(out, "reason",
			json_object_new_string(
				!strcmp(field(payload, "reason"), "auto") ? "threshold" : "manual"));
	} else if (!strcmp(event, "session_compact")) {
		out = record("compaction_end");
		json_object *result = json_object_new_object();
		copy_field(result, "summary", payload, "summary");
		copy_field(result, "tokensBefore", payload, "tokens_before");
		copy_field(result, "fromExtension", payload, "from_extension");
		json_object_object_add(out, "result", result);
		json_object_object_add(out, "aborted", json_object_new_boolean(0));
	} else if (!strcmp(event, "session_compact_failed")) {
		out = record("compaction_end");
		json_object_object_add(out, "aborted", json_object_new_boolean(0));
		copy_field(out, "errorMessage", payload, "error");
	} else if (!strcmp(event, "session_info_changed")) {
		out = record("session_info_changed");
		copy_field(out, "name", payload, "name");
	} else if (!strcmp(event, "thinking_level_select")) {
		out = record("thinking_level_changed");
		copy_field(out, "level", payload, "level");
	} else if (!strcmp(event, "model_select")) {
		out = record("model_changed");
		copy_field(out, "model", payload, "model");
	} else if (!strcmp(event, "session_start") && strcmp(field(payload, "reason"), "startup")) {
		out = record("session_changed");
		copy_field(out, "reason", payload, "reason");
		copy_field(out, "sessionFile", payload, "session_file");
	} else if (!strcmp(event, "auto_retry_start") || !strcmp(event, "auto_retry_end")) {
		out = json_object_new_object();
		json_object_object_foreach(payload, key, value) {
			json_object_object_add(out, key, json_object_get(value));
		}
		json_object_object_add(out, "type", json_object_new_string(event));
	}
	if (out) {
		write_record(out);
	}
}

/* ---- RPC ----------------------------------------------------------------- */

#define RPC_INPUT_SIZE (1u << 20)
static char *input;
static size_t input_used;
static unsigned long ui_sequence;
static char waiting_id[32]; /* extension_ui_request awaiting a reply */
static int waiting_done;
static json_object *waiting_reply;

static void queue_update(void) {

	json_object *snapshot = host_queue_snapshot(), *out = record("queue_update");
	json_object_object_foreach(snapshot, key, value) {
		json_object_object_add(out, key, json_object_get(value));
	}
	json_object_put(snapshot);
	write_record(out);
}

static void respond(
	json_object *command, const char *type, int success, json_object *data, const char *error) {

	json_object *out = json_object_new_object();
	json_object *id = json_object_object_get(command, "id");
	if (id) {
		json_object_object_add(out, "id", json_object_get(id));
	}
	json_object_object_add(out, "type", json_object_new_string("response"));
	json_object_object_add(out, "command", json_object_new_string(type));
	json_object_object_add(out, "success", json_object_new_boolean(success));
	if (success && data) {
		json_object_object_add(out, "data", data);
	} else if (data) {
		json_object_put(data);
	}
	if (!success) {
		json_object_object_add(out, "error", json_object_new_string(error ? error : "failed"));
	}
	write_record(out);
}

/* Run a host method and answer with its result. */
static void respond_host(
	json_object *command, const char *type, const char *method, json_object *request) {

	if (!request) {
		request = json_object_new_object();
	}
	json_object *answer = host_call("rpc", method, request); /* borrows request */
	json_object_put(request);
	if (!answer) {
		respond(command, type, 0, NULL, "failed");
		return;
	}
	const char *error = field(answer, "error");
	if (*error) {
		respond(command, type, 0, NULL, error);
		json_object_put(answer);
		return;
	}
	respond(command, type, 1, answer, NULL);
}

static json_object *one(const char *key, json_object *value) {

	json_object *object = json_object_new_object();
	json_object_object_add(object, key, value);

	return object;
}

static const char *text_of(json_object *command, const char *key) {

	json_object *value = json_object_object_get(command, key);

	return json_object_is_type(value, json_type_string) ? json_object_get_string(value) : NULL;
}

/* Pi's thinking levels are Phi's ("off" is "none"); a level the model
   does not take is adjusted when the request is sent. */
static const char *effort_for(const char *level) {

	return level ? settings_effort(level) : NULL;
}

static const char *current_effort(void) {

	return agent->host.effort ? agent->host.effort() : agent->effort;
}

static char *last_assistant_text(void) {

	for (size_t i = json_object_array_length(agent->history); i > 0; i--) {
		json_object *item = json_object_array_get_idx(agent->history, i - 1);
		if (strcmp(field(item, "role"), "assistant")) {
			continue;
		}
		char text[65536];
		session_item_text(item, text, sizeof(text));
		return strdup(text);
	}

	return NULL;
}

static json_object *models_list(void) {

	json_object *models = json_object_new_array();
	for (size_t i = 0; i < phi_catalog_count(); i++) {
		const struct phi_catalog_model *model = phi_catalog_at(i);
		if (!model) {
			continue;
		}
		json_object *item = json_object_new_object();
		json_object_object_add(item, "id", json_object_new_string(model->id));
		json_object_object_add(item, "name", json_object_new_string(model->label));
		json_object_array_add(models, item);
	}
	for (size_t i = 0; i < PHI_MODEL_COUNT; i++) {
		json_object *item = json_object_new_object();
		json_object_object_add(item, "id", json_object_new_string(phi_models[i].id));
		json_object_object_add(item, "name", json_object_new_string(phi_models[i].name));
		json_object_object_add(
			item, "contextWindow", json_object_new_int64(phi_models[i].context_window));
		json_object_array_add(models, item);
	}
	for (size_t i = 0; i < extensions_model_count(); i++) {
		const char *id, *name, *provider;
		int64_t context_window;
		if (!extensions_model_at(i, &id, &name, &provider, &context_window)) {
			continue;
		}
		json_object *item = json_object_new_object();
		json_object_object_add(item, "id", json_object_new_string(id));
		json_object_object_add(item, "name", json_object_new_string(name));
		if (context_window > 0) {
			json_object_object_add(item, "contextWindow", json_object_new_int64(context_window));
		}
		if (provider) {
			json_object_object_add(item, "provider", json_object_new_string(provider));
		} else {
			json_object_object_add(item, "virtual", json_object_new_boolean(1));
		}
		json_object_array_add(models, item);
	}

	return models;
}

static json_object *tree_entries(int users_only, char *error, size_t size) {

	struct session_entry entry;
	struct session_tree tree;
	if (!*agent->transcript.path) {
		snprintf(error, size, "No saved session yet");
		return NULL;
	}
	if (agent->transcript.file) {
		fflush(agent->transcript.file);
	}
	if (!session_from_path(agent->transcript.path, &entry, error, size) ||
		!session_tree_index(&entry, &tree, error, size)) {
		return NULL;
	}
	json_object *entries = json_object_new_array();
	for (size_t i = 0; i < tree.count; i++) {
		const struct session_node *node = tree.nodes + i;
		int user = node->kind == SESSION_ITEM && (node->display & 1u) && !node->hidden;
		if (users_only && !user) {
			continue;
		}
		json_object *data = session_tree_item(&tree, i, error, size);
		if (!data) {
			json_object_put(entries);
			session_tree_free(&tree);
			return NULL;
		}
		json_object *item = json_object_new_object();
		json_object_object_add(item, "entryId", json_object_new_int64((int64_t)node->id));
		if (users_only) {
			char text[4096];
			session_item_text(data, text, sizeof(text));
			json_object_object_add(item, "text", json_object_new_string(text));
		} else {
			json_object_object_add(item, "parentId", json_object_new_int64((int64_t)node->parent));
			json_object_object_add(item, "type",
				json_object_new_string(node->kind == SESSION_COMPACTION ? "compaction" : "item"));
			json_object_object_add(item, "active", json_object_new_boolean(node->active));
			if (*node->label) {
				json_object_object_add(item, "label", json_object_new_string(node->label));
			}
			json_object_object_add(item, "data", json_object_get(data));
		}
		json_object_array_add(entries, item);
		json_object_put(data);
	}
	session_tree_free(&tree);

	return entries;
}

static void bash_update(const char *text, void *context) {

	json_object *out = record("bash_execution_update");
	json_object *id = json_object_object_get(context, "id");
	if (id) {
		json_object_object_add(out, "id", json_object_get(id));
	}
	json_object_object_add(out, "delta", json_object_new_string(text));
	write_record(out);
}

/* Pi's RPC bash: run now, stream output, and put the result in context
   for the next prompt (unless excludeFromContext). */
static void rpc_bash(json_object *command) {

	const char *shell = text_of(command, "command");
	if (!shell || !*shell) {
		respond(command, "bash", 0, NULL, "command is required");
		return;
	}
	json_object *call = json_object_new_object(), *arguments = json_object_new_object();
	json_object_object_add(arguments, "command", json_object_new_string(shell));
	json_object_object_add(arguments, "timeout", NULL);
	json_object_object_add(call, "name", json_object_new_string("bash"));
	json_object_object_add(call, "arguments",
		json_object_new_string(json_object_to_json_string_ext(arguments, JSON_C_TO_STRING_PLAIN)));
	json_object_put(arguments);
	tools_set_update_hook(bash_update, command);
	json_object *answer = tools_execute(call);
	tools_set_update_hook(NULL, NULL);
	json_object_put(call);
	if (*field(answer, "error")) {
		respond(command, "bash", 0, NULL, field(answer, "error"));
		json_object_put(answer);
		return;
	}
	const char *output = field(answer, "output");
	size_t size = strlen(shell) + strlen(output) + 32;
	char *text = malloc(size);
	if (text) {
		snprintf(text, size, "Ran `%s`\n```\n%s\n```", shell, output);
		if (json_object_get_boolean(json_object_object_get(command, "excludeFromContext"))) {
			json_object *entry = json_object_new_object();
			json_object_object_add(entry, "custom_type", json_object_new_string("bash_execution"));
			json_object_object_add(entry, "data", json_object_new_string(text));
			json_object *done = host_call("rpc", "append_entry", entry);
			json_object_put(entry);
			if (done) {
				json_object_put(done);
			}
		} else {
			const char *effort = current_effort();
			transcript_begin(&agent->transcript, agent->model, effort);
			history_add_user(agent->history, text);
			transcript_sync(&agent->transcript, agent->history);
		}
		free(text);
	}
	json_object *data = json_object_new_object();
	json_object_object_add(data, "output", json_object_new_string(output));
	copy_field(data, "exitCode", answer, "exit_code");
	copy_field(data, "cancelled", answer, "cancelled");
	copy_field(data, "truncated", answer, "truncated");
	copy_field(data, "fullOutputPath", answer, "output_file");
	json_object_put(answer);
	respond(command, "bash", 1, data, NULL);
}

static void rpc_message(json_object *command, const char *type, const char *message);

static void rpc_prompt(json_object *command, const char *type) {

	const char *message = text_of(command, "message");
	if (!message || !*message) {
		respond(command, type, 0, NULL, "message is required");
		return;
	}
	/* Prompt templates and skills expand, then behave as ordinary prompts. */
	char *expanded = resources_expand(message);
	rpc_message(command, type, expanded ? expanded : message);
	free(expanded);
}

static void rpc_message(json_object *command, const char *type, const char *message) {

	if (json_object_object_get(command, "images")) {
		respond(command, type, 0, NULL, "images are not supported yet");
		return;
	}
	int busy = host_busy();
	if (!strcmp(type, "prompt") && message[0] == '/') {
		/* Commands run now; mid-run only extension commands, as in Pi. */
		if (busy) {
			if (!extensions_command(message)) {
				respond(command, type, 0, NULL,
					"Only extension commands run while the agent is working");
				return;
			}
		} else if (command_run(agent, settings, message) == 2) {
			json_object *empty = json_object_new_object(),
						*done = host_call("rpc", "shutdown", empty);
			json_object_put(empty);
			if (done) {
				json_object_put(done);
			}
		}
		respond(command, type, 1, NULL, NULL);
		return;
	}
	const char *behavior = text_of(command, "streamingBehavior");
	int follow_up;
	if (!strcmp(type, "steer")) {
		follow_up = 0;
	} else if (!strcmp(type, "follow_up")) {
		follow_up = 1;
	} else if (!busy) {
		follow_up = 1; /* idle: runs next */
	} else if (behavior && !strcmp(behavior, "steer")) {
		follow_up = 0;
	} else if (behavior && !strcmp(behavior, "followUp")) {
		follow_up = 1;
	} else {
		respond(
			command, type, 0, NULL, "Agent is working; set streamingBehavior to steer or followUp");
		return;
	}
	if (!host_enqueue_prompt(message, follow_up)) {
		respond(command, type, 0, NULL, "Too many queued messages");
		return;
	}
	respond(command, type, 1, NULL, NULL);
	queue_update();
}

static int rpc_idle(json_object *command, const char *type) {

	if (!host_busy()) {
		return 1;
	}
	respond(command, type, 0, NULL, "Only available while the agent is idle");

	return 0;
}

static void rpc_command(json_object *command) {

	const char *type = field(command, "type");
	if (!strcmp(type, "extension_ui_response")) {
		if (*waiting_id && !strcmp(field(command, "id"), waiting_id)) {
			waiting_done = 1;
			if (waiting_reply) {
				json_object_put(waiting_reply);
			}
			waiting_reply = json_object_get(command);
		}
		return;
	}
	if (!strcmp(type, "prompt") || !strcmp(type, "steer") || !strcmp(type, "follow_up")) {
		rpc_prompt(command, type);
		return;
	}
	if (!strcmp(type, "abort")) {
		if (host_busy()) {
			terminal_request_cancel();
		}
		respond(command, type, 1, NULL, NULL);
		return;
	}
	if (!strcmp(type, "clear_queue")) {
		respond(command, type, 1, host_clear_queue(), NULL);
		queue_update();
		return;
	}
	if (!strcmp(type, "get_state")) {
		json_object *empty = json_object_new_object(),
					*session = host_call("rpc", "get_session", empty);
		json_object_put(empty);
		json_object *data = json_object_new_object(), *model = json_object_new_object();
		json_object_object_add(model, "id", json_object_new_string(agent->model));
		json_object_object_add(data, "model", model);
		json_object_object_add(data, "thinkingLevel", json_object_new_string(current_effort()));
		json_object_object_add(data, "isStreaming", json_object_new_boolean(host_busy()));
		copy_field(data, "sessionFile", session, "file");
		copy_field(data, "temporary", session, "temporary");
		if (*field(session, "name")) {
			copy_field(data, "sessionName", session, "name");
		}
		copy_field(data, "cwd", session, "cwd");
		json_object_object_add(data, "messageCount",
			json_object_new_int64((int64_t)json_object_array_length(agent->history)));
		json_object_object_add(
			data, "pendingMessageCount", json_object_new_int64((int64_t)host_pending_count()));
		if (session) {
			json_object_put(session);
		}
		respond(command, type, 1, data, NULL);
		return;
	}
	if (!strcmp(type, "get_messages")) {
		respond(command, type, 1, one("messages", json_object_get(agent->history)), NULL);
		return;
	}
	if (!strcmp(type, "get_last_assistant_text")) {
		char *text = last_assistant_text();
		respond(command, type, 1, one("text", text ? json_object_new_string(text) : NULL), NULL);
		free(text);
		return;
	}
	if (!strcmp(type, "set_model")) {
		const char *model = text_of(command, "modelId");
		if (!model) {
			model = text_of(command, "model");
		}
		if (!model) {
			respond(command, type, 0, NULL, "modelId is required");
			return;
		}
		respond_host(command, type, "set_model", one("model", json_object_new_string(model)));
		return;
	}
	if (!strcmp(type, "get_available_models")) {
		respond(command, type, 1, one("models", models_list()), NULL);
		return;
	}
	if (!strcmp(type, "cycle_model")) {
		json_object *models = models_list();
		size_t count = json_object_array_length(models), next = 0;
		for (size_t i = 0; i < count; i++) {
			if (!strcmp(field(json_object_array_get_idx(models, i), "id"), agent->model)) {
				next = (i + 1) % count;
			}
		}
		const char *id =
			count ? field(json_object_array_get_idx(models, next), "id") : agent->model;
		respond_host(command, type, "set_model", one("model", json_object_new_string(id)));
		json_object_put(models);
		return;
	}
	if (!strcmp(type, "set_thinking_level") || !strcmp(type, "cycle_thinking_level")) {
		static const char *levels[] = {"none", "low", "medium", "high"};
		const char *level = NULL;
		if (!strcmp(type, "set_thinking_level")) {
			level = effort_for(text_of(command, "level"));
		} else {
			for (int i = 0; i < 4; i++) {
				if (!strcmp(levels[i], current_effort())) {
					level = levels[(i + 1) % 4];
				}
			}
		}
		if (!level) {
			respond(command, type, 0, NULL,
				"level must be off, minimal, low, medium, high, xhigh or max");
			return;
		}
		respond_host(
			command, type, "set_thinking_level", one("level", json_object_new_string(level)));
		return;
	}
	if (!strcmp(type, "get_available_thinking_levels")) {
		json_object *levels = json_object_new_array();
		const char *names[] = {"none", "low", "medium", "high"};
		for (int i = 0; i < 4; i++) {
			json_object_array_add(levels, json_object_new_string(names[i]));
		}
		respond(command, type, 1, one("levels", levels), NULL);
		return;
	}
	if (!strcmp(type, "new_session")) {
		if (rpc_idle(command, type)) {
			respond_host(command, type, "new_session", NULL);
		}
		return;
	}
	if (!strcmp(type, "compact")) {
		if (!rpc_idle(command, type)) {
			return;
		}
		const char *focus = text_of(command, "customInstructions");
		respond_host(
			command, type, "compact", one("focus", json_object_new_string(focus ? focus : "")));
		return;
	}
	if (!strcmp(type, "bash")) {
		rpc_bash(command);
		return;
	}
	if (!strcmp(type, "get_session_stats")) {
		json_object *data = json_object_new_object();
		json_object_object_add(data, "sessionFile", json_object_new_string(agent->transcript.path));
		json_object_object_add(data, "messageCount",
			json_object_new_int64((int64_t)json_object_array_length(agent->history)));
		json_object_object_add(
			data, "tokensEstimate", json_object_new_int64(compaction_estimate(agent->history)));
		json_object_object_add(
			data, "contextWindow", json_object_new_int64(context_window(agent->model)));
		respond(command, type, 1, data, NULL);
		return;
	}
	if (!strcmp(type, "export_html")) {
		const char *path = text_of(command, "outputPath");
		char error[256];
		if (!path) {
			respond(command, type, 0, NULL, "outputPath is required");
			return;
		}
		if (!phi_export_html(agent->history, path, error, sizeof(error))) {
			respond(command, type, 0, NULL, error);
			return;
		}
		respond(command, type, 1, one("path", json_object_new_string(path)), NULL);
		return;
	}
	if (!strcmp(type, "switch_session")) {
		if (!rpc_idle(command, type)) {
			return;
		}
		const char *path = text_of(command, "sessionPath");
		if (!path) {
			respond(command, type, 0, NULL, "sessionPath is required");
			return;
		}
		respond_host(command, type, "switch_session", one("file", json_object_new_string(path)));
		return;
	}
	if (!strcmp(type, "fork") || !strcmp(type, "navigate_tree")) {
		if (!rpc_idle(command, type)) {
			return;
		}
		json_object *id = json_object_object_get(command, "entryId");
		int64_t entry = json_object_is_type(id, json_type_string)
			? strtoll(json_object_get_string(id), NULL, 10)
			: json_object_get_int64(id);
		respond_host(command, type, type, one("entry_id", json_object_new_int64(entry)));
		return;
	}
	if (!strcmp(type, "clone")) {
		if (!rpc_idle(command, type)) {
			return;
		}
		int done = terminal_clone(agent);
		if (done) {
			respond(command, type, 1, one("cancelled", json_object_new_boolean(0)), NULL);
		} else {
			respond(command, type, 0, NULL, "Could not clone the session");
		}
		return;
	}
	if (!strcmp(type, "get_fork_messages") || !strcmp(type, "get_entries") ||
		!strcmp(type, "get_tree")) {
		char error[256] = "";
		json_object *entries =
			tree_entries(!strcmp(type, "get_fork_messages"), error, sizeof(error));
		if (!entries) {
			respond(command, type, 0, NULL, error);
			return;
		}
		json_object *data =
			one(!strcmp(type, "get_fork_messages") ? "messages" : "entries", entries);
		if (!strcmp(type, "get_tree")) {
			json_object_object_add(
				data, "head", json_object_new_int64((int64_t)agent->transcript.head));
		}
		respond(command, type, 1, data, NULL);
		return;
	}
	if (!strcmp(type, "set_session_name")) {
		respond_host(command, type, "set_session_name",
			one("name", json_object_new_string(field(command, "name"))));
		return;
	}
	if (!strcmp(type, "get_commands")) {
		respond_host(command, type, "get_commands", NULL);
		return;
	}
	if (!strcmp(type, "set_steering_mode") || !strcmp(type, "set_follow_up_mode") ||
		!strcmp(type, "set_auto_compaction") || !strcmp(type, "set_auto_retry") ||
		!strcmp(type, "abort_retry") || !strcmp(type, "abort_bash")) {
		respond(command, type, 0, NULL, "Not supported by Phi");
		return;
	}
	respond(command, *type ? type : "unknown", 0, NULL, "Unknown command");
}

/* Handle every complete line in the input buffer. Each line leaves the
   buffer before it runs: a command can read more input (an approval dialog
   waits for its reply), which re-enters this function. */
static void drain_lines(void) {

	char *end;
	while ((end = memchr(input, '\n', input_used))) {
		size_t length = (size_t)(end - input);
		char *line = malloc(length + 1);
		if (line) {
			memcpy(line, input, length);
			line[length] = 0;
		}
		input_used -= length + 1;
		memmove(input, end + 1, input_used);
		if (!line) {
			continue;
		}
		if (length && line[length - 1] == '\r') {
			line[--length] = 0;
		}
		if (*line) {
			json_tokener *parser = json_tokener_new();
			json_object *command =
				parser ? json_tokener_parse_ex(parser, line, (int)length + 1) : NULL;
			int valid = parser && json_tokener_get_error(parser) == json_tokener_success &&
				json_object_is_type(command, json_type_object);
			if (parser) {
				json_tokener_free(parser);
			}
			if (valid) {
				rpc_command(command);
			} else {
				json_object *out = record("response");
				json_object_object_add(out, "command", json_object_new_string("parse"));
				json_object_object_add(out, "success", json_object_new_boolean(0));
				json_object_object_add(out, "error",
					json_object_new_string(
						"Failed to parse command: expected one JSON object per line"));
				write_record(out);
			}
			if (command) {
				json_object_put(command);
			}
		}
		free(line);
	}
}

int modes_rpc_wait(int timeout_ms) {

	if (stdin_closed) {
		return 0;
	}
	/* Interactive chats never need the RPC line buffer. Allocate it only
       when an RPC client actually starts reading, retaining the same limit. */
	if (!input) {
		input = malloc(RPC_INPUT_SIZE);
		if (!input) {
			fputs("Cannot allocate RPC input buffer.\n", stderr);
			stdin_closed = 1;
			return 0;
		}
	}
	struct pollfd poller = {STDIN_FILENO, POLLIN, 0};
	int ready = poll(&poller, 1, timeout_ms);
	if (ready <= 0) {
		return 1;
	}
	for (;;) {
		if (input_used == RPC_INPUT_SIZE - 1) {
			/* A line longer than the buffer cannot be a valid command. */
			input_used = 0;
			json_object *out = record("response");
			json_object_object_add(out, "command", json_object_new_string("parse"));
			json_object_object_add(out, "success", json_object_new_boolean(0));
			json_object_object_add(out, "error", json_object_new_string("Command exceeds 1 MiB"));
			write_record(out);
		}
		ssize_t n = read(STDIN_FILENO, input + input_used, RPC_INPUT_SIZE - 1 - input_used);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			if (!n) {
				stdin_closed = 1;
			}
			break;
		}
		input_used += (size_t)n;
		drain_lines();
		if (!poll(&poller, 1, 0)) {
			break;
		}
	}

	return !stdin_closed;
}

/* ---- headless hooks ------------------------------------------------------- */

static void rpc_poll(void) {

	modes_rpc_wait(0);
}

/* Ask the client (Pi's extension UI protocol) and wait for its reply.
   The reply is returned (caller releases it), or NULL when stdin closed. */
static json_object *rpc_dialog(json_object *request) {

	snprintf(waiting_id, sizeof(waiting_id), "ui-%lu", ++ui_sequence);
	waiting_done = 0;
	if (waiting_reply) {
		json_object_put(waiting_reply);
		waiting_reply = NULL;
	}
	json_object_object_add(request, "type", json_object_new_string("extension_ui_request"));
	json_object_object_add(request, "id", json_object_new_string(waiting_id));
	write_record(request);
	while (!waiting_done && modes_rpc_wait(50)) {
	}
	waiting_id[0] = 0;
	json_object *reply = waiting_reply;
	waiting_reply = NULL;

	return waiting_done ? reply : NULL;
}

/* Tool and native-code approvals become Pi's extension UI confirm dialog. */
static int rpc_confirm(const char *title, const char *message) {

	json_object *request = json_object_new_object();
	json_object_object_add(request, "method", json_object_new_string("confirm"));
	json_object_object_add(request, "title", json_object_new_string(title));
	json_object_object_add(request, "message", json_object_new_string(message));
	json_object *reply = rpc_dialog(request);
	const char *value = field(reply, "value");
	int yes = reply && !json_object_get_boolean(json_object_object_get(reply, "cancelled")) &&
		(json_object_get_boolean(json_object_object_get(reply, "confirmed")) ||
			!strcmp(value, "Allow") || !strcmp(value, "yes"));
	if (reply) {
		json_object_put(reply);
	}

	return yes;
}

/* Extension UI calls ("ui_*") in Pi's RPC shapes. */
static json_object *rpc_ui(const char *method, json_object *request) {

	static const char *const names[][2] = {{"ui_confirm", "confirm"}, {"ui_select", "select"},
		{"ui_input", "input"}, {"ui_editor", "editor"}, {"ui_notify", "notify"}};
	const char *pi = NULL;
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
		if (!strcmp(method, names[i][0])) {
			pi = names[i][1];
		}
	}
	json_object *out = json_object_new_object();
	if (!strcmp(method, "ui_set_editor_text")) {
		json_object *message = record("extension_ui_request");
		char id[32];
		snprintf(id, sizeof(id), "ui-%lu", ++ui_sequence);
		json_object_object_add(message, "id", json_object_new_string(id));
		json_object_object_add(message, "method", json_object_new_string("set_editor_text"));
		copy_field(message, "text", request, "text");
		write_record(message);
		return out;
	}
	if (!strcmp(method, "ui_widget") || !strcmp(method, "ui_title")) {
		/* Fire and forget, in Pi's shapes. */
		json_object *message = record("extension_ui_request");
		char id[32];
		snprintf(id, sizeof(id), "ui-%lu", ++ui_sequence);
		json_object_object_add(message, "id", json_object_new_string(id));
		if (!strcmp(method, "ui_title")) {
			json_object_object_add(message, "method", json_object_new_string("setTitle"));
			copy_field(message, "title", request, "title");
		} else {
			const char *placement = field(request, "placement");
			json_object_object_add(message, "method", json_object_new_string("setWidget"));
			copy_field(message, "widgetKey", request, "key");
			json_object *lines = json_object_object_get(request, "lines");
			if (json_object_is_type(lines, json_type_array)) {
				json_object_object_add(message, "widgetLines", json_object_get(lines));
			}
			json_object_object_add(message, "widgetPlacement",
				json_object_new_string(
					!strcmp(placement, "below_editor") || !strcmp(placement, "belowEditor")
						? "belowEditor"
						: "aboveEditor"));
		}
		write_record(message);
		return out;
	}
	if (!strcmp(method, "ui_header") || !strcmp(method, "ui_footer") ||
		!strcmp(method, "ui_working") || !strcmp(method, "ui_reset")) {
		return out; /* terminal only, as in Pi */
	}
	if (!pi) {
		json_object_object_add(out, "error", json_object_new_string("Unknown UI method"));
		return out;
	}
	json_object *message = json_object_new_object();
	json_object_object_add(message, "method", json_object_new_string(pi));
	const char *keys[][2] = {{"title", "title"}, {"message", "message"}, {"options", "options"},
		{"placeholder", "placeholder"}, {"value", "prefill"}, {"timeout_ms", "timeout"}};
	for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
		copy_field(message, keys[i][1], request, keys[i][0]);
	}
	if (!strcmp(pi, "notify")) {
		char id[32];
		snprintf(id, sizeof(id), "ui-%lu", ++ui_sequence);
		json_object_object_add(message, "type", json_object_new_string("extension_ui_request"));
		json_object_object_add(message, "id", json_object_new_string(id));
		json_object_object_add(message, "notifyType",
			json_object_new_string(*field(request, "level") ? field(request, "level") : "info"));
		write_record(message);
		return out;
	}
	json_object *reply = rpc_dialog(message);
	int given = reply && !json_object_get_boolean(json_object_object_get(reply, "cancelled"));
	if (!strcmp(pi, "confirm")) {
		json_object_object_add(out, "confirmed",
			json_object_new_boolean(
				given && json_object_get_boolean(json_object_object_get(reply, "confirmed"))));
	} else {
		json_object *value = given ? json_object_object_get(reply, "value") : NULL;
		json_object_object_add(out, "value",
			json_object_is_type(value, json_type_string) ? json_object_get(value) : NULL);
		if (!strcmp(pi, "select") && value) {
			json_object *options = json_object_object_get(request, "options");
			for (size_t i = 0; i < json_object_array_length(options); i++) {
				if (!strcmp(json_object_get_string(json_object_array_get_idx(options, i)),
						json_object_get_string(value))) {
					json_object_object_add(out, "index", json_object_new_int((int)i));
				}
			}
		}
		if (!value) {
			given = 0;
		}
	}
	if (!given) {
		json_object_object_add(out, "cancelled", json_object_new_boolean(1));
	}
	if (reply) {
		json_object_put(reply);
	}

	return out;
}

static void rpc_notify(const char *text) {

	json_object *out = record("extension_ui_request");
	char id[32];
	snprintf(id, sizeof(id), "ui-%lu", ++ui_sequence);
	json_object_object_add(out, "id", json_object_new_string(id));
	json_object_object_add(out, "method", json_object_new_string("notify"));
	json_object_object_add(out, "message", json_object_new_string(text));
	json_object_object_add(out, "notifyType", json_object_new_string("info"));
	write_record(out);
}

static void rpc_status(const char *text) {

	static int shown;
	if (!*text && !shown) {
		return;
	}
	shown = *text != 0;
	json_object *out = record("extension_ui_request");
	char id[32];
	snprintf(id, sizeof(id), "ui-%lu", ++ui_sequence);
	json_object_object_add(out, "id", json_object_new_string(id));
	json_object_object_add(out, "method", json_object_new_string("setStatus"));
	json_object_object_add(out, "statusKey", json_object_new_string("extensions"));
	json_object_object_add(out, "statusText", json_object_new_string(text));
	write_record(out);
}

static void stderr_notify(const char *text) {

	fprintf(stderr, "%s\n", text);
}

static const struct headless_ops print_ops = {.notify = stderr_notify};
static const struct headless_ops json_ops = {.notify = stderr_notify};
static const struct headless_ops rpc_ops = {.poll = rpc_poll,
	.confirm = rpc_confirm,
	.notify = rpc_notify,
	.status = rpc_status,
	.ui = rpc_ui};

void modes_start(enum phi_mode value, struct agent *active, struct settings *config) {

	mode = value;
	agent = active;
	settings = config;
	static const char *names[] = {"interactive", "print", "json", "rpc"};
	host_set_mode(names[mode]);
	if (mode == MODE_INTERACTIVE) {
		return;
	}
	terminal_set_headless(mode == MODE_PRINT ? &print_ops
			: mode == MODE_JSON              ? &json_ops
											 : &rpc_ops);
	events_set_observer(observe);
}

void modes_header(void) {

	if (mode != MODE_JSON) {
		return;
	}
	char cwd[4096];
	json_object *out = record("session");
	json_object_object_add(out, "format", json_object_new_string("phi"));
	json_object_object_add(out, "cwd", json_object_new_string(getcwd(cwd, sizeof(cwd)) ? cwd : ""));
	json_object_object_add(out, "model", json_object_new_string(agent->model));
	json_object_object_add(out, "thinkingLevel", json_object_new_string(current_effort()));
	write_record(out);
}

int modes_failed(void) {

	return strcmp(last_status, "completed") != 0;
}

char *modes_last_text(void) {

	return last_assistant_text();
}

void modes_stop(void) {

	free(input);
	input = NULL;
	input_used = 0;
	events_set_observer(NULL);
	terminal_set_headless(NULL);
}
