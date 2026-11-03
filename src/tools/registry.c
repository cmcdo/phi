#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <limits.h>

#define PHI_TOOL(name) extern const struct phi_tool phi_tool_##name;
#include "registry.def"
#undef PHI_TOOL

static const struct phi_tool *const registry[] = {
#define PHI_TOOL(name) &phi_tool_##name,
#include "registry.def"
#undef PHI_TOOL
};

#define TOOL_COUNT (sizeof(registry) / sizeof(*registry))

static struct tool_provider provider;

void tools_set_provider(const struct tool_provider *value) {

	provider = value ? *value : (struct tool_provider){0};
}

int tool_builtin_exists(const char *name) {

	if (!name) {
		return 0;
	}
	for (size_t i = 0; i < TOOL_COUNT; i++) {
		if (!strcmp(name, registry[i]->name)) {
			return 1;
		}
	}

	return 0;
}

const struct phi_tool *tool_find(const char *name) {

	if (!name) {
		return NULL;
	}
	for (size_t i = 0; i < TOOL_COUNT; i++) {
		if (!strcmp(name, registry[i]->name)) {
			return registry[i];
		}
	}

	return provider.find ? provider.find(name) : NULL;
}

static int descriptor_is_valid(const struct phi_tool *tool, size_t index, json_object *schema) {

	if (!tool->name || !tool->run || strcmp(field(schema, "name"), tool->name) ||
		strcmp(field(schema, "type"), "function")) {
		return 0;
	}
	for (size_t i = 0; i < index; i++) {
		if (!strcmp(tool->name, registry[i]->name)) {
			return 0;
		}
	}

	return 1;
}

static json_object *schema_cache(void) {

	static json_object *cache;
	if (cache) {
		return cache;
	}

	json_object *built = json_object_new_array();
	if (!built) {
		return NULL;
	}
	for (size_t i = 0; i < TOOL_COUNT; i++) {
		json_object *schema = json_tokener_parse(registry[i]->schema);
		if (!schema || !descriptor_is_valid(registry[i], i, schema)) {
			if (schema) {
				json_object_put(schema);
			}
			json_object_put(built);
			return NULL;
		}
		json_object_array_add(built, schema);
	}
	cache = built;

	return cache;
}

/* Active-tool filter (extensions' set_active_tools). NULL means all tools. */
static json_object *active_tools;

int tools_set_active(json_object *names) {

	if (names && !json_object_is_type(names, json_type_array)) {
		return 0;
	}
	if (active_tools) {
		json_object_put(active_tools);
	}
	active_tools = names ? json_object_get(names) : NULL;

	return 1;
}

int tool_is_active(const char *name) {

	if (!active_tools) {
		return 1;
	}
	for (size_t i = 0; i < json_object_array_length(active_tools); i++) {
		if (!strcmp(json_object_get_string(json_object_array_get_idx(active_tools, i)), name)) {
			return 1;
		}
	}

	return 0;
}

json_object *tools_active_names(void) {

	return active_tools ? json_object_get(active_tools) : NULL;
}

/* Every registered schema, active or not. */
static json_object *all_schemas(void) {

	json_object *cached = schema_cache();
	if (!cached) {
		return NULL;
	}

	/* Protect the cached array itself; its schema objects are immutable by contract. */
	json_object *copy = json_object_new_array();
	if (!copy) {
		return NULL;
	}
	for (size_t i = 0; i < TOOL_COUNT; i++) {
		json_object_array_add(copy, json_object_get(json_object_array_get_idx(cached, i)));
	}
	if (provider.append_schema && !provider.append_schema(copy)) {
		json_object_put(copy);
		return NULL;
	}

	return copy;
}

json_object *tools_schema(void) {

	json_object *all = all_schemas();
	if (!all || !active_tools) {
		return all;
	}
	json_object *shown = json_object_new_array();
	for (size_t i = 0; shown && i < json_object_array_length(all); i++) {
		json_object *schema = json_object_array_get_idx(all, i);
		if (tool_is_active(field(schema, "name"))) {
			json_object_array_add(shown, json_object_get(schema));
		}
	}
	json_object_put(all);

	return shown;
}

json_object *tools_all_names(void) {

	json_object *all = all_schemas(), *names = json_object_new_array();
	for (size_t i = 0; all && names && i < json_object_array_length(all); i++) {
		json_object_array_add(
			names, json_object_new_string(field(json_object_array_get_idx(all, i), "name")));
	}
	if (all) {
		json_object_put(all);
	}

	return names;
}

/* Hints for the built-in tools, with the meaning of MCP tool annotations.
   Missing hints take the MCP defaults (not read-only, may be destructive,
   may reach an open world). */
