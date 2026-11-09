#define _POSIX_C_SOURCE 200809L
#include "agent.h"
#include "sessions.h"
#include "compaction.h"
#include "context.h"
#include "events.h"
#include "instructions.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "history.h"
#include "json.h"
#include "../tools.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* At most this many extension-requested continuations per prompt, so an
   unconditional turn_end/agent_before_settle handler cannot loop forever. */
#define MAX_CONTINUATIONS 8

/* Legacy observe-only hooks (register_hook): turn_start/turn_end/session_load. */
static void agent_event(struct agent *agent, const char *name, const char *detail) {

	if (!agent->host.event) {
		return;
	}
	json_object *payload = json_object_new_object();
	if (!payload) {
		return;
	}
	json_object_object_add(payload, "detail", json_object_new_string(detail));
	agent->host.event(name, payload);
	json_object_put(payload);
}

static void session_event(const char *event, const char *reason, const char *file) {

	if (!events_wanted(event)) {
		return;
	}
	json_object *payload = json_object_new_object();
	if (!payload) {
		return;
	}
	json_object_object_add(payload, "reason", json_object_new_string(reason));
	json_object_object_add(payload, "session_file", json_object_new_string(file ? file : ""));
	events_emit(event, payload, NULL, NULL);
	json_object_put(payload);
}

/* The UI can change effort mid-turn (Shift+Tab); it applies to the next request. */
static const char *current_effort(struct agent *agent) {

	return agent->host.effort ? agent->host.effort() : agent->effort;
}

/* Virtual models (Pi's registerVirtualModel): the selected model may be a
   router an extension registered. Before each request, model_route asks
   its owner which real model and effort to send it to. The last choice is
   "previous" for the next request (to keep prompt caches), and a router's
   state is kept for the conversation in memory (persist with set_state). */
static char routed_model[96], routed_effort[24];
static json_object *route_memory;

struct route_choice {
	char model[96], effort[24];
	int chosen;
};

static int route_apply(
	json_object *payload, json_object *result, const char *extension, void *opaque) {

	(void)payload;
	(void)extension;
	struct route_choice *choice = opaque;
	const char *model = field(result, "model"), *effort = field(result, "effort");
	size_t length = strlen(model);
	if (!length || length >= sizeof(choice->model) || strlen(effort) >= sizeof(choice->effort)) {
		return 0;
	}
	snprintf(choice->model, sizeof(choice->model), "%s", model);
	snprintf(choice->effort, sizeof(choice->effort), "%s", effort);
	choice->chosen = 1;
	json_object *state = NULL;
	if (json_object_object_get_ex(result, "state", &state)) {
		if (route_memory) {
			json_object_put(route_memory);
		}
		route_memory = state ? json_object_get(state) : NULL;
	}

	return 1;
}

static void route_forget(void) {

	routed_model[0] = routed_effort[0] = 0;
	if (route_memory) {
		json_object_put(route_memory);
	}
	route_memory = NULL;
}

/* The model for this request: agent->model, or a router's choice
   (reason "user", "continuation" or "direct"); *effort may change too. */
static const char *route(struct agent *agent, const char *reason, const char **effort) {

	if (!events_wanted("model_route")) {
		return agent->model;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "model", json_object_new_string(agent->model));
	json_object_object_add(payload, "reason", json_object_new_string(reason));
	json_object_object_add(payload, "effort", json_object_new_string(*effort));
	if (*routed_model && strcmp(reason, "direct")) {
		json_object *previous = json_object_new_object();
		json_object_object_add(previous, "model", json_object_new_string(routed_model));
		json_object_object_add(previous, "effort", json_object_new_string(routed_effort));
		json_object_object_add(payload, "previous", previous);
	}
	if (route_memory && strcmp(reason, "direct")) {
		json_object_object_add(payload, "state", json_object_get(route_memory));
	}
	static struct route_choice choice;
	choice = (struct route_choice){0};
	json_object *kept = route_memory ? json_object_get(route_memory) : NULL;
	events_emit("model_route", payload, route_apply, &choice);
	json_object_put(payload);
	if (!strcmp(reason, "direct")) { /* direct requests have no state */
		if (route_memory) {
			json_object_put(route_memory);
		}
		route_memory = kept;
	} else if (kept) {
		json_object_put(kept);
	}
	if (!choice.chosen) {
		return agent->model;
	}
	if (*choice.effort) {
		*effort = choice.effort;
	}
	if (strcmp(reason, "direct")) {
		snprintf(routed_model, sizeof(routed_model), "%s", choice.model);
		snprintf(routed_effort, sizeof(routed_effort), "%s", *effort);
	}
	char note[160];
	snprintf(note, sizeof(note), "%s -> %s %s (%s)", agent->model, choice.model, *effort, reason);
	transcript_note(&agent->transcript, "model_route", note);

	return choice.model;
}

void agent_note_selection(struct agent *agent, const char *source) {

	const char *effort = current_effort(agent);
	const char *pairs[2][4] = {
		{"model_select", "model", agent->model, agent->announced_model},
		{"thinking_level_select", "level", effort, agent->announced_effort},
	};
	size_t sizes[2] = {sizeof(agent->announced_model), sizeof(agent->announced_effort)};
	for (int i = 0; i < 2; i++) {
		char *announced = (char *)pairs[i][3];
		if (!strcmp(announced, pairs[i][2])) {
			continue;
		}
		if (events_wanted(pairs[i][0])) {
			json_object *payload = json_object_new_object();
			json_object_object_add(payload, pairs[i][1], json_object_new_string(pairs[i][2]));
			json_object_object_add(payload, "previous", json_object_new_string(announced));
			json_object_object_add(payload, "source", json_object_new_string(source));
			events_emit(pairs[i][0], payload, NULL, NULL);
			json_object_put(payload);
		}
		snprintf(announced, sizes[i], "%s", pairs[i][2]);
	}
}

