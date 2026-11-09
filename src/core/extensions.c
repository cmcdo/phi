#define _POSIX_C_SOURCE 200809L
#include "extensions.h"
#include "extension_build.h"
#include "../../include/phi_extension.h"
#include "json.h"
#include "history.h"
#include "http.h"
#include "events.h"
#include "fsutil.h"
#include <sys/file.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <curl/curl.h>

#define MAX_EXTENSIONS 32
#define MAX_MEMBERS 32 /* commands and hooks per extension */
#define MAX_TOOLS 128 /* tools per extension (MCP servers can have many) */
#define MAX_SCAN_ENTRIES 1024
#define OUTPUT_SIZE 65536
#define MAX_STATE_KEYS 64
#define MAX_STATE_VALUE 4096
#define MAX_STATE_FILE 65536
#define MAX_STATUS 64
#define MAX_STATUS_SLOTS 4
#define MAX_INTERCEPT_OUTPUT 65536
#define MAX_CONTEXT_FRAGMENT 8192
#define MAX_CONTEXT_TOTAL 32768
#define MAX_TURN_MESSAGE 8192
#define MAX_TURN_MESSAGE_TOTAL 32768
#define MAX_PROVIDERS 8
#define MAX_VIRTUAL_MODELS 8
#define MAX_PROVIDER_WRAPPERS 8
#define MAX_PROVIDER_RESULT (4u * 1024u * 1024u)
#define MAX_PROVIDER_MODELS 64
#define MAX_HANDLERS 64
#define STRUCTURED_OUTPUT (1024u * 1024u)
#define MAX_TOOL_NOTES 16384
#define MAX_BUS_DEPTH 4

/* Lifecycle events an extension can subscribe to with api->on(). Payload and
   result shapes are documented in include/phi_extension.h. */
static const char *const lifecycle_events[] = {"session_start", "session_shutdown",
	"session_info_changed", "session_before_switch", "session_before_fork", "session_before_tree",
	"session_tree", "session_before_compact", "session_compact", "session_compact_failed", "input",
	"user_bash", "before_agent_start", "agent_start", "agent_end", "agent_before_settle",
	"agent_settled", "turn_start", "turn_end", "message_start", "message_update", "message_end",
	"context", "context_with_system", "before_provider_request", "before_provider_headers",
	"after_provider_response", "tool_call", "tool_execution_start", "tool_execution_update",
	"tool_execution_end", "tool_result", "model_select", "thinking_level_select", "ui_prompt_start",
	"ui_prompt_end", "resources_discover", "project_trust", "shortcut", "terminal_input",
	"autocomplete", "external_message_admission", "external_message_delivered", "render_message",
	"render_markdown", "tick", "tool_render", "provider_stream_event", "model_route"};

struct native_tool {
	struct phi_tool info;
	json_object *schema;
	phi_extension_call_v1 call;
	void *userdata;
	/* register_tool_ex extras */
	int structured, parallel;
	char *snippet;
	json_object *guidelines;
};

struct native_command {
	struct phi_command info;
	phi_extension_call_v1 call;
	void *userdata;
	int during_turn; /* set_command_options: runs at once while a turn runs */
};

struct native_cli {
	char name[49];
	int (*call)(void *, int, const char *const *);
	void *userdata;
};

struct native_hook {
	const char *event; /* Points into the fixed event-name table. */
	phi_extension_hook_v1 call;
	void *userdata;
};

struct native_handler {
	char event[65]; /* a lifecycle event name, or "ext:<name>" for the bus */
	phi_event_handler_v1 call;
	void *userdata;
	int failing; /* report repeated failures once */
};

struct native_provider {
	char id[49];
	char *name;
	json_object *models; /* validated owned array */
	struct phi_provider_auth_v1 auth;
	phi_provider_stream_v1 stream;
	void *userdata;
	struct extension *owner;
};

struct native_provider_wrapper {
	char target[49]; /* provider id or * */
	phi_provider_wrapper_v1 call;
	void *userdata;
};

struct extension {
	char name[49], key[65], error[256];
	void *library;
	const struct phi_extension_v1 *descriptor;
	struct phi_api_v1 api;
	struct native_tool tools[MAX_TOOLS];
	struct native_command commands[MAX_MEMBERS];
	struct native_hook hooks[MAX_MEMBERS];
	size_t tool_count, command_count, hook_count;
	struct native_cli cli[MAX_MEMBERS];
	size_t cli_count;
	phi_extension_tool_interceptor_v1 interceptor;
	void *interceptor_userdata;
	phi_extension_context_provider_v1 context_provider;
	void *context_userdata;
	int context_failed;
	phi_extension_turn_message_provider_v1 turn_message_provider;
	void *turn_message_userdata;
	char turn_message_type[49];
	int turn_message_failed;
	struct native_provider providers[MAX_PROVIDERS];
	struct native_provider_wrapper provider_wrappers[MAX_PROVIDER_WRAPPERS];
	size_t provider_count, provider_wrapper_count;

	struct {
		char id[64], name[65];
	} virtual_models[MAX_VIRTUAL_MODELS]; /* routed by model_route */

	size_t virtual_model_count;
	struct native_handler *handlers;
	size_t handler_count;
	json_object *state;
	char status[MAX_STATUS + 1];

	struct {
		char key[33], text[MAX_STATUS + 1];
	} slots[MAX_STATUS_SLOTS]; /* ui_status */

	int initializing, initialized, shutting_down;
	int tick_ms; /* ui_tick: deliver "tick" every tick_ms; 0 = off */
	double tick_next;
};
static struct extension *loaded[MAX_EXTENSIONS];
static size_t loaded_count;
static struct extension_host host;
static char *source_directory, *cache_directory, *state_directory;
static int callback_depth, updating;
static int nested_tools; /* execute_tool calls in progress (no reload meanwhile) */
static struct extension *running_owner; /* whose tool is executing (tool_update) */
static const char *const event_names[] = {
	"turn_start", "turn_end", "tool_start", "tool_end", "session_load"};

static json_object *result(const char *key, const char *text) {

	json_object *object = json_object_new_object();
	if (object) {
		json_object_object_add(object, key, json_object_new_string(text ? text : ""));
	}

	return object;
}

static void message(const char *text) {

	if (!host.message || !text) {
		return;
	}
	/* Extensions must use this instead of printing terminal escapes. Native
       code could bypass it; this is presentation hygiene, not isolation. */
	char clean[4097];
	size_t used = 0;
	for (size_t i = 0; text[i] && used + 4 < sizeof(clean); i++) {
		unsigned char c = (unsigned char)text[i];
		if ((c < 32 && c != '\n' && c != '\t') || c == 127) {
			snprintf(clean + used, 5, "\\x%02x", c);
			used += 4;
		} else {
			clean[used++] = (char)c;
		}
	}
	clean[used] = 0;
	host.message(clean);
}

static int cwd(char *buffer, size_t capacity) {

	if (!buffer || !capacity) {
		return -1;
	}

	return getcwd(buffer, capacity) ? 0 : -1;
}

static int cancelled(void) {

	return host.cancel ? host.cancel() : 0;
}

static char *path_join(const char *directory, const char *leaf) {

	size_t n = strlen(directory) + strlen(leaf) + 2;
	char *path = malloc(n);
	if (path) {
		snprintf(path, n, "%s/%s", directory, leaf);
	}

	return path;
}

static int state_key_valid(const char *key) {

	if (!key || !*key || strlen(key) > 48) {
		return 0;
	}
	for (size_t i = 0; key[i]; i++) {
		unsigned char c = (unsigned char)key[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '_' || c == '-' || c == '.') {
			continue;
		}
		return 0;
	}

	return 1;
}

static char *state_path(const char *name) {

	char leaf[64];
	if (!state_directory || !extension_name_valid(name)) {
		return NULL;
	}
	snprintf(leaf, sizeof(leaf), "%s.json", name);

	return path_join(state_directory, leaf);
}

static int state_object_valid(json_object *state) {

	if (!json_object_is_type(state, json_type_object) ||
		json_object_object_length(state) > MAX_STATE_KEYS) {
		return 0;
	}
	json_object_object_foreach(state, key, value) {
		if (!state_key_valid(key) || !json_object_is_type(value, json_type_string) ||
			strlen(json_object_get_string(value)) != (size_t)json_object_get_string_len(value) ||
			(size_t)json_object_get_string_len(value) > MAX_STATE_VALUE) {
			return 0;
		}
	}

	return 1;
}

static json_object *state_load(const char *name, char *error, size_t size) {

	json_object *state = NULL;
	char *path = state_path(name);
	if (!path) {
		snprintf(error, size, "Cannot allocate extension state path");
		return NULL;
	}
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		int saved = errno;
		free(path);
		if (saved == ENOENT) {
			return json_object_new_object();
		}
		snprintf(error, size, "Read extension state: %s", strerror(saved));
		return NULL;
	}
	free(path);
	struct stat info;
	if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
		(info.st_mode & 077) || info.st_size < 0 || (uintmax_t)info.st_size > MAX_STATE_FILE) {
		int saved = errno ? errno : EPERM;
		close(fd);
		snprintf(error, size, "Unsafe extension state file: %s", strerror(saved));
		return NULL;
	}
	size_t expected = (size_t)info.st_size, used = 0;
	char *text = malloc(expected + 1);
	if (!text) {
		close(fd);
		snprintf(error, size, "Cannot allocate extension state");
		return NULL;
	}
	while (used < expected) {
		ssize_t n = read(fd, text + used, expected - used);
		if (n > 0) {
			used += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			free(text);
			close(fd);
			snprintf(error, size, "Read extension state: %s", strerror(errno ? errno : EIO));
			return NULL;
		}
	}
	char extra;
	ssize_t n;
	do {
		n = read(fd, &extra, 1);
	} while (n < 0 && errno == EINTR);
	close(fd);
	if (n != 0) {
		free(text);
		snprintf(error, size, "Extension state changed while reading");
		return NULL;
	}
	text[used] = 0;
	json_tokener *parser = json_tokener_new();
	if (parser) {
		json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
		state = json_tokener_parse_ex(parser, text, (int)used + 1);
		if (json_tokener_get_error(parser) != json_tokener_success) {
			if (state) {
				json_object_put(state);
			}
			state = NULL;
		}
		json_tokener_free(parser);
	}
	free(text);
	if (!state || !state_object_valid(state)) {
		if (state) {
			json_object_put(state);
		}
		snprintf(error, size, "Extension state is not a valid bounded string map");
		return NULL;
	}

	return state;
}

static int state_write(const char *name, json_object *state) {

	if (!state_directory || !state_object_valid(state)) {
		return -1;
	}
	char error[256];
	if (!extension_directory(state_directory, 1, 1, error, sizeof(error))) {
		return -1;
	}
	const char *text = json_object_to_json_string_ext(state, JSON_C_TO_STRING_PLAIN);
	size_t length = strlen(text);
	if (length > MAX_STATE_FILE) {
		return -1;
	}
	size_t n = strlen(state_directory) + strlen(name) + 32;
	char *temporary = malloc(n), *target = state_path(name);
	if (!temporary || !target) {
		free(temporary);
		free(target);
		return -1;
	}
	snprintf(temporary, n, "%s/.%s-state-XXXXXX", state_directory, name);
	int fd = mkstemp(temporary), ok = 0;
	if (fd >= 0) {
		if (!fchmod(fd, 0600)) {
			size_t used = 0;
			while (used < length) {
				ssize_t wrote = write(fd, text + used, length - used);
				if (wrote > 0) {
					used += (size_t)wrote;
				} else if (wrote < 0 && errno == EINTR) {
					continue;
				} else {
					break;
				}
			}
			if (used == length && !fsync(fd) && !close(fd)) {
				fd = -1;
				if (!rename(temporary, target)) {
					int directory = open(state_directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
					if (directory >= 0) {
						ok = !fsync(directory);
						close(directory);
					} else {
						ok = 1; /* Replacement succeeded; directory fsync unavailable. */
					}
				}
			}
		}
		if (fd >= 0) {
			close(fd);
		}
		if (!ok) {
			unlink(temporary);
		}
	}
	free(temporary);
	free(target);

	return ok ? 0 : -1;
}

static int state_lock(const char *name) {

	char error[256];
	if (!state_directory || !extension_directory(state_directory, 1, 1, error, sizeof(error))) {
		return -1;
	}
	char leaf[64];
	snprintf(leaf, sizeof(leaf), ".%s.lock", name);
	char *path = path_join(state_directory, leaf);
	if (!path) {
		return -1;
	}
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	free(path);
	if (fd < 0) {
		return -1;
	}
	struct stat info;
	if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
		(info.st_mode & 077)) {
		close(fd);
		return -1;
	}
	struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
	while (fcntl(fd, F_SETLKW, &lock) < 0) {
		if (errno == EINTR) {
			continue;
		}
		close(fd);
		return -1;
	}

	return fd;
}

static void state_unlock(int fd) {

	if (fd < 0) {
		return;
	}
	struct flock lock = {.l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
	fcntl(fd, F_SETLK, &lock);
	close(fd);
}

static void notify_status(void) {

	if (!host.status) {
		return;
	}
	char line[256] = "";
	size_t used = 0, shown = 0;
	for (size_t k = 0; k < loaded_count * (MAX_STATUS_SLOTS + 1); k++) {
		/* Each extension's status_set text (named), then its ui_status slots. */
		const struct extension *e = loaded[k / (MAX_STATUS_SLOTS + 1)];
		size_t slot = k % (MAX_STATUS_SLOTS + 1);
		const char *text = slot ? e->slots[slot - 1].text : e->status;
		if (!*text) {
			continue;
		}
		char segment[128];
		int count = slot ? snprintf(segment, sizeof(segment), "%s", text)
						 : snprintf(segment, sizeof(segment), "%s %s", e->name, text);
		if (count < 0) {
			continue;
		}
		size_t length = (size_t)count < sizeof(segment) ? (size_t)count : sizeof(segment) - 1;
		size_t separator = shown ? strlen(" · ") : 0;
		if (used + separator + length >= sizeof(line)) {
			if (used + 4 < sizeof(line)) {
				memcpy(line + used, " …", 4), used += 4;
			}
			break;
		}
		if (shown) {
			memcpy(line + used, " · ", separator);
			used += separator;
		}
		memcpy(line + used, segment, length);
		used += length;
		line[used] = 0;
		shown++;
	}
	host.status(line);
}

static int extension_state_get(void *context, const char *key, char *buffer, size_t capacity) {

	struct extension *owner = context;
	if (!owner || owner->shutting_down || !state_key_valid(key) || !buffer || !capacity) {
		return -1;
	}
	char error[256];
	json_object *latest = state_load(owner->name, error, sizeof(error));
	if (!latest) {
		return -1;
	}
	if (owner->state) {
		json_object_put(owner->state);
	}
	owner->state = latest;
	json_object *value = NULL;
	if (!json_object_object_get_ex(owner->state, key, &value)) {
		buffer[0] = 0;
		return 1;
	}
	const char *text = json_object_get_string(value);
	size_t length = (size_t)json_object_get_string_len(value);
	if (length + 1 > capacity) {
		return -1;
	}
	memcpy(buffer, text, length);
	buffer[length] = 0;

	return 0;
}

static int extension_state_set(void *context, const char *key, const char *value) {

	struct extension *owner = context;
	if (!owner || owner->initializing || owner->shutting_down || !owner->initialized ||
		!state_key_valid(key) || (value && strlen(value) > MAX_STATE_VALUE)) {
		return -1;
	}
	int lock = state_lock(owner->name);
	if (lock < 0) {
		return -1;
	}
	char error[256];
	json_object *copy = state_load(owner->name, error, sizeof(error));
	if (!copy) {
		state_unlock(lock);
		return -1;
	}
	json_object *existing = NULL;
	int present = json_object_object_get_ex(copy, key, &existing);
	if (!value) {
		json_object_object_del(copy, key);
	} else {
		if (!present && json_object_object_length(copy) >= MAX_STATE_KEYS) {
			json_object_put(copy);
			state_unlock(lock);
			return -1;
		}
		json_object *string = json_object_new_string(value);
		if (!string) {
			json_object_put(copy);
			state_unlock(lock);
			return -1;
		}
		json_object_object_add(copy, key, string);
	}
	int written = state_object_valid(copy) ? state_write(owner->name, copy) : -1;
	state_unlock(lock);
	if (written) {
		json_object_put(copy);
		return -1;
	}
	if (owner->state) {
		json_object_put(owner->state);
	}
	owner->state = copy;

	return 0;
}

static int extension_status_set(void *context, const char *text) {

	struct extension *owner = context;
	if (!owner || owner->shutting_down || (!owner->initializing && !owner->initialized) || !text ||
		strnlen(text, MAX_STATUS + 1) > MAX_STATUS) {
		return -1;
	}
	for (size_t i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c < 32 || c == 127) {
			return -1;
		}
	}
	snprintf(owner->status, sizeof(owner->status), "%s", text);
	if (!owner->initializing) {
		notify_status();
	}

	return 0;
}