static const char *const builtin_hints[][2] = {
	{"read", "{\"readOnlyHint\":true,\"openWorldHint\":false}"},
	{"write",
		"{\"readOnlyHint\":false,\"destructiveHint\":true,\"idempotentHint\":true,\"openWorldHint\":false}"},
	{"edit",
		"{\"readOnlyHint\":false,\"destructiveHint\":true,\"idempotentHint\":false,\"openWorldHint\":false}"},
	{"apply_patch",
		"{\"readOnlyHint\":false,\"destructiveHint\":true,\"idempotentHint\":false,\"openWorldHint\":false}"},
	{"git", "{\"readOnlyHint\":false,\"destructiveHint\":true,\"openWorldHint\":false}"},
	{"bash", "{\"readOnlyHint\":false,\"destructiveHint\":true,\"openWorldHint\":true}"},
	{"bash_start", "{\"readOnlyHint\":false,\"destructiveHint\":true,\"openWorldHint\":true}"},
	{"bash_poll", "{\"readOnlyHint\":true,\"openWorldHint\":false}"},
	{"bash_write", "{\"readOnlyHint\":false,\"destructiveHint\":true,\"openWorldHint\":true}"},
	{"bash_kill",
		"{\"readOnlyHint\":false,\"destructiveHint\":true,\"idempotentHint\":true,\"openWorldHint\":false}"},
};

static const char *tool_hints(const char *name, const struct phi_tool *tool) {

	if (tool && tool->annotations) {
		return tool->annotations;
	}
	for (size_t i = 0; i < sizeof(builtin_hints) / sizeof(*builtin_hints); i++) {
		if (!strcmp(builtin_hints[i][0], name)) {
			return builtin_hints[i][1];
		}
	}

	return NULL;
}

json_object *tools_info(int active_only) {

	json_object *all = all_schemas(), *tools = json_object_new_array();
	for (size_t i = 0; all && tools && i < json_object_array_length(all); i++) {
		json_object *schema = json_object_array_get_idx(all, i);
		const char *name = field(schema, "name");
		int active = tool_is_active(name);
		if (active_only && !active) {
			continue;
		}
		const struct phi_tool *tool = tool_find(name);
		json_object *info = json_object_new_object();
		json_object_object_add(info, "name", json_object_new_string(name));
		json_object_object_add(
			info, "description", json_object_new_string(field(schema, "description")));
		json_object *parameters = json_object_object_get(schema, "parameters");
		if (parameters) {
			json_object_object_add(info, "parameters", json_object_get(parameters));
		}
		const char *hints = tool_hints(name, tool);
		json_object *annotations = hints ? json_tokener_parse(hints) : NULL;
		json_object_object_add(
			info, "annotations", annotations ? annotations : json_object_new_object());
		json_object_object_add(info, "active", json_object_new_boolean(active));
		json_object_object_add(info, "source",
			json_object_new_string(tool_builtin_exists(name) ? "builtin" : "extension"));
		if (tool && tool->label) {
			json_object_object_add(info, "label", json_object_new_string(tool->label));
		}
		json_object_array_add(tools, info);
	}
	if (all) {
		json_object_put(all);
	}

	return tools;
}

static json_object *parse_arguments(json_object *encoded, const char **error) {

	size_t size = 0;
	const char *text = tool_plain_string(encoded, &size);
	if (!text) {
		*error = "Expected JSON-encoded tool arguments";
		return NULL;
	}
	if (size >= INT_MAX) {
		*error = "Tool arguments are too large";
		return NULL;
	}

	json_tokener *parser = json_tokener_new();
	if (!parser) {
		*error = "Cannot allocate argument parser";
		return NULL;
	}
	json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
	json_object *args = json_tokener_parse_ex(parser, text, (int)size + 1);
	int valid = json_tokener_get_error(parser) == json_tokener_success &&
		json_object_is_type(args, json_type_object);
	json_tokener_free(parser);
	if (!valid) {
		if (args) {
			json_object_put(args);
		}
		*error = "Tool arguments must be a valid JSON object";
		return NULL;
	}

	return args;
}

json_object *tools_execute(json_object *call) {

	json_object *name_value = NULL;
	if (!call || !json_object_object_get_ex(call, "name", &name_value)) {
		return tool_error("Expected a tool name string");
	}

	const char *name = tool_plain_string(name_value, NULL);
	if (!name) {
		return tool_error("Expected a tool name string");
	}
	const struct phi_tool *tool = tool_find(name);
	if (!tool) {
		return tool_error("Unknown tool");
	}
	if (!tool_is_active(name)) {
		return tool_error("Tool is not active in this session");
	}

	json_object *encoded = NULL;
	if (!json_object_object_get_ex(call, "arguments", &encoded)) {
		return tool_error("Expected JSON-encoded tool arguments");
	}
	const char *error = NULL;
	json_object *args = parse_arguments(encoded, &error);
	if (!args) {
		return tool_error(error);
	}

	if (provider.event) {
		provider.event("tool_start", call);
	}
	json_object *answer = tool->run ? tool->run(args)
		: provider.execute          ? provider.execute(name, args)
									: tool_error("Tool provider is unavailable");
	if (provider.event) {
		json_object *event = json_object_new_object();
		if (event) {
			json_object_object_add(event, "name", json_object_new_string(name));
			json_object_object_add(event, "result", answer ? json_object_get(answer) : NULL);
			provider.event("tool_end", event);
			json_object_put(event);
		}
	}
	json_object_put(args);

	return answer ? answer : tool_error("Tool returned no result");
}