int agent_open(struct agent *agent, const char *model, const char *effort) {

	*agent = (struct agent){.model = model, .effort = effort};
	snprintf(agent->announced_model, sizeof(agent->announced_model), "%s", model);
	snprintf(agent->announced_effort, sizeof(agent->announced_effort), "%s", effort);
	agent->client = codex_open();
	if (!agent->client) {
		return 0;
	}
	agent->history = json_object_new_array();
	if (!agent->history) {
		agent_close(agent);
		return 0;
	}

	return 1;
}

void agent_close(struct agent *agent) {

	route_forget();
	transcript_close(&agent->transcript);
	if (agent->history) {
		json_object_put(agent->history);
	}
	codex_close(agent->client);
	*agent = (struct agent){0};
}

void agent_clear(struct agent *agent) {

	session_event("session_shutdown", "new", agent->transcript.path);
	transcript_note(&agent->transcript, "clear", "new conversation");
	transcript_close(&agent->transcript);
	json_object_array_del_idx(agent->history, 0, json_object_array_length(agent->history));
	route_forget();
	if (agent->host.transcript_changed) {
		agent->host.transcript_changed("", 0);
	}
	agent_event(agent, "session_load", "new session");
	session_event("session_start", "new", "");
}

int agent_resume(struct agent *agent, struct session_entry *entry, char *error, size_t size) {

	if (transcript_temporary()) {
		snprintf(error, size, "Temporary chats cannot resume saved sessions. Start Phi without --temp to resume.");
		return 0;
	}
	route_forget();
	struct session_tree tree = {0};
	uint64_t source_head = 0;
	json_object *history = NULL;
	if (entry->version == 2) {
		if (!session_tree_index(entry, &tree, error, size)) {
			return 0;
		}
		source_head = tree.head;
		history = session_tree_history(&tree, source_head, error, size);
	} else {
		history = session_load_ex(entry, &source_head, error, size);
	}
	if (!history) {
		session_tree_free(&tree);
		return 0;
	}

	int previous = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (previous < 0 || chdir(entry->cwd)) {
		snprintf(error, size, "Cannot enter saved directory: %s", strerror(errno));
		if (previous >= 0) {
			close(previous);
		}
		json_object_put(history);
		session_tree_free(&tree);
		return 0;
	}
	struct transcript next = {0};
	const char *effort = current_effort(agent);
	if (entry->version == 2) {
		/* Resume in place: the file is an append-only tree, so the session
           continues in the same file. Only /fork and /clone make copies. */
		uint64_t next_id = tree.count ? tree.nodes[tree.count - 1].id + 1 : 1;
		if (!transcript_reopen(
				&next, entry->path, source_head, next_id, json_object_array_length(history)) &&
			!transcript_disabled()) {
			snprintf(error, size, "Cannot open %s for writing: %s", entry->path, strerror(errno));
			int restored = !fchdir(previous);
			close(previous);
			json_object_put(history);
			session_tree_free(&tree);
			if (!restored) {
				snprintf(
					error, size, "Cannot reopen the session or restore the working directory.");
			}
			return 0;
		}
		transcript_note(&next, "resumed", effort);
		goto loaded;
	}
	/* A version-1 (linear) transcript cannot take tree records, so it is
       converted once into a new version-2 file. */
	transcript_begin(&next, agent->model, effort);
	transcript_note(&next, "resumed_from", entry->path);
	transcript_link(&next, "resume", entry->path, source_head);
	if (entry->named) {
		transcript_note(&next, "name", entry->title);
	}

	int saved = next.file && !next.failed;
	if (saved && entry->version == 2) {
		for (size_t i = 0; i < tree.count && saved; i++) {
			const struct session_node *node = tree.nodes + i;
			saved = transcript_import_node(&next,
				node->kind == SESSION_COMPACTION ? "compaction" : "item", node->data, node->id,
				node->parent);
			if (saved && (*node->label || node->bookmarked)) {
				saved = transcript_annotate(&next, node->id, node->label, node->bookmarked);
			}
		}
		if (saved) {
			saved = transcript_select(&next, source_head, json_object_array_length(history));
		}
	} else if (saved) {
		transcript_sync(&next, history);
		saved = !next.failed && next.saved == json_object_array_length(history);
	}
	if (!saved || next.failed || !next.file) {
		transcript_close(&next);
		int restored = !fchdir(previous);
		close(previous);
		json_object_put(history);
		session_tree_free(&tree);
		snprintf(error, size,
			restored ? "Could not save continuation; conversation unchanged."
					 : "Could not save continuation or restore working directory.");
		return 0;
	}
loaded:
	close(previous);
	session_tree_free(&tree);
	session_event("session_shutdown", "resume", agent->transcript.path);
	transcript_close(&agent->transcript);
	agent->transcript = next;
	json_object_put(agent->history);
	agent->history = history;
	if (agent->host.transcript_changed) {
		agent->host.transcript_changed(agent->transcript.path, 0);
	}
	agent_event(agent, "session_load", agent->transcript.path);
	session_event("session_start", "resume", agent->transcript.path);

	return 1;
}