static struct extension *find_extension(const char *name) {

	for (size_t i = 0; i < loaded_count; i++) {
		if (!strcmp(loaded[i]->name, name)) {
			return loaded[i];
		}
	}

	return NULL;
}

static int registration_error_name(
	struct extension *extension, const char *kind, const char *name, const char *reason) {

	if (extension && !*extension->error) {
		snprintf(extension->error, sizeof(extension->error), "%s '%s': %s", kind,
			name ? name : "(null)", reason);
	}

	return -1;
}

static int valid_description(const char *description) {

	return description && *description && strnlen(description, 513) <= 512;
}

static int tool_name_available(struct extension *owner, const char *name) {

	if (!extension_name_valid(name) || !strcmp(name, "extensions") ||
		(host.reserved_tool && host.reserved_tool(name))) {
		return 0;
	}
	for (size_t i = 0; i < owner->tool_count; i++) {
		if (!strcmp(owner->tools[i].info.name, name)) {
			return 0;
		}
	}
	for (size_t i = 0; i < loaded_count; i++) {
		if (!strcmp(loaded[i]->name, owner->name)) {
			continue; /* Staged replacement. */
		}
		for (size_t j = 0; j < loaded[i]->tool_count; j++) {
			if (!strcmp(loaded[i]->tools[j].info.name, name)) {
				return 0;
			}
		}
	}

	return 1;
}

static json_object *parse_object_limit(const char *text, size_t limit) {

	size_t length;
	if (!text || (length = strlen(text)) > limit || length >= INT_MAX) {
		return NULL;
	}
	json_tokener *parser = json_tokener_new();
	if (!parser) {
		return NULL;
	}
	json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
	json_object *object = json_tokener_parse_ex(parser, text, (int)length + 1);
	int valid = json_tokener_get_error(parser) == json_tokener_success &&
		json_object_is_type(object, json_type_object);
	json_tokener_free(parser);
	if (!valid && object) {
		json_object_put(object);
		object = NULL;
	}

	return object;
}

static json_object *parse_object(const char *text) {

	return parse_object_limit(text, 16384);
}

/* Validate and add one tool; takes ownership of params. Returns the new
   tool or NULL (with the extension's error set). */
static struct native_tool *add_tool(struct extension *owner, const char *name,
	const char *description, json_object *params, phi_extension_call_v1 call, void *userdata) {

	const char *problem = NULL;
	/* Tools may also be added after init (e.g. an MCP server that appears
       later); the model sees them from its next request. */
	if (!owner->initializing && (!owner->initialized || owner->shutting_down || updating)) {
		problem = "registration is only allowed during init or while loaded";
	} else if (!call) {
		problem = "callback is required";
	} else if (!valid_description(description)) {
		problem = "description must be 1-512 bytes";
	} else if (owner->tool_count == MAX_TOOLS) {
		problem = "128-tool registration limit reached";
	} else if (!extension_name_valid(name)) {
		problem = "invalid name";
	} else if (!strcmp(name, "extensions") || (host.reserved_tool && host.reserved_tool(name))) {
		problem = "duplicate built-in tool name";
	} else if (!tool_name_available(owner, name)) {
		problem = "duplicate extension tool name";
	} else if (!params || strcmp(field(params, "type"), "object")) {
		problem = "parameters must be a JSON Schema object with type 'object'";
	}
	json_object *schema = problem ? NULL : result("type", "function");
	char *copy = problem ? NULL : strdup(name);
	if (!problem && (!schema || !copy)) {
		problem = "cannot allocate registration";
	}
	if (problem) {
		if (schema) {
			json_object_put(schema);
		}
		if (params) {
			json_object_put(params);
		}
		free(copy);
		registration_error_name(owner, "Tool", name, problem);
		return NULL;
	}
	json_object_object_add(schema, "name", json_object_new_string(name));
	json_object_object_add(schema, "description", json_object_new_string(description));
	json_object_object_add(schema, "parameters", params);
	struct native_tool *tool = &owner->tools[owner->tool_count++];
	*tool = (struct native_tool){
		.info = {.name = copy}, .schema = schema, .call = call, .userdata = userdata};

	return tool;
}

static int register_tool(void *context, const char *name, const char *description,
	const char *parameters, phi_extension_call_v1 call, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}

	return add_tool(owner, name, description, parse_object(parameters), call, userdata) ? 0 : -1;
}

static void tool_free(struct native_tool *tool) {

	free((char *)tool->info.name);
	free((char *)tool->info.label);
	free((char *)tool->info.detail_key);
	free((char *)tool->info.annotations);
	free(tool->snippet);
	if (tool->guidelines) {
		json_object_put(tool->guidelines);
	}
	json_object_put(tool->schema);
}

/* unregister_tool {name}: remove one of the caller's tools. Not while one
   of its tools runs, since the running call still refers to its entry. */
static const char *unregister_tool(struct extension *owner, const char *name) {

	if (running_owner == owner) {
		return "cannot unregister a tool while this extension's tool is running";
	}
	for (size_t i = 0; i < owner->tool_count; i++) {
		if (strcmp(owner->tools[i].info.name, name)) {
			continue;
		}
		tool_free(&owner->tools[i]);
		memmove(owner->tools + i, owner->tools + i + 1,
			(owner->tool_count - i - 1) * sizeof(*owner->tools));
		owner->tool_count--;
		return NULL;
	}

	return "no such tool of this extension";
}

static int short_text(json_object *value, size_t limit) {

	if (!json_object_is_type(value, json_type_string)) {
		return 0;
	}
	const char *text = json_object_get_string(value);
	size_t n = strlen(text);
	if (!n || n > limit || n != (size_t)json_object_get_string_len(value)) {
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		if ((unsigned char)text[i] < 32 && text[i] != '\n') {
			return 0;
		}
	}

	return 1;
}

/* annotations: MCP tool hints, booleans only. */
static int annotations_valid(json_object *annotations) {

	static const char *const hints[] = {
		"readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"};
	if (!json_object_is_type(annotations, json_type_object)) {
		return 0;
	}
	json_object_object_foreach(annotations, key, value) {
		int known = 0;
		for (size_t i = 0; i < sizeof(hints) / sizeof(*hints); i++) {
			known |= !strcmp(key, hints[i]);
		}
		if (!known || !json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
	}

	return 1;
}

/* Pi-style tool definition: see register_tool_ex in the header. */
static int register_tool_ex(
	void *context, const char *definition_json, phi_extension_call_v1 call, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	json_object *definition = parse_object_limit(definition_json, 65536);
	if (!definition) {
		return registration_error_name(
			owner, "Tool", "(definition)", "definition must be a JSON object up to 64 KiB");
	}
	const char *name = field(definition, "name");
	json_object *label = json_object_object_get(definition, "label");
	json_object *detail = json_object_object_get(definition, "detail_key");
	json_object *snippet = json_object_object_get(definition, "prompt_snippet");
	json_object *guidelines = json_object_object_get(definition, "prompt_guidelines");
	json_object *mode = json_object_object_get(definition, "execution_mode");
	json_object *structured = json_object_object_get(definition, "structured");
	json_object *hide_success = NULL;
	int has_hide_success = json_object_object_get_ex(definition, "hide_success", &hide_success);
	json_object *annotations = json_object_object_get(definition, "annotations");
	const char *problem = NULL;
	if (label && !short_text(label, 64)) {
		problem = "label must be 1-64 bytes of text";
	} else if (detail && !short_text(detail, 64)) {
		problem = "detail_key must name an argument";
	} else if (snippet && !short_text(snippet, 512)) {
		problem = "prompt_snippet must be 1-512 bytes";
	} else if (guidelines &&
		(!json_object_is_type(guidelines, json_type_array) ||
			json_object_array_length(guidelines) > 8)) {
		problem = "prompt_guidelines must be an array of up to 8 strings";
	} else if (mode &&
		(!json_object_is_type(mode, json_type_string) ||
			(strcmp(json_object_get_string(mode), "sequential") &&
				strcmp(json_object_get_string(mode), "parallel")))) {
		problem = "execution_mode must be sequential or parallel";
	} else if (structured && !json_object_is_type(structured, json_type_boolean)) {
		problem = "structured must be true or false";
	} else if (has_hide_success && !json_object_is_type(hide_success, json_type_boolean)) {
		problem = "hide_success must be true or false";
	} else if (annotations && !annotations_valid(annotations)) {
		problem =
			"annotations takes readOnlyHint, destructiveHint, idempotentHint and openWorldHint as booleans";
	}
	for (size_t i = 0; !problem && guidelines && i < json_object_array_length(guidelines); i++) {
		if (!short_text(json_object_array_get_idx(guidelines, i), 512)) {
			problem = "each guideline must be 1-512 bytes";
		}
	}
	if (problem) {
		int code = registration_error_name(owner, "Tool", *name ? name : "(definition)", problem);
		json_object_put(definition);
		return code;
	}
	json_object *params = json_object_object_get(definition, "parameters");
	struct native_tool *tool = add_tool(owner, name, field(definition, "description"),
		params ? json_object_get(params) : NULL, call, userdata);
	if (!tool) {
		json_object_put(definition);
		return -1;
	}
	tool->structured = structured ? json_object_get_boolean(structured) : 1;
	tool->info.hide_success = hide_success && json_object_get_boolean(hide_success);
	tool->parallel = mode && !strcmp(json_object_get_string(mode), "parallel");
	if (label) {
		tool->info.label = strdup(json_object_get_string(label));
	}
	if (detail) {
		tool->info.detail_key = strdup(json_object_get_string(detail));
	}
	if (snippet) {
		tool->snippet = strdup(json_object_get_string(snippet));
	}
	if (guidelines) {
		tool->guidelines = json_object_get(guidelines);
	}
	if (annotations) {
		tool->info.annotations =
			strdup(json_object_to_json_string_ext(annotations, JSON_C_TO_STRING_PLAIN));
	}
	json_object_put(definition);

	return 0;
}

static int command_name_available(struct extension *owner, const char *name) {

	if (!name || name[0] != '/' || !extension_name_valid(name + 1)) {
		return 0;
	}
	for (size_t i = 0; i < PHI_COMMAND_COUNT; i++) {
		if (!strcmp(phi_commands[i].name, name)) {
			return 0;
		}
	}
	for (size_t i = 0; i < owner->command_count; i++) {
		if (!strcmp(owner->commands[i].info.name, name)) {
			return 0;
		}
	}
	for (size_t i = 0; i < loaded_count; i++) {
		if (!strcmp(loaded[i]->name, owner->name)) {
			continue;
		}
		for (size_t j = 0; j < loaded[i]->command_count; j++) {
			if (!strcmp(loaded[i]->commands[j].info.name, name)) {
				return 0;
			}
		}
	}

	return 1;
}

static int register_command(void *context, const char *name, const char *description, int arguments,
	phi_extension_call_v1 call, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing) {
		return registration_error_name(
			owner, "Command", name, "registration is only allowed during init");
	}
	if (!call) {
		return registration_error_name(owner, "Command", name, "callback is required");
	}
	if (!valid_description(description)) {
		return registration_error_name(owner, "Command", name, "description must be 1-512 bytes");
	}
	if (arguments != 0 && arguments != 1) {
		return registration_error_name(owner, "Command", name, "takes_arguments must be 0 or 1");
	}
	if (owner->command_count == MAX_MEMBERS) {
		return registration_error_name(
			owner, "Command", name, "32-command registration limit reached");
	}
	if (!command_name_available(owner, name)) {
		return registration_error_name(owner, "Command", name, "invalid or duplicate command name");
	}
	char *n = strdup(name), *d = strdup(description);
	if (!n || !d) {
		free(n);
		free(d);
		return registration_error_name(owner, "Command", name, "cannot allocate registration");
	}
	owner->commands[owner->command_count++] =
		(struct native_command){.info = {n, d, arguments}, .call = call, .userdata = userdata};

	return 0;
}

static int register_cli_command(void *context, const char *name,
	int (*call)(void *, int, const char *const *), void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing || !call || !extension_name_valid(name) ||
		owner->cli_count == MAX_MEMBERS) {
		return registration_error_name(owner, "CLI command", name, "invalid registration");
	}
	const char *reserved[] = {"ls", "tail", "wait", "send", "new", "board", "attach", "rm",
		"install", "remove", "uninstall", "update", "list"};
	for (size_t i = 0; i < sizeof(reserved) / sizeof(*reserved); i++) {
		if (!strcmp(name, reserved[i])) {
			return registration_error_name(owner, "CLI command", name, "built-in name is reserved");
		}
	}
	for (size_t i = 0; i < owner->cli_count; i++) {
		if (!strcmp(name, owner->cli[i].name)) {
			return registration_error_name(owner, "CLI command", name, "duplicate name");
		}
	}
	for (size_t i = 0; i < loaded_count; i++) {
		if (strcmp(loaded[i]->name, owner->name)) {
			for (size_t j = 0; j < loaded[i]->cli_count; j++) {
				if (!strcmp(name, loaded[i]->cli[j].name)) {
					return registration_error_name(
						owner, "CLI command", name, "name belongs to another extension");
				}
			}
		}
	}
	struct native_cli *entry = &owner->cli[owner->cli_count++];
	snprintf(entry->name, sizeof(entry->name), "%s", name);
	entry->call = call;
	entry->userdata = userdata;

	return 0;
}

/* Discovery only: stale names remain reserved and fail clearly if their
   extension is no longer approved. Never use this index to bypass approval. */
static void publish_cli_names(struct extension *owner) {

	if (!owner->cli_count || !state_directory) {
		return;
	}
	char error[256], path[4096], lockpath[4096];
	if (!extension_directory(state_directory, 1, 1, error, sizeof(error))) {
		return;
	}
	snprintf(path, sizeof(path), "%s/.cli-index.json", state_directory);
	snprintf(lockpath, sizeof(lockpath), "%s/.cli-index.lock", state_directory);
	int fd = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
	struct stat st;
	if (fd < 0) {
		return;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) {
		close(fd);
		return;
	}
	int locked;
	do {
		locked = flock(fd, LOCK_EX);
	} while (locked && errno == EINTR);
	if (locked) {
		close(fd);
		return;
	}
	char *text = phi_read_file(path, 128 * 1024, PHI_READ_OWNED, NULL);
	json_object *index = text ? json_tokener_parse(text) : NULL;
	free(text);
	if (!json_object_is_type(index, json_type_object)) {
		json_object_put(index);
		index = json_object_new_object();
	}
	for (size_t i = 0; i < owner->cli_count && json_object_object_length(index) < 2048; i++) {
		json_object_object_add(index, owner->cli[i].name, json_object_new_string(owner->name));
	}
	text = (char *)json_object_to_json_string_ext(index, JSON_C_TO_STRING_PLAIN);
	phi_write_file_atomic(path, text, strlen(text));
	json_object_put(index);
	flock(fd, LOCK_UN);
	close(fd);
}

int extensions_cli_known(const char *name) {

	if (!extension_name_valid(name)) {
		return 0;
	}
	char path[4096];
	if (!phi_xdg_path(path, sizeof(path), "XDG_STATE_HOME", ".local/state",
			"phi/extensions/.cli-index.json")) {
		return 0;
	}
	char *text = phi_read_file(path, 128 * 1024, PHI_READ_OWNED, NULL);
	json_object *index = text ? json_tokener_parse(text) : NULL;
	free(text);
	int known = json_object_is_type(index, json_type_object) &&
		json_object_object_get_ex(index, name, NULL);
	json_object_put(index);

	return known;
}

