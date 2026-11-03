/* Older saved messages share the ordinary transcript viewport and composer.
   Keep an offset index, one decoded message/tool pair and its styled bytes;
   paint through the normal renderer into a screen-sized page. Never execute
   saved tools, replace model context, or enter the /history viewer. */
struct scrollback_group {
	size_t position;
	unsigned counts[ACT_COUNT];
};

static struct {
	struct session_tree tree;
	struct scrollback_group *groups;
	size_t group_count;
	size_t *records, *calls, count, selected, cached;
	int *heights, top, width, rows, page_dirty, tail_maximum;
	json_object *item, *call;
	char *body;
	struct styled_capture display;
	size_t body_start, generation;
	int details, full, thinking;
	WINDOW *page;
} scrollback;

static int scrollback_active(void) {

	return scrollback.records != NULL;
}

static WINDOW *scrollback_page(void) {

	return scrollback.page;
}

static void scrollback_close(void) {

	if (scrollback_active()) {
		transcript_selection_clear();
	}
	if (scrollback.page) {
		delwin(scrollback.page);
	}
	if (scrollback.item) {
		json_object_put(scrollback.item);
	}
	if (scrollback.call) {
		json_object_put(scrollback.call);
	}
	free(scrollback.display.text);
	free(scrollback.display.styles);
	free(scrollback.body);
	free(scrollback.records);
	free(scrollback.calls);
	free(scrollback.heights);
	free(scrollback.groups);
	session_tree_free(&scrollback.tree);
	memset(&scrollback, 0, sizeof(scrollback));
}

static char *scrollback_body(json_object *item) {

	if (!strcmp(field(item, "type"), "reasoning")) {
		json_object *summary = json_object_object_get(item, "summary");
		size_t bytes = 1;
		for (size_t i = 0;
			json_object_is_type(summary, json_type_array) && i < json_object_array_length(summary);
			i++) {
			json_object *part = json_object_array_get_idx(summary, i);
			if (!strcmp(field(part, "type"), "summary_text")) {
				bytes += strlen(field(part, "text")) + 1;
			}
		}
		char *text = malloc(bytes);
		if (!text) {
			return NULL;
		}
		size_t at = 0;
		for (size_t i = 0;
			json_object_is_type(summary, json_type_array) && i < json_object_array_length(summary);
			i++) {
			json_object *part = json_object_array_get_idx(summary, i);
			if (strcmp(field(part, "type"), "summary_text")) {
				continue;
			}
			const char *value = field(part, "text");
			size_t n = strlen(value);
			if (at) {
				text[at++] = '\n';
			}
			memcpy(text + at, value, n);
			at += n;
		}
		text[at] = 0;
		return text;
	}
	if (!strcmp(field(item, "type"), "function_call")) {
		json_object *args = json_tokener_parse(field(item, "arguments"));
		const char *content = field(args, "content"), *command = field(args, "command");
		const char *body = *content ? content : command;
		if (*body) {
			const char *path = *content ? field(args, "path") : "";
			size_t bytes = strlen(body) + strlen(path) + 2;
			char *text = malloc(bytes);
			if (text) {
				snprintf(text, bytes, "%s%s%s", path, *path ? "\n" : "", body);
			}
			if (args) {
				json_object_put(args);
			}
			return text;
		}
		if (args) {
			json_object_put(args);
		}
	}
	if (!strcmp(field(item, "type"), "function_call_output")) {
		const char *output = field(item, "output");
		json_object *answer = json_tokener_parse(output);
		const char *text = field(answer, "error");
		if (!*text) {
			text = field(answer, "text");
		}
		if (!*text) {
			text = field(answer, "output");
		}
		char *body = strdup(*text ? text : output);
		if (answer) {
			json_object_put(answer);
		}
		return body;
	}
	const char *role = field(item, "role");
	if (!strcmp(role, "user") || !strcmp(role, "assistant")) {
		json_object *content = json_object_object_get(item, "content");
		if (!json_object_is_type(content, json_type_string) &&
			!json_object_is_type(content, json_type_array)) {
			return strdup("");
		}
		if (json_object_is_type(content, json_type_array)) {
			int text = 0;
			for (size_t i = 0; i < json_object_array_length(content); i++) {
				json_object *part = json_object_array_get_idx(content, i);
				const char *type = field(part, "type");
				if ((!strcmp(type, "input_text") || !strcmp(type, "output_text")) &&
					*field(part, "text")) {
					text = 1;
					break;
				}
			}
			if (!text) {
				return strdup("[non-text content]");
			}
		}
	}

	return history_record_body(item, 0);
}

/* A completed tool is one ordinary card at its invocation's position, not a
   raw call followed by an unrelated JSON result. Pending calls remain visible. */
