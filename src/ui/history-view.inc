/* Read-only, nonblocking saved-history view. Keep an index and one decoded
   record. Normal agent/network polling continues while old history is shown. */
static int (*history_reader)(struct session_tree *, char *, size_t);
/* Keep draining a paste even if the view closes because of a read error. */
static int history_paste_pending;

static int history_view_paste(int state) {

	if (state == 0) {
		history_paste_pending = 0;
	} else if (state > 0 && history_view_active()) {
		history_paste_pending = 1;
	}

	return history_paste_pending;
}

static struct {
	int open, raw, searching, dirty, page_dirty, was_working;
	struct session_tree tree;
	size_t *records, count, selected, cached;
	json_object *item;
	char *body;
	int top, total, width, rows, cached_raw, markdown;
	WINDOW *page;
	char query[128];
	size_t query_size;
} history_view;

void ui_set_history_reader(int (*reader)(struct session_tree *, char *, size_t)) {

	if (reader != history_reader) {
		scrollback_close();
	}
	history_reader = reader;
}

static int history_view_active(void) {

	return history_view.open;
}

static void history_view_close(void) {

	if (history_view.page) {
		delwin(history_view.page);
	}
	if (history_view.item) {
		json_object_put(history_view.item);
	}
	free(history_view.body);
	free(history_view.records);
	session_tree_free(&history_view.tree);
	memset(&history_view, 0, sizeof(history_view));
	dirty = 1;
	cursor_shape = -1;
}

static char *history_record_body(json_object *item, int raw) {

	if (raw) {
		if (!json_object_is_type(item, json_type_array)) {
			return strdup(json_object_to_json_string_ext(item, JSON_C_TO_STRING_PRETTY));
		}
		json_object *visible = json_object_new_array();
		if (!visible) {
			return NULL;
		}
		for (size_t i = 0; i < json_object_array_length(item); i++) {
			json_object *entry = json_object_array_get_idx(item, i);
			if (!history_is_hidden(entry)) {
				json_object_array_add(visible, json_object_get(entry));
			}
		}
		char *text = strdup(json_object_to_json_string_ext(visible, JSON_C_TO_STRING_PRETTY));
		json_object_put(visible);
		return text;
	}
	if (json_object_is_type(item, json_type_array)) {
		return strdup("Context checkpoint. Earlier saved messages remain available above.");
	}
	json_object *content = json_object_object_get(item, "content");
	if (json_object_is_type(content, json_type_string)) {
		return strdup(json_object_get_string(content));
	}
	if (json_object_is_type(content, json_type_array)) {
		size_t bytes = 1, count = json_object_array_length(content);
		for (size_t i = 0; i < count; i++) {
			json_object *part = json_object_array_get_idx(content, i);
			const char *type = field(part, "type");
			if (!strcmp(type, "input_text") || !strcmp(type, "output_text")) {
				bytes += strlen(field(part, "text")) + 1;
			}
		}
		char *text = malloc(bytes);
		if (!text) {
			return NULL;
		}
		size_t used_bytes = 0;
		for (size_t i = 0; i < count; i++) {
			json_object *part = json_object_array_get_idx(content, i);
			const char *type = field(part, "type");
			if (strcmp(type, "input_text") && strcmp(type, "output_text")) {
				continue;
			}
			if (used_bytes) {
				text[used_bytes++] = '\n';
			}
			const char *part_text = field(part, "text");
			size_t n = strlen(part_text);
			memcpy(text + used_bytes, part_text, n);
			used_bytes += n;
		}
		text[used_bytes] = 0;
		if (used_bytes) {
			return text;
		}
		free(text);
		return strdup("[non-text content; Ctrl+O shows the saved record]");
	}
	if (!strcmp(field(item, "type"), "function_call")) {
		return strdup(field(item, "arguments"));
	}
	if (!strcmp(field(item, "type"), "function_call_output")) {
		const char *output = field(item, "output");
		json_object *result = json_tokener_parse(output);
		char *text = json_object_is_type(result, json_type_object) && *field(result, "text")
			? strdup(field(result, "text"))
			: strdup(output);
		if (result) {
			json_object_put(result);
		}
		return text;
	}

	return strdup(json_object_to_json_string_ext(item, JSON_C_TO_STRING_PRETTY));
}

/* Reuse normal inline Markdown, UTF-8 and wrapping. Paint only a viewport pad;
   prefix layout also locates a search match without storing every text line. */
