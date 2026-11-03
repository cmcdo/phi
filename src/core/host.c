#define _POSIX_C_SOURCE 200809L
#include "host.h"
#include "resources.h"
#include "agent.h"
#include "commands.h"
#include "compaction.h"
#include "context.h"
#include "events.h"
#include "extensions.h"
#include "history.h"
#include "instructions.h"
#include "json.h"
#include "settings.h"
#include "terminal.h"
#include "../tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define QUEUE_LIMIT 32

static void (*display_hook)(const char *extension, const char *type, const char *content);

void host_set_display(
	void (*display)(const char *extension, const char *type, const char *content)) {

	display_hook = display;
}

static struct agent *agent;
static struct settings *settings;
static int busy, shutdown_requested, reload_requested;

enum item_kind {
	VISIBLE,
	FOLLOW_UP,
	HIDDEN,
	CONTINUE
};

struct queued {
	enum item_kind kind;
	char *text, *extension, *custom_type;
	int trigger;
};
static struct queued queue[QUEUE_LIMIT];
static size_t queued_count;

void host_attach(struct agent *value, struct settings *config) {

	agent = value;
	settings = config;
}

void host_set_busy(int value) {

	busy = value;
}

int host_shutdown_requested(void) {

	return shutdown_requested;
}

int host_take_reload(void) {

	int value = reload_requested;
	reload_requested = 0;

	return value;
}

static void item_free(struct queued *item) {

	free(item->text);
	free(item->extension);
	free(item->custom_type);
	*item = (struct queued){0};
}

void host_reset(void) {

	while (queued_count) {
		item_free(&queue[--queued_count]);
	}
	shutdown_requested = reload_requested = busy = 0;
}

static int enqueue(enum item_kind kind, const char *text, const char *extension,
	const char *custom_type, int trigger) {

	if (queued_count == QUEUE_LIMIT) {
		return 0;
	}
	struct queued item = {kind, text ? strdup(text) : NULL, strdup(extension ? extension : ""),
		strdup(custom_type ? custom_type : ""), trigger};
	if ((text && !item.text) || !item.extension || !item.custom_type) {
		item_free(&item);
		return 0;
	}
	queue[queued_count++] = item;

	return 1;
}

static struct queued dequeue(size_t index) {

	struct queued item = queue[index];
	memmove(queue + index, queue + index + 1, (queued_count - index - 1) * sizeof(*queue));
	queued_count--;

	return item;
}

/* Idle work: the first queued visible message or triggered run. Hidden
   context queued while idle was already added to history. */
enum host_work host_next_work(char **message) {

	*message = NULL;
	for (size_t i = 0; i < queued_count; i++) {
		if (queue[i].kind == HIDDEN) {
			continue;
		}
		struct queued item = dequeue(i);
		enum host_work work = item.kind == CONTINUE ? HOST_CONTINUE : HOST_USER_MESSAGE;
		*message = item.text;
		item.text = NULL;
		item_free(&item);
		return work;
	}

	return HOST_NONE;
}

char *host_take_steer(void) {

	for (size_t i = 0; i < queued_count; i++) {
		if (queue[i].kind != VISIBLE) {
			continue;
		}
		struct queued item = dequeue(i);
		char *text = item.text;
		item.text = NULL;
		item_free(&item);
		return text;
	}

	return NULL;
}

int host_boundary(json_object *history) {

	int trigger = 0;
	for (size_t i = 0; i < queued_count;) {
		if (queue[i].kind != HIDDEN) {
			i++;
			continue;
		}
		struct queued item = dequeue(i);
		if (history_add_hidden_user(history, item.text, item.extension, item.custom_type) &&
			item.trigger) {
			trigger = 1;
		}
		item_free(&item);
	}

	return trigger;
}

int host_enqueue_prompt(const char *text, int follow_up) {

	return enqueue(follow_up ? FOLLOW_UP : VISIBLE, text, NULL, NULL, 0);
}

size_t host_pending_count(void) {

	size_t count = 0;
	for (size_t i = 0; i < queued_count; i++) {
		count += queue[i].kind == VISIBLE || queue[i].kind == FOLLOW_UP;
	}

	return count;
}