static int scrollback_records(const struct session_tree *tree, size_t *records, size_t *calls,
	size_t *count, size_t *checkpoint) {

	json_object *pending = json_object_new_object();
	if (!pending) {
		return 0;
	}
	*count = 0;
	if (checkpoint) {
		*checkpoint = SIZE_MAX;
	}
	for (size_t i = 0; i < tree->count; i++) {
		const struct session_node *node = &tree->nodes[i];
		if (!node->active) {
			continue;
		}
		if (node->kind == SESSION_COMPACTION) {
			if (checkpoint) {
				*checkpoint = *count;
			}
			continue;
		}
		int visible = (node->display & 16u) ? skin.thinking_visible && (node->display & 32u)
											: (node->display & 11u) != 0;
		if (node->hidden || !visible) {
			continue;
		}
		if (node->display & 8u) {
			char error[256];
			json_object *item = session_tree_item(tree, i, error, sizeof(error));
			if (!item) {
				json_object_put(pending);
				return 0;
			}
			const char *id = field(item, "call_id"), *type = field(item, "type");
			json_object *id_value = json_object_object_get(item, "call_id"), *slot = NULL;
			int valid = *id && strlen(id) <= 1024 &&
				(size_t)json_object_get_string_len(id_value) == strlen(id);
			if (valid && !strcmp(type, "function_call_output") &&
				json_object_object_get_ex(pending, id, &slot)) {
				size_t at = (size_t)json_object_get_int64(slot);
				calls[at] = records[at];
				records[at] = i;
				json_object_object_del(pending, id);
				json_object_put(item);
				continue;
			}
			if (valid && !strcmp(type, "function_call")) {
				slot = json_object_new_int64((int64_t)*count);
				if (!slot || json_object_object_add(pending, id, slot) < 0) {
					if (slot) {
						json_object_put(slot);
					}
					json_object_put(item);
					json_object_put(pending);
					return 0;
				}
			}
			json_object_put(item);
		}
		records[*count] = i;
		calls[*count] = SIZE_MAX;
		(*count)++;
	}
	json_object_put(pending);

	return 1;
}

/* Inspect only bounded tool records while forming folded activity groups.
   Large payloads stay individually disk-backed instead of being decoded just
   to build an index. No tool or native renderer callback is replayed. */
static int scrollback_tool_info(
	const struct session_tree *tree, size_t record, size_t call, int *category, long long *job) {

	if (call == SIZE_MAX || !(tree->nodes[record].display & 8u) ||
		tree->nodes[record].bytes > LOG_SIZE || tree->nodes[call].bytes > LOG_SIZE) {
		return 0;
	}
	char error[256];
	json_object *item = session_tree_item(tree, record, error, sizeof(error));
	json_object *invocation = session_tree_item(tree, call, error, sizeof(error));
	if (!item || !invocation) {
		if (item) {
			json_object_put(item);
		}
		if (invocation) {
			json_object_put(invocation);
		}
		return -1;
	}
	int mergeable = !strcmp(field(item, "type"), "function_call_output") &&
		strcmp(field(invocation, "name"), "edit");
	json_object *answer = mergeable ? json_tokener_parse(field(item, "output")) : NULL;
	char summary[768];
	mergeable = mergeable && answer && !tool_answer_failed(answer, summary, sizeof(summary));
	if (mergeable) {
		*job = 0;
		if (!strcmp(field(invocation, "name"), "bash_poll")) {
			json_object *args = json_tokener_parse(field(invocation, "arguments"));
			*job = json_object_get_int64(json_object_object_get(args, "job_id"));
			if (args) {
				json_object_put(args);
			}
		}
		*category = tool_activity_category(field(invocation, "name"), *job);
	}
	if (answer) {
		json_object_put(answer);
	}
	json_object_put(invocation);
	json_object_put(item);

	return mergeable;
}

static int scrollback_save_group(struct scrollback_group **groups, size_t *count, size_t *capacity,
	const struct scrollback_group *group) {

	if (*count == *capacity) {
		size_t n = *capacity ? *capacity * 2 : 8;
		if (n < *capacity || n > SIZE_MAX / sizeof(**groups)) {
			return 0;
		}
		void *p = realloc(*groups, n * sizeof(**groups));
		if (!p) {
			return 0;
		}
		*groups = p;
		*capacity = n;
	}
	(*groups)[(*count)++] = *group;

	return 1;
}