int agent_navigate(
	struct agent *agent, json_object *history, uint64_t head, char *error, size_t size) {

	route_forget();
	if (!history) {
		snprintf(error, size, "No branch history to select.");
		return 0;
	}
	size_t saved = json_object_array_length(history);
	if (!transcript_select(&agent->transcript, head, saved)) {
		snprintf(error, size, "Could not save the selected tree position.");
		return 0;
	}
	json_object_put(agent->history);
	agent->history = history; /* takes ownership */
	agent_event(agent, "session_load", agent->transcript.path);

	return 1;
}

int agent_branch(struct agent *agent, json_object *history, const char *kind, uint64_t source_entry,
	char *error, size_t size) {

	if (!history || !agent->transcript.file || agent->transcript.failed ||
		!*agent->transcript.path) {
		snprintf(error, size, "Current session is not available for branching.");
		return 0;
	}
	char parent[sizeof(agent->transcript.path)];
	snprintf(parent, sizeof(parent), "%s", agent->transcript.path);
	struct transcript next = {0};
	const char *effort = current_effort(agent);
	transcript_begin(&next, agent->model, effort);
	transcript_link(&next, kind, parent, source_entry);
	transcript_sync(&next, history);
	if (next.failed || !next.file || next.saved != json_object_array_length(history)) {
		transcript_close(&next);
		snprintf(error, size, "Could not save the new branch; conversation unchanged.");
		return 0;
	}
	session_event("session_shutdown", kind, parent);
	transcript_close(&agent->transcript);
	agent->transcript = next;
	json_object_put(agent->history);
	agent->history = history; /* takes ownership */
	if (agent->host.transcript_changed) {
		agent->host.transcript_changed(agent->transcript.path, 1);
	}
	agent_event(agent, "session_load", agent->transcript.path);
	session_event("session_start", kind, agent->transcript.path);

	return 1;
}

static int cancelled(struct agent *agent) {

	return agent->host.response.cancel && agent->host.response.cancel();
}

struct compact_state {
	int cancel;
	char *summary;
};

static int compact_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	(void)source;
	struct compact_state *compact = state;
	if (json_object_get_boolean(json_object_object_get(result, "cancel"))) {
		compact->cancel = 1;
		return 1;
	}
	json_object *summary = json_object_object_get(result, "summary");
	if (json_object_is_type(summary, json_type_string) && *json_object_get_string(summary)) {
		compact->summary = strdup(json_object_get_string(summary));
		return 1; /* the first handler to supply a summary wins */
	}

	return 0;
}

/* The checkpoint text now at the start of history, for session_compact. */
static const char *checkpoint_text(json_object *history) {

	return field(json_object_array_get_idx(history, 0), "content");
}

static int compact(
	struct agent *agent, const char *focus, const char *reason, char *error, size_t size) {

	struct response_sink sink = agent->host.response;
	/* The summary is internal context, not an assistant reply or executable intent. */
	sink.text = NULL;
	sink.observe_text = NULL;
	sink.tool_pending = NULL;
	sink.usage = NULL;
	const char *effort = current_effort(agent);
	int64_t tokens_before = compaction_estimate(agent->history);
	struct compact_state state = {0};
	if (events_wanted("session_before_compact")) {
		size_t cut = compaction_boundary(agent->history);
		if (!cut) {
			snprintf(error, size, "Not enough history to compact.");
			return 0;
		}
		json_object *payload = json_object_new_object(), *messages = json_object_new_array();
		for (size_t i = 0; i < cut; i++) {
			json_object_array_add(
				messages, json_object_get(json_object_array_get_idx(agent->history, i)));
		}
		json_object_object_add(payload, "reason", json_object_new_string(reason));
		json_object_object_add(payload, "focus", json_object_new_string(focus ? focus : ""));
		json_object_object_add(payload, "tokens_before", json_object_new_int64(tokens_before));
		json_object_object_add(payload, "messages", messages);
		json_object_object_add(payload, "kept",
			json_object_new_int64((int64_t)(json_object_array_length(agent->history) - cut)));
		events_emit("session_before_compact", payload, compact_apply, &state);
		json_object_put(payload);
		if (state.cancel) {
			free(state.summary);
			snprintf(error, size, "Compaction cancelled by an extension.");
			return 0;
		}
		if (state.summary && !compaction_replace(&agent->history, cut, state.summary)) {
			free(state.summary);
			snprintf(error, size, "Out of memory.");
			return 0;
		}
	}
	const char *model = state.summary ? agent->model : route(agent, "direct", &effort);
	int ok = state.summary ? 1
						   : compaction_run(agent->client, &agent->history, model, effort,
								 focus ? focus : "", sink, error, size);
	if (ok) {
		transcript_note(&agent->transcript, "compaction_begin", "context checkpoint");
		transcript_compact(&agent->transcript, agent->history);
		transcript_note(&agent->transcript, "compaction_end", "completed");
		if (events_wanted("session_compact")) {
			json_object *payload = json_object_new_object();
			json_object_object_add(
				payload, "summary", json_object_new_string(checkpoint_text(agent->history)));
			json_object_object_add(
				payload, "from_extension", json_object_new_boolean(state.summary != NULL));
			json_object_object_add(payload, "tokens_before", json_object_new_int64(tokens_before));
			events_emit("session_compact", payload, NULL, NULL);
			json_object_put(payload);
		}
	} else if (!cancelled(agent)) {
		events_notify("session_compact_failed", "error", error);
	}
	free(state.summary);

	return ok;
}