static int history_record_markdown(json_object *item, int raw) {

	return !raw && json_object_is_type(item, json_type_object) &&
		!strcmp(field(item, "role"), "assistant") &&
		(!*field(item, "type") || !strcmp(field(item, "type"), "message"));
}

static int history_record_rows(
	WINDOW *pad, const char *text, size_t bytes, int width, int first, int markdown) {

	struct md_canvas canvas = {.pad = pad, .width = width, .origin_y = -first, .word_wrap = 1};
	size_t at = 0;
	int fenced = 0;
	while (at < bytes && canvas.y < INT_MAX - 16) {
		if (pad && canvas.y - first >= getmaxy(pad)) {
			break;
		}
		const char *end = memchr(text + at, '\n', bytes - at);
		size_t n = end ? (size_t)(end - (text + at)) : bytes - at;
		const char *line = text + at;
		if (!markdown) {
			md_text(&canvas, line, n, COLOR_PAIR(27), 0);
		} else if (n >= 3 && (!memcmp(line, "```", 3) || !memcmp(line, "~~~", 3))) {
			fenced = !fenced;
			md_text(&canvas, line, n, COLOR_PAIR(3), 0);
		} else if (fenced) {
			md_text(&canvas, line, n, COLOR_PAIR(6), 0);
		} else {
			md_inline(&canvas, line, n, COLOR_PAIR(27), 0);
		}
		at += n + (end ? 1 : 0);
		if (at < bytes) {
			canvas.y++;
			canvas.x = 0;
		}
	}

	return canvas.y + 1;
}

int ui_history_open(void) {

	if (history_view.open) {
		return 1;
	}
	if (!history_reader || !active || modal_active || pasting || history_view_paste(-1) ||
		transcript_selection.dragging || cinema.painting) {
		return 0;
	}
	scrollback_close();
	char error[256] = "";
	if (!history_reader(&history_view.tree, error, sizeof(error))) {
		session_tree_free(&history_view.tree);
		if (*error) {
			ui_notice(error, 1);
		}
		return 0;
	}
	struct session_tree *tree = &history_view.tree;
	history_view.records = malloc((tree->count ? tree->count : 1) * sizeof(*history_view.records));
	if (!history_view.records) {
		history_view_close();
		return 0;
	}
	for (size_t i = 0; i < tree->count; i++) {
		if (tree->nodes[i].active && !tree->nodes[i].hidden) {
			history_view.records[history_view.count++] = i;
		}
	}
	if (!history_view.count) {
		history_view_close();
		return 0;
	}
	history_view.open = 1;
	history_view.cached = SIZE_MAX;
	history_view.cached_raw = -1;
	ctrl_c_armed = esc_armed = left_armed = delete_pending = 0;
	history_view.dirty = history_view.page_dirty = 1;
	ui_layout_close();
	dirty = 1;
	draw(1);

	return 1;
}