static int scrollback_fold_records(const struct session_tree *tree, size_t *records, size_t *calls,
	size_t *count, size_t *checkpoint, struct scrollback_group **groups, size_t *group_count) {

	*groups = NULL;
	*group_count = 0;
	if (tool_details) {
		return 1;
	}
	size_t out = 0, members = 0, bytes = 0, capacity = 0, job_count = 0, last_origin = 0;
	size_t boundary = checkpoint ? *checkpoint : SIZE_MAX, new_boundary = SIZE_MAX;
	long long jobs[64];
	struct scrollback_group group = {0};
	for (size_t i = 0; i < *count; i++) {
		int category = 0;
		long long job = 0;
		int eligible = scrollback_tool_info(tree, records[i], calls[i], &category, &job);
		if (eligible < 0) {
			return 0;
		}
		size_t piece = tree->nodes[records[i]].bytes;
		if (calls[i] != SIZE_MAX) {
			piece += tree->nodes[calls[i]].bytes;
		}
		size_t origin = calls[i] != SIZE_MAX ? calls[i] : records[i];
		int separator = 0;
		for (size_t j = last_origin + 1; members && j < origin; j++) {
			const struct session_node *node = &tree->nodes[j];
			if (node->active && !node->hidden &&
				(node->kind == SESSION_COMPACTION || (node->display & 48u) == 48u)) {
				separator = 1;
				break;
			}
		}
		last_origin = origin;
		int merge = members && eligible && !separator && i != boundary && job_count < 64 &&
			bytes < LOG_SIZE && piece < LOG_SIZE - bytes;
		if (!merge) {
			if (members > 1 && !scrollback_save_group(groups, group_count, &capacity, &group)) {
				return 0;
			}
			members = bytes = job_count = 0;
		}
		if (i == boundary) {
			new_boundary = out;
		}
		if (!merge) {
			group = (struct scrollback_group){.position = out};
			records[out] = records[i];
			calls[out] = calls[i];
			out++;
		}
		if (!eligible) {
			continue;
		}
		int add = 1;
		if (category == ACT_JOB) {
			for (size_t j = 0; j < job_count; j++) {
				if (jobs[j] == job) {
					add = 0;
				}
			}
			if (add) {
				jobs[job_count++] = job;
			}
		}
		group.counts[category] += (unsigned)add;
		members++;
		bytes += piece;
	}
	if (members > 1 && !scrollback_save_group(groups, group_count, &capacity, &group)) {
		return 0;
	}
	if (boundary == *count) {
		new_boundary = out;
	}
	*count = out;
	if (checkpoint) {
		*checkpoint = new_boundary;
	}
	if (*group_count) {
		void *p = realloc(*groups, *group_count * sizeof(**groups));
		if (p) {
			*groups = p;
		}
	}

	return 1;
}

static const struct scrollback_group *scrollback_group_at(size_t position) {

	size_t low = 0, high = scrollback.group_count;
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		if (scrollback.groups[mid].position < position) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}

	return low < scrollback.group_count && scrollback.groups[low].position == position
		? &scrollback.groups[low]
		: NULL;
}

static uint64_t scrollback_id(
	const struct session_tree *tree, const size_t *records, const size_t *calls, size_t position) {

	return tree->nodes[calls[position] != SIZE_MAX ? calls[position] : records[position]].id;
}