int agent_compact(struct agent *agent, const char *focus, char *error, size_t size) {

	return compact(agent, focus, "manual", error, size);
}

static int should_compact(struct agent *agent) {

	/* A virtual model's window is that of the model it last routed to. */
	int64_t window = context_window(*routed_model ? routed_model : agent->model);

	return window > PHI_COMPACTION_RESERVE &&
		compaction_estimate(agent->history) > window - PHI_COMPACTION_RESERVE;
}

/* Compact when history nears the context window. Returns 0 only when a
   needed compaction failed or was cancelled. */
static int compact_if_needed(struct agent *agent) {

	if (!should_compact(agent)) {
		return 1;
	}
	char error[256];
	if (compact(agent, "", "auto", error, sizeof(error))) {
		tool_report(stdout, "\nContext compacted.\n");
		return 1;
	}
	if (!cancelled(agent)) {
		tool_report(stderr, "\n%s\n", error);
	}

	return 0;
}

static json_object *error_result(const char *message) {

	json_object *answer = json_object_new_object();
	if (answer) {
		json_object_object_add(answer, "error", json_object_new_string(message));
	}

	return answer;
}

/* Arguments as an object when they parse, else the raw string. */
static json_object *call_arguments(json_object *call) {

	const char *text = field(call, "arguments");
	json_object *parsed = json_tokener_parse(text);
	if (json_object_is_type(parsed, json_type_object)) {
		return parsed;
	}
	if (parsed) {
		json_object_put(parsed);
	}

	return json_object_new_string(text);
}

static json_object *call_payload(json_object *call) {

	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "call_id", json_object_new_string(field(call, "call_id")));
	json_object_object_add(payload, "name", json_object_new_string(field(call, "name")));
	json_object_object_add(payload, "arguments", call_arguments(call));
	/* A call another tool made (execute_tool) names the call that made it. */
	const char *parent = field(call, "_phi_parent_call_id");
	if (*parent) {
		json_object_object_add(payload, "parent_call_id", json_object_new_string(parent));
	}

	return payload;
}

struct tool_call_state {
	int block;
	char reason[384];
	json_object *arguments;
};

static int tool_call_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	struct tool_call_state *call = state;
	if (json_object_get_boolean(json_object_object_get(result, "block"))) {
		call->block = 1;
		const char *reason = field(result, "reason");
		snprintf(call->reason, sizeof(call->reason), "Blocked by extension %s%s%s", source,
			*reason ? ": " : "", reason);
		return 1;
	}
	json_object *arguments = json_object_object_get(result, "arguments");
	if (json_object_is_type(arguments, json_type_object)) {
		json_object_object_add(payload, "arguments", json_object_get(arguments));
		if (call->arguments) {
			json_object_put(call->arguments);
		}
		call->arguments = json_object_get(arguments);
	}

	return 0;
}

/* A copy of call whose arguments are replaced by the JSON text of arguments. */
static json_object *call_with_arguments(json_object *call, json_object *arguments) {

	json_object *copy = json_object_new_object();
	json_object_object_foreach(call, key, value) {
		json_object_object_add(copy, key,
			strcmp(key, "arguments") ? json_object_get(value)
									 : json_object_new_string(json_object_to_json_string_ext(
										   arguments, JSON_C_TO_STRING_PLAIN)));
	}

	return copy;
}

static int tool_result_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	(void)state;
	json_object *replacement = json_object_object_get(result, "result");
	if (json_object_is_type(replacement, json_type_object)) {
		json_object_object_add(payload, "result", json_object_get(replacement));
	}

	return 0;
}

struct running_tool {
	const char *call_id, *name, *parent;
};

static void tool_update(const char *text, void *context) {

	struct running_tool *tool = context;
	if (!*text || !events_wanted("tool_execution_update")) {
		return;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "call_id", json_object_new_string(tool->call_id));
	json_object_object_add(payload, "name", json_object_new_string(tool->name));
	if (*tool->parent) {
		json_object_object_add(payload, "parent_call_id", json_object_new_string(tool->parent));
	}
	json_object_object_add(payload, "text", json_object_new_string(text));
	events_emit("tool_execution_update", payload, NULL, NULL);
	json_object_put(payload);
}

static int result_is_error(json_object *answer) {

	return *field(answer, "error") != 0;
}

/* The calls running now, outermost first: a tool that runs another tool
   (execute_tool) is its parent. */
#define MAX_NESTING 8
#define MAX_NESTED_CALLS 256
static const char *running_ids[MAX_NESTING];
static int running_depth, nested_counts[MAX_NESTING];

/* Run the tool with execution events around it. */
static json_object *execute_call(json_object *call) {

	int observed = events_wanted("tool_execution_start") || events_wanted("tool_execution_end") ||
		events_wanted("tool_execution_update");
	struct running_tool running = {
		field(call, "call_id"), field(call, "name"), field(call, "_phi_parent_call_id")};
	void (*outer_hook)(const char *, void *) = NULL;
	void *outer_context = NULL;
	if (observed) {
		json_object *payload = call_payload(call);
		events_emit("tool_execution_start", payload, NULL, NULL);
		json_object_put(payload);
		tools_get_update_hook(&outer_hook, &outer_context);
		tools_set_update_hook(tool_update, &running);
	}
	struct timespec began, ended;
	clock_gettime(CLOCK_MONOTONIC, &began);
	int pushed = running_depth < MAX_NESTING;
	if (pushed) {
		running_ids[running_depth] = running.call_id;
		nested_counts[running_depth++] = 0;
	}
	json_object *answer = tools_execute(call);
	if (pushed) {
		running_depth--;
	}
	clock_gettime(CLOCK_MONOTONIC, &ended);
	if (observed) {
		/* A nested call gives the update hook back to the tool that made it. */
		tools_set_update_hook(outer_hook, outer_context);
		json_object *payload = call_payload(call);
		json_object_object_add(payload, "result", answer ? json_object_get(answer) : NULL);
		json_object_object_add(
			payload, "is_error", json_object_new_boolean(!answer || result_is_error(answer)));
		int64_t elapsed = (int64_t)(ended.tv_sec - began.tv_sec) * 1000 +
			(ended.tv_nsec - began.tv_nsec) / 1000000;
		json_object_object_add(payload, "duration_ms", json_object_new_int64(elapsed));
		events_emit("tool_execution_end", payload, NULL, NULL);
		json_object_put(payload);
	}

	return answer;
}

