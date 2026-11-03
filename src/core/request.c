#include "codex.h"
#include "instructions.h"
#include "history.h"
#include "../tools.h"
#include "models.h"
#include "auth.h"
#include <stdlib.h>

static json_object *request_body(json_object *history, const char *model, const char *effort,
	const char *instructions, int with_tools) {

	json_object *body = json_object_new_object();
	json_object_object_add(body, "model", json_object_new_string(model));
	char *loaded = instructions ? NULL : instructions_load();
	json_object_object_add(body, "instructions",
		json_object_new_string(instructions ? instructions
				: loaded                    ? loaded
											: "You are a helpful assistant."));
	free(loaded);
	json_object *available_tools = with_tools ? tools_schema() : json_object_new_array();
	if (!available_tools) {
		tool_report(stderr, "Invalid tool registry.\n");
		json_object_put(body);
		return NULL;
	}
	/* Native image generation is server-executed, not a local function. */
	if (with_tools) {
		json_object_array_add(available_tools,
			json_tokener_parse(
				"{\"type\":\"image_generation\",\"model\":\"gpt-image-2\",\"output_format\":\"png\"}"));
	}
	json_object_object_add(body, "tools", available_tools);
	json_object_object_add(body, "store", json_object_new_boolean(0));
	json_object_object_add(body, "prompt_cache_key", json_object_new_string(phi_cache_key()));
	json_object_object_add(body, "stream", json_object_new_boolean(1));
	json_object *input = history_request_input(history);
	if (!input) {
		json_object_put(body);
		return NULL;
	}
	json_object_object_add(body, "input", input);
	json_object *include = json_object_new_array();
	json_object_array_add(include, json_object_new_string("reasoning.encrypted_content"));
	json_object_object_add(body, "include", include);
	json_object *reasoning = json_object_new_object();
	/* A level the model does not take (gpt-6 has no "none") becomes the
       nearest it does, so a turn never fails on it. */
	json_object_object_add(
		reasoning, "effort", json_object_new_string(phi_effort_for(model, effort)));
	json_object_object_add(reasoning, "summary", json_object_new_string("auto"));
	json_object_object_add(body, "reasoning", reasoning);

	return body;
}

json_object *codex_request_body(json_object *history, const char *model, const char *effort) {

	return request_body(history, model, effort, NULL, 1);
}

json_object *codex_summary_body(json_object *history, const char *model, const char *effort) {

	return request_body(history, model, effort,
		"You are a context summarization assistant. Do not continue the conversation or answer questions in it. Only output the requested structured summary.",
		0);
}

json_object *codex_custom_body(
	json_object *history, const char *model, const char *effort, const char *instructions) {

	return request_body(history, model, effort, instructions, 0);
}
