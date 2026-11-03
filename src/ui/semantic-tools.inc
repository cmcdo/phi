/* Completed-tool semantic snapshots and pure native projections. Never parse
   the rendered transcript to reconstruct a call/result. The compatibility
   bytes remain intact and are used whenever any contract/budget check fails. */
static json_object *semantic_copy(json_object *value) {

	if (!value) {
		return NULL;
	}
	const char *text = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
	if (!text || strlen(text) > PHI_RENDERER_MAX_JSON) {
		return NULL;
	}

	return json_tokener_parse(text);
}

void ui_tool_invocation(json_object *call) {

	semantic_live_changed();
	if (tool_pending_semantic) {
		json_object_put(tool_pending_semantic);
	}
	tool_pending_semantic = NULL;
	if (!tool_active || !json_object_is_type(call, json_type_object) ||
		semantic_block_id >= INT64_MAX) {
		return;
	}
	json_object *copy = semantic_copy(call);
	if (!copy) {
		return;
	}
	tool_pending_semantic = json_object_new_object();
	json_object_object_add(
		tool_pending_semantic, "id", json_object_new_int64((int64_t)++semantic_block_id));
	json_object_object_add(tool_pending_semantic, "kind", json_object_new_string("tool"));
	json_object_object_add(tool_pending_semantic, "invocation", copy);
	json_object *arguments = json_object_object_get(copy, "arguments");
	json_object *parsed = json_object_is_type(arguments, json_type_string)
		? json_tokener_parse(json_object_get_string(arguments))
		: semantic_copy(arguments);
	json_object_object_add(tool_pending_semantic, "arguments", parsed);
}

void ui_tool_answer(json_object *answer) {

	semantic_live_changed();
	if (!tool_pending_semantic) {
		return;
	}
	json_object *copy = semantic_copy(answer);
	if (!copy) {
		json_object_put(tool_pending_semantic);
		tool_pending_semantic = NULL;
		return;
	}
	json_object_object_add(tool_pending_semantic, "result", copy);
}

static void semantic_tool_finish(int failed) {

	if (!tool_pending_semantic) {
		return;
	}
	json_object *result = json_object_object_get(tool_pending_semantic, "result");
	if (!result) {
		json_object_put(tool_pending_semantic);
		tool_pending_semantic = NULL;
		return;
	}
	int cancelled = json_object_get_boolean(json_object_object_get(result, "cancelled"));
	json_object_object_add(tool_pending_semantic, "state",
		json_object_new_string(cancelled ? "cancelled"
				: failed                 ? "failed"
										 : "completed"));
	json_object_object_add(tool_pending_semantic, "duration_ms",
		tool_historical ? NULL : json_object_new_double((now() - tool_started) * 1000));
	json_object *changes = json_object_new_object();
	json_object_object_add(changes, "added", json_object_new_int64((int64_t)tool_added));
	json_object_object_add(changes, "removed", json_object_new_int64((int64_t)tool_removed));
	json_object_object_add(tool_pending_semantic, "changes", changes);
	if (strlen(json_object_to_json_string_ext(tool_pending_semantic, JSON_C_TO_STRING_PLAIN)) >
		PHI_RENDERER_MAX_JSON - 512) {
		json_object_put(tool_pending_semantic);
		tool_pending_semantic = NULL;
	}
}

static const char *semantic_provider(json_object *block) {

	json_object *call = json_object_object_get(block, "invocation");
	json_object *name = json_object_object_get(call, "name");

	return skin_renderer_for(
		&skin, json_object_is_type(name, json_type_string) ? json_object_get_string(name) : "");
}

static int semantic_tools_payload_mergeable(json_object *previous, json_object *current) {

	if (!previous || !current) {
		return !previous && !current;
	}
	if (json_object_array_length(previous) >= 32 ||
		strcmp(semantic_provider(json_object_array_get_idx(previous, 0)),
			semantic_provider(current))) {
		return 0;
	}
	size_t before = strlen(json_object_to_json_string_ext(previous, JSON_C_TO_STRING_PLAIN));
	size_t added = strlen(json_object_to_json_string_ext(current, JSON_C_TO_STRING_PLAIN));

	return before + added + 512 < PHI_RENDERER_MAX_JSON;
}

static int semantic_tools_mergeable(json_object *previous, json_object *current) {

	/* Plain folded summaries need no renderer payload. Large tool results
       may omit that payload; they must not split an adjacent activity count. */
	return !skin_renderers_any(&skin) || semantic_tools_payload_mergeable(previous, current);
}

static struct tool_card *semantic_tool_frame(size_t at, int width) {

	if (!skin_renderers_any(&skin) || modal_active || pending_approval || width < 1 ||
		width > 1024) {
		return NULL;
	}
	struct tool_card *card = NULL;
	for (size_t i = 0; i < tool_card_count; i++) {
		if (tool_cards[i].start == at) {
			card = &tool_cards[i];
			break;
		}
	}
	if (!card || !card->semantic || card->end > used) {
		return NULL;
	}
	size_t blocks = json_object_array_length(card->semantic);
	if (!blocks) {
		return NULL;
	}
	const char *provider = semantic_provider(json_object_array_get_idx(card->semantic, 0));
	if (!*provider) {
		return NULL;
	}
	/* A mixed compatibility group is replaced only by one renderer that owns
       every member. Changing per-tool bindings never silently drops members. */
	for (size_t i = 1; i < blocks; i++) {
		if (strcmp(provider, semantic_provider(json_object_array_get_idx(card->semantic, i)))) {
			return NULL;
		}
	}
	if (!semantic_frame_matches(&card->projection, provider, width, (uint64_t)card->expanded)) {
		json_object *context = json_object_new_object();
		json_object_object_add(context, "schema", json_object_new_int(1));
		json_object_object_add(context, "kind", json_object_new_string("tool_group"));
		json_object_object_add(context, "expanded", json_object_new_boolean(card->expanded));
		json_object_object_add(context, "blocks", json_object_get(card->semantic));
		semantic_frame_prepare(
			&card->projection, provider, width, (uint64_t)card->expanded, context);
		json_object_put(context);
	}

	return card->projection.rows ? card : NULL;
}