/* Let tool_result handlers replace the result the model will see. Image
   data is kept out of the payload and carried over to the replacement. */
static json_object *filter_result(
	json_object *call, json_object *actual_call, json_object *answer) {

	if (!events_wanted("tool_result")) {
		return answer;
	}
	/* Handlers see the model-facing result; image data and Phi-private
       keys (_phi_details, _phi_display, ...) are carried over separately. */
	json_object *shown = json_object_new_object();
	json_object_object_foreach(answer, key, value) {
		if (strcmp(key, "image_url") && strncmp(key, "_phi_", 5)) {
			json_object_object_add(shown, key, json_object_get(value));
		}
	}
	json_object *payload = call_payload(actual_call);
	json_object_object_add(payload, "call_id", json_object_new_string(field(call, "call_id")));
	json_object_object_add(payload, "result", shown);
	json_object_object_add(payload, "is_error", json_object_new_boolean(result_is_error(answer)));
	json_object *details = json_object_object_get(answer, "_phi_details");
	if (details) {
		json_object_object_add(payload, "details", json_object_get(details));
	}
	events_emit("tool_result", payload, tool_result_apply, NULL);
	json_object *final = json_object_object_get(payload, "result");
	if (final != shown) {
		json_object *replacement = json_object_get(final);
		json_object_object_foreach(answer, private_key, private_value) {
			if (!strcmp(private_key, "image_url") || !strncmp(private_key, "_phi_", 5)) {
				json_object_object_add(replacement, private_key, json_object_get(private_value));
			}
		}
		json_object_put(answer);
		answer = replacement;
	}
	json_object_put(payload);

	return answer;
}

/* Interceptors, then tool_call handlers, which may block the call or
   rewrite its arguments (*effective, owned, when rewritten). A failing
   handler blocks it, since a guard that crashed must not pass. Returns 0
   if an interceptor refused it (reason in interception). */
static int gate_call(struct agent *agent, json_object *call, json_object **effective,
	char *interception, size_t size, struct tool_call_state *gate) {

	int allowed = 1;
	if (!cancelled(agent) && agent->host.tool_intercept) {
		allowed = agent->host.tool_intercept(call, effective, interception, size);
	}
	json_object *actual = *effective ? *effective : call;
	if (allowed && !cancelled(agent) && events_wanted("tool_call")) {
		json_object *payload = call_payload(actual);
		if (events_emit("tool_call", payload, tool_call_apply, gate) && !gate->block) {
			gate->block = 1;
			snprintf(gate->reason, sizeof(gate->reason),
				"Blocked: an extension tool_call handler failed");
		}
		json_object_put(payload);
		if (!gate->block && gate->arguments) {
			json_object *rewritten = call_with_arguments(actual, gate->arguments);
			if (*effective) {
				json_object_put(*effective);
			}
			*effective = rewritten;
		}
	}
	if (gate->arguments) {
		json_object_put(gate->arguments);
		gate->arguments = NULL;
	}

	return allowed;
}

/* A tool running another tool (execute_tool). The same checks,
   interceptors, tool_call and tool_result handlers and execution events
   as a model-requested call, with parent_call_id; but no approval prompt
   (the caller is trusted native code, as with exec; the sandbox still
   applies) and no transcript entry: only the calling tool gets the result.
   Returns {result, is_error, details?} or {error}. */