json_object *host_queue_snapshot(void) {

	json_object *answer = json_object_new_object(), *steering = json_object_new_array(),
				*follow = json_object_new_array();
	for (size_t i = 0; i < queued_count; i++) {
		if (queue[i].kind == VISIBLE || queue[i].kind == FOLLOW_UP) {
			json_object_array_add(queue[i].kind == VISIBLE ? steering : follow,
				json_object_new_string(queue[i].text));
		}
	}
	json_object_object_add(answer, "steering", steering);
	json_object_object_add(answer, "followUp", follow);

	return answer;
}

int host_busy(void) {

	return busy;
}

json_object *host_clear_queue(void) {

	json_object *answer = json_object_new_object(), *steering = json_object_new_array(),
				*follow = json_object_new_array();
	for (size_t i = 0; i < queued_count;) {
		if (queue[i].kind != VISIBLE && queue[i].kind != FOLLOW_UP) {
			i++;
			continue;
		}
		struct queued item = dequeue(i);
		json_object_array_add(
			item.kind == VISIBLE ? steering : follow, json_object_new_string(item.text));
		item_free(&item);
	}
	json_object_object_add(answer, "steering", steering);
	json_object_object_add(answer, "followUp", follow);

	return answer;
}

/* ---- method helpers ------------------------------------------------------ */

static json_object *error(const char *message) {

	json_object *answer = json_object_new_object();
	if (answer) {
		json_object_object_add(answer, "error", json_object_new_string(message));
	}

	return answer;
}

static json_object *ok(void) {

	return json_object_new_object();
}

static const char *text_field(json_object *request, const char *key) {

	json_object *value = json_object_object_get(request, key);

	return json_object_is_type(value, json_type_string) ? json_object_get_string(value) : NULL;
}