int extensions_cli_execute(int argc, char **argv) {

	if (argc < 1 || updating || callback_depth) {
		return -1;
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->cli_count; j++) {
			struct native_cli *entry = &loaded[i]->cli[j];
			if (strcmp(entry->name, argv[0])) {
				continue;
			}
			callback_depth++;
			int status = entry->call(entry->userdata, argc, (const char *const *)argv);
			callback_depth--;
			return status >= 0 && status <= 255 ? status : 1;
		}
	}

	return -1;
}

static int register_hook(
	void *context, const char *event, phi_extension_hook_v1 call, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing) {
		return registration_error_name(
			owner, "Hook", event, "registration is only allowed during init");
	}
	if (!event || !call) {
		return registration_error_name(owner, "Hook", event, "event and callback are required");
	}
	if (owner->hook_count == MAX_MEMBERS) {
		return registration_error_name(owner, "Hook", event, "32-hook registration limit reached");
	}
	const char *known = NULL;
	for (size_t i = 0; i < sizeof(event_names) / sizeof(*event_names); i++) {
		if (!strcmp(event, event_names[i])) {
			known = event_names[i];
		}
	}
	for (size_t i = 0; i < owner->hook_count; i++) {
		if (!strcmp(owner->hooks[i].event, event)) {
			known = NULL;
		}
	}
	if (!known) {
		return registration_error_name(owner, "Hook", event, "unknown or duplicate event");
	}
	owner->hooks[owner->hook_count++] = (struct native_hook){known, call, userdata};

	return 0;
}

static int register_tool_interceptor(
	void *context, phi_extension_tool_interceptor_v1 interceptor, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing) {
		return registration_error_name(
			owner, "Tool interceptor", owner->name, "registration is only allowed during init");
	}
	if (!interceptor) {
		return registration_error_name(
			owner, "Tool interceptor", owner->name, "callback is required");
	}
	if (owner->interceptor) {
		return registration_error_name(owner, "Tool interceptor", owner->name,
			"only one interceptor is allowed per extension");
	}
	owner->interceptor = interceptor;
	owner->interceptor_userdata = userdata;

	return 0;
}

static int register_context_provider(
	void *context, phi_extension_context_provider_v1 provider, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing) {
		return registration_error_name(
			owner, "Context provider", owner->name, "registration is only allowed during init");
	}
	if (!provider) {
		return registration_error_name(
			owner, "Context provider", owner->name, "callback is required");
	}
	if (owner->context_provider) {
		return registration_error_name(
			owner, "Context provider", owner->name, "only one provider is allowed per extension");
	}
	owner->context_provider = provider;
	owner->context_userdata = userdata;

	return 0;
}

static int turn_message_type_valid(const char *type) {

	if (!type || !*type || strlen(type) > 48) {
		return 0;
	}
	for (const unsigned char *p = (const unsigned char *)type; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.')) {
			return 0;
		}
	}

	return 1;
}

static int register_turn_message_provider(void *context, const char *custom_type,
	phi_extension_turn_message_provider_v1 provider, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing) {
		return registration_error_name(owner, "Turn-message provider", owner->name,
			"registration is only allowed during init");
	}
	if (!provider) {
		return registration_error_name(
			owner, "Turn-message provider", owner->name, "callback is required");
	}
	if (owner->turn_message_provider) {
		return registration_error_name(owner, "Turn-message provider", owner->name,
			"only one provider is allowed per extension");
	}
	if (!turn_message_type_valid(custom_type)) {
		return registration_error_name(owner, "Turn-message provider",
			custom_type ? custom_type : "", "custom type must be 1-48 letters/numbers/_/.-");
	}
	owner->turn_message_provider = provider;
	owner->turn_message_userdata = userdata;
	snprintf(owner->turn_message_type, sizeof(owner->turn_message_type), "%s", custom_type);

	return 0;
}

static int provider_id_valid(const char *id) {

	return id && strlen(id) <= 43 && extension_name_valid(id);
}

static int provider_id_taken(struct extension *owner, const char *id) {

	for (size_t i = 0; i < owner->provider_count; i++) {
		if (!strcmp(owner->providers[i].id, id)) {
			return 1;
		}
	}
	for (size_t i = 0; i < loaded_count; i++) {
		if (loaded[i] == owner || !strcmp(loaded[i]->name, owner->name)) {
			continue;
		}
		for (size_t j = 0; j < loaded[i]->provider_count; j++) {
			if (!strcmp(loaded[i]->providers[j].id, id)) {
				return 1;
			}
		}
	}

	return 0;
}

static int model_id_valid(const char *id) {

	if (!id || !*id || strlen(id) > 127) {
		return 0;
	}

	return strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/") ==
		strlen(id);
}

static int model_id_taken(struct extension *owner, const char *id) {

	for (size_t i = 0; i < owner->provider_count; i++) {
		for (size_t j = 0; j < json_object_array_length(owner->providers[i].models); j++) {
			if (!strcmp(
					field(json_object_array_get_idx(owner->providers[i].models, j), "id"), id)) {
				return 1;
			}
		}
	}
	for (size_t i = 0; i < loaded_count; i++) {
		if (loaded[i] == owner || !strcmp(loaded[i]->name, owner->name)) {
			continue;
		}
		for (size_t p = 0; p < loaded[i]->provider_count; p++) {
			for (size_t j = 0; j < json_object_array_length(loaded[i]->providers[p].models); j++) {
				if (!strcmp(
						field(json_object_array_get_idx(loaded[i]->providers[p].models, j), "id"),
						id)) {
					return 1;
				}
			}
		}
	}

	return 0;
}

static int register_provider(void *context, const struct phi_provider_v1 *provider) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing) {
		return registration_error_name(owner, "Provider", provider ? provider->id : NULL,
			"registration is only allowed during init");
	}
	if (!provider || provider->size < sizeof(*provider) || !provider->stream) {
		return registration_error_name(owner, "Provider", provider ? provider->id : NULL,
			"descriptor size and stream callback are required");
	}
	if (!provider_id_valid(provider->id)) {
		return registration_error_name(owner, "Provider", provider->id,
			"id must use extension-name syntax and be at most 43 bytes");
	}
	if (provider_id_taken(owner, provider->id)) {
		return registration_error_name(owner, "Provider", provider->id, "duplicate provider id");
	}
	if (!provider->name || !*provider->name || strlen(provider->name) > 128) {
		return registration_error_name(owner, "Provider", provider->id, "name must be 1-128 bytes");
	}
	if (owner->provider_count == MAX_PROVIDERS) {
		return registration_error_name(
			owner, "Provider", provider->id, "8-provider registration limit reached");
	}
	if (provider->auth.size && provider->auth.size < sizeof(provider->auth)) {
		return registration_error_name(
			owner, "Provider", provider->id, "incompatible auth descriptor size");
	}

	json_object *models = NULL;
	if (provider->models_json) {
		json_tokener *tok = json_tokener_new();
		if (tok) {
			json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
			models = json_tokener_parse_ex(
				tok, provider->models_json, (int)strlen(provider->models_json) + 1);
			if (json_tokener_get_error(tok) != json_tokener_success) {
				if (models) {
					json_object_put(models);
				}
				models = NULL;
			}
			json_tokener_free(tok);
		}
	}
	if (!json_object_is_type(models, json_type_array) || !json_object_array_length(models) ||
		json_object_array_length(models) > MAX_PROVIDER_MODELS) {
		if (models) {
			json_object_put(models);
		}
		return registration_error_name(
			owner, "Provider", provider->id, "models_json must be an array of 1-64 models");
	}
	for (size_t i = 0; i < json_object_array_length(models); i++) {
		json_object *model = json_object_array_get_idx(models, i), *window = NULL;
		const char *id = field(model, "id");
		if (!json_object_is_type(model, json_type_object) || !model_id_valid(id) ||
			model_id_taken(owner, id)) {
			json_object_put(models);
			return registration_error_name(
				owner, "Provider model", id, "invalid or duplicate model id");
		}
		if (json_object_object_get_ex(model, "context_window", &window) &&
			(!json_object_is_type(window, json_type_int) || json_object_get_int64(window) <= 0 ||
				json_object_get_int64(window) > 1000000000)) {
			json_object_put(models);
			return registration_error_name(owner, "Provider model", id, "invalid context_window");
		}
		for (size_t j = 0; j < i; j++) {
			if (!strcmp(field(json_object_array_get_idx(models, j), "id"), id)) {
				json_object_put(models);
				return registration_error_name(owner, "Provider model", id, "duplicate model id");
			}
		}
	}
	char *name = strdup(provider->name);
	if (!name) {
		json_object_put(models);
		return registration_error_name(
			owner, "Provider", provider->id, "cannot allocate registration");
	}
	struct native_provider *out = &owner->providers[owner->provider_count++];
	*out = (struct native_provider){.name = name,
		.models = models,
		.auth = provider->auth,
		.stream = provider->stream,
		.userdata = provider->userdata,
		.owner = owner};
	snprintf(out->id, sizeof(out->id), "%s", provider->id);

	return 0;
}

static int wrap_provider(
	void *context, const char *provider_id, phi_provider_wrapper_v1 wrapper, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing) {
		return registration_error_name(
			owner, "Provider wrapper", provider_id, "registration is only allowed during init");
	}
	if (!wrapper || !provider_id || (strcmp(provider_id, "*") && !provider_id_valid(provider_id))) {
		return registration_error_name(owner, "Provider wrapper", provider_id,
			"callback and valid provider id (or *) are required");
	}
	if (owner->provider_wrapper_count == MAX_PROVIDER_WRAPPERS) {
		return registration_error_name(
			owner, "Provider wrapper", provider_id, "8-wrapper registration limit reached");
	}
	struct native_provider_wrapper *out =
		&owner->provider_wrappers[owner->provider_wrapper_count++];
	snprintf(out->target, sizeof(out->target), "%s", provider_id);
	out->call = wrapper;
	out->userdata = userdata;

	return 0;
}

static struct native_provider *owned_provider(struct extension *owner, const char *id) {

	if (!owner || !id) {
		return NULL;
	}
	for (size_t i = 0; i < owner->provider_count; i++) {
		if (!strcmp(owner->providers[i].id, id)) {
			return &owner->providers[i];
		}
	}

	return NULL;
}

static int credential_key(const char *provider_id, char key[49]) {

	if (!provider_id_valid(provider_id)) {
		return 0;
	}

	return snprintf(key, 49, "auth.%s", provider_id) > 0;
}

static int credential_get(void *context, const char *provider_id, char *buffer, size_t capacity) {

	struct extension *owner = context;
	char key[49];
	if (!owned_provider(owner, provider_id) || !credential_key(provider_id, key)) {
		return -1;
	}

	return extension_state_get(owner, key, buffer, capacity);
}

static int valid_json_object_text(const char *text) {

	if (!text || !*text || strlen(text) > MAX_STATE_VALUE) {
		return 0;
	}
	json_tokener *tok = json_tokener_new();
	if (!tok) {
		return 0;
	}
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
	json_object *value = json_tokener_parse_ex(tok, text, (int)strlen(text) + 1);
	int ok = json_tokener_get_error(tok) == json_tokener_success &&
		json_object_is_type(value, json_type_object);
	if (value) {
		json_object_put(value);
	}
	json_tokener_free(tok);

	return ok;
}

static int credential_set(void *context, const char *provider_id, const char *credential_json) {

	struct extension *owner = context;
	char key[49];
	if (!owned_provider(owner, provider_id) || !credential_key(provider_id, key) ||
		!valid_json_object_text(credential_json)) {
		return -1;
	}

	return extension_state_set(owner, key, credential_json);
}

static int credential_delete(void *context, const char *provider_id) {

	struct extension *owner = context;
	char key[49];
	if (!owned_provider(owner, provider_id) || !credential_key(provider_id, key)) {
		return -1;
	}

	return extension_state_set(owner, key, NULL);
}

struct http_bridge {
	phi_http_chunk_v1 chunk;
	void *userdata;
	int failed;
	json_object *headers;
};

/* Response headers for http_request_ex, names lowercased; a new status
   line (a redirect, 100 Continue) starts them again. */
static size_t http_header(char *data, size_t size, size_t count, void *userdata) {

	struct http_bridge *bridge = userdata;
	if (count && size > SIZE_MAX / count) {
		return 0;
	}
	size_t n = size * count;
	if (!bridge->headers) {
		return n;
	}
	if (n >= 5 && !strncmp(data, "HTTP/", 5)) {
		json_object_put(bridge->headers);
		bridge->headers = json_object_new_object();
		return n;
	}
	const char *colon = memchr(data, ':', n);
	if (!colon || colon == data || (size_t)(colon - data) > 128) {
		return n;
	}
	char name[129];
	size_t length = (size_t)(colon - data);
	for (size_t i = 0; i < length; i++) {
		name[i] = (char)tolower((unsigned char)data[i]);
	}
	name[length] = 0;
	const char *value = colon + 1, *end = data + n;
	while (value < end && (*value == ' ' || *value == '\t')) {
		value++;
	}
	while (end > value && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) {
		end--;
	}
	if (end - value <= 8192) {
		json_object_object_add(
			bridge->headers, name, json_object_new_string_len(value, (int)(end - value)));
	}

	return n;
}

static size_t http_receive(char *data, size_t size, size_t count, void *userdata) {

	struct http_bridge *bridge = userdata;
	if (count && size > SIZE_MAX / count) {
		return 0;
	}
	size_t n = size * count;
	if (!bridge->chunk) {
		return n;
	}
	if (bridge->chunk(bridge->userdata, data, n)) {
		bridge->failed = 1;
		return 0;
	}

	return n;
}

static int http_progress(void *userdata, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {

	(void)userdata;
	(void)a;
	(void)b;
	(void)c;
	(void)d;

	return cancelled();
}

/* http on the local machine (for local servers, as MCP's); https elsewhere. */
static int loopback_url(const char *url) {

	const char *hosts[] = {"http://localhost", "http://127.0.0.1", "http://[::1]"};
	for (size_t i = 0; i < 3; i++) {
		size_t n = strlen(hosts[i]);
		if (!strncmp(url, hosts[i], n) && (!url[n] || url[n] == ':' || url[n] == '/')) {
			return 1;
		}
	}

	return 0;
}

static int http_exchange(struct extension *owner, const char *method, const char *url,
	const char *headers_json, const char *body, long timeout_ms, phi_http_chunk_v1 chunk,
	void *userdata, json_object **response_headers, long *http_status, char *error,
	size_t error_capacity) {

	if (!owner || owner->shutting_down || !method || !*method || !url ||
		(strncmp(url, "https://", 8) && !(response_headers && loopback_url(url))) || !error ||
		!error_capacity) {
		return -1;
	}
	error[0] = 0;
	if (http_status) {
		*http_status = 0;
	}
	json_object *headers = NULL;
	if (headers_json && *headers_json) {
		json_tokener *tok = json_tokener_new();
		if (!tok) {
			snprintf(error, error_capacity, "Cannot allocate HTTP parser");
			return -1;
		}
		json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
		headers = json_tokener_parse_ex(tok, headers_json, (int)strlen(headers_json) + 1);
		int ok = json_tokener_get_error(tok) == json_tokener_success &&
			json_object_is_type(headers, json_type_object);
		json_tokener_free(tok);
		if (!ok) {
			if (headers) {
				json_object_put(headers);
			}
			snprintf(error, error_capacity, "headers_json must be an object");
			return -1;
		}
	}
	CURL *easy = curl_easy_init();
	CURLM *multi = curl_multi_init();
	struct curl_slist *list = NULL;
	struct http_bridge bridge = {.chunk = chunk,
		.userdata = userdata,
		.headers = response_headers ? json_object_new_object() : NULL};
	if (!easy || !multi) {
		snprintf(error, error_capacity, "Cannot initialize HTTP transport");
		goto failed;
	}
	if (headers) {
		json_object_object_foreach(headers, key, value) {
			if (!json_object_is_type(value, json_type_string) || strpbrk(key, "\r\n:") ||
				strpbrk(json_object_get_string(value), "\r\n")) {
				snprintf(error, error_capacity, "Invalid HTTP header");
				goto failed;
			}
			size_t n = strlen(key) + strlen(json_object_get_string(value)) + 3;
			char *line = malloc(n);
			if (!line) {
				snprintf(error, error_capacity, "Cannot allocate HTTP header");
				goto failed;
			}
			snprintf(line, n, "%s: %s", key, json_object_get_string(value));
			struct curl_slist *next = curl_slist_append(list, line);
			free(line);
			if (!next) {
				snprintf(error, error_capacity, "Cannot allocate HTTP headers");
				goto failed;
			}
			list = next;
		}
	}
	curl_easy_setopt(easy, CURLOPT_URL, url);
	curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, http_receive);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &bridge);
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 300L);
	curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, http_progress);
	curl_easy_setopt(easy, CURLOPT_XFERINFODATA, owner);
	if (body) {
		curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
	}
	if (bridge.headers) {
		curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, http_header);
		curl_easy_setopt(easy, CURLOPT_HEADERDATA, &bridge);
	}
	if (timeout_ms > 0) {
		curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, timeout_ms);
	}
	CURLcode result = phi_http_perform(multi, easy, cancelled);
	long status = 0;
	curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
	if (http_status) {
		*http_status = status;
	}
	if (result != CURLE_OK || bridge.failed) {
		snprintf(error, error_capacity, "HTTP transport: %s", curl_easy_strerror(result));
		goto failed;
	}
	curl_slist_free_all(list);
	curl_easy_cleanup(easy);
	curl_multi_cleanup(multi);
	if (headers) {
		json_object_put(headers);
	}
	if (response_headers) {
		*response_headers = bridge.headers;
	}
	return 0;