json_object *agent_execute_nested(struct agent *agent, const char *name, json_object *arguments) {

	json_object *answer = json_object_new_object();
	if (!running_depth) {
		json_object_object_add(answer, "error",
			json_object_new_string("execute_tool only works while a tool is running"));
		return answer;
	}
	const char *refusal = running_depth >= MAX_NESTING ? "Tools are nested too deeply"
		: nested_counts[running_depth - 1] >= MAX_NESTED_CALLS
		? "At most 256 nested calls per tool call"
		: !name || !*name             ? "execute_tool needs a tool name"
		: !strcmp(name, "extensions") ? "The extensions tool cannot be called from another tool"
		: arguments && !json_object_is_type(arguments, json_type_object)
		? "arguments must be a JSON object"
		: NULL;
	if (refusal) {
		json_object_object_add(answer, "error", json_object_new_string(refusal));
		return answer;
	}
	json_object_put(answer);

	const char *parent = running_ids[running_depth - 1];
	char id[512];
	snprintf(id, sizeof(id), "%s/%d", parent, ++nested_counts[running_depth - 1]);
	json_object *call = json_object_new_object();
	json_object_object_add(call, "type", json_object_new_string("function_call"));
	json_object_object_add(call, "call_id", json_object_new_string(id));
	json_object_object_add(call, "name", json_object_new_string(name));
	json_object_object_add(call, "arguments",
		json_object_new_string(
			arguments ? json_object_to_json_string_ext(arguments, JSON_C_TO_STRING_PLAIN) : "{}"));
	json_object_object_add(call, "_phi_parent_call_id", json_object_new_string(parent));

	json_object *effective = NULL;
	char interception[512] = "";
	struct tool_call_state gate = {0};
	int allowed = gate_call(agent, call, &effective, interception, sizeof(interception), &gate);
	json_object *actual = effective ? effective : call;
	json_object *result;
	if (!allowed || gate.block) {
		result = error_result(gate.block ? gate.reason
				: *interception          ? interception
										 : "Tool blocked by an extension");
	} else if (cancelled(agent)) {
		result = error_result("Cancelled before execution");
	} else {
		result = filter_result(call, actual, execute_call(actual));
	}
	if (!result) {
		result = error_result("Cannot allocate tool result");
	}

	/* The caller sees what the model would: no image data or private keys. */
	answer = json_object_new_object();
	json_object *shown = json_object_new_object();
	json_object_object_foreach(result, key, value) {
		if (strcmp(key, "image_url") && strncmp(key, "_phi_", 5)) {
			json_object_object_add(shown, key, json_object_get(value));
		}
	}
	json_object_object_add(answer, "result", shown);
	json_object_object_add(answer, "is_error", json_object_new_boolean(result_is_error(result)));
	json_object *details = json_object_object_get(result, "_phi_details");
	if (details) {
		json_object_object_add(answer, "details", json_object_get(details));
	}
	json_object_put(result);
	if (effective) {
		json_object_put(effective);
	}
	json_object_put(call);

	return answer;
}

/* Run (or refuse) one model-requested call and record its result. *steer is
   queued user input that supersedes calls not yet started. Returns 1 if the
   tool asked to end the run after this batch ("terminate"). */
static int run_call(struct agent *agent, json_object *call, char **steer) {

	/* Input submitted while the model was responding can supersede this call
       before any side effect begins. */
	if (!*steer && !cancelled(agent) && agent->host.steer) {
		*steer = agent->host.steer();
	}

	json_object *answer, *effective_call = NULL;
	char interception[512] = "";
	int interception_allowed = 1;
	struct tool_call_state gate = {0};
	if (!*steer) {
		interception_allowed =
			gate_call(agent, call, &effective_call, interception, sizeof(interception), &gate);
	}
	json_object *actual_call = effective_call ? effective_call : call;

	if (*steer) {
		answer = error_result("Superseded by user steering before execution");
	} else if (!interception_allowed || gate.block) {
		answer = error_result(gate.block ? gate.reason
				: *interception          ? interception
										 : "Tool blocked by an extension");
		if (answer && agent->host.tool_refused) {
			agent->host.tool_refused(actual_call, answer);
		}
	} else {
		int permitted = !agent->host.tool_begin || agent->host.tool_begin(actual_call);
		if (!permitted) {
			answer = error_result("Tool denied by user");
		} else if (cancelled(agent)) {
			answer = error_result("Cancelled before execution");
		} else {
			answer = execute_call(actual_call);
		}
		if (answer && *interception) {
			json_object_object_add(
				answer, "_phi_interception", json_object_new_string(interception));
		}
		if (answer && permitted) {
			answer = filter_result(call, actual_call, answer);
		}
		if (permitted && agent->host.tool_end) {
			agent->host.tool_end(answer);
		}
	}
	if (!answer) {
		answer = error_result("Cannot allocate tool result");
	}
	int terminate =
		answer && json_object_get_boolean(json_object_object_get(answer, "_phi_terminate"));
	history_add_tool(agent->history, call, answer);
	transcript_sync(&agent->transcript, agent->history);
	if (answer) {
		json_object_put(answer);
	}
	if (effective_call) {
		json_object_put(effective_call);
	}

	/* Never interrupt a running tool. Once it has completed, queued user
       input supersedes sibling calls chosen by the old request. */
	if (!*steer && !cancelled(agent) && agent->host.steer) {
		*steer = agent->host.steer();
	}

	return terminate;
}

/* Adds a user message plus any extension turn messages, persisting each. */
static void add_user_message(struct agent *agent, const char *message) {

	history_add_user(agent->history, message);
	json_object *origin = agent->host.user_origin ? agent->host.user_origin() : NULL;
	if (origin) {
		json_object *item =
			json_object_array_get_idx(agent->history, json_object_array_length(agent->history) - 1);
		json_object_object_add(item, "_phi_origin", origin);
	}
	transcript_sync(&agent->transcript, agent->history);
	/* Persist the visible user input before running trusted native callbacks.
       A crashing/hanging extension must not erase an already accepted prompt. */
	if (agent->host.turn_messages) {
		agent->host.turn_messages(agent->history, message);
		transcript_sync(&agent->transcript, agent->history);
	}
}

/* Hidden context an extension added (before_agent_start or a continuation). */
static void add_hidden_message(
	struct agent *agent, const char *text, const char *source, const char *type) {

	if (history_add_hidden_user(agent->history, text, source, type)) {
		transcript_sync(&agent->transcript, agent->history);
	}
}

struct start_state {
	struct agent *agent;
	char *replace;
	char *append;
};