static void scrollback_build_display(void) {

	struct styled_capture *out = &scrollback.display;
	free(out->text);
	free(out->styles);
	*out = (struct styled_capture){0};
	const char *role = field(scrollback.item, "role"), *type = field(scrollback.item, "type");
	if (!(scrollback.tree.nodes[scrollback.records[scrollback.cached]].display & 8u)) {
		if (!strcmp(type, "reasoning")) {
			capture_bytes(out, "\xee\x80\x87", 3, 80);
			scrollback.body_start = out->used;
			capture_bytes(out, scrollback.body, strlen(scrollback.body), 81);
			capture_bytes(out, "\n\n", 2, 81);
		} else {
			if (!strcmp(role, "user")) {
				const char *via =
					field(json_object_object_get(scrollback.item, "_phi_origin"), "via");
				if (*via) {
					char label[180];
					snprintf(label, sizeof(label), "[via %.128s]\n", via);
					capture_bytes(out, label, strlen(label), 2);
				}
				capture_bytes(out, GLYPH_PROMPT, 3, 19);
			} else {
				capture_bytes(out, GLYPH_REPLY, 3, 0);
			}
			scrollback.body_start = out->used;
			capture_bytes(
				out, scrollback.body, strlen(scrollback.body), !strcmp(role, "user") ? 1 : 3);
			capture_bytes(out, "\n\n", 2, 0);
		}
		return;
	}
	json_object *call = scrollback.call  ? scrollback.call
		: !strcmp(type, "function_call") ? scrollback.item
										 : NULL;
	const char *name = call ? field(call, "name") : "tool";
	json_object *args = call ? json_tokener_parse(field(call, "arguments")) : NULL;
	const char *keys[] = {"path", "command", "query", "url"}, *value = "";
	for (size_t i = 0; args && i < sizeof(keys) / sizeof(*keys); i++) {
		if (*(value = field(args, keys[i]))) {
			break;
		}
	}
	char detail[512];
	snprintf(detail, sizeof(detail), "%.500s", value);
	if (args && !strcmp(name, "bash_poll")) {
		snprintf(detail, sizeof(detail), "%lld",
			(long long)json_object_get_int64(json_object_object_get(args, "job_id")));
	}
	for (char *p = detail; *p; p++) {
		if ((unsigned char)*p < 32 || *p == 127) {
			*p = ' ';
		}
	}
	int completed = !strcmp(type, "function_call_output");
	json_object *answer = completed ? json_tokener_parse(field(scrollback.item, "output")) : NULL;
	char summary[768] = "";
	int failed = completed && (!answer || tool_answer_failed(answer, summary, sizeof(summary)));
	if (!tool_details && completed && strcmp(name, "edit")) {
		const struct scrollback_group *group = scrollback_group_at(scrollback.cached);
		unsigned counts[ACT_COUNT] = {0};
		if (group) {
			memcpy(counts, group->counts, sizeof(counts));
		} else {
			counts[tool_activity_category(
				name, !strcmp(name, "bash_poll") ? strtoll(detail, NULL, 10) : 0)] = 1;
		}
		char folded[1024];
		size_t failure_at;
		const char *failure = failed ? (*summary ? summary : "Saved result is invalid") : NULL;
		size_t n = tool_activity_summary(folded, sizeof(folded), counts,
			!group && !strcmp(name, "write") ? detail : NULL, failure, &failure_at);
		char *body = n ? strdup(folded) : NULL;
		if (!body) {
			out->failed = 1;
		} else {
			free(scrollback.body);
			scrollback.body = body;
			scrollback.body_start = 0;
			if (failure) {
				capture_bytes(out, folded, failure_at, 45);
				capture_bytes(out, folded + failure_at, n - failure_at - 1, 7);
				capture_bytes(out, "\n", 1, 45);
			} else {
				capture_bytes(out, folded, n, 45);
			}
			capture_bytes(out, "\n", 1, 73);
		}
		if (args) {
			json_object_put(args);
		}
		if (answer) {
			json_object_put(answer);
		}
		return;
	}
	const char *label = !strcmp(name, "bash_poll") ? "bash output"
		: !strcmp(name, "bash_start")              ? "bash"
												   : name;
	char metadata[1024];
	snprintf(metadata, sizeof(metadata), "%s\t%s\t%s\t0", name, label, detail);
	capture_bytes(out, metadata, strlen(metadata), 71);
	capture_bytes(out, GLYPH_CARD, 3, completed ? (failed ? 44 : 43) : 14);
	capture_bytes(out, label, strlen(label), 4);
	if (*detail) {
		capture_bytes(out, " ", 1, 14);
		capture_bytes(out, detail, strlen(detail), 14);
	}
	size_t heading_end = out->used;
	capture_bytes(out, "\n", 1, 4);
	scrollback.body_start = out->body_start = out->used;
	out->tool_body = 1;
	out->edit = !strcmp(name, "edit");
	int syntax = tool_syntax, output_line = tool_output_line;
	size_t added = tool_added, removed = tool_removed;
	tool_syntax = tool_syntax_for_path(detail);
	tool_added = tool_removed = 0;
	styled_capture = out;
	if (call && answer) {
		resume_tool_preview(call, answer);
	} else {
		append_styled(scrollback.body, 5);
	}
	if (failed) {
		append_styled("✗ ", 7);
		append_styled(*summary ? summary : "Saved result is invalid", 7);
		append_styled("\n", 7);
	}
	styled_capture = NULL;
	size_t saved_added = tool_added, saved_removed = tool_removed;
	tool_syntax = syntax;
	tool_output_line = output_line;
	tool_added = added;
	tool_removed = removed;
	if (out->edit && (saved_added || saved_removed)) {
		char counts[128];
		size_t n = (size_t)snprintf(counts, sizeof(counts), "  +%zu", saved_added);
		size_t red = n;
		n += (size_t)snprintf(counts + n, sizeof(counts) - n, " −%zu", saved_removed);
		size_t end = out->used;
		capture_bytes(out, counts, n, 47);
		if (!out->failed) {
			memmove(out->text + heading_end + n, out->text + heading_end, end - heading_end);
			memmove(out->styles + heading_end + n, out->styles + heading_end, end - heading_end);
			memcpy(out->text + heading_end, counts, n);
			memset(out->styles + heading_end, 47, red);
			memset(out->styles + heading_end + red, 46, n - red);
			out->body_start += n;
			scrollback.body_start += n;
		}
	}
	capture_bytes(out, "\xee\x80\x86", 3, 72);
	capture_bytes(out, "\n\n", 2, 73);
	if (args) {
		json_object_put(args);
	}
	if (answer) {
		json_object_put(answer);
	}
}