static int history_view_paint(int force) {

	if (!history_view.open) {
		return 0;
	}
	if (force || history_view.was_working != working || title_redraw_due()) {
		history_view.dirty = 1;
	}
	history_view.was_working = working;
	int width = COLS > 4 ? COLS - 2 : 1, rows = LINES > 5 ? LINES - 4 : 1;
	if (width != history_view.width || rows != history_view.rows) {
		if (width != history_view.width) {
			history_view.total = 0;
		}
		history_view.width = width;
		history_view.rows = rows;
		history_view.dirty = history_view.page_dirty = 1;
	}
	if (!history_view.dirty) {
		return 0;
	}
	char error[256] = "";
	if (history_view.selected != history_view.cached ||
		history_view.raw != history_view.cached_raw) {
		free(history_view.body);
		history_view.body = NULL;
		if (history_view.item) {
			json_object_put(history_view.item);
		}
		history_view.item = session_tree_item(
			&history_view.tree, history_view.records[history_view.selected], error, sizeof(error));
		if (history_view.item) {
			history_view.body = history_record_body(history_view.item, history_view.raw);
		}
		if (!history_view.item || !history_view.body) {
			if (!*error) {
				snprintf(error, sizeof(error), "Not enough memory to display this saved record.");
			}
			history_view_close();
			ui_notice(error, 1);
			return 0;
		}
		history_view.cached = history_view.selected;
		history_view.cached_raw = history_view.raw;
		history_view.markdown = history_record_markdown(history_view.item, history_view.raw);
		history_view.total = 0;
		history_view.page_dirty = 1;
	}
	if (history_view.page_dirty) {
		if (!history_view.total) {
			history_view.total = history_record_rows(NULL, history_view.body,
				strlen(history_view.body), width, 0, history_view.markdown);
		}
		int maximum = history_view.total > rows ? history_view.total - rows : 0;
		if (history_view.top > maximum) {
			history_view.top = maximum;
		}
		if (history_view.top < 0) {
			history_view.top = 0;
		}
		if (history_view.page) {
			delwin(history_view.page);
		}
		history_view.page = newpad(rows, width);
		if (history_view.page) {
			history_record_rows(history_view.page, history_view.body, strlen(history_view.body),
				width, history_view.top, history_view.markdown);
		}
		history_view.page_dirty = 0;
	}
	welcome_scroll = 0;
	welcome_clip = -1;
	erase();
	char heading[192];
	snprintf(heading, sizeof(heading), "Saved history (snapshot) · %zu/%zu · %s%s",
		history_view.selected + 1, history_view.count,
		history_view.tree.nodes[history_view.records[history_view.selected]].kind ==
				SESSION_COMPACTION
			? "checkpoint"
			: *field(history_view.item, "role") ? field(history_view.item, "role")
												: field(history_view.item, "type"),
		working ? " · live turn running" : "");
	welcome_text(0, 1, width, heading, 28, 1);
	if (history_view.searching || *history_view.query) {
		char label[160];
		snprintf(label, sizeof(label), "Find: %s", history_view.query);
		welcome_text(1, 1, width, label, 3, 0);
	}
	if (history_view.page) {
		copywin(history_view.page, stdscr, 0, 0, 2, 1, 2 + rows - 1, width, 0);
	}
	const char *help = width < 50 ? "Esc close · ↑↓ scroll · / find"
		: width < 90
		? "Esc close · ↑↓ scroll · ←→ entry · / find · Ctrl+O details"
		: "↑↓ scroll · ←→ entry · PgUp/PgDn · Home first · End live · / find · n next · Ctrl+O details · Esc close";
	welcome_text(LINES - 1, 1, width, help, 3, 0);
	curs_set(0);
	attrset(A_NORMAL);
	frame_begin();
	wnoutrefresh(stdscr);
	title_update_begin();
	doupdate();
	title_update_end();
	frame_end();
	history_view.dirty = 0;

	return 1;
}