static int type_valid(const char *type) {

	size_t n = type ? strlen(type) : 0;

	return n && n <= 48 &&
		strspn(type, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == n;
}

/* Records of one type from the current transcript file. */
static json_object *transcript_records(const char *type) {

	json_object *records = json_object_new_array();
	if (!records || !*agent->transcript.path) {
		return records;
	}
	if (agent->transcript.file) {
		fflush(agent->transcript.file);
	}
	FILE *file = fopen(agent->transcript.path, "r");
	if (!file) {
		return records;
	}
	char *line = NULL;
	size_t capacity = 0;
	while (getline(&line, &capacity, file) > 0) {
		if (!strstr(line, type)) {
			continue;
		}
		json_object *record = json_tokener_parse(line);
		if (record && !strcmp(field(record, "type"), type)) {
			json_object_array_add(records, record);
		} else if (record) {
			json_object_put(record);
		}
	}
	free(line);
	fclose(file);

	return records;
}

static const char *session_name(json_object *names) {

	size_t count = json_object_array_length(names);

	return count ? field(json_object_array_get_idx(names, count - 1), "data") : "";
}

static void ensure_transcript(void) {

	const char *effort = agent->host.effort ? agent->host.effort() : agent->effort;
	transcript_begin(&agent->transcript, agent->model, effort);
}

static json_object *run_command(const char *command) {

	if (command_run(agent, settings, command) == 2) {
		shutdown_requested = 1;
	}

	return ok();
}

/* ---- methods ------------------------------------------------------------- */

static json_object *send_user_message(json_object *request) {

	const char *text = text_field(request, "text");
	if (!text || !*text) {
		return error("text is required");
	}
	if (!enqueue(VISIBLE, text, NULL, NULL, 0)) {
		return error("Too many queued messages");
	}
	json_object *answer = ok();
	json_object_object_add(answer, "queued", json_object_new_boolean(1));

	return answer;
}

static json_object *send_message(const char *extension, json_object *request) {

	const char *content = text_field(request, "content");
	const char *type = text_field(request, "custom_type");
	if (!type) {
		type = "message";
	}
	if (!content || !*content) {
		return error("content is required");
	}
	if (!type_valid(type)) {
		return error("custom_type must be 1-48 letters, digits, _ . -");
	}
	int trigger = json_object_get_boolean(json_object_object_get(request, "trigger_turn"));
	if (display_hook && json_object_get_boolean(json_object_object_get(request, "display"))) {
		display_hook(extension, type, content);
	}
	if (busy) {
		/* Mid-run history must stay call/output ordered: add at the next boundary. */
		if (!enqueue(HIDDEN, content, extension, type, trigger)) {
			return error("Too many queued messages");
		}
		return ok();
	}
	ensure_transcript();
	if (!history_add_hidden_user(agent->history, content, extension, type)) {
		return error("Cannot add message");
	}
	transcript_sync(&agent->transcript, agent->history);
	if (trigger && !enqueue(CONTINUE, NULL, extension, type, 1)) {
		return error("Too many queued messages");
	}

	return ok();
}

static json_object *append_entry(const char *extension, json_object *request) {

	const char *type = text_field(request, "custom_type");
	if (!type_valid(type)) {
		return error("custom_type must be 1-48 letters, digits, _ . -");
	}
	json_object *data = json_object_object_get(request, "data");
	ensure_transcript();
	json_object *entry = json_object_new_object();
	json_object_object_add(entry, "extension", json_object_new_string(extension));
	json_object_object_add(entry, "custom_type", json_object_new_string(type));
	json_object_object_add(entry, "data", json_object_get(data));
	int written = transcript_entry(&agent->transcript, "custom_entry", entry);
	json_object_put(entry);
	if (written && display_hook &&
		json_object_get_boolean(json_object_object_get(request, "display"))) {
		const char *text = json_object_is_type(data, json_type_string)
			? json_object_get_string(data)
			: *field(data, "text") ? field(data, "text")
								   : json_object_to_json_string_ext(data, JSON_C_TO_STRING_PRETTY);
		display_hook(extension, type, text);
	}

	return written ? ok() : error("Could not save the entry");
}

static json_object *get_custom_entries(json_object *request) {

	const char *type = text_field(request, "custom_type");
	json_object *records = transcript_records("custom_entry"), *entries = json_object_new_array();
	for (size_t i = 0; i < json_object_array_length(records); i++) {
		json_object *record = json_object_array_get_idx(records, i);
		json_object *data = json_object_object_get(record, "data");
		if (type && strcmp(field(data, "custom_type"), type)) {
			continue;
		}
		json_object *entry = json_object_new_object();
		json_object_object_add(
			entry, "extension", json_object_new_string(field(data, "extension")));
		json_object_object_add(
			entry, "custom_type", json_object_new_string(field(data, "custom_type")));
		json_object_object_add(
			entry, "data", json_object_get(json_object_object_get(data, "data")));
		json_object_object_add(
			entry, "time", json_object_get(json_object_object_get(record, "time")));
		json_object_array_add(entries, entry);
	}
	json_object_put(records);
	json_object *answer = ok();
	json_object_object_add(answer, "entries", entries);

	return answer;
}

static char **given_flags;
static size_t given_flag_count;
static char claimed_flags[32][65];
static size_t claimed_count;

void host_set_flags(char **flags, size_t count) {

	given_flags = flags;
	given_flag_count = count;
}

/* "--name" or "--name=value": the name part. */
static int flag_is(const char *given, const char *name) {

	size_t n = strlen(name);

	return !strncmp(given + 2, name, n) && (given[2 + n] == 0 || given[2 + n] == '=');
}

const char *host_unclaimed_flag(void) {

	for (size_t i = 0; i < given_flag_count; i++) {
		int claimed = 0;
		for (size_t k = 0; k < claimed_count; k++) {
			if (flag_is(given_flags[i], claimed_flags[k])) {
				claimed = 1;
			}
		}
		if (!claimed) {
			return given_flags[i];
		}
	}

	return NULL;
}

static json_object *register_flag(json_object *request) {

	const char *name = text_field(request, "name");
	size_t length = name ? strlen(name) : 0;
	int valid = length && length <= 64 && name[0] != '-';
	for (size_t i = 0; valid && i < length; i++) {
		if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') ||
				name[i] == '-')) {
			valid = 0;
		}
	}
	if (!valid) {
		return error(
			"Flag names are lowercase letters, digits and hyphens (without the leading --)");
	}
	for (size_t k = 0; k < claimed_count; k++) {
		if (!strcmp(claimed_flags[k], name)) {
			return ok();
		}
	}
	if (claimed_count == 32) {
		return error("Too many flags");
	}
	snprintf(claimed_flags[claimed_count++], sizeof(claimed_flags[0]), "%s", name);

	return ok();
}