failed:
	curl_slist_free_all(list);
	if (easy) {
		curl_easy_cleanup(easy);
	}
	if (multi) {
		curl_multi_cleanup(multi);
	}
	if (headers) {
		json_object_put(headers);
	}
	if (bridge.headers) {
		json_object_put(bridge.headers);
	}

	return -1;
}

static int http_request(void *context, const char *method, const char *url,
	const char *headers_json, const char *body, phi_http_chunk_v1 chunk, void *userdata,
	long *http_status, char *error, size_t error_capacity) {

	return http_exchange(context, method, url, headers_json, body, 0, chunk, userdata, NULL,
		http_status, error, error_capacity);
}

static int write_result(json_object *result, char *output, size_t capacity);

/* {method, url, headers?, body?, timeout_ms?} -> {status, headers}; the
   body goes to chunk. */
static int http_request_ex(void *context, const char *request_json, phi_http_chunk_v1 chunk,
	void *userdata, char *response_json, size_t capacity) {

	if (!response_json || capacity < 64) {
		return -1;
	}
	json_object *request =
		request_json ? parse_object_limit(request_json, 64u * 1024u * 1024u) : NULL;
	json_object *headers = request ? json_object_object_get(request, "headers") : NULL,
				*timeout = NULL;
	json_object *body = request ? json_object_object_get(request, "body") : NULL, *received = NULL;
	if (request) {
		json_object_object_get_ex(request, "timeout_ms", &timeout);
	}
	char error[256] = "";
	long status = 0;
	int code = !request || (headers && !json_object_is_type(headers, json_type_object)) ||
			(body && !json_object_is_type(body, json_type_string))
		? -1
		: http_exchange(context, *field(request, "method") ? field(request, "method") : "GET",
			  field(request, "url"),
			  headers ? json_object_to_json_string_ext(headers, JSON_C_TO_STRING_PLAIN) : NULL,
			  body ? json_object_get_string(body) : NULL,
			  timeout ? json_object_get_int64(timeout) : 0, chunk, userdata, &received, &status,
			  error, sizeof(error));
	json_object *answer = json_object_new_object();
	if (code) {
		json_object_object_add(answer, "error",
			json_object_new_string(*error
					? error
					: "http_request_ex takes {method, url (https, or http on localhost), headers?, body?, timeout_ms?}"));
	} else {
		json_object_object_add(answer, "status", json_object_new_int64(status));
		json_object_object_add(answer, "headers", received);
	}
	write_result(answer, response_json, capacity);
	json_object_put(answer);
	if (request) {
		json_object_put(request);
	}

	return code ? -1 : 0;
}

static int open_url(void *context, const char *url) {

	struct extension *owner = context;
	if (!owner || owner->shutting_down || !url || strncmp(url, "https://", 8) ||
		strlen(url) > 4096 || strpbrk(url, "\r\n")) {
		return -1;
	}
	pid_t child = fork();
	if (child < 0) {
		return -1;
	}
	if (!child) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		execlp("xdg-open", "xdg-open", url, (char *)NULL);
		_exit(127);
	}

	return 0;
}

static int event_name_valid(const char *event) {

	if (!event) {
		return 0;
	}
	if (!strncmp(event, "ext:", 4)) {
		size_t n = strlen(event + 4);
		return n && n <= 60 &&
			strspn(event + 4,
				"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:") == n;
	}
	for (size_t i = 0; i < sizeof(lifecycle_events) / sizeof(*lifecycle_events); i++) {
		if (!strcmp(event, lifecycle_events[i])) {
			return 1;
		}
	}

	return 0;
}

static int register_handler(
	void *context, const char *event, phi_event_handler_v1 handler, void *userdata) {

	struct extension *owner = context;
	if (!owner) {
		return -1;
	}
	if (!owner->initializing) {
		return registration_error_name(
			owner, "Event handler", event, "registration is only allowed during init");
	}
	if (!handler) {
		return registration_error_name(owner, "Event handler", event, "callback is required");
	}
	if (!event_name_valid(event)) {
		return registration_error_name(
			owner, "Event handler", event, "unknown event (use a lifecycle event or ext:<name>)");
	}
	if (owner->handler_count == MAX_HANDLERS) {
		return registration_error_name(
			owner, "Event handler", event, "64-handler registration limit reached");
	}
	struct native_handler *grown =
		realloc(owner->handlers, (owner->handler_count + 1) * sizeof(*grown));
	if (!grown) {
		return registration_error_name(
			owner, "Event handler", event, "cannot allocate registration");
	}
	owner->handlers = grown;
	struct native_handler *slot = &grown[owner->handler_count++];
	*slot = (struct native_handler){.call = handler, .userdata = userdata};
	snprintf(slot->event, sizeof(slot->event), "%s", event);

	return 0;
}

static int handlers_for(const char *event) {

	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->handler_count; j++) {
			if (!strcmp(loaded[i]->handlers[j].event, event)) {
				return 1;
			}
		}
	}

	return 0;
}

static int bus_depth;
static struct extension *handlers_only; /* deliver to this extension alone (tick) */

/* Run every handler for event in load order. A handler's JSON result is
   passed to apply before the next handler sees the payload. */
static int run_handlers(
	const char *event, json_object *payload, event_apply_fn apply, void *state, int *failed) {

	int ran = 0, stop = 0;
	for (size_t i = 0; i < loaded_count && !stop; i++) {
		struct extension *extension = loaded[i];
		if (handlers_only && extension != handlers_only) {
			continue;
		}
		for (size_t j = 0; j < extension->handler_count && !stop; j++) {
			struct native_handler *handler = &extension->handlers[j];
			if (strcmp(handler->event, event)) {
				continue;
			}
			const char *json =
				payload ? json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN) : "{}";
			size_t length = strlen(json);
			size_t capacity =
				length > 16u * 1024u * 1024u ? 32u * 1024u * 1024u : 2 * length + 65536;
			char *out = malloc(capacity);
			if (!out) {
				if (failed) {
					(*failed)++;
				}
				continue;
			}
			out[0] = 0;
			callback_depth++;
			int code = handler->call(handler->userdata, event, json, out, capacity);
			callback_depth--;
			out[capacity - 1] = 0;
			ran++;
			const char *problem = code ? "handler returned an error" : NULL;
			json_object *result = NULL;
			if (!code && *out && apply) {
				result = parse_object_limit(out, capacity - 1);
				if (!result) {
					problem = "handler result is not a JSON object";
				}
			}
			free(out);
			if (problem) {
				if (failed) {
					(*failed)++;
				}
				if (!handler->failing) {
					char line[256];
					snprintf(line, sizeof(line), "%s: %s %s.\n", extension->name, event, problem);
					message(line);
				}
				handler->failing = 1;
				continue;
			}
			handler->failing = 0;
			if (result) {
				stop = apply(payload, result, extension->name, state);
				json_object_put(result);
			}
		}
	}

	return ran;
}

/* Event bus hooks installed into the core (see core/events.h). Events are
   not delivered while an extension callback is running, so work an
   extension triggers itself cannot recurse into handlers. */
static int bus_wanted(const char *event) {

	return !updating && !callback_depth && handlers_for(event);
}

static int bus_blocked(void) {

	return callback_depth > 0 || updating;
}

static double monotonic_seconds(void) {

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	return (double)now.tv_sec + now.tv_nsec / 1e9;
}

/* "tick" for extensions that asked with ui_tick, each at its own interval.
   Only from the UI's idle loop, never inside a callback. */
static void bus_tick(void) {

	if (updating || callback_depth) {
		return;
	}
	double now = 0;
	for (size_t i = 0; i < loaded_count; i++) {
		struct extension *extension = loaded[i];
		if (!extension->tick_ms || extension->shutting_down) {
			continue;
		}
		if (!now) {
			now = monotonic_seconds();
		}
		if (now < extension->tick_next) {
			continue;
		}
		extension->tick_next = now + extension->tick_ms / 1000.0;
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "interval_ms", json_object_new_int(extension->tick_ms));
		handlers_only = extension;
		run_handlers("tick", payload, NULL, NULL, NULL);
		handlers_only = NULL;
		json_object_put(payload);
	}
}

static struct extension *virtual_model_owner(const char *model) {

	for (size_t i = 0; model && i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->virtual_model_count; j++) {
			if (!strcmp(loaded[i]->virtual_models[j].id, model)) {
				return loaded[i];
			}
		}
	}

	return NULL;
}

/* model_route goes to the virtual model's owner alone; a choice of another
   virtual model is refused (the request then goes to the selected id and
   fails, as in Pi). */
static event_apply_fn route_next;

static int route_refuse(
	json_object *payload, json_object *result, const char *extension, void *state) {

	if (virtual_model_owner(field(result, "model")) || !*field(result, "model")) {
		char line[256];
		snprintf(line, sizeof(line), "%s: model_route must choose a real model, not \"%.60s\".\n",
			extension, field(result, "model"));
		message(line);
		return 1;
	}

	return route_next(payload, result, extension, state);
}

static int bus_emit(
	const char *event, json_object *payload, event_apply_fn apply, void *state, int *failed) {

	if (!bus_wanted(event)) {
		return 0;
	}
	if (!strcmp(event, "model_route")) {
		struct extension *owner = virtual_model_owner(field(payload, "model"));
		if (!owner || !apply) {
			return 0;
		}
		route_next = apply;
		handlers_only = owner;
		int ran = run_handlers(event, payload, route_refuse, state, failed);
		handlers_only = NULL;
		return ran;
	}

	return run_handlers(event, payload, apply, state, failed);
}

/* api->emit: extension-to-extension messages on "ext:<name>". Allowed from
   inside callbacks, with bounded nesting. */
static int extension_emit(void *context, const char *name, const char *payload_json) {

	struct extension *owner = context;
	char event[80];
	if (!owner || owner->shutting_down || !name ||
		snprintf(event, sizeof(event), "ext:%s", name) >= (int)sizeof(event) ||
		!event_name_valid(event) || bus_depth >= MAX_BUS_DEPTH) {
		return -1;
	}
	json_object *data = payload_json && *payload_json
		? parse_object_limit(payload_json, 1024u * 1024u)
		: json_object_new_object();
	if (!data) {
		return -1;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "from", json_object_new_string(owner->name));
	json_object_object_add(payload, "data", data);
	bus_depth++;
	int failed = 0;
	run_handlers(event, payload, NULL, NULL, &failed);
	bus_depth--;
	json_object_put(payload);

	return failed ? 1 : 0;
}

/* Write a JSON result into an extension's buffer. Returns 0 on success, 1 if
   the result carries an error or did not fit (then output says how much
   space is needed). */
static int write_result(json_object *result, char *output, size_t capacity) {

	const char *text = json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN);
	size_t length = strlen(text);
	int failed = *field(result, "error") != 0;
	if (length < capacity) {
		memcpy(output, text, length + 1);
		return failed;
	}
	snprintf(
		output, capacity, "{\"error\":\"output capacity too small\",\"needed\":%zu}", length + 1);

	return 1;
}

static int service_error(char *output, size_t capacity, const char *message) {

	json_object *error = result("error", message);
	if (!error) {
		if (capacity) {
			output[0] = 0;
		}
		return 1;
	}
	write_result(error, output, capacity);
	json_object_put(error);

	return 1;
}

/* Shared shape of the JSON-in/JSON-out host services. */
static int host_service(struct extension *owner, json_object *(*service)(json_object *),
	const char *request_json, char *output, size_t capacity) {

	if (!output || !capacity) {
		return 1;
	}
	if (!owner || owner->shutting_down) {
		return service_error(output, capacity, "Host services are unavailable during shutdown");
	}
	if (!service) {
		return service_error(output, capacity, "This host does not provide the service");
	}
	json_object *request = parse_object_limit(request_json, 4u * 1024u * 1024u);
	if (!request) {
		return service_error(output, capacity, "Request must be a JSON object (at most 4 MiB)");
	}
	json_object *answer = service(request);
	json_object_put(request);
	if (!answer) {
		return service_error(output, capacity, "Host service failed");
	}
	int code = write_result(answer, output, capacity);
	json_object_put(answer);

	return code;
}

/* complete and complete_stream: a nested model request; streaming hands
   the reply to the extension's callback as it arrives. */
struct stream_bridge {
	phi_text_v1 on_text;
	void *userdata;
};

static int stream_text(const char *text, void *state) {

	struct stream_bridge *bridge = state;

	return bridge->on_text(bridge->userdata, text);
}

static int complete_with(void *context, const char *request_json, phi_text_v1 on_text,
	void *userdata, char *output, size_t capacity) {

	struct extension *owner = context;
	if (!output || !capacity) {
		return 1;
	}
	if (!owner || owner->shutting_down) {
		return service_error(output, capacity, "Host services are unavailable during shutdown");
	}
	if (!host.complete) {
		return service_error(output, capacity, "This host does not provide the service");
	}
	json_object *request = parse_object_limit(request_json, 4u * 1024u * 1024u);
	if (!request) {
		return service_error(output, capacity, "Request must be a JSON object (at most 4 MiB)");
	}
	struct stream_bridge bridge = {on_text, userdata};
	json_object *answer = host.complete(request, on_text ? stream_text : NULL, &bridge);
	json_object_put(request);
	if (!answer) {
		return service_error(output, capacity, "Host service failed");
	}
	int code = write_result(answer, output, capacity);
	json_object_put(answer);

	return code;
}

static int complete_service(
	void *context, const char *request_json, char *output, size_t capacity) {

	return complete_with(context, request_json, NULL, NULL, output, capacity);
}

static int complete_stream_service(void *context, const char *request_json, phi_text_v1 on_text,
	void *userdata, char *output, size_t capacity) {

	if (!on_text) {
		return service_error(output, capacity, "complete_stream needs a text callback");
	}

	return complete_with(context, request_json, on_text, userdata, output, capacity);
}

/* complete_async: each request's callbacks, on the extension that asked. */
struct async_bridge {
	struct extension *owner;
	phi_text_v1 on_text;
	phi_done_v1 on_done;
	void *userdata;
	int id;
	struct async_bridge *next;
};
static struct async_bridge *async_bridges;

static void async_forget(struct async_bridge *bridge) {

	for (struct async_bridge **at = &async_bridges; *at; at = &(*at)->next) {
		if (*at == bridge) {
			*at = bridge->next;
			free(bridge);
			return;
		}
	}
}