static int start_apply(json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	struct start_state *start = state;
	const char *message = field(result, "message");
	if (*message) {
		add_hidden_message(start->agent, message, source, "before_agent_start");
	}
	const char *replace = field(result, "system_prompt");
	if (*replace) {
		free(start->replace);
		start->replace = strdup(replace);
	}
	const char *append = field(result, "system_prompt_append");
	if (*append) {
		size_t old = start->append ? strlen(start->append) : 0, n = strlen(append);
		char *joined = realloc(start->append, old + n + 2);
		if (joined) {
			if (old) {
				joined[old++] = '\n';
			}
			memcpy(joined + old, append, n + 1);
			start->append = joined;
		}
	}

	return 0;
}

struct continue_state {
	struct agent *agent;
	int requested;
};

static int continue_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	struct continue_state *next = state;
	if (!json_object_get_boolean(json_object_object_get(result, "continue"))) {
		return 0;
	}
	const char *message = field(result, "message");
	if (*message) {
		add_hidden_message(next->agent, message, source, "continue");
	}
	next->requested = 1;

	return 0;
}

/* Emit turn_end or agent_before_settle; returns 1 if a handler asked to continue. */
static int continue_requested(struct agent *agent, const char *event, json_object *payload) {

	struct continue_state next = {agent, 0};
	if (events_wanted(event)) {
		events_emit(event, payload, continue_apply, &next);
	}
	json_object_put(payload);

	return next.requested;
}

/* The response being received: its text so far and the tool call being
   written, for get_branch's "streaming" (a /btw question can then see what
   the agent is in the middle of). Cleared when it enters history. */
static char *partial_text;
static size_t partial_used, partial_capacity;
static json_object *partial_call;
static void (*partial_pending_next)(json_object *call);

static void partial_clear(void) {

	partial_used = 0;
	if (partial_text) {
		partial_text[0] = 0;
	}
	if (partial_call) {
		json_object_put(partial_call);
		partial_call = NULL;
	}
}

static void partial_append(const char *text) {

	size_t n = strlen(text);
	if (partial_used + n + 1 > partial_capacity) {
		size_t next = (partial_used + n + 1) * 2;
		char *grown = realloc(partial_text, next);
		if (!grown) {
			return;
		}
		partial_text = grown;
		partial_capacity = next;
	}
	memcpy(partial_text + partial_used, text, n + 1);
	partial_used += n;
}

static void partial_pending(json_object *call) {

	if (partial_call) {
		json_object_put(partial_call);
	}
	partial_call = NULL;
	if (call) {
		json_object_deep_copy(call, &partial_call, NULL);
	}
	if (partial_pending_next) {
		partial_pending_next(call);
	}
}

const char *agent_streaming_text(void) {

	return partial_used ? partial_text : "";
}

json_object *agent_streaming_call(void) {

	return partial_call;
}

/* Streamed text goes to the transcript, the partial reply, and, when
   wanted, message events. */
struct text_observer {
	struct agent *agent;
	int started;
};

static void observe_text(const char *text, void *context) {

	struct text_observer *observer = context;
	transcript_text(text, &observer->agent->transcript);
	if (text) {
		partial_append(text);
	}
	if (!events_wanted("message_start") && !events_wanted("message_update")) {
		return;
	}
	if (!observer->started) {
		observer->started = 1;
		events_notify("message_start", "role", "assistant");
	}
	events_notify("message_update", "delta", text);
}

static int message_end_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	(void)state;
	json_object *text = json_object_object_get(result, "text");
	if (!json_object_is_type(text, json_type_string)) {
		return 0;
	}
	json_object *message = json_object_object_get(payload, "message");
	json_object *content = json_object_new_array(), *part = json_object_new_object();
	json_object_object_add(part, "type", json_object_new_string("output_text"));
	json_object_object_add(part, "text", json_object_get(text));
	json_object_object_add(part, "annotations", json_object_new_array());
	json_object_array_add(content, part);
	json_object_object_add(message, "content", content);

	return 0;
}

/* message_end may replace an assistant message's text before it is saved. */
static void finish_messages(json_object *output) {

	if (!events_wanted("message_end")) {
		return;
	}
	for (size_t i = 0; i < json_object_array_length(output); i++) {
		json_object *item = json_object_array_get_idx(output, i);
		if (strcmp(field(item, "role"), "assistant")) {
			continue;
		}
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "message", json_object_get(item));
		events_emit("message_end", payload, message_end_apply, NULL);
		json_object_put(payload);
	}
}

static json_object *status_payload(const char *key, const char *value) {

	json_object *payload = json_object_new_object();
	json_object_object_add(payload, key, json_object_new_string(value));

	return payload;
}

/* One user prompt: request, run requested tools, repeat until the model stops
   asking for tools, no steering input is waiting and no extension asks to
   continue. Pi calls this an agent run; each request is a turn. */