/* Consume archive keys without entering a nested input/network loop. */
static int history_view_key(int kind, wint_t key) {

	if (kind == KEY_CODE_YES && key == KEY_PASTE_BEGIN) {
		history_view_paste(1);
		return 0;
	}
	if (kind == KEY_CODE_YES && key == KEY_PASTE_END) {
		history_view_paste(0);
		return 0;
	}
	if (history_view_paste(-1)) {
		return 0;
	}
	if (kind == KEY_CODE_YES && (key == KEY_FOCUS_IN || key == KEY_FOCUS_OUT)) {
		focused = key == KEY_FOCUS_IN;
		return 0;
	}
	if (kind == KEY_CODE_YES && key == KEY_RESIZE) {
		title_resized();
		history_view.width = 0;
		history_view.dirty = 1;
		return 1;
	}
	if (history_view.searching) {
		if ((kind == OK && (key == 27 || key == 3)) ||
			(kind == KEY_CODE_YES && (key == KEY_ESCAPE || key == KEY_INTERRUPT))) {
			history_view.searching = 0;
			history_view.dirty = 1;
			return 1;
		}
		if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
			(kind == OK && (key == 127 || key == 8))) {
			if (history_view.query_size) {
				history_view.query_size--;
				while (history_view.query_size &&
					((unsigned char)history_view.query[history_view.query_size] & 0xc0) == 0x80) {
					history_view.query_size--;
				}
				history_view.query[history_view.query_size] = 0;
			}
			history_view.dirty = 1;
			return 1;
		}
		if (kind == OK && key >= 32 && key <= WCHAR_MAX) {
			char encoded[MB_LEN_MAX];
			mbstate_t state = {0};
			size_t bytes = wcrtomb(encoded, (wchar_t)key, &state);
			if (bytes != (size_t)-1 &&
				history_view.query_size + bytes < sizeof(history_view.query)) {
				memcpy(history_view.query + history_view.query_size, encoded, bytes);
				history_view.query_size += bytes;
				history_view.query[history_view.query_size] = 0;
			}
			history_view.dirty = 1;
			return 1;
		}
		if (!((kind == OK && (key == '\r' || key == '\n')) ||
				(kind == KEY_CODE_YES && key == KEY_ENTER))) {
			return 0;
		}
		history_view.searching = 0;
		key = 'n';
		kind = OK;
	} else if ((kind == OK && (key == 27 || key == 3 || key == 4 || key == 'q')) ||
		(kind == KEY_CODE_YES &&
			(key == KEY_ESCAPE || key == KEY_INTERRUPT || key == KEY_EOF || key == KEY_END))) {
		if (kind == KEY_CODE_YES && key == KEY_END) {
			scroll_offset = 0;
			working_view_valid = 0;
		}
		history_view_close();
		return 1;
	}
	if (kind == OK && (key == '/' || key == 6)) {
		history_view.searching = 1;
		history_view.query_size = 0;
		*history_view.query = 0;
	} else if (kind == OK && key == 'n' && *history_view.query) {
		char error[256];
		for (size_t step = 1; step <= history_view.count; step++) {
			size_t candidate = (history_view.selected + step) % history_view.count;
			json_object *record = session_tree_item(
				&history_view.tree, history_view.records[candidate], error, sizeof(error));
			char *text = record ? history_record_body(record, history_view.raw) : NULL;
			const char *match = text ? strcasestr(text, history_view.query) : NULL;
			int found = match != NULL;
			if (found) {
				history_view.selected = candidate;
				history_view.top =
					history_record_rows(NULL, text, (size_t)(match - text), history_view.width, 0,
						history_record_markdown(record, history_view.raw)) -
					1;
			}
			free(text);
			if (record) {
				json_object_put(record);
			}
			if (found) {
				break;
			}
		}
	} else if (kind == OK && key == 15) {
		history_view.raw = !history_view.raw;
		history_view.top = 0;
	} else if ((kind == KEY_CODE_YES && key == KEY_HOME) || (kind == OK && key == 'g')) {
		history_view.selected = 0;
		history_view.top = 0;
	} else if (kind == OK && key == 'G') {
		history_view.selected = history_view.count - 1;
		history_view.top = INT_MAX;
	} else {
		int movement = 0, entry = 0;
		if ((kind == KEY_CODE_YES && key == KEY_UP) || (kind == OK && key == 'k')) {
			movement = -1;
		}
		if ((kind == KEY_CODE_YES && key == KEY_DOWN) || (kind == OK && key == 'j')) {
			movement = 1;
		}
		if (kind == KEY_CODE_YES && key == KEY_PPAGE) {
			movement = -history_view.rows;
		}
		if (kind == KEY_CODE_YES && key == KEY_NPAGE) {
			movement = history_view.rows;
		}
		if (kind == KEY_CODE_YES && key == KEY_LEFT) {
			entry = -1;
		}
		if (kind == KEY_CODE_YES && key == KEY_RIGHT) {
			entry = 1;
		}
		if (kind == KEY_CODE_YES && key == KEY_MOUSE) {
			MEVENT mouse;
			if (getmouse(&mouse) == OK) {
				if (mouse.bstate & BUTTON4_PRESSED) {
					movement = -wheel_lines(1);
				}
#ifdef BUTTON5_PRESSED
				if (mouse.bstate & BUTTON5_PRESSED) {
					movement = wheel_lines(-1);
				}
#endif
			}
		}
		if (!movement && !entry) {
			return 0;
		}
		int maximum =
			history_view.total > history_view.rows ? history_view.total - history_view.rows : 0;
		if (entry < 0 || (movement < 0 && !history_view.top)) {
			if (history_view.selected) {
				history_view.selected--;
				history_view.top = INT_MAX;
			}
		} else if (entry > 0 || (movement > 0 && history_view.top >= maximum)) {
			if (history_view.selected + 1 < history_view.count) {
				history_view.selected++;
				history_view.top = 0;
			} else if (movement > 0) {
				scroll_offset = 0;
				working_view_valid = 0;
				history_view_close();
				return 1;
			}
		} else if (movement) {
			history_view.top += movement;
			if (history_view.top < 0) {
				history_view.top = 0;
			}
		}
	}
	history_view.dirty = history_view.page_dirty = 1;

	return 1;
}