static int scrollback_load(size_t position) {

	if (scrollback.cached == position && scrollback.body) {
		return 1;
	}
	if (scrollback.item) {
		json_object_put(scrollback.item);
	}
	if (scrollback.call) {
		json_object_put(scrollback.call);
	}
	scrollback.call = NULL;
	free(scrollback.body);
	scrollback.body = NULL;
	char error[256] = "";
	scrollback.item =
		session_tree_item(&scrollback.tree, scrollback.records[position], error, sizeof(error));
	if (scrollback.calls[position] != SIZE_MAX) {
		scrollback.call =
			session_tree_item(&scrollback.tree, scrollback.calls[position], error, sizeof(error));
	}
	int folded = !tool_details && !strcmp(field(scrollback.item, "type"), "function_call_output") &&
		strcmp(field(scrollback.call, "name"), "edit");
	if (scrollback.item) {
		scrollback.body = folded ? strdup("") : scrollback_body(scrollback.item);
	}
	if (!folded && scrollback.call && !strcmp(field(scrollback.call, "name"), "write")) {
		json_object *answer = json_tokener_parse(field(scrollback.item, "output"));
		if (answer && !*field(answer, "error")) {
			free(scrollback.body);
			scrollback.body = scrollback_body(scrollback.call);
		}
		if (answer) {
			json_object_put(answer);
		}
	}
	if (!scrollback.body) {
		scrollback_close();
		ui_notice(*error ? error : "Could not load older messages.", 1);
		return 0;
	}
	scrollback.cached = position;
	scrollback_build_display();
	if (scrollback.display.failed || !scrollback.display.text) {
		scrollback_close();
		ui_notice("Could not render older messages.", 1);
		return 0;
	}
	/* The cache needs styled bytes, not the decoded graph or opaque payloads. */
	if (scrollback.item) {
		json_object_put(scrollback.item);
	}
	if (scrollback.call) {
		json_object_put(scrollback.call);
	}
	scrollback.item = scrollback.call = NULL;

	return 1;
}

static int scrollback_text_row(size_t offset) {

	size_t bytes = scrollback.body_start + offset;
	if (scrollback.tree.nodes[scrollback.records[scrollback.cached]].display & 8u) {
		size_t n = strcspn(scrollback.body + offset, "\n");
		if (n > 160) {
			n = 160;
		}
		char *needle = strndup(scrollback.body + offset, n);
		const char *match = needle && *needle
			? strstr(scrollback.display.text + scrollback.body_start, needle)
			: NULL;
		if (!match && needle && *needle) {
			match = strstr(scrollback.display.text, needle);
			while (match && scrollback.display.styles[match - scrollback.display.text] == 71) {
				match = strstr(match + 1, needle);
			}
		}
		free(needle);
		bytes = match ? (size_t)(match - scrollback.display.text) : scrollback.body_start;
	}

	return layout_saved_rows(NULL, scrollback.width, 0, &scrollback.display, bytes) - 1;
}

static int scrollback_height(size_t position) {

	if (scrollback.heights[position]) {
		return scrollback.heights[position];
	}
	if (!scrollback_load(position)) {
		return 0;
	}
	int rows =
		layout_saved_rows(NULL, scrollback.width, 0, &scrollback.display, scrollback.display.used);

	return scrollback.heights[position] = rows > 1 ? rows - 1 : 1;
}

/* Extend only the same pinned file, preserving the saved row under the
   cursor. A rewritten/replaced pathname is never silently followed. */
static int scrollback_refresh(void) {

	struct stat old_info, new_info;
	if (!scrollback.tree.source || fstat(fileno(scrollback.tree.source), &old_info) ||
		old_info.st_size <= scrollback.tree.source_bytes) {
		return 0;
	}
	struct session_tree tree = {0};
	char error[256] = "";
	if (!history_reader || !history_reader(&tree, error, sizeof(error))) {
		session_tree_free(&tree);
		return 0;
	}
	if (!tree.source || fstat(fileno(tree.source), &new_info) ||
		old_info.st_dev != new_info.st_dev || old_info.st_ino != new_info.st_ino) {
		session_tree_free(&tree);
		return 0;
	}
	size_t *records = malloc((tree.count ? tree.count : 1) * sizeof(*records)),
		   *calls = malloc((tree.count ? tree.count : 1) * sizeof(*calls));
	int *heights = calloc(tree.count ? tree.count : 1, sizeof(*heights));
	size_t count = 0, selected = SIZE_MAX, checkpoint = SIZE_MAX, group_count = 0;
	struct scrollback_group *groups = NULL;
	if (!records || !calls || !heights ||
		!scrollback_records(&tree, records, calls, &count, &checkpoint) ||
		!scrollback_fold_records(
			&tree, records, calls, &count, &checkpoint, &groups, &group_count)) {
		free(records);
		free(calls);
		free(heights);
		free(groups);
		session_tree_free(&tree);
		return 0;
	}
	uint64_t id =
		scrollback_id(&scrollback.tree, scrollback.records, scrollback.calls, scrollback.selected);
	int still_active = 0;
	for (size_t i = 0; i < tree.count; i++) {
		if (tree.nodes[i].id == id) {
			still_active = tree.nodes[i].active && !tree.nodes[i].hidden;
			break;
		}
	}
	if (still_active) {
		for (size_t i = 0; i < count; i++) {
			if (scrollback_id(&tree, records, calls, i) <= id) {
				selected = i;
			}
		}
	}
	if (tree.source_bytes <= scrollback.tree.source_bytes || selected == SIZE_MAX) {
		free(records);
		free(calls);
		free(heights);
		free(groups);
		session_tree_free(&tree);
		return 0;
	}
	if (scrollback_id(&tree, records, calls, selected) != id) {
		scrollback.top = 0;
	}
	if (scrollback.item) {
		json_object_put(scrollback.item);
	}
	if (scrollback.call) {
		json_object_put(scrollback.call);
	}
	free(scrollback.display.text);
	free(scrollback.display.styles);
	scrollback.display = (struct styled_capture){0};
	free(scrollback.body);
	free(scrollback.records);
	free(scrollback.calls);
	free(scrollback.heights);
	free(scrollback.groups);
	session_tree_free(&scrollback.tree);
	scrollback.tree = tree;
	scrollback.records = records;
	scrollback.calls = calls;
	scrollback.heights = heights;
	scrollback.groups = groups;
	scrollback.group_count = group_count;
	scrollback.count = count;
	scrollback.selected = selected;
	scrollback.cached = SIZE_MAX;
	scrollback.item = scrollback.call = NULL;
	scrollback.body = NULL;
	scrollback.page_dirty = 1;
	scrollback.tail_maximum = conversation_maximum;

	return 1;
}