static int async_text(const char *text, void *state) {

	struct async_bridge *bridge = state;
	if (!bridge->on_text || bridge->owner->shutting_down) {
		return 0;
	}
	callback_depth++;
	int stop = bridge->on_text(bridge->userdata, text);
	callback_depth--;

	return stop;
}

static void async_done(json_object *answer, void *state) {

	struct async_bridge *bridge = state;
	if (bridge->on_done && !bridge->owner->shutting_down) {
		const char *json = answer ? json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN)
								  : "{\"error\":\"Model request failed\"}";
		callback_depth++;
		bridge->on_done(bridge->userdata, json);
		callback_depth--;
	}
	async_forget(bridge);
}

static int complete_async_service(void *context, const char *request_json, phi_text_v1 on_text,
	phi_done_v1 on_done, void *userdata, char *output, size_t capacity) {

	struct extension *owner = context;
	if (!output || !capacity) {
		return 0;
	}
	if (!owner || owner->shutting_down) {
		service_error(output, capacity, "Host services are unavailable during shutdown");
		return 0;
	}
	if (!host.complete_async) {
		service_error(output, capacity, "This host does not provide the service");
		return 0;
	}
	json_object *request = parse_object_limit(request_json, 4u * 1024u * 1024u);
	if (!request) {
		service_error(output, capacity, "Request must be a JSON object (at most 4 MiB)");
		return 0;
	}
	struct async_bridge *bridge = calloc(1, sizeof(*bridge));
	if (!bridge) {
		json_object_put(request);
		service_error(output, capacity, "Out of memory");
		return 0;
	}
	*bridge = (struct async_bridge){owner, on_text, on_done, userdata, 0, async_bridges};
	async_bridges = bridge;
	char error[256] = "";
	int id =
		host.complete_async(request, async_text, async_done, bridge, owner, error, sizeof(error));
	json_object_put(request);
	if (id <= 0) {
		/* A host that answered at once already called async_done (and freed
           the bridge); otherwise the request never started. */
		for (struct async_bridge *b = async_bridges; b; b = b->next) {
			if (b == bridge) {
				async_forget(bridge);
				break;
			}
		}
		if (id < 0) {
			snprintf(output, capacity, "{\"done\":true}");
			return -id;
		}
		service_error(output, capacity, *error ? error : "The request could not start");
		return 0;
	}
	bridge->id = id;
	snprintf(output, capacity, "{\"id\":%d}", id);

	return id;
}

static int complete_cancel_service(void *context, int id) {

	struct extension *owner = context;
	for (struct async_bridge *bridge = async_bridges; bridge; bridge = bridge->next) {
		if (bridge->owner == owner && bridge->id == id && id > 0) {
			if (host.complete_cancel) {
				host.complete_cancel(id, NULL);
			}
			async_forget(bridge);
			return 0;
		}
	}

	return 1;
}

/* An unloading extension's requests stop, with no callbacks into it. */
static void async_cancel_owner(struct extension *owner) {

	if (host.complete_cancel) {
		host.complete_cancel(0, owner);
	}
	for (struct async_bridge **at = &async_bridges; *at;) {
		if ((*at)->owner == owner) {
			struct async_bridge *gone = *at;
			*at = gone->next;
			free(gone);
		} else {
			at = &(*at)->next;
		}
	}
}

static int exec_service(void *context, const char *request_json, char *output, size_t capacity) {

	return host_service(context, host.exec, request_json, output, capacity);
}

static struct native_provider *provider_for_model_internal(const char *model);

static int call_service(
	void *context, const char *method, const char *request_json, char *output, size_t capacity) {

	struct extension *owner = context;
	if (!output || !capacity) {
		return 1;
	}
	if (!owner || owner->shutting_down) {
		return service_error(output, capacity, "Host services are unavailable during shutdown");
	}
	if (!host.call) {
		return service_error(output, capacity, "This host does not provide session control");
	}
	if (!method || !*method || strlen(method) > 64) {
		return service_error(output, capacity, "Invalid method name");
	}
	json_object *request =
		parse_object_limit(request_json && *request_json ? request_json : "{}", 4u * 1024u * 1024u);
	if (!request) {
		return service_error(output, capacity, "Request must be a JSON object (at most 4 MiB)");
	}
	if (!strcmp(method, "set_command_options")) {
		/* {name, during_turn}: the command runs at once even while a turn
           runs (Enter otherwise queues it until the turn ends). */
		const char *name = field(request, "name");
		json_object *during = json_object_object_get(request, "during_turn");
		struct native_command *found = NULL;
		for (size_t i = 0; i < owner->command_count; i++) {
			if (!strcmp(owner->commands[i].info.name, name)) {
				found = &owner->commands[i];
			}
		}
		int valid = !during || json_object_is_type(during, json_type_boolean);
		int value = valid && json_object_get_boolean(during);
		json_object_put(request);
		if (!found) {
			return service_error(
				output, capacity, "Register the command first (name includes the slash)");
		}
		if (!valid) {
			return service_error(output, capacity, "during_turn must be true or false");
		}
		found->during_turn = value;
		snprintf(output, capacity, "{}");
		return 0;
	}
	if (!strcmp(method, "ui_status")) {
		/* Keyed status slots (Pi's setStatus): text replaces, empty clears. */
		char key[64], text[MAX_STATUS + 2];
		int valid =
			strlen(field(request, "key")) <= 32 && strlen(field(request, "text")) <= MAX_STATUS;
		snprintf(key, sizeof(key), "%s", field(request, "key"));
		snprintf(text, sizeof(text), "%s", field(request, "text"));
		for (size_t i = 0; valid && text[i]; i++) {
			if ((unsigned char)text[i] < 32 || text[i] == 127) {
				valid = 0;
			}
		}
		size_t slot = MAX_STATUS_SLOTS;
		for (size_t i = 0; valid && i < MAX_STATUS_SLOTS && slot == MAX_STATUS_SLOTS; i++) {
			if (!strcmp(owner->slots[i].key, key) && (*owner->slots[i].text || !*key)) {
				slot = i;
			}
		}
		for (size_t i = 0; valid && *text && i < MAX_STATUS_SLOTS && slot == MAX_STATUS_SLOTS;
			i++) {
			if (!*owner->slots[i].text) {
				slot = i;
			}
		}
		json_object_put(request);
		if (!valid) {
			return service_error(output, capacity,
				"ui_status takes {key, text} with text of at most 64 printable bytes");
		}
		if (slot == MAX_STATUS_SLOTS && *text) {
			return service_error(output, capacity, "At most 4 status slots per extension");
		}
		if (slot < MAX_STATUS_SLOTS) {
			snprintf(owner->slots[slot].key, sizeof(owner->slots[slot].key), "%.32s", key);
			snprintf(owner->slots[slot].text, sizeof(owner->slots[slot].text), "%.64s", text);
			if (!owner->initializing) {
				notify_status();
			}
		}
		json_object *done = json_object_new_object();
		int code = write_result(done, output, capacity);
		json_object_put(done);
		return code;
	}
	if (!strcmp(method, "unregister_tool")) {
		char name[65];
		snprintf(name, sizeof(name), "%s", field(request, "name"));
		json_object_put(request);
		const char *problem = unregister_tool(owner, name);
		if (problem) {
			return service_error(output, capacity, problem);
		}
		json_object *done = json_object_new_object();
		int code = write_result(done, output, capacity);
		json_object_put(done);
		return code;
	}
	if (!strcmp(method, "register_virtual_model") || !strcmp(method, "unregister_virtual_model")) {
		/* Pi's registerVirtualModel: a model id /model can select, routed
           per request by this extension's model_route handler. */
		int adding = !strcmp(method, "register_virtual_model");
		const char *id = field(request, "id"),
				   *name = *field(request, "name") ? field(request, "name") : id;
		size_t length = strlen(id), index = owner->virtual_model_count;
		for (size_t i = 0; i < owner->virtual_model_count; i++) {
			if (!strcmp(owner->virtual_models[i].id, id)) {
				index = i;
			}
		}
		const char *problem = NULL;
		if (!length || length >= sizeof(owner->virtual_models[0].id) ||
			strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/") !=
				length) {
			problem = "a virtual model needs an id of 1-63 letters, digits and -_.:/";
		} else if (!adding && index == owner->virtual_model_count) {
			problem = "no such virtual model";
		} else if (adding && index == owner->virtual_model_count &&
			(virtual_model_owner(id) || provider_for_model_internal(id))) {
			problem = "that model id is taken";
		} else if (adding && index == MAX_VIRTUAL_MODELS) {
			problem = "at most 8 virtual models per extension";
		}
		if (!problem && adding) {
			snprintf(
				owner->virtual_models[index].id, sizeof(owner->virtual_models[index].id), "%s", id);
			snprintf(owner->virtual_models[index].name, sizeof(owner->virtual_models[index].name),
				"%s", name);
			if (index == owner->virtual_model_count) {
				owner->virtual_model_count++;
			}
		} else if (!problem) {
			memmove(&owner->virtual_models[index], &owner->virtual_models[index + 1],
				(owner->virtual_model_count - index - 1) * sizeof(owner->virtual_models[0]));
			owner->virtual_model_count--;
		}
		json_object_put(request);
		if (problem) {
			return service_error(output, capacity, problem);
		}
		json_object *done = json_object_new_object();
		int code = write_result(done, output, capacity);
		json_object_put(done);
		return code;
	}
	if (!strcmp(method, "ui_tick")) {
		/* A periodic "tick" event for this extension: at least 1 s apart; 0 stops. */
		json_object *value = json_object_object_get(request, "interval_ms");
		int64_t ms = json_object_is_type(value, json_type_int) ? json_object_get_int64(value) : -1;
		json_object_put(request);
		if (ms != 0 && (ms < 1000 || ms > 3600000)) {
			return service_error(
				output, capacity, "ui_tick takes {interval_ms}: 0 to stop, or 1000-3600000");
		}
		owner->tick_ms = (int)ms;
		owner->tick_next = ms ? monotonic_seconds() + ms / 1000.0 : 0;
		json_object *done = json_object_new_object();
		int code = write_result(done, output, capacity);
		json_object_put(done);
		return code;
	}
	if (!strncmp(method, "ui_", 3)) {
		/* Dialogs, notices and the rest of the extension UI (phase 5). */
		json_object *answer = host.ui ? host.ui(owner->name, method, request) : NULL;
		json_object_put(request);
		if (!answer) {
			return service_error(
				output, capacity, host.ui ? "UI call failed" : "This host has no UI");
		}
		int code = write_result(answer, output, capacity);
		json_object_put(answer);
		return code;
	}
	if (!strcmp(method, "tool_update")) {
		/* Progress from a running tool: shown in its card and sent as
           tool_execution_update. */
		const char *text = field(request, "text");
		int valid = running_owner == owner && *text;
		if (valid) {
			tool_report(stderr, "%s", text);
		}
		json_object_put(request);
		if (!valid) {
			return service_error(output, capacity,
				"tool_update needs text and only works while this extension's tool runs");
		}
		json_object *done = json_object_new_object();
		int code = write_result(done, output, capacity);
		json_object_put(done);
		return code;
	}
	if (!strcmp(method, "execute_tool")) {
		/* Another tool, from inside this extension's running tool. Events
           are delivered for it as for a model's call (handlers themselves
           still cannot recurse: they run as callbacks). */
		if (running_owner != owner || !host.call) {
			json_object_put(request);
			return service_error(
				output, capacity, "execute_tool only works while this extension's tool runs");
		}
		int depth = callback_depth;
		callback_depth = 0;
		nested_tools++;
		json_object *answer = host.call(owner->name, method, request);
		callback_depth = depth;
		nested_tools--;
		running_owner = owner;
		json_object_put(request);
		if (!answer) {
			return service_error(output, capacity, "Host call failed");
		}
		int code = write_result(answer, output, capacity);
		json_object_put(answer);
		return code;
	}
	json_object *answer = host.call(owner->name, method, request);
	json_object_put(request);
	if (!answer) {
		return service_error(output, capacity, "Host call failed");
	}
	int code = write_result(answer, output, capacity);
	json_object_put(answer);

	return code;
}

static void destroy(struct extension *extension) {

	if (!extension) {
		return;
	}
	struct extension *current = *extension->name ? find_extension(extension->name) : NULL;
	if (host.ui && *extension->name && (!current || current == extension)) {
		/* Its widgets, header and footer go with it (not when a reload replaced it). */
		json_object *empty = json_object_new_object(),
					*done = host.ui(extension->name, "ui_reset", empty);
		json_object_put(empty);
		if (done) {
			json_object_put(done);
		}
	}
	extension->initializing = 0;
	extension->shutting_down = 1;
	async_cancel_owner(extension);
	if (extension->initialized && extension->descriptor && extension->descriptor->shutdown) {
		callback_depth++;
		extension->descriptor->shutdown();
		callback_depth--;
	}
	for (size_t i = 0; i < extension->tool_count; i++) {
		tool_free(&extension->tools[i]);
	}
	for (size_t i = 0; i < extension->command_count; i++) {
		free((char *)extension->commands[i].info.name);
		free((char *)extension->commands[i].info.description);
	}
	for (size_t i = 0; i < extension->provider_count; i++) {
		free(extension->providers[i].name);
		if (extension->providers[i].models) {
			json_object_put(extension->providers[i].models);
		}
	}
	free(extension->handlers);
	if (extension->state) {
		json_object_put(extension->state);
	}
	phi_dynlib_close(extension->library);
	free(extension);
}

static struct extension *stage(
	const char *name, const char *key, const char *artifact, char *error, size_t size) {

	struct extension *next = calloc(1, sizeof(*next));
	if (!next) {
		snprintf(error, size, "Cannot allocate extension");
		return NULL;
	}
	strcpy(next->name, name);
	strcpy(next->key, key);
	next->state = state_load(name, error, size);
	if (!next->state) {
		goto failed;
	}
	next->library = phi_dynlib_open(artifact, error, size);
	if (!next->library) {
		goto failed;
	}
	next->descriptor = phi_dynlib_symbol(next->library, "phi_extension", error, size);
	const struct phi_extension_v1 *descriptor = next->descriptor;
	if (!descriptor) {
		goto failed;
	}
	if (descriptor->abi_version != PHI_EXTENSION_ABI_V1 ||
		descriptor->size != sizeof(*descriptor)) {
		snprintf(error, size, "Incompatible extension ABI/descriptor size");
		goto failed;
	}
	if (!descriptor->name || strcmp(descriptor->name, name) || !descriptor->init) {
		snprintf(error, size, "Descriptor name must match source basename, and init is required");
		goto failed;
	}
	next->api = (struct phi_api_v1){.abi_version = PHI_EXTENSION_ABI_V1,
		.size = sizeof(next->api),
		.context = next,
		.register_tool = register_tool,
		.register_command = register_command,
		.register_hook = register_hook,
		.register_tool_interceptor = register_tool_interceptor,
		.register_context_provider = register_context_provider,
		.register_turn_message_provider = register_turn_message_provider,
		.register_provider = register_provider,
		.wrap_provider = wrap_provider,
		.credential_get = credential_get,
		.credential_set = credential_set,
		.credential_delete = credential_delete,
		.http_request = http_request,
		.open_url = open_url,
		.complete = complete_service,
		.complete_stream = complete_stream_service,
		.exec = exec_service,
		.complete_async = complete_async_service,
		.complete_cancel = complete_cancel_service,
		.register_cli_command = register_cli_command,
		.http_request_ex = http_request_ex,
		.on = register_handler,
		.emit = extension_emit,
		.call = call_service,
		.register_tool_ex = register_tool_ex,
		.message = message,
		.cwd = cwd,
		.cancelled = cancelled,
		.state_get = extension_state_get,
		.state_set = extension_state_set,
		.status_set = extension_status_set};
	next->initializing = next->initialized = 1;
	callback_depth++;
	int code = descriptor->init(&next->api);
	callback_depth--;
	next->initializing = 0;
	if (code || *next->error) {
		snprintf(
			error, size, "%s", *next->error ? next->error : "Extension init returned an error");
		goto failed;
	}
	return next;
failed:
	destroy(next);

	return NULL;
}