static json_object *get_flag(json_object *request) {

	const char *name = text_field(request, "name");
	json_object *answer = ok();
	json_object *value = NULL;
	for (size_t i = 0; name && i < given_flag_count; i++) {
		if (flag_is(given_flags[i], name)) {
			const char *equals = strchr(given_flags[i], '=');
			value = equals ? json_object_new_string(equals + 1) : json_object_new_boolean(1);
		}
	}
	json_object_object_add(answer, "value", value);

	return answer;
}

static const char *run_mode = "interactive";

void host_set_mode(const char *mode) {

	run_mode = mode;
}

static json_object *get_session(void) {

	json_object *answer = ok(), *names = transcript_records("name");
	char cwd[4096];
	const char *effort = agent->host.effort ? agent->host.effort() : agent->effort;
	json_object_object_add(answer, "file", json_object_new_string(agent->transcript.path));
	json_object_object_add(answer, "name", json_object_new_string(session_name(names)));
	json_object_object_add(answer, "head", json_object_new_int64((int64_t)agent->transcript.head));
	json_object_object_add(answer, "idle", json_object_new_boolean(!busy));
	json_object_object_add(answer, "mode", json_object_new_string(run_mode));
	json_object_object_add(answer, "has_ui",
		json_object_new_boolean(!strcmp(run_mode, "interactive") || !strcmp(run_mode, "rpc")));
	json_object_object_add(
		answer, "cwd", json_object_new_string(getcwd(cwd, sizeof(cwd)) ? cwd : ""));
	json_object_object_add(answer, "model", json_object_new_string(agent->model));
	json_object_object_add(answer, "effort", json_object_new_string(effort));
	json_object_object_add(
		answer, "tokens_estimate", json_object_new_int64(compaction_estimate(agent->history)));
	json_object_object_add(
		answer, "context_window", json_object_new_int64(context_window(agent->model)));
	json_object_put(names);

	return answer;
}

static json_object *get_commands(void) {

	json_object *answer = ok(), *list = json_object_new_array();
	size_t extensions = extensions_command_count(),
		   total = PHI_COMMAND_COUNT + extensions + resources_command_count();
	for (size_t i = 0; i < total; i++) {
		const struct phi_command *command = i < PHI_COMMAND_COUNT ? &phi_commands[i]
			: i < PHI_COMMAND_COUNT + extensions
			? extensions_command_at(i - PHI_COMMAND_COUNT)
			: resources_command_at(i - PHI_COMMAND_COUNT - extensions);
		if (!command) {
			continue;
		}
		const char *source = i < PHI_COMMAND_COUNT  ? "builtin"
			: i < PHI_COMMAND_COUNT + extensions    ? "extension"
			: !strncmp(command->name, "/skill:", 7) ? "skill"
													: "prompt";
		json_object *item = json_object_new_object();
		json_object_object_add(item, "source", json_object_new_string(source));
		json_object_object_add(item, "name", json_object_new_string(command->name));
		json_object_object_add(item, "description", json_object_new_string(command->description));
		json_object_object_add(item, "takes_arguments", json_object_new_boolean(command->argument));
		json_object_array_add(list, item);
	}
	json_object_object_add(answer, "commands", list);

	return answer;
}

