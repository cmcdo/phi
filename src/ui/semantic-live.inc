/* Executing-tool projection: original invocation plus actual sink output.
   No transcript parsing, execution hooks, approvals, or provider-owned timers. */
static struct semantic_frame_cache semantic_live_projection;
static char *semantic_live_output;
static size_t semantic_live_length, semantic_live_capacity;
static uint64_t semantic_live_revision;
static int semantic_live_unavailable;

static void semantic_live_changed(void) {

	semantic_live_revision++;
	transcript_revision++;
	dirty = 1;
}

static void semantic_live_clear(void) {

	free(semantic_live_projection.cells);
	semantic_live_projection = (struct semantic_frame_cache){0};
	free(semantic_live_output);
	semantic_live_output = NULL;
	semantic_live_length = semantic_live_capacity = 0;
	semantic_live_unavailable = 0;
	semantic_live_revision++;
}

static void semantic_live_stop(void) {

	semantic_live_clear();
	semantic_live_unavailable = 1;
}

void ui_tool_stream(const char *text) {

	if (!tool_active || !text || !*text) {
		return;
	}
	semantic_live_changed();
	if (semantic_live_unavailable) {
		return;
	}
	size_t extra = strlen(text), limit = PHI_RENDERER_MAX_JSON - 2048;
	if (extra > limit - semantic_live_length) {
		free(semantic_live_output);
		semantic_live_output = NULL;
		semantic_live_length = semantic_live_capacity = 0;
		semantic_live_unavailable = 1;
		return;
	}
	size_t needed = semantic_live_length + extra + 1;
	if (needed > semantic_live_capacity) {
		size_t capacity = semantic_live_capacity * 2;
		if (capacity < needed) {
			capacity = needed;
		}
		if (capacity > limit + 1) {
			capacity = limit + 1;
		}
		char *grown = realloc(semantic_live_output, capacity);
		if (!grown) {
			semantic_live_unavailable = 1;
			return;
		}
		semantic_live_output = grown;
		semantic_live_capacity = capacity;
	}
	memcpy(semantic_live_output + semantic_live_length, text, extra + 1);
	semantic_live_length += extra;
}

static struct semantic_frame_cache *semantic_live_frame(int width) {

	if (!tool_active || tool_historical || !tool_pending_semantic || semantic_live_unavailable ||
		modal_active || pending_approval) {
		return NULL;
	}
	json_object *call = json_object_object_get(tool_pending_semantic, "invocation"),
				*name = json_object_object_get(call, "name");
	const char *provider = skin_live_renderer_for(&skin,
		json_object_is_type(name, json_type_string) ? json_object_get_string(name)
													: tool_live_name);
	if (!*provider) {
		return NULL;
	}
	uint64_t version = semantic_live_revision * 2 + (uint64_t)(tool_details != 0);
	if (!semantic_frame_matches(&semantic_live_projection, provider, width, version)) {
		json_object *block = semantic_copy(tool_pending_semantic);
		if (!block) {
			return NULL;
		}
		json_object_object_add(block, "state", json_object_new_string("running"));
		json_object_object_add(
			block, "revision", json_object_new_int64((int64_t)semantic_live_revision));
		json_object_object_add(block, "output",
			json_object_new_string_len(
				semantic_live_output ? semantic_live_output : "", (int)semantic_live_length));
		json_object_object_add(block, "output_complete", json_object_new_boolean(0));
		json_object *context = json_object_new_object(), *blocks = json_object_new_array();
		json_object_object_add(context, "schema", json_object_new_int(1));
		json_object_object_add(context, "kind", json_object_new_string("tool_live"));
		json_object_object_add(context, "expanded", json_object_new_boolean(tool_details));
		json_object_array_add(blocks, block);
		json_object_object_add(context, "blocks", blocks);
		semantic_frame_prepare(&semantic_live_projection, provider, width, version, context);
		json_object_put(context);
	}

	return semantic_live_projection.rows ? &semantic_live_projection : NULL;
}