struct scan {
	char names[MAX_EXTENSIONS][49];
	size_t count;
	int incomplete;
	char error[256];
};

static int compare_names(const void *a, const void *b) {

	return strcmp(a, b);
}

static struct scan scan_sources(void) {

	struct scan scan = {0};
	if (!source_directory) {
		return scan;
	}
	if (!extension_directory(source_directory, 0, 0, scan.error, sizeof(scan.error))) {
		if (errno == ENOENT) {
			scan.error[0] = 0;
		}
		return scan;
	}
	DIR *directory = opendir(source_directory);
	if (!directory) {
		snprintf(scan.error, sizeof(scan.error), "Cannot open extensions directory: %s",
			strerror(errno));
		return scan;
	}
	size_t examined = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(directory);
		if (!entry) {
			if (errno) {
				snprintf(scan.error, sizeof(scan.error), "Incomplete extension scan: %s",
					strerror(errno));
			}
			break;
		}
		if (++examined > MAX_SCAN_ENTRIES) {
			scan.incomplete = 1;
			break;
		}
		size_t length = strlen(entry->d_name);
		if (length <= 2 || length > 50 || strcmp(entry->d_name + length - 2, ".c")) {
			continue;
		}
		char name[49];
		memcpy(name, entry->d_name, length - 2);
		name[length - 2] = 0;
		if (!extension_name_valid(name)) {
			continue;
		}
		if (scan.count == MAX_EXTENSIONS) {
			scan.incomplete = 1;
			break;
		}
		strcpy(scan.names[scan.count++], name);
	}
	closedir(directory);
	qsort(scan.names, scan.count, sizeof(*scan.names), compare_names);

	return scan;
}

static json_object *load_one(const char *name, int interactive) {

	char error[512] = "";
	if (!source_directory || !cache_directory) {
		return result("error", "Extension paths are unavailable");
	}
	if (!extension_name_valid(name)) {
		return result("error", "Invalid extension name");
	}
	if (!extension_directory(source_directory, 0, 0, error, sizeof(error))) {
		return result("error", error);
	}
	if (!interactive && extension_disabled(cache_directory, name)) {
		return result("status", "disabled");
	}
	struct extension_source source;
	if (!extension_source_read(source_directory, name, &source, error, sizeof(error))) {
		return result("error", error);
	}
	struct extension *old = find_extension(name), *next = NULL;
	char *artifact = NULL;
	struct phi_compile_result *compile = NULL;
	json_object *answer = NULL;
	if (old && !strcmp(old->key, source.key)) {
		answer = result("status", "unchanged");
		goto done;
	}
	if (!old && loaded_count == MAX_EXTENSIONS) {
		answer = result("error", "32 loaded-extension limit reached");
		goto done;
	}
	int cached =
		extension_cached(cache_directory, name, source.key, &artifact, error, sizeof(error));
	char binary_hash[65] = "";
	if (!cached) {
		if (!interactive) {
			answer = result("status", "pending approval");
			goto done;
		}
		size_t n = strlen(source_directory) + strlen(name) + 4;
		char *path = malloc(n);
		if (path) {
			snprintf(path, n, "%s/%s.c", source_directory, name);
		}
		int permitted = path && host.confirm_native && host.confirm_native(name, path, source.key);
		free(path);
		if (!permitted) {
			answer =
				result("error", "Native extension not approved; previous version remains loaded");
			goto done;
		}
		if (!extension_source_matches(source_directory, name, source.key)) {
			answer = result("error", "Source changed during approval; reload and review again");
			goto done;
		}
		compile = calloc(1, sizeof(*compile));
		if (!compile) {
			answer = result("error", "Cannot allocate compiler diagnostics");
			goto done;
		}
		const char *compiler = getenv("PHI_EXTENSION_CC");
		if (!compiler || !*compiler) {
			compiler = "/usr/bin/cc";
		}
		error[0] = 0;
		if (!extension_build(cache_directory, name, &source, compiler, cancelled, &artifact,
				binary_hash, compile, error, sizeof(error))) {
			answer = result("error", error);
			goto done;
		}
	}
	if (!extension_source_matches(source_directory, name, source.key)) {
		answer =
			result("error", "Source changed during compilation; previous version remains loaded");
		goto done;
	}
	next = stage(name, source.key, artifact, error, sizeof(error));
	if (!next) {
		answer = result("error", error);
		goto done;
	}
	if (!extension_source_matches(source_directory, name, source.key)) {
		answer = result(
			"error", "Source changed during initialization; previous version remains loaded");
		goto done;
	}
	if ((!cached &&
			!extension_approve(
				cache_directory, name, source.key, binary_hash, error, sizeof(error))) ||
		(interactive && !extension_set_disabled(cache_directory, name, 0, error, sizeof(error)))) {
		answer = result("error", error);
		goto done;
	}
	/* All fallible work is complete. Callbacks cannot be executing here. Old
       state is released only after new registration and approval succeeded. */
	if (old) {
		for (size_t i = 0; i < loaded_count; i++) {
			if (loaded[i] == old) {
				loaded[i] = next;
			}
		}
		destroy(old);
	} else {
		loaded[loaded_count++] = next;
	}
	publish_cli_names(next);
	next = NULL;
	notify_status();
	answer = result("status", cached ? "loaded from cache" : "compiled and loaded");
done:
	destroy(next);
	if (compile) {
		if (answer) {
			json_object_object_add(
				answer, "compiler_output", json_object_new_string(compile->text));
			json_object_object_add(
				answer, "diagnostics_truncated", json_object_new_boolean(compile->truncated));
		}
		free(compile);
	}
	free(artifact);
	free(source.text);
	if (answer) {
		json_object_object_add(answer, "name", json_object_new_string(name));
	}

	return answer;
}

static json_object *list_extensions(void) {

	struct scan scan = scan_sources();
	json_object *answer = result("source_directory", source_directory);
	if (!answer) {
		return NULL;
	}
	json_object_object_add(
		answer, "cache_directory", json_object_new_string(cache_directory ? cache_directory : ""));
	json_object_object_add(
		answer, "state_directory", json_object_new_string(state_directory ? state_directory : ""));
	json_object *sources = json_object_new_array(), *active = json_object_new_array();
	if (!sources || !active) {
		if (sources) {
			json_object_put(sources);
		}
		if (active) {
			json_object_put(active);
		}
		json_object_put(answer);
		return NULL;
	}
	for (size_t i = 0; i < scan.count; i++) {
		struct extension_source source;
		char error[256];
		struct extension *current = find_extension(scan.names[i]);
		int readable =
			extension_source_read(source_directory, scan.names[i], &source, error, sizeof(error));
		const char *status = !readable                           ? "unreadable"
			: extension_disabled(cache_directory, scan.names[i]) ? "disabled"
			: current && !strcmp(current->key, source.key)       ? "loaded"
			: current                                            ? "changed (old version loaded)"
																 : "not loaded";
		json_object *item = result("name", scan.names[i]);
		json_object_object_add(item, "status", json_object_new_string(status));
		if (current && *current->status) {
			json_object_object_add(item, "footer_status", json_object_new_string(current->status));
		}
		if (current && current->interceptor) {
			json_object_object_add(item, "tool_interceptor", json_object_new_boolean(1));
		}
		if (current && current->context_provider) {
			json_object_object_add(item, "context_provider", json_object_new_boolean(1));
		}
		if (current && current->turn_message_provider) {
			json_object_object_add(item, "turn_message_provider", json_object_new_boolean(1));
			json_object_object_add(
				item, "turn_message_type", json_object_new_string(current->turn_message_type));
		}
		if (current && current->provider_count) {
			json_object *providers = json_object_new_array();
			for (size_t p = 0; p < current->provider_count; p++) {
				json_object *provider = json_object_new_object();
				json_object_object_add(
					provider, "id", json_object_new_string(current->providers[p].id));
				json_object_object_add(
					provider, "name", json_object_new_string(current->providers[p].name));
				json_object_object_add(
					provider, "models", json_object_get(current->providers[p].models));
				json_object_array_add(providers, provider);
			}
			json_object_object_add(item, "providers", providers);
		}
		if (current && current->virtual_model_count) {
			json_object *models = json_object_new_array();
			for (size_t v = 0; v < current->virtual_model_count; v++) {
				json_object *model = json_object_new_object();
				json_object_object_add(
					model, "id", json_object_new_string(current->virtual_models[v].id));
				json_object_object_add(
					model, "name", json_object_new_string(current->virtual_models[v].name));
				json_object_array_add(models, model);
			}
			json_object_object_add(item, "virtual_models", models);
		}
		if (current && current->handler_count) {
			json_object *events = json_object_new_array();
			for (size_t h = 0; h < current->handler_count; h++) {
				json_object_array_add(events, json_object_new_string(current->handlers[h].event));
			}
			json_object_object_add(item, "events", events);
		}
		if (current && current->provider_wrapper_count) {
			json_object_object_add(item, "provider_wrappers",
				json_object_new_int64((int64_t)current->provider_wrapper_count));
		}
		if (!readable) {
			json_object_object_add(item, "error", json_object_new_string(error));
		}
		json_object_array_add(sources, item);
		free(source.text);
	}
	for (size_t i = 0; i < loaded_count; i++) {
		json_object_array_add(active, json_object_new_string(loaded[i]->name));
	}
	json_object_object_add(answer, "sources", sources);
	json_object_object_add(answer, "loaded", active);
	json_object_object_add(
		answer, "scan_incomplete", json_object_new_boolean(scan.incomplete || *scan.error));
	if (*scan.error) {
		json_object_object_add(answer, "error", json_object_new_string(scan.error));
	}
	if (scan.incomplete) {
		json_object_object_add(answer, "notice",
			json_object_new_string(
				"Scan limited to 32 sources/1024 entries. Use a named reload for another source."));
	}

	return answer;
}

static json_object *reload(const char *name, int interactive) {

	if (callback_depth || updating || nested_tools) {
		return result("error", "Cannot reload during an extension callback");
	}
	updating = 1;
	if (interactive && host.activity) {
		host.activity(1);
	}
	json_object *answer;
	if (name && *name) {
		answer = load_one(name, interactive);
	} else {
		struct scan scan = scan_sources();
		answer = result("status", "reload finished");
		json_object *items = json_object_new_array();
		for (size_t i = 0; i < scan.count; i++) {
			/* A named reload re-enables; a bulk reload respects disabled entries. */
			if (extension_disabled(cache_directory, scan.names[i])) {
				json_object_array_add(items, result("disabled", scan.names[i]));
			} else {
				json_object_array_add(items, load_one(scan.names[i], interactive));
			}
		}
		json_object_object_add(answer, "results", items);
		if (scan.incomplete) {
			json_object_object_add(answer, "notice",
				json_object_new_string(
					"Extension scan incomplete (32 sources/1024 entries); use a named reload."));
		}
		if (*scan.error) {
			json_object_object_add(answer, "error", json_object_new_string(scan.error));
		}
	}
	if (interactive && host.activity) {
		host.activity(0);
	}
	updating = 0;

	return answer;
}

static const char *cli_startup_owner;

void extensions_open_cli(const struct extension_host *value, const char *command) {

	char path[4096];
	if (!phi_xdg_path(path, sizeof(path), "XDG_STATE_HOME", ".local/state",
			"phi/extensions/.cli-index.json")) {
		return;
	}
	char *text = phi_read_file(path, 128 * 1024, PHI_READ_OWNED, NULL);
	json_object *index = text ? json_tokener_parse(text) : NULL;
	free(text);
	const char *name = field(index, command);
	char *owner = extension_name_valid(name) ? strdup(name) : NULL;
	json_object_put(index);
	if (!owner) {
		return;
	}
	cli_startup_owner = owner;
	extensions_open(value);
	cli_startup_owner = NULL;
	free(owner);
}

void extensions_open(const struct extension_host *value) {

	extensions_close();
	if (value) {
		host = *value;
	}
	static const struct event_bus extension_bus = {
		.wanted = bus_wanted, .emit = bus_emit, .blocked = bus_blocked, .tick = bus_tick};
	events_set_bus(&extension_bus);
	const char *disabled = getenv("PHI_NO_EXTENSIONS");
	if (disabled && !strcmp(disabled, "1")) {
		message("Native extensions disabled by PHI_NO_EXTENSIONS=1.\n");
		return;
	}
	char error[256];
	if (!extension_paths(&source_directory, &cache_directory, error, sizeof(error)) ||
		!extension_state_directory(&state_directory, error, sizeof(error))) {
		message(error);
		free(source_directory);
		free(cache_directory);
		free(state_directory);
		source_directory = cache_directory = state_directory = NULL;
		return;
	}
	json_object *loaded_result =
		reload(cli_startup_owner, 0); /* Never compile or ask at startup. */
	if (loaded_result) {
		const char *error_text = field(loaded_result, "error");
		if (*error_text) {
			char line[512];
			snprintf(line, sizeof(line), "%s\n", error_text);
			message(line);
		}
		json_object *items = json_object_object_get(loaded_result, "results");
		for (size_t i = 0; items && i < json_object_array_length(items); i++) {
			json_object *item = json_object_array_get_idx(items, i);
			const char *item_error = field(item, "error");
			if (*item_error || !strcmp(field(item, "status"), "pending approval")) {
				char line[768];
				snprintf(line, sizeof(line), "%s: %s\n", field(item, "name"),
					*item_error ? item_error : "pending approval; use /reload-extensions");
				message(line);
			}
		}
		const char *notice = field(loaded_result, "notice");
		if (*notice) {
			char line[512];
			snprintf(line, sizeof(line), "%s\n", notice);
			message(line);
		}
		json_object_put(loaded_result);
	}
}

void extensions_close(void) {

	if (callback_depth || updating) {
		return;
	}
	updating = 1;
	while (loaded_count) {
		destroy(loaded[--loaded_count]);
	}
	notify_status();
	free(source_directory);
	free(cache_directory);
	free(state_directory);
	source_directory = cache_directory = state_directory = NULL;
	events_set_bus(NULL);
	host = (struct extension_host){0};
	updating = 0;
}

json_object *extensions_manage(json_object *arguments) {

	if (!json_object_is_type(arguments, json_type_object)) {
		return result("error", "Expected an extension request object");
	}
	const char *action = field(arguments, "action"), *name = field(arguments, "name");
	json_object *a = json_object_object_get(arguments, "action");
	if (!a || !json_object_is_type(a, json_type_string) ||
		strlen(action) != (size_t)json_object_get_string_len(a)) {
		return result("error", "Expected an action string");
	}
	json_object *n = json_object_object_get(arguments, "name");
	if (n &&
		(!json_object_is_type(n, json_type_string) ||
			strlen(name) != (size_t)json_object_get_string_len(n))) {
		return result("error", "Invalid extension name");
	}
	if (!strcmp(action, "list")) {
		return list_extensions();
	}
	if (!strcmp(action, "api")) {
		json_object *answer = list_extensions();
		if (answer) {
			json_object_object_add(answer, "header", json_object_new_string(phi_extension_sdk));
			json_object_object_add(answer, "guide", json_object_new_string(phi_extension_guide));
			json_object_object_add(answer, "contract",
				json_object_new_string(
					"Use the returned guide and exact header; do not guess the ABI. Single C source per extension, no local header dependencies. "
					"Export phi_extension; descriptor name equals filename stem. Call reload with name after writing the source. "
					"New/changed native code always requires human approval, even with /approval off. "
					"Trusted native callbacks run in Phi, outside the built-in sandbox; a crash can terminate Phi. "
					"Use state_get/state_set for small durable string state and status_set for a short footer contribution. "
					"Use register_turn_message_provider for persisted model-visible/UI-hidden per-user-turn context; use dynamic context only for live state. "
					"Tool interceptors can allow/deny/rewrite model tool arguments before approval; context providers add bounded dynamic system context."));
		}
		return answer;
	}
	if (!source_directory || !cache_directory) {
		return result("error", "Extension paths are unavailable");
	}
	if (!strcmp(action, "reload")) {
		return reload(name, 1);
	}
	if (!strcmp(action, "disable")) {
		if (callback_depth || updating) {
			return result("error", "Cannot disable during a callback");
		}
		if (!extension_name_valid(name)) {
			return result("error", "disable requires an extension name");
		}
		char error[256];
		if (!extension_set_disabled(cache_directory, name, 1, error, sizeof(error))) {
			return result("error", error);
		}
		updating = 1;
		for (size_t i = 0; i < loaded_count; i++) {
			if (!strcmp(loaded[i]->name, name)) {
				struct extension *old = loaded[i];
				memmove(loaded + i, loaded + i + 1, (--loaded_count - i) * sizeof(*loaded));
				destroy(old);
				break;
			}
		}
		updating = 0;
		notify_status();
		return result("disabled", name);
	}

	return result("error", "Expected action list, api, reload or disable");
}