static json_object *set_active_tools(json_object *request) {

	json_object *wanted = json_object_object_get(request, "tools");
	if (!wanted) {
		tools_set_active(NULL);
		return ok();
	} /* null: every tool */
	if (!json_object_is_type(wanted, json_type_array)) {
		return error("tools must be an array of names or null");
	}
	/* Unknown names are ignored, as in Pi. */
	json_object *all = tools_all_names(), *active = json_object_new_array();
	for (size_t i = 0; i < json_object_array_length(wanted); i++) {
		const char *name = json_object_get_string(json_object_array_get_idx(wanted, i));
		for (size_t j = 0; name && j < json_object_array_length(all); j++) {
			if (!strcmp(name, json_object_get_string(json_object_array_get_idx(all, j)))) {
				json_object_array_add(active, json_object_new_string(name));
				break;
			}
		}
	}
	json_object_put(all);
	tools_set_active(active);
	json_object_put(active);

	return ok();
}

static json_object *names_answer(json_object *names) {

	json_object *answer = ok();
	json_object_object_add(answer, "tools", names ? names : tools_all_names());

	return answer;
}

static json_object *idle_only(const char *method) {

	char message[160];
	snprintf(message, sizeof(message), "%s is only available while no prompt is running", method);

	return error(message);
}