static void run(struct agent *agent, const char *message) {

	const char *prompt = message ? message : "";
	agent_event(agent, "turn_start", prompt);
	agent_note_selection(agent, "shortcut");
	const char *initial_effort = current_effort(agent);
	transcript_begin(&agent->transcript, agent->model, initial_effort);
	if (agent->host.transcript_changed) {
		agent->host.transcript_changed(agent->transcript.path, 1);
	}
	compact_if_needed(agent);
	transcript_note(&agent->transcript, "turn_begin", initial_effort);
	if (message) {
		add_user_message(agent, message);
	}

	struct start_state start = {agent, NULL, NULL};
	if (events_wanted("before_agent_start")) {
		json_object *payload = status_payload("prompt", prompt);
		/* As Pi: "user" for a prompt, "custom" for a run an extension's
           send_message trigger_turn started, with that message. */
		json_object_object_add(
			payload, "message_type", json_object_new_string(message ? "user" : "custom"));
		size_t count = agent->history ? json_object_array_length(agent->history) : 0;
		json_object *last = count ? json_object_array_get_idx(agent->history, count - 1) : NULL;
		if (!message && history_is_hidden(last)) {
			json_object *custom = json_object_new_object();
			json_object_object_add(
				custom, "content", json_object_new_string(field(last, "content")));
			json_object_object_add(
				custom, "custom_type", json_object_new_string(field(last, "_phi_custom_type")));
			json_object_object_add(
				custom, "extension", json_object_new_string(field(last, "_phi_extension")));
			json_object_object_add(payload, "message", custom);
		}
		events_emit("before_agent_start", payload, start_apply, &start);
		json_object_put(payload);
		instructions_set_run(start.replace, start.append);
	}
	json_object *started = status_payload("prompt", prompt);
	if (events_wanted("agent_start")) {
		events_emit("agent_start", started, NULL, NULL);
	}
	json_object_put(started);

	const char *status = "completed";
	int turn = 0, continuations = 0;
	for (;;) {
		agent_note_selection(agent, "shortcut");
		const char *effort = current_effort(agent);
		if (events_wanted("turn_start")) {
			json_object *payload = json_object_new_object();
			json_object_object_add(payload, "turn", json_object_new_int(turn));
			events_emit("turn_start", payload, NULL, NULL);
			json_object_put(payload);
		}
		struct text_observer observer = {agent, 0};
		struct response_sink sink = agent->host.response;
		sink.observe_text = observe_text;
		sink.observer = &observer;
		partial_pending_next = sink.tool_pending;
		sink.tool_pending = partial_pending;
		partial_clear();
		transcript_note(&agent->transcript, "request", effort);
		const char *model = route(agent, !turn && message ? "user" : "continuation", &effort);
		json_object *output = codex_respond(agent->client, agent->history, model, effort, sink);
		partial_clear(); /* in history now, or failed */
		const char *usage = stream_take_usage();
		if (*usage) {
			transcript_note(&agent->transcript, "usage", usage);
		}
		tool_report(stdout, "\n\n");
		/* Failed/incomplete responses never enter history. Earlier successful
           rounds and the user message remain intact. */
		if (!output) {
			status = cancelled(agent) ? "cancelled" : "failed";
		}
		if (!output && !cancelled(agent)) {
			/* Record why, so scripts can tell quota exhaustion from a transient error. */
			char reason[320];
			const char *why = codex_last_error(agent->client);
			snprintf(reason, sizeof(reason), "status=%ld %s", codex_last_status(agent->client),
				why ? why : "unknown");
			transcript_note(&agent->transcript, "turn_error", reason);
		}
		int calls = 0, steered = 0, terminated = 0;
		if (output) {
			finish_messages(output);
			history_add_output(agent->history, output);
			transcript_sync(&agent->transcript, agent->history);
			char *steer = NULL;
			for (size_t i = 0; i < json_object_array_length(output); i++) {
				json_object *call = json_object_array_get_idx(output, i);
				if (strcmp(field(call, "type"), "function_call")) {
					continue;
				}
				calls++;
				terminated += run_call(agent, call, &steer);
			}
			json_object_put(output);
			if (cancelled(agent)) {
				free(steer);
				steer = NULL;
				status = "cancelled";
			}
			/* A text-only reply has no tool boundary, but submitted steering still
               needs a follow-up response rather than waiting for another Enter. */
			if (!cancelled(agent) && !steer && agent->host.steer) {
				steer = agent->host.steer();
			}
			/* Drain everything the user submitted before this boundary. Multiple
               messages remain separate user turns, in the order they were sent. */
			while (steer) {
				add_user_message(agent, steer);
				free(steer);
				steered = 1;
				steer = agent->host.steer ? agent->host.steer() : NULL;
			}
			if (!cancelled(agent) && agent->host.boundary) {
				if (agent->host.boundary(agent->history)) {
					steered = 1;
				}
				transcript_sync(&agent->transcript, agent->history);
			}
		}

		json_object *turn_payload = json_object_new_object();
		json_object_object_add(turn_payload, "turn", json_object_new_int(turn++));
		json_object_object_add(turn_payload, "tool_calls", json_object_new_int(calls));
		json_object_object_add(turn_payload, "status", json_object_new_string(status));
		/* Every tool in the batch asked to terminate: no follow-up request. */
		int more = (calls && terminated < calls) || steered;
		int asked = continue_requested(agent, "turn_end", turn_payload);
		if (strcmp(status, "completed")) {
			break;
		}
		if (!more && asked && continuations < MAX_CONTINUATIONS) {
			continuations++;
			more = 1;
		}
		if (!more &&
			continue_requested(agent, "agent_before_settle", status_payload("status", status)) &&
			continuations < MAX_CONTINUATIONS) {
			continuations++;
			more = 1;
		}
		if (!more) {
			break;
		}
		if (!compact_if_needed(agent)) {
			status = cancelled(agent) ? "cancelled" : "failed";
			break;
		}
	}
	instructions_set_run(NULL, NULL);
	free(start.replace);
	free(start.append);
	transcript_note(&agent->transcript, "turn_end", status);
	agent_event(agent, "turn_end", status);
	events_notify("agent_end", "status", status);
	events_notify("agent_settled", "status", status);
}

void agent_turn(struct agent *agent, const char *message) {

	run(agent, message);
}

void agent_continue(struct agent *agent) {

	run(agent, NULL);
}