static const struct phi_tool manager_tool = {.name = "extensions", .detail_key = "action"};
static const char manager_schema[] =
	"{\"type\":\"function\",\"name\":\"extensions\",\"description\":\"List native C extensions, read their SDK (api), reload a source, or disable it. New native code requires human approval.\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"api\",\"reload\",\"disable\"]},\"name\":{\"type\":[\"string\",\"null\"]}},\"required\":[\"action\"],\"additionalProperties\":false}}";

const struct phi_tool *extensions_tool_find(const char *name) {

	if (!strcmp(name, "extensions")) {
		return &manager_tool;
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->tool_count; j++) {
			if (!strcmp(loaded[i]->tools[j].info.name, name)) {
				return &loaded[i]->tools[j].info;
			}
		}
	}

	return NULL;
}

int extensions_append_schema(json_object *array) {

	static json_object *schema;
	if (!schema) {
		schema = json_tokener_parse(manager_schema);
	}
	if (!schema || json_object_array_add(array, json_object_get(schema))) {
		return 0;
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->tool_count; j++) {
			if (json_object_array_add(array, json_object_get(loaded[i]->tools[j].schema))) {
				return 0;
			}
		}
	}

	return 1;
}

static json_object *invoke(phi_extension_call_v1 call, void *userdata, const char *input) {

	char *output = calloc(1, OUTPUT_SIZE);
	if (!output) {
		return result("error", "Cannot allocate extension result");
	}
	callback_depth++;
	int code = call(userdata, input, output, OUTPUT_SIZE);
	callback_depth--;
	json_object *answer = memchr(output, 0, OUTPUT_SIZE)
		? result(code ? "error" : "output", output)
		: result("error", "Extension did not terminate its output buffer");
	free(output);

	return answer;
}

/* Structured result: {content, details, display, is_error, terminate,
   image_url}. content goes to the model; the rest is kept under private
   _phi_* keys that Phi strips before the model sees the result. Output
   that is not a JSON object is taken as plain content. */
static json_object *invoke_structured(struct native_tool *tool, const char *input) {

	char *output = malloc(STRUCTURED_OUTPUT);
	if (!output) {
		return result("error", "Cannot allocate extension result");
	}
	output[0] = 0;
	callback_depth++;
	int code = tool->call(tool->userdata, input, output, STRUCTURED_OUTPUT);
	callback_depth--;
	if (!memchr(output, 0, STRUCTURED_OUTPUT)) {
		free(output);
		return result("error", "Extension did not terminate its output buffer");
	}
	json_object *reply = parse_object_limit(output, STRUCTURED_OUTPUT - 1);
	json_object *answer;
	if (!reply) {
		answer = result(code ? "error" : "output", output);
	} else {
		json_object *content = json_object_object_get(reply, "content");
		/* A reply without "content" is passed on whole rather than dropped. */
		const char *text = json_object_is_type(content, json_type_string)
			? json_object_get_string(content)
			: content ? json_object_to_json_string_ext(content, JSON_C_TO_STRING_PLAIN)
					  : output;
		int failed = code || json_object_get_boolean(json_object_object_get(reply, "is_error"));
		answer = result(failed ? "error" : "output", text);
		json_object *details = json_object_object_get(reply, "details");
		if (details) {
			json_object_object_add(answer, "_phi_details", json_object_get(details));
		}
		json_object *display = json_object_object_get(reply, "display");
		if (json_object_is_type(display, json_type_string)) {
			json_object_object_add(answer, "_phi_display", json_object_get(display));
		}
		if (json_object_get_boolean(json_object_object_get(reply, "terminate"))) {
			json_object_object_add(answer, "_phi_terminate", json_object_new_boolean(1));
		}
		const char *image = field(reply, "image_url");
		if (!strncmp(image, "data:image/", 11)) {
			json_object_object_add(answer, "image_url", json_object_new_string(image));
		}
		json_object_put(reply);
	}
	free(output);

	return answer;
}

json_object *extensions_execute(const char *name, json_object *arguments) {

	if (!strcmp(name, "extensions")) {
		return extensions_manage(arguments);
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->tool_count; j++) {
			struct native_tool *tool = &loaded[i]->tools[j];
			if (strcmp(tool->info.name, name)) {
				continue;
			}
			const char *input = json_object_to_json_string_ext(arguments, JSON_C_TO_STRING_PLAIN);
			struct extension *outer =
				running_owner; /* a tool that called this one (execute_tool) */
			running_owner = loaded[i];
			json_object *answer = tool->structured ? invoke_structured(tool, input)
												   : invoke(tool->call, tool->userdata, input);
			running_owner = outer;
			return answer;
		}
	}

	return result("error", "Extension tool is no longer loaded");
}

/* Prompt snippets and guidelines of active extension tools, for the system prompt. */
static void append_tool_notes(char **combined, size_t *used) {

	char notes[MAX_TOOL_NOTES];
	size_t length = 0;
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->tool_count; j++) {
			struct native_tool *tool = &loaded[i]->tools[j];
			if ((!tool->snippet && !tool->guidelines) || !tool_is_active(tool->info.name)) {
				continue;
			}
			int n = snprintf(notes + length, sizeof(notes) - length, "- %s%s%s\n", tool->info.name,
				tool->snippet ? ": " : "", tool->snippet ? tool->snippet : "");
			if (n < 0 || (size_t)n >= sizeof(notes) - length) {
				break;
			}
			length += (size_t)n;
			for (size_t g = 0; tool->guidelines && g < json_object_array_length(tool->guidelines);
				g++) {
				n = snprintf(notes + length, sizeof(notes) - length, "  - %s\n",
					json_object_get_string(json_object_array_get_idx(tool->guidelines, g)));
				if (n < 0 || (size_t)n >= sizeof(notes) - length) {
					break;
				}
				length += (size_t)n;
			}
		}
	}
	if (!length) {
		return;
	}
	char *next = realloc(*combined, *used + length + 32);
	if (!next) {
		return;
	}
	*combined = next;
	*used += (size_t)snprintf(next + *used, length + 32, "[Tool notes]\n%.*s", (int)length, notes);
}

size_t extensions_command_count(void) {

	size_t count = 0;
	for (size_t i = 0; i < loaded_count; i++) {
		count += loaded[i]->command_count;
	}

	return count;
}

const struct phi_command *extensions_command_at(size_t index) {

	for (size_t i = 0; i < loaded_count; i++) {
		if (index < loaded[i]->command_count) {
			return &loaded[i]->commands[index].info;
		}
		index -= loaded[i]->command_count;
	}

	return NULL;
}

int extensions_command_during_turn(const char *text, char *output, size_t size) {

	size_t length = strcspn(text, " \t\r\n");
	if (output && size) {
		*output = 0;
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->command_count; j++) {
			struct native_command *command = &loaded[i]->commands[j];
			if (!command->during_turn) {
				continue;
			}
			if (strlen(command->info.name) != length || strncmp(text, command->info.name, length)) {
				continue;
			}
			const char *input = text + length;
			input += strspn(input, " \t\r\n");
			if (!command->info.argument && *input) {
				snprintf(output, size, "This extension command takes no arguments.");
				return 1;
			}
			json_object *answer = invoke(command->call, command->userdata, input);
			if (answer) {
				const char *error = field(answer, "error");
				snprintf(output, size, "%s", *error ? error : field(answer, "output"));
				json_object_put(answer);
			}
			return 1;
		}
	}

	return 0;
}

int extensions_command(const char *text) {

	size_t length = strcspn(text, " \t\r\n");
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->command_count; j++) {
			struct native_command *command = &loaded[i]->commands[j];
			if (strlen(command->info.name) != length || strncmp(text, command->info.name, length)) {
				continue;
			}
			const char *input = text + length;
			input += strspn(input, " \t\r\n");
			if (!command->info.argument && *input) {
				message("This extension command takes no arguments.\n");
				return 1;
			}
			if (host.activity) {
				host.activity(1);
			}
			json_object *answer = invoke(command->call, command->userdata, input);
			if (host.activity) {
				host.activity(0);
			}
			if (answer) {
				const char *error = field(answer, "error");
				const char *body = *error ? error : field(answer, "output");
				size_t body_length = strlen(body);
				char *line =
					body_length ? malloc(body_length + 2) : NULL; /* nothing said: no line */
				if (line) {
					memcpy(line, body, body_length);
					line[body_length] = '\n';
					line[body_length + 1] = 0;
					message(line);
					free(line);
				} else if (body_length) {
					message(body);
				}
				json_object_put(answer);
			}
			return 1;
		}
	}

	return 0;
}

static int clean_text(const char *source, char *destination, size_t capacity) {

	if (!destination || !capacity) {
		return 0;
	}
	size_t used = 0;
	for (size_t i = 0; source && source[i]; i++) {
		unsigned char c = (unsigned char)source[i];
		if ((c < 32 && c != '\n' && c != '\t') || c == 127) {
			return 0;
		}
		if (used + 1 >= capacity) {
			return 0;
		}
		destination[used++] = (char)c;
	}
	destination[used] = 0;

	return 1;
}

static json_object *clone_call_with_arguments(json_object *call, const char *arguments) {

	json_object *copy = json_object_new_object();
	if (!copy) {
		return NULL;
	}
	json_object_object_foreach(call, key, value) {
		json_object *next =
			!strcmp(key, "arguments") ? json_object_new_string(arguments) : json_object_get(value);
		if (!next) {
			json_object_put(copy);
			return NULL;
		}
		json_object_object_add(copy, key, next);
	}

	return copy;
}

int extensions_intercept_tool(
	json_object *call, json_object **effective, char *note, size_t note_size) {

	if (effective) {
		*effective = NULL;
	}
	if (note && note_size) {
		note[0] = 0;
	}
	if (updating || callback_depth || !loaded_count) {
		return 1;
	}
	const char *name = field(call, "name"), *arguments = field(call, "arguments");
	if (!*name || !*arguments) {
		return 1;
	}

	char *output = calloc(1, MAX_INTERCEPT_OUTPUT);
	if (!output) {
		if (note && note_size) {
			snprintf(note, note_size, "Cannot allocate extension interceptor output");
		}
		return 0;
	}
	json_object *current = NULL;
	int changed = 0, allowed = 1;
	for (size_t i = 0; i < loaded_count; i++) {
		struct extension *extension = loaded[i];
		if (!extension->interceptor) {
			continue;
		}
		const char *current_args = current ? field(current, "arguments") : arguments;
		memset(output, 0, MAX_INTERCEPT_OUTPUT);
		callback_depth++;
		int action = extension->interceptor(
			extension->interceptor_userdata, name, current_args, output, MAX_INTERCEPT_OUTPUT);
		callback_depth--;
		output[MAX_INTERCEPT_OUTPUT - 1] = 0;
		if (action == PHI_TOOL_INTERCEPT_ALLOW) {
			continue;
		}
		if (action == PHI_TOOL_INTERCEPT_DENY) {
			char reason[384];
			if (!*output || !clean_text(output, reason, sizeof(reason))) {
				snprintf(reason, sizeof(reason), "blocked without a valid reason");
			}
			if (note && note_size) {
				snprintf(note, note_size, "Blocked by extension %s: %s", extension->name, reason);
			}
			allowed = 0;
			break;
		}
		if (action == PHI_TOOL_INTERCEPT_REPLACE) {
			json_object *replacement = parse_object_limit(output, MAX_INTERCEPT_OUTPUT - 1);
			if (!replacement) {
				if (note && note_size) {
					snprintf(note, note_size, "Extension %s produced invalid replacement arguments",
						extension->name);
				}
				allowed = 0;
				break;
			}
			const char *encoded =
				json_object_to_json_string_ext(replacement, JSON_C_TO_STRING_PLAIN);
			json_object *next = clone_call_with_arguments(current ? current : call, encoded);
			json_object_put(replacement);
			if (!next) {
				if (note && note_size) {
					snprintf(note, note_size, "Cannot allocate arguments rewritten by extension %s",
						extension->name);
				}
				allowed = 0;
				break;
			}
			if (current) {
				json_object_put(current);
			}
			current = next;
			changed = 1;
			if (note && note_size) {
				size_t used = strlen(note);
				snprintf(note + used, note_size > used ? note_size - used : 0,
					"%sArguments modified by extension %s", used ? "; " : "", extension->name);
			}
			continue;
		}
		if (note && note_size) {
			snprintf(note, note_size, "Extension %s interceptor returned invalid action %d",
				extension->name, action);
		}
		allowed = 0;
		break;
	}
	free(output);
	if ((changed || !allowed) && note && *note) {
		char line[640];
		snprintf(line, sizeof(line), "Tool interception: %s\n", note);
		message(line);
	}
	if (!allowed) {
		if (current) {
			json_object_put(current);
		}
		return 0;
	}
	if (changed && effective) {
		*effective = current;
	} else if (current) {
		json_object_put(current);
	}

	return 1;
}

char *extensions_context(void) {

	if (updating || callback_depth || !loaded_count) {
		return NULL;
	}
	char *combined = NULL;
	size_t used = 0;
	for (size_t i = 0; i < loaded_count; i++) {
		struct extension *extension = loaded[i];
		if (!extension->context_provider) {
			continue;
		}
		char *fragment = calloc(1, MAX_CONTEXT_FRAGMENT + 1);
		if (!fragment) {
			break;
		}
		callback_depth++;
		int code = extension->context_provider(
			extension->context_userdata, fragment, MAX_CONTEXT_FRAGMENT + 1);
		callback_depth--;
		fragment[MAX_CONTEXT_FRAGMENT] = 0;
		char clean[MAX_CONTEXT_FRAGMENT + 1];
		int valid = !code && clean_text(fragment, clean, sizeof(clean));
		free(fragment);
		if (!valid) {
			if (!extension->context_failed) {
				char line[160];
				snprintf(line, sizeof(line),
					"%s: dynamic context provider failed; context omitted.\n", extension->name);
				message(line);
			}
			extension->context_failed = 1;
			continue;
		}
		extension->context_failed = 0;
		if (!*clean) {
			continue;
		}
		size_t length = strlen(clean), heading = strlen(extension->name) + 17;
		if (used + heading + length + 2 > MAX_CONTEXT_TOTAL) {
			if (!extension->context_failed) {
				char line[160];
				snprintf(line, sizeof(line),
					"%s: dynamic context omitted because the aggregate limit was reached.\n",
					extension->name);
				message(line);
			}
			extension->context_failed = 1;
			continue;
		}
		char *next = realloc(combined, used + heading + length + 2);
		if (!next) {
			free(combined);
			return NULL;
		}
		combined = next;
		int n = snprintf(combined + used, heading + 1, "[Extension %s]\n", extension->name);
		if (n < 0) {
			free(combined);
			return NULL;
		}
		used += (size_t)n;
		memcpy(combined + used, clean, length);
		used += length;
		combined[used++] = '\n';
		combined[used] = 0;
	}
	append_tool_notes(&combined, &used);

	return combined;
}