json_object *host_call(const char *extension, const char *method, json_object *request) {

	if (!strcmp(method, "register_flag")) {
		return register_flag(request);
	}
	if (!strcmp(method, "get_flag")) {
		return get_flag(request);
	}
	if (!agent) {
		return error("Session control is unavailable");
	}
	if (!strcmp(method, "send_user_message")) {
		return send_user_message(request);
	}
	if (!strcmp(method, "send_message")) {
		return send_message(extension, request);
	}
	if (!strcmp(method, "append_entry")) {
		return append_entry(extension, request);
	}
	if (!strcmp(method, "get_custom_entries")) {
		return get_custom_entries(request);
	}
	if (!strcmp(method, "get_session")) {
		return get_session();
	}
	if (!strcmp(method, "get_branch")) {
		json_object *answer = ok();
		json_object_object_add(answer, "items", json_object_get(agent->history));
		/* The reply still arriving (not in items until it completes). */
		json_object *call = agent_streaming_call();
		if (*agent_streaming_text() || call) {
			json_object *streaming = json_object_new_object();
			json_object_object_add(
				streaming, "text", json_object_new_string(agent_streaming_text()));
			if (call) {
				json_object_object_add(streaming, "tool", json_object_get(call));
			}
			json_object_object_add(answer, "streaming", streaming);
		}
		return answer;
	}
	if (!strcmp(method, "get_system_prompt")) {
		char *text = instructions_load();
		json_object *answer = ok();
		json_object_object_add(answer, "text", json_object_new_string(text ? text : ""));
		free(text);
		return answer;
	}
	if (!strcmp(method, "set_session_name")) {
		const char *name = text_field(request, "name");
		if (!name || !*name || strchr(name, '\n') || strlen(name) > 200) {
			return error("name must be one line of 1-200 bytes");
		}
		ensure_transcript();
		transcript_note(&agent->transcript, "name", name);
		events_notify("session_info_changed", "name", name);
		return ok();
	}
	if (!strcmp(method, "set_label")) {
		int64_t id = json_object_get_int64(json_object_object_get(request, "entry_id"));
		const char *label = text_field(request, "label");
		int bookmarked = json_object_get_boolean(json_object_object_get(request, "bookmarked"));
		if (id <= 0 || !label) {
			return error("entry_id and label are required");
		}
		return transcript_annotate(&agent->transcript, (uint64_t)id, label, bookmarked)
			? ok()
			: error("Unknown entry or label too long");
	}
	if (!strcmp(method, "get_commands")) {
		return get_commands();
	}
	if (!strcmp(method, "has_pending_messages")) {
		json_object *answer = ok();
		size_t count = host_pending_count();
		json_object_object_add(answer, "pending", json_object_new_boolean(count > 0));
		json_object_object_add(answer, "count", json_object_new_int64((int64_t)count));
		return answer;
	}
	if (!strcmp(method, "set_model")) {
		const char *model = text_field(request, "model");
		if (!model || !settings_model_valid(model)) {
			return error("Invalid model name");
		}
		char command[160];
		snprintf(command, sizeof(command), "/model %s", model);
		return run_command(command);
	}
	if (!strcmp(method, "set_thinking_level")) {
		const char *level = text_field(request, "level");
		if (!level || !settings_effort(level)) {
			return error("level must be off, minimal, low, medium, high, xhigh, max or ultra");
		}
		char command[64];
		snprintf(command, sizeof(command), "/effort %s", level);
		return run_command(command);
	}
	if (!strcmp(method, "get_all_tools")) {
		/* details:true gives Pi's ToolInfo: description, parameters, annotations, ... */
		if (json_object_get_boolean(json_object_object_get(request, "details"))) {
			return names_answer(tools_info(0));
		}
		return names_answer(tools_all_names());
	}
	if (!strcmp(method, "get_callable_tools")) {
		return names_answer(tools_info(1));
	}
	if (!strcmp(method, "execute_tool")) {
		if (!agent) {
			return error("No session");
		}
		return agent_execute_nested(
			agent, field(request, "name"), json_object_object_get(request, "arguments"));
	}
	if (!strcmp(method, "get_settings")) {
		/* The settings in effect: Phi's saved defaults as changed this session. */
		json_object *answer = ok();
		if (settings) {
			json_object_object_add(answer, "model", json_object_new_string(settings->model));
			json_object_object_add(answer, "effort", json_object_new_string(settings->effort));
			json_object_object_add(answer, "approval",
				json_object_new_string(settings_approval_name(settings->approval)));
			json_object_object_add(answer, "sandbox",
				json_object_new_string(settings_sandbox_name(settings->sandbox)));
		}
		return answer;
	}
	if (!strcmp(method, "get_active_tools")) {
		return names_answer(tools_active_names());
	}
	if (!strcmp(method, "set_active_tools")) {
		return set_active_tools(request);
	}
	if (!strcmp(method, "abort")) {
		if (busy) {
			terminal_request_cancel();
		}
		return ok();
	}
	if (!strcmp(method, "shutdown")) {
		shutdown_requested = 1;
		if (busy) {
			terminal_request_cancel();
		}
		return ok();
	}
	if (!strcmp(method, "reload")) {
		reload_requested = 1;
		return ok();
	}

	/* Methods that replace or reshape the session: idle only. */
	int reshape = !strcmp(method, "compact") || !strcmp(method, "new_session") ||
		!strcmp(method, "switch_session") || !strcmp(method, "navigate_tree") ||
		!strcmp(method, "fork");
	if (reshape && busy) {
		return idle_only(method);
	}
	char message[256];
	if (!strcmp(method, "compact")) {
		const char *focus = text_field(request, "focus");
		return agent_compact(agent, focus ? focus : "", message, sizeof(message)) ? ok()
																				  : error(message);
	}
	if (!strcmp(method, "new_session")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "reason", json_object_new_string("new"));
		json_object_object_add(payload, "target_file", json_object_new_string(""));
		int allowed = events_allowed("session_before_switch", payload);
		if (allowed) {
			agent_clear(agent);
			terminal_clear();
		}
		json_object *answer = ok();
		json_object_object_add(answer, "cancelled", json_object_new_boolean(!allowed));
		return answer;
	}
	if (!strcmp(method, "switch_session")) {
		const char *file = text_field(request, "file");
		if (!file || !*file) {
			return error("file is required");
		}
		return terminal_resume_path(agent, file) ? ok() : error("Could not open that session");
	}
	if (!strcmp(method, "navigate_tree") || !strcmp(method, "fork")) {
		int64_t id = json_object_get_int64(json_object_object_get(request, "entry_id"));
		if (id <= 0) {
			return error("entry_id is required");
		}
		int done = !strcmp(method, "fork")
			? terminal_fork_at(agent, (uint64_t)id, message, sizeof(message))
			: terminal_navigate(agent, (uint64_t)id, message, sizeof(message));
		return done ? ok() : error(message);
	}
	char unknown[128];
	snprintf(unknown, sizeof(unknown), "Unknown method: %.64s", method);

	return error(unknown);
}