static void scrollback_rejoin(void) {

	int offset = conversation_maximum > scrollback.tail_maximum
		? conversation_maximum - scrollback.tail_maximum
		: 0;
	scrollback_close();
	scroll_offset = offset;
	working_view_valid = 0;
}

static int scrollback_move(int movement) {

	transcript_selection_clear();
	while (movement && scrollback_active()) {
		int height = scrollback_height(scrollback.selected);
		if (!height) {
			return 0;
		}
		if (movement < 0) {
			if (scrollback.top) {
				int step = movement == INT_MIN ? scrollback.top : -movement;
				if (step > scrollback.top) {
					step = scrollback.top;
				}
				scrollback.top -= step;
				movement += step;
			} else if (scrollback.selected) {
				scrollback.selected--;
				height = scrollback_height(scrollback.selected);
				if (!height) {
					return 0;
				}
				scrollback.top = height - 1;
				movement++;
			} else {
				break;
			}
		} else {
			if (scrollback.selected + 1 == scrollback.count &&
				scrollback.top >= height - scrollback.rows) {
				if (scrollback_refresh()) {
					continue;
				}
				scrollback_rejoin();
				return 1;
			}
			int step = height - scrollback.top;
			if (movement < step) {
				scrollback.top += movement;
				movement = 0;
			} else if (scrollback.selected + 1 < scrollback.count) {
				movement -= step;
				scrollback.selected++;
				scrollback.top = 0;
			} else {
				if (scrollback_refresh()) {
					continue;
				}
				scrollback_rejoin();
				return 1;
			}
		}
	}
	scrollback.page_dirty = dirty = 1;

	return 1;
}