size_t extensions_append_turn_messages(json_object *history, const char *user_message) {

	if (updating || callback_depth || !loaded_count || !history || !user_message) {
		return 0;
	}
	size_t appended = 0, total = 0;
	for (size_t i = 0; i < loaded_count; i++) {
		struct extension *extension = loaded[i];
		if (!extension->turn_message_provider) {
			continue;
		}
		char *fragment = calloc(1, MAX_TURN_MESSAGE + 1);
		if (!fragment) {
			break;
		}
		callback_depth++;
		int code = extension->turn_message_provider(
			extension->turn_message_userdata, user_message, fragment, MAX_TURN_MESSAGE + 1);
		callback_depth--;
		fragment[MAX_TURN_MESSAGE] = 0;
		char clean[MAX_TURN_MESSAGE + 1];
		int valid = !code && clean_text(fragment, clean, sizeof(clean));
		free(fragment);
		if (!valid) {
			if (!extension->turn_message_failed) {
				char line[192];
				snprintf(line, sizeof(line),
					"%s: turn-message provider failed; hidden message omitted.\n", extension->name);
				message(line);
			}
			extension->turn_message_failed = 1;
			continue;
		}
		extension->turn_message_failed = 0;
		if (!*clean) {
			continue;
		}
		size_t length = strlen(clean);
		if (length > MAX_TURN_MESSAGE || total + length > MAX_TURN_MESSAGE_TOTAL) {
			if (!extension->turn_message_failed) {
				char line[192];
				snprintf(line, sizeof(line),
					"%s: hidden turn message omitted because the aggregate limit was reached.\n",
					extension->name);
				message(line);
			}
			extension->turn_message_failed = 1;
			continue;
		}
		if (!history_add_hidden_user(
				history, clean, extension->name, extension->turn_message_type)) {
			if (!extension->turn_message_failed) {
				char line[192];
				snprintf(line, sizeof(line), "%s: could not append hidden turn message.\n",
					extension->name);
				message(line);
			}
			extension->turn_message_failed = 1;
			continue;
		}
		total += length;
		appended++;
	}

	return appended;
}

void extensions_event(const char *event, json_object *payload) {

	if (updating || callback_depth || !loaded_count) {
		return;
	}
	callback_depth++;
	const char *json =
		payload ? json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN) : "{}";
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->hook_count; j++) {
			struct native_hook *hook = &loaded[i]->hooks[j];
			if (!strcmp(hook->event, event)) {
				hook->call(hook->userdata, event, json);
			}
		}
	}
	callback_depth--;
}

static struct native_provider *provider_for_model_internal(const char *model) {

	if (!model) {
		return NULL;
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t p = 0; p < loaded[i]->provider_count; p++) {
			struct native_provider *provider = &loaded[i]->providers[p];
			for (size_t j = 0; j < json_object_array_length(provider->models); j++) {
				if (!strcmp(field(json_object_array_get_idx(provider->models, j), "id"), model)) {
					return provider;
				}
			}
		}
	}

	return NULL;
}

const char *extensions_provider_for_model(const char *model, int64_t *context_window) {

	if (context_window) {
		*context_window = 0;
	}
	struct native_provider *provider = provider_for_model_internal(model);
	if (!provider) {
		return NULL;
	}
	if (context_window) {
		for (size_t i = 0; i < json_object_array_length(provider->models); i++) {
			json_object *entry = json_object_array_get_idx(provider->models, i);
			if (strcmp(field(entry, "id"), model)) {
				continue;
			}
			json_object *window = json_object_object_get(entry, "context_window");
			if (json_object_is_type(window, json_type_int)) {
				*context_window = json_object_get_int64(window);
			}
			break;
		}
	}

	return provider->id;
}

static struct native_provider *provider_by_id(const char *id) {

	if (!id) {
		return NULL;
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t p = 0; p < loaded[i]->provider_count; p++) {
			if (!strcmp(loaded[i]->providers[p].id, id)) {
				return &loaded[i]->providers[p];
			}
		}
	}

	return NULL;
}

/* JSON array of {id, name} for every loaded provider; caller frees. NULL when
   there are no providers. */
json_object *extensions_provider_list(void) {

	json_object *array = json_object_new_array();
	if (!array) {
		return NULL;
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t p = 0; p < loaded[i]->provider_count; p++) {
			struct native_provider *provider = &loaded[i]->providers[p];
			json_object *entry = json_object_new_object();
			json_object_object_add(entry, "id", json_object_new_string(provider->id));
			json_object_object_add(entry, "name", json_object_new_string(provider->name));
			json_object_array_add(array, entry);
		}
	}
	if (!json_object_array_length(array)) {
		json_object_put(array);
		return NULL;
	}

	return array;
}

size_t extensions_model_count(void) {

	size_t count = 0;
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t p = 0; p < loaded[i]->provider_count; p++) {
			count += json_object_array_length(loaded[i]->providers[p].models);
		}
		count += loaded[i]->virtual_model_count;
	}

	return count;
}

int extensions_model_at(size_t index, const char **id, const char **name, const char **provider_id,
	int64_t *context_window) {

	if (id) {
		*id = NULL;
	}
	if (name) {
		*name = NULL;
	}
	if (provider_id) {
		*provider_id = NULL;
	}
	if (context_window) {
		*context_window = 0;
	}
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t p = 0; p < loaded[i]->provider_count; p++) {
			struct native_provider *provider = &loaded[i]->providers[p];
			size_t count = json_object_array_length(provider->models);
			if (index >= count) {
				index -= count;
				continue;
			}
			json_object *model = json_object_array_get_idx(provider->models, index);
			const char *model_id = field(model, "id"), *model_name = field(model, "name");
			if (id) {
				*id = model_id;
			}
			if (name) {
				*name = *model_name ? model_name : model_id;
			}
			if (provider_id) {
				*provider_id = provider->id;
			}
			if (context_window) {
				json_object *window = json_object_object_get(model, "context_window");
				if (json_object_is_type(window, json_type_int)) {
					*context_window = json_object_get_int64(window);
				}
			}
			return 1;
		}
		if (index < loaded[i]->virtual_model_count) {
			if (id) {
				*id = loaded[i]->virtual_models[index].id;
			}
			if (name) {
				*name = loaded[i]->virtual_models[index].name;
			}
			return 1;
		}
		index -= loaded[i]->virtual_model_count;
	}

	return 0;
}

int extensions_provider_login(const char *id) {

	struct native_provider *provider = provider_by_id(id);
	if (!provider || !provider->auth.login) {
		message("Unknown provider or provider has no login.\n");
		return 0;
	}
	char key[49];
	if (!credential_key(id, key)) {
		message("Invalid provider id.\n");
		return 0;
	}
	char fresh[MAX_STATE_VALUE + 1] = "";
	callback_depth++;
	int code = provider->auth.login(provider->userdata, "{}", fresh, sizeof(fresh));
	callback_depth--;
	if (code || !valid_json_object_text(fresh) ||
		extension_state_set(provider->owner, key, fresh)) {
		message("Provider login failed.\n");
		return 0;
	}
	message("Provider login complete.\n");

	return 1;
}

int extensions_provider_logout(const char *id) {

	struct native_provider *provider = provider_by_id(id);
	if (!provider) {
		message("Unknown provider.\n");
		return 0;
	}
	char key[49];
	if (!credential_key(id, key)) {
		message("Invalid provider id.\n");
		return 0;
	}
	extension_state_set(provider->owner, key, NULL);
	message("Provider credentials removed.\n");

	return 1;
}

static int provider_emit_bridge(void *context, const char *event, const char *data) {

	struct response_sink *sink = context;
	if (!sink || !event || !data) {
		return -1;
	}
	if (!strcmp(event, "text")) {
		if (sink->text) {
			sink->text(data);
		}
		if (sink->observe_text) {
			sink->observe_text(data, sink->observer);
		}
		return 0;
	}
	if (!strcmp(event, "tool")) {
		if (sink->tool_pending) {
			json_object *call = json_tokener_parse(data);
			if (json_object_is_type(call, json_type_object)) {
				sink->tool_pending(call);
			}
			if (call) {
				json_object_put(call);
			}
		}
		return 0;
	}
	if (!strcmp(event, "usage")) {
		if (sink->usage) {
			json_object *usage = json_tokener_parse(data);
			json_object *in = usage ? json_object_object_get(usage, "input_tokens") : NULL;
			json_object *out = usage ? json_object_object_get(usage, "output_tokens") : NULL;
			if (json_object_is_type(in, json_type_int) && json_object_is_type(out, json_type_int)) {
				int64_t a = json_object_get_int64(in), b = json_object_get_int64(out);
				if (a >= 0 && b >= 0) {
					sink->usage(a, b);
				}
			}
			if (usage) {
				json_object_put(usage);
			}
		}
		return 0;
	}
	/* reasoning is intentionally accepted but display-only support is optional. */
	if (!strcmp(event, "reasoning")) {
		return 0;
	}

	return 0;
}

static int provider_auth_prepare(struct native_provider *provider, json_object *request) {

	if (!provider) {
		return 1;
	}
	struct phi_provider_auth_v1 *auth = &provider->auth;
	if (!auth->login && !auth->refresh && !auth->resolve && !auth->needs_refresh) {
		return 1;
	}

	char key[49];
	if (!credential_key(provider->id, key)) {
		return 0;
	}
	char credential[MAX_STATE_VALUE + 1] = "";
	int have = extension_state_get(provider->owner, key, credential, sizeof(credential)) == 0;

	if (!have && auth->login) {
		char fresh[MAX_STATE_VALUE + 1] = "";
		callback_depth++;
		int code = auth->login(provider->userdata, "{}", fresh, sizeof(fresh));
		callback_depth--;
		if (code || !valid_json_object_text(fresh) ||
			extension_state_set(provider->owner, key, fresh)) {
			return 0;
		}
		snprintf(credential, sizeof(credential), "%s", fresh);
		have = 1;
	}
	if (have && auth->needs_refresh) {
		callback_depth++;
		int refresh = auth->needs_refresh(provider->userdata, credential);
		callback_depth--;
		if (refresh) {
			int lock = state_lock(provider->owner->name);
			if (lock < 0) {
				return 0;
			}
			char error[256];
			json_object *latest = state_load(provider->owner->name, error, sizeof(error));
			json_object *stored = latest ? json_object_object_get(latest, key) : NULL;
			const char *current = json_object_is_type(stored, json_type_string)
				? json_object_get_string(stored)
				: credential;
			callback_depth++;
			int still = auth->needs_refresh(provider->userdata, current);
			callback_depth--;
			if (!latest) {
				state_unlock(lock);
				return 0;
			}
			if (still && auth->refresh) {
				char fresh[MAX_STATE_VALUE + 1] = "";
				callback_depth++;
				int code = auth->refresh(provider->userdata, current, fresh, sizeof(fresh));
				callback_depth--;
				if (code || !valid_json_object_text(fresh)) {
					if (latest) {
						json_object_put(latest);
					}
					state_unlock(lock);
					return 0;
				}
				json_object_object_add(latest, key, json_object_new_string(fresh));
				if (state_write(provider->owner->name, latest)) {
					json_object_put(latest);
					state_unlock(lock);
					return 0;
				}
				snprintf(credential, sizeof(credential), "%s", fresh);
			} else {
				snprintf(credential, sizeof(credential), "%s", current);
			}
			if (latest) {
				json_object_put(latest);
			}
			state_unlock(lock);
		}
	}
	if (!have && (auth->resolve || auth->needs_refresh || auth->refresh)) {
		return 0;
	}
	if (have && auth->resolve) {
		char resolved[MAX_STATE_VALUE + 1] = "";
		callback_depth++;
		int code = auth->resolve(provider->userdata, credential, resolved, sizeof(resolved));
		callback_depth--;
		if (code || !valid_json_object_text(resolved)) {
			return 0;
		}
		json_object *value = json_tokener_parse(resolved);
		if (!json_object_is_type(value, json_type_object)) {
			if (value) {
				json_object_put(value);
			}
			return 0;
		}
		json_object_object_add(request, "auth", value);
	}

	return 1;
}

struct provider_chain {
	struct native_provider *provider;
	const char *provider_id;
	struct response_sink sink;
	extensions_provider_fallback_fn fallback;
	void *fallback_userdata;
	struct native_provider_wrapper *wrappers[MAX_EXTENSIONS * MAX_PROVIDER_WRAPPERS];
	size_t wrapper_count;
};

struct provider_chain_step {
	struct provider_chain *chain;
	size_t index;
};

static int provider_chain_next(void *context, const char *request_json, phi_provider_emit_v1 emit,
	void *emit_context, char *result_json, size_t result_capacity) {

	struct provider_chain_step *step = context;
	struct provider_chain *chain = step->chain;
	if (step->index < chain->wrapper_count) {
		struct native_provider_wrapper *wrapper = chain->wrappers[step->index];
		struct provider_chain_step next = {.chain = chain, .index = step->index + 1};
		callback_depth++;
		int code = wrapper->call(wrapper->userdata, chain->provider_id, request_json,
			provider_chain_next, &next, emit, emit_context, result_json, result_capacity);
		callback_depth--;
		return code;
	}
	if (chain->provider) {
		callback_depth++;
		int code = chain->provider->stream(chain->provider->userdata, request_json, emit,
			emit_context, result_json, result_capacity);
		callback_depth--;
		return code;
	}
	json_object *request = json_tokener_parse(request_json);
	if (!json_object_is_type(request, json_type_object)) {
		if (request) {
			json_object_put(request);
		}
		return -1;
	}
	json_object *output =
		chain->fallback ? chain->fallback(request, chain->sink, chain->fallback_userdata) : NULL;
	json_object_put(request);
	if (!output) {
		return -1;
	}
	const char *encoded = json_object_to_json_string_ext(output, JSON_C_TO_STRING_PLAIN);
	size_t n = strlen(encoded);
	if (n + 1 > result_capacity) {
		json_object_put(output);
		return -1;
	}
	memcpy(result_json, encoded, n + 1);
	json_object_put(output);

	return 0;
}

json_object *extensions_provider_dispatch(json_object *request, struct response_sink sink,
	extensions_provider_fallback_fn fallback, void *userdata) {

	if (!json_object_is_type(request, json_type_object)) {
		return NULL;
	}
	const char *model = field(request, "model");
	struct native_provider *provider = provider_for_model_internal(model);
	const char *provider_id = provider ? provider->id : "codex";
	if (provider && !provider_auth_prepare(provider, request)) {
		message("Provider authentication failed.\n");
		return NULL;
	}
	struct provider_chain chain = {.provider = provider,
		.provider_id = provider_id,
		.sink = sink,
		.fallback = fallback,
		.fallback_userdata = userdata};
	for (size_t i = 0; i < loaded_count; i++) {
		for (size_t j = 0; j < loaded[i]->provider_wrapper_count; j++) {
			struct native_provider_wrapper *wrapper = &loaded[i]->provider_wrappers[j];
			if (!strcmp(wrapper->target, "*") || !strcmp(wrapper->target, provider_id)) {
				chain.wrappers[chain.wrapper_count++] = wrapper;
			}
		}
	}
	/* Common case: no extension involved. Hand the request object straight to
       the built-in transport instead of serializing the whole conversation,
       parsing it back and round-tripping the output through a 4 MiB buffer. */
	if (!provider && !chain.wrapper_count) {
		return fallback ? fallback(request, sink, userdata) : NULL;
	}
	const char *encoded = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
	char *result_json = malloc(MAX_PROVIDER_RESULT);
	if (!result_json) {
		return NULL;
	}
	result_json[0] = 0;
	struct provider_chain_step step = {.chain = &chain, .index = 0};
	int code = provider_chain_next(
		&step, encoded, provider_emit_bridge, &sink, result_json, MAX_PROVIDER_RESULT);
	result_json[MAX_PROVIDER_RESULT - 1] = 0;
	if (code || (sink.cancel && sink.cancel())) {
		free(result_json);
		return NULL;
	}
	json_tokener *tok = json_tokener_new();
	json_object *output = NULL;
	if (tok) {
		/* Non-strict parsing: providers may emit lenient JSON. The output
           type is checked below. */
		output = json_tokener_parse_ex(tok, result_json, (int)strlen(result_json) + 1);
		if (json_tokener_get_error(tok) != json_tokener_success ||
			!json_object_is_type(output, json_type_array)) {
			if (output) {
				json_object_put(output);
			}
			output = NULL;
		}
		json_tokener_free(tok);
	}
	free(result_json);
	if (!output && !(sink.cancel && sink.cancel())) {
		message("Provider returned invalid output JSON.\n");
	}

	return output;
}
