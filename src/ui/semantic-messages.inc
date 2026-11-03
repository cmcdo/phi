/* Original message text lives beside, never inside, its styled projection.
   A stream keeps one identity; immutable native snapshots carry a revision. */
#define SEMANTIC_MESSAGE_LIMIT 32

static struct semantic_message {
	uint64_t id, revision;
	size_t start, end, length, capacity;
	int kind;
	char state[16];
	char *text;
	struct semantic_frame_cache projection;
} semantic_messages[SEMANTIC_MESSAGE_LIMIT];

static size_t semantic_message_count;
static uint64_t semantic_message_active;

static struct semantic_message *semantic_message_find(uint64_t id) {

	for (size_t i = 0; id && i < semantic_message_count; i++) {
		if (semantic_messages[i].id == id) {
			return &semantic_messages[i];
		}
	}

	return NULL;
}

static void semantic_message_remove(size_t index) {

	struct semantic_message *message = &semantic_messages[index];
	if (message->id == semantic_message_active) {
		semantic_message_active = 0;
	}
	free(message->text);
	free(message->projection.cells);
	memmove(message, message + 1, (semantic_message_count - index - 1) * sizeof(*message));
	semantic_message_count--;
}

static void semantic_messages_clear(void) {

	while (semantic_message_count) {
		semantic_message_remove(semantic_message_count - 1);
	}
}

static void semantic_message_finish(const char *state) {

	struct semantic_message *message = semantic_message_find(semantic_message_active);
	if (message) {
		snprintf(message->state, sizeof(message->state), "%s", state);
		message->revision++;
		transcript_revision++;
		dirty = 1;
	}
	semantic_message_active = 0;
}

static int semantic_message_is_active(int kind) {

	struct semantic_message *message = semantic_message_find(semantic_message_active);

	return message && message->kind == kind;
}

static void semantic_message_begin(int kind, size_t start) {

	semantic_message_finish("completed");
	if (semantic_block_id >= INT64_MAX || start > used) {
		return;
	}
	if (semantic_message_count == SEMANTIC_MESSAGE_LIMIT) {
		semantic_message_remove(0);
	}
	struct semantic_message *message = &semantic_messages[semantic_message_count++];
	*message = (struct semantic_message){
		.id = ++semantic_block_id, .revision = 1, .start = start, .end = used, .kind = kind};
	snprintf(message->state, sizeof(message->state), "streaming");
	message->text = strdup("");
	message->capacity = message->text ? 1 : 0;
	semantic_message_active = message->id;
}

static void semantic_message_append(uint64_t id, const char *text, size_t end) {

	struct semantic_message *message = semantic_message_find(id);
	if (!message) {
		return;
	}
	size_t extra = strlen(text);
	message->end = end;
	message->revision++;
	if (message->text) {
		if (extra > PHI_RENDERER_MAX_JSON - 1024 - message->length) {
			free(message->text);
			message->text = NULL;
			message->length = message->capacity = 0;
		} else {
			size_t needed = message->length + extra + 1;
			if (needed > message->capacity) {
				size_t capacity = message->capacity * 2;
				if (capacity < needed) {
					capacity = needed;
				}
				if (capacity > PHI_RENDERER_MAX_JSON - 1023) {
					capacity = PHI_RENDERER_MAX_JSON - 1023;
				}
				char *grown = realloc(message->text, capacity);
				if (!grown) {
					free(message->text);
					message->text = NULL;
					message->length = message->capacity = 0;
				} else {
					message->text = grown;
					message->capacity = capacity;
				}
			}
			if (message->text) {
				memcpy(message->text + message->length, text, extra + 1);
				message->length += extra;
			}
		}
	}
	transcript_revision++;
	dirty = 1;
}

static void semantic_messages_drop(size_t bytes) {

	for (size_t i = 0; i < semantic_message_count;) {
		struct semantic_message *message = &semantic_messages[i];
		if (message->start < bytes) {
			semantic_message_remove(i);
		} else {
			message->start -= bytes;
			message->end -= bytes;
			i++;
		}
	}
}

static void semantic_messages_truncate(size_t end) {

	for (size_t i = 0; i < semantic_message_count;) {
		struct semantic_message *message = &semantic_messages[i];
		if (message->start >= end) {
			semantic_message_remove(i);
		} else {
			if (message->end > end) {
				message->end = end;
			}
			i++;
		}
	}
}

static void semantic_messages_shift(size_t start, size_t end, ptrdiff_t delta) {

	for (size_t i = 0; i < semantic_message_count;) {
		struct semantic_message *message = &semantic_messages[i];
		if (message->start >= end) {
			message->start = (size_t)((ptrdiff_t)message->start + delta);
			message->end = (size_t)((ptrdiff_t)message->end + delta);
			i++;
		} else if (message->end > start) {
			semantic_message_remove(i);
		} else {
			i++;
		}
	}
}

/* Legacy whitespace projection must not cut the interior of a native block:
   its returned frame, not a byte-level newline guess, owns its measured height. */
static size_t semantic_messages_floor(size_t end, size_t limit) {

	if (!skin_messages_any(&skin)) {
		return end;
	}
	for (size_t i = 0; i < semantic_message_count; i++) {
		struct semantic_message *message = &semantic_messages[i];
		if (message->text && message->start < end && message->end > end && message->end <= limit &&
			*skin_message_renderer(&skin, message->kind)) {
			end = message->end;
		}
	}

	return end;
}

static struct semantic_message *semantic_message_frame(size_t at, int width) {

	if (!skin_messages_any(&skin) || modal_active || pending_approval) {
		return NULL;
	}
	struct semantic_message *message = NULL;
	for (size_t i = 0; i < semantic_message_count; i++) {
		if (semantic_messages[i].start == at) {
			message = &semantic_messages[i];
			break;
		}
	}
	if (!message || !message->text || message->end <= at || message->end > used) {
		return NULL;
	}
	const char *provider = skin_message_renderer(&skin, message->kind);
	if (!*provider) {
		return NULL;
	}
	if (!semantic_frame_matches(&message->projection, provider, width, message->revision)) {
		const char *kinds[] = {"user", "assistant", "thinking"};
		json_object *context = json_object_new_object(), *blocks = json_object_new_array(),
					*block = json_object_new_object();
		json_object_object_add(context, "schema", json_object_new_int(1));
		json_object_object_add(context, "kind", json_object_new_string("message"));
		json_object_object_add(block, "id", json_object_new_int64((int64_t)message->id));
		json_object_object_add(
			block, "revision", json_object_new_int64((int64_t)message->revision));
		json_object_object_add(block, "kind", json_object_new_string(kinds[message->kind]));
		json_object_object_add(block, "state", json_object_new_string(message->state));
		json_object_object_add(
			block, "text", json_object_new_string_len(message->text, (int)message->length));
		json_object_array_add(blocks, block);
		json_object_object_add(context, "blocks", blocks);
		semantic_frame_prepare(&message->projection, provider, width, message->revision, context);
		json_object_put(context);
	}

	return message->projection.rows ? message : NULL;
}