static int scrollback_open(int movement) {

	if (scrollback_active()) {
		return scrollback_move(movement);
	}
	if (!history_reader || !active || modal_active || pending_approval || pasting ||
		history_view_paste(-1) || transcript_selection.dragging || cinema.painting) {
		return 0;
	}
	char error[256] = "";
	if (!history_reader(&scrollback.tree, error, sizeof(error))) {
		scrollback_close();
		if (*error) {
			ui_notice(error, 1);
		}
		return 0;
	}
	size_t count = scrollback.tree.count;
	scrollback.records = malloc((count ? count : 1) * sizeof(*scrollback.records));
	scrollback.calls = malloc((count ? count : 1) * sizeof(*scrollback.calls));
	scrollback.heights = calloc(count ? count : 1, sizeof(*scrollback.heights));
	size_t checkpoint = SIZE_MAX;
	if (!scrollback.records || !scrollback.calls || !scrollback.heights ||
		!scrollback_records(&scrollback.tree, scrollback.records, scrollback.calls,
			&scrollback.count, &checkpoint) ||
		!scrollback_fold_records(&scrollback.tree, scrollback.records, scrollback.calls,
			&scrollback.count, &checkpoint, &scrollback.groups, &scrollback.group_count)) {
		scrollback_close();
		return 0;
	}
	if (!scrollback.count) {
		scrollback_close();
		return 0;
	}
	scrollback.width = conversation_width > 0 ? conversation_width : 1;
	scrollback.rows = conversation_height > 0 ? conversation_height : 1;
	scrollback.cached = SIZE_MAX;
	scrollback.tail_maximum = conversation_maximum;
	scrollback.generation = theme_generation;
	scrollback.details = tool_details;
	scrollback.full = tool_full;
	scrollback.thinking = skin.thinking_visible;
	/* Locate the first complete saved text still in the live byte window.
       This also handles a live cache trimmed after the latest checkpoint.
       A checkpoint's synthetic model summary is not a saved conversation
       message: fall back to the preceding real messages, never show it as a
       separate "context checkpoint" page. The complete branch stays available. */
	size_t anchor = SIZE_MAX, best = SIZE_MAX;
	int anchor_top = 0;
	char first_line[512] = "";
	size_t first = 0;
	/* Byte zero may be a clipped suffix of a long line. Prefer the next
       complete line rather than matching that suffix elsewhere in a record. */
	const char *newline = memchr(transcript, '\n', used);
	if (newline) {
		first = (size_t)(newline - transcript) + 1;
	}
	while (first < used) {
		size_t bytes = strcspn(transcript + first, "\n");
		if (bytes >= 8 && transcript_style[first] < 64) {
			if (bytes >= sizeof(first_line)) {
				bytes = sizeof(first_line) - 1;
			}
			memcpy(first_line, transcript + first, bytes);
			first_line[bytes] = 0;
			break;
		}
		first += bytes + (transcript[first + bytes] == '\n');
	}
	for (size_t i = scrollback.count; i; i--) {
		if (!scrollback_load(i - 1)) {
			return 0;
		}
		size_t bytes = strlen(scrollback.body);
		const char *match =
			bytes >= 8 && bytes <= used ? strstr(transcript, scrollback.body) : NULL;
		if (match && (size_t)(match - transcript) < best) {
			best = (size_t)(match - transcript);
			anchor = i - 1;
			anchor_top = 0;
		}
		match = *first_line ? strstr(scrollback.display.text + scrollback.body_start, first_line)
							: NULL;
		if (match && first <= best) {
			best = first;
			anchor = i - 1;
			anchor_top = layout_saved_rows(NULL, scrollback.width, 0, &scrollback.display,
							 (size_t)(match - scrollback.display.text)) -
				1;
		}
		/* A leading semantic glyph occupies three bytes, not visible text.
           Once its following text matches, older records cannot anchor sooner. */
		if (best <= 3) {
			break;
		}
	}
	if (anchor != SIZE_MAX) {
		scrollback.selected = anchor;
	} else if (checkpoint != SIZE_MAX && checkpoint < scrollback.count) {
		scrollback.selected = checkpoint;
	} else {
		scrollback.selected = scrollback.count - 1;
		int height = scrollback_height(scrollback.selected);
		if (!height) {
			return 0;
		}
		anchor_top = height - 1;
	}
	if (anchor == 0 && checkpoint == SIZE_MAX) {
		if (!scrollback_load(0)) {
			return 0;
		}
		if (*scrollback.body && strstr(transcript, scrollback.body)) {
			scrollback_close();
			return 0; /* The oldest saved message is already in the live cache. */
		}
	}
	scrollback.top = anchor_top;
	scrollback.page_dirty = 1;
	transcript_selection_clear();
	transcript_cache_drop();
	int moved = scrollback_move(movement < 0 ? movement : -1);
	if (moved) {
		draw(1);
	}

	return moved;
}

static int scrollback_restyle(void) {

	if (scrollback.generation == theme_generation && scrollback.details == tool_details &&
		scrollback.full == tool_full && scrollback.thinking == skin.thinking_visible) {
		return 1;
	}
	if (scrollback.thinking != skin.thinking_visible || scrollback.details != tool_details) {
		size_t capacity = scrollback.tree.count ? scrollback.tree.count : 1, count = 0,
			   selected = 0;
		size_t *records = malloc(capacity * sizeof(*records)),
			   *calls = malloc(capacity * sizeof(*calls));
		int *heights = calloc(capacity, sizeof(*heights));
		struct scrollback_group *groups = NULL;
		size_t group_count = 0, checkpoint = SIZE_MAX;
		if (!records || !calls || !heights ||
			!scrollback_records(&scrollback.tree, records, calls, &count, &checkpoint) ||
			!scrollback_fold_records(
				&scrollback.tree, records, calls, &count, &checkpoint, &groups, &group_count) ||
			!count) {
			free(records);
			free(calls);
			free(heights);
			free(groups);
			scrollback_close();
			return 0;
		}
		uint64_t id = scrollback_id(
			&scrollback.tree, scrollback.records, scrollback.calls, scrollback.selected);
		for (size_t i = 0; i < count; i++) {
			if (scrollback_id(&scrollback.tree, records, calls, i) <= id) {
				selected = i;
			}
		}
		if (scrollback_id(&scrollback.tree, records, calls, selected) != id) {
			scrollback.top = 0;
		}
		free(scrollback.records);
		free(scrollback.calls);
		free(scrollback.heights);
		free(scrollback.groups);
		scrollback.records = records;
		scrollback.calls = calls;
		scrollback.heights = heights;
		scrollback.groups = groups;
		scrollback.group_count = group_count;
		scrollback.count = count;
		scrollback.selected = selected;
	} else {
		memset(scrollback.heights, 0, scrollback.count * sizeof(*scrollback.heights));
	}
	scrollback.cached = SIZE_MAX;
	scrollback.page_dirty = 1;
	scrollback.generation = theme_generation;
	scrollback.details = tool_details;
	scrollback.full = tool_full;
	scrollback.thinking = skin.thinking_visible;

	return 1;
}

static int scrollback_paint(int width, int rows, int left) {

	if (!scrollback_active() || pending_approval) {
		return 0;
	}
	if (!scrollback_restyle()) {
		return 0;
	}
	if (width < 1 || rows < 1) {
		return 0;
	}
	if (width != scrollback.width) {
		transcript_selection_clear();
		memset(scrollback.heights, 0, scrollback.count * sizeof(*scrollback.heights));
		scrollback.width = width;
		scrollback.page_dirty = 1;
	}
	if (rows != scrollback.rows) {
		scrollback.rows = rows;
		scrollback.page_dirty = 1;
	}
	if (scrollback.page_dirty || !scrollback.page) {
		if (scrollback.page) {
			delwin(scrollback.page);
		}
		scrollback.page = newpad(rows, width);
		if (!scrollback.page) {
			scrollback_close();
			return 0;
		}
		wbkgd(scrollback.page, COLOR_PAIR(27) | ' ');
		size_t position = scrollback.selected;
		int y = 0, top = scrollback.top;
		while (position < scrollback.count && y < rows) {
			int height = scrollback_height(position);
			if (!height || !scrollback_load(position)) {
				return 0;
			}
			if (top >= height) {
				top = height - 1;
			}
			int visible = height - top;
			if (visible > rows - y) {
				visible = rows - y;
			}
			if (visible > 0) {
				WINDOW *part = derwin(scrollback.page, visible, width, y, 0);
				if (part) {
					layout_saved_rows(
						part, width, top, &scrollback.display, scrollback.display.used);
					delwin(part);
				}
			}
			if (height - top >= rows - y) {
				break;
			}
			y += height - top;
			position++;
			top = 0;
		}
		scrollback.page_dirty = 0;
	}
	copywin(scrollback.page, stdscr, 0, 0, 0, left, rows - 1, left + width - 1, 0);
	conversation_source = conversation_destination = 0;
	conversation_copy_rows = conversation_pad_rows = rows;
	WINDOW *shown = derwin(stdscr, rows, width, 0, left);
	if (shown) {
		transcript_selection_highlight(shown, 0);
		delwin(shown);
	}

	return 1;
}

static void scrollback_search(void) {

	char *query = ui_ask_text("Find", "Find in conversation", "", 0, 0);
	if (!query || !*query || !scrollback_active()) {
		free(query);
		return;
	}
	int found = 0, old_details = tool_details, old_full = tool_full;
	transcript_selection_clear();
	/* Search the actual records, not just a folded activity sentence, and
       leave a successful match expanded so its text is visible. */
	tool_details = tool_full = 1;
	if (!scrollback_restyle()) {
		free(query);
		return;
	}
	for (size_t i = 0; i < scrollback.count; i++) {
		size_t position = (scrollback.selected + i) % scrollback.count;
		if (!scrollback_load(position)) {
			break;
		}
		const char *match = strcasestr(scrollback.body, query), *rendered = NULL;
		if (!match) {
			rendered = strcasestr(scrollback.display.text, query);
			while (
				rendered && scrollback.display.styles[rendered - scrollback.display.text] == 71) {
				rendered = strcasestr(rendered + 1, query);
			}
		}
		if (!match && !rendered) {
			continue;
		}
		scrollback.selected = position;
		scrollback.top = match ? scrollback_text_row((size_t)(match - scrollback.body))
							   : layout_saved_rows(NULL, scrollback.width, 0, &scrollback.display,
									 (size_t)(rendered - scrollback.display.text)) -
				1;
		scrollback.page_dirty = 1;
		found = 1;
		break;
	}
	free(query);
	if (!found && scrollback_active()) {
		tool_details = old_details;
		tool_full = old_full;
		scrollback_restyle();
		ui_notice("No match in the conversation.", 0);
	}
}

static int scrollback_key(int kind, wint_t key) {

	if (!scrollback_active() || modal_active || pending_approval || pasting) {
		return 0;
	}
	if (kind == OK && key == 6) {
		scrollback_search();
		return 1;
	}
	if (kind == OK && key == 7) {
		scrollback_close();
		scroll_offset = 0;
		working_view_valid = 0;
		return 1;
	}
	if (kind == KEY_CODE_YES && (key == KEY_PPAGE || key == KEY_NPAGE)) {
		int step = scrollback.rows > 1 ? scrollback.rows - 1 : 1;
		scrollback_move(key == KEY_PPAGE ? -step : step);
		return 1;
	}
	/* Editing, paste, permissions and cancellation stay on the normal path. */

	return 0;
}
