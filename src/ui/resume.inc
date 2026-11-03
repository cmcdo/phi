#include "../core/history.h"

/* A local, modal fuzzy finder. Nothing here submits a prompt or runs tools. */
#include "../core/sessions.h"

struct resume_match {
	size_t index;
	int score, starred;
};

static int resume_rank(const void *a, const void *b) {

	const struct resume_match *x = a, *y = b;
	if (x->starred != y->starred) {
		return x->starred ? -1 : 1;
	}
	if (x->score != y->score) {
		return x->score > y->score ? -1 : 1;
	}

	return x->index < y->index ? -1 : x->index != y->index;
}

static void resume_age(time_t stamp, char *text, size_t size) {

	time_t clock = time(NULL);
	long age = clock > stamp ? (long)(clock - stamp) : 0;
	if (age < 60) {
		snprintf(text, size, "now");
	} else if (age < 3600) {
		snprintf(text, size, "%ldm ago", age / 60);
	} else if (age < 86400) {
		snprintf(text, size, "%ldh ago", age / 3600);
	} else {
		snprintf(text, size, "%ldd ago", age / 86400);
	}
}

static void resume_preview(const struct session_entry *entry, char *text, size_t size) {

	char error[256];
	if (!session_recent_text(entry, text, size, error, sizeof(error))) {
		snprintf(text, size, "%s", error);
		return;
	}
	if (!*text) {
		snprintf(text, size, "No complete text messages in the last 64 KiB.");
	}
}

/* Paint into a bounded pad so wrapping cannot overwrite the modal footer. */
static int resume_preview_layout(WINDOW *pad, const char *text, int width, int origin_y) {

	struct md_canvas canvas = {.pad = pad, .width = width, .word_wrap = 1, .origin_y = origin_y};
	const char *line = text;
	while (*line) {
		const char *end = strchr(line, '\n');
		size_t bytes = end ? (size_t)(end - line) : strlen(line);
		md_text(&canvas, line, bytes, COLOR_PAIR(27), 0);
		canvas.y++;
		canvas.x = 0;
		if (!end) {
			break;
		}
		line = end + 1;
	}

	return canvas.y;
}

static void resume_preview_paint(WINDOW *pad, const char *text, int width, int rows) {

	int height = resume_preview_layout(NULL, text, width, 0);
	resume_preview_layout(pad, text, width, height > rows ? rows - height : 0);
}

int ui_resume(struct session_entry *entries, size_t count) {

	struct resume_match *matches = malloc((count ? count : 1) * sizeof(*matches));
	if (!matches) {
		return -1;
	}
	wchar_t query[256] = L"";
	size_t query_length = 0, matched = 0, selected = 0, list_first = 0, preview_index = (size_t)-1;
	int changed = 1, preview = 0, paste = 0, result = -1;
	char preview_text[65537] = "", star_error[256] = "";
	int stars_only = 0;
	modal_active = 1;
	for (;;) {
		if (idle_callback) {
			idle_callback();
		}
		if (changed) {
			char utf8[1024];
			size_t bytes = wcstombs(utf8, query, sizeof(utf8) - 1);
			if (bytes == (size_t)-1) {
				bytes = 0;
			}
			utf8[bytes] = 0;
			matched = 0;
			for (size_t i = 0; i < count; i++) {
				if (stars_only && !entries[i].starred) {
					continue;
				}
				int score = session_score(entries + i, utf8);
				if (score >= 0) {
					matches[matched++] = (struct resume_match){i, score, entries[i].starred};
				}
			}
			qsort(matches, matched, sizeof(*matches), resume_rank);
			selected = list_first = 0;
			changed = 0;
		}
		draw(1);
		welcome_scroll = 0;
		welcome_clip = -1;
		int width = COLS - 8, height = LINES - 4;
		if (width > 104) {
			width = 104;
		}
		if (height > 24) {
			height = 24;
		}
		int usable = width >= 42 && height >= 12;
		if (usable) {
			int x = (COLS - width) / 2, y = (LINES - height) / 2;
			for (int row = 0; row < height; row++) {
				attrset(A_NORMAL);
				mvhline(y + row, x, ' ', width);
			}
			attrset(COLOR_PAIR(5));
			mvhline(y, x, ACS_HLINE, width);
			mvhline(y + height - 1, x, ACS_HLINE, width);
			mvvline(y, x, ACS_VLINE, height);
			mvvline(y, x + width - 1, ACS_VLINE, height);
			mvaddch(y, x, ACS_ULCORNER);
			mvaddch(y, x + width - 1, ACS_URCORNER);
			mvaddch(y + height - 1, x, ACS_LLCORNER);
			mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
			welcome_text(y, x + 2, 22, stars_only ? " Starred transcripts " : " Resume ", 28, 1);
			char status[80];
			snprintf(status, sizeof(status), " %zu / %zu ", matched, count);
			welcome_text(y, x + width - (int)strlen(status) - 2, (int)strlen(status), status, 3, 0);
			char utf8[1024];
			size_t bytes = wcstombs(utf8, query, sizeof(utf8) - 1);
			if (bytes == (size_t)-1) {
				bytes = 0;
			}
			utf8[bytes] = 0;
			welcome_text(y + 2, x + 2, width - 4,
				*utf8 ? utf8 : "Search titles, folders & recent messages…", *utf8 ? 27 : 3, 0);
			attrset(COLOR_PAIR(5));
			mvhline(y + 3, x + 1, ACS_HLINE, width - 2);
			int split = preview && width >= 80;
			int list_width = split ? (width - 4) / 2 : width - 4;
			if (!preview || split) {
				size_t rows = (size_t)(height - 7) / 2;
				list_first = phi_list_scroll(list_first, selected, rows, matched);
				size_t first = list_first;
				if (!matched) {
					welcome_text(y + 5, x + 2, list_width,
						count ? "No matches" : "No saved sessions yet", 3, 0);
				}
				for (size_t i = first; i < matched && i < first + rows; i++) {
					const struct session_entry *entry = entries + matches[i].index;
					int row = y + 4 + (int)(i - first) * 2, highlighted = i == selected;
					char age[32];
					resume_age(entry->modified, age, sizeof(age));
					int age_width = (int)strlen(age);
					char title[272];
					snprintf(
						title, sizeof(title), "%s%s", entry->starred ? "★ " : "", entry->title);
					welcome_text(row, x + 2, list_width - age_width - 2, title,
						highlighted ? 28 : 27, highlighted);
					welcome_text(row, x + 2 + list_width - age_width, age_width, age, 3, 0);
					welcome_text(row + 1, x + 2, list_width, entry->cwd, 3, 0);
				}
			}
			if (preview && matched) {
				size_t index = matches[selected].index;
				if (preview_index != index) {
					resume_preview(entries + index, preview_text, sizeof(preview_text));
					preview_index = index;
				}
				int px = split ? x + width / 2 + 1 : x + 2;
				int pw = split ? width / 2 - 3 : width - 4;
				if (split) {
					attrset(COLOR_PAIR(5));
					mvvline(y + 4, px - 2, ACS_VLINE, height - 7);
				}
				int rows = height - 7;
				WINDOW *pad = newpad(rows, pw);
				if (pad) {
					resume_preview_paint(pad, preview_text, pw, rows);
					copywin(pad, stdscr, 0, 0, y + 4, px, y + 4 + rows - 1, px + pw - 1, 0);
					delwin(pad);
				}
			}
			welcome_text(y + height - 3, x + 2, width - 4,
				*star_error ? star_error
							: "Ctrl+B star/unstar · Ctrl+F starred only · /name after resume",
				3, 0);
			welcome_text(y + height - 2, x + 2, width - 4,
				width >= 72 ? "↑↓ Ctrl-J/K · Enter resume · Tab preview · Esc cancel"
							: "↑↓ select · Enter · Tab preview · Esc",
				3, 0);
		} else {
			welcome_text(
				LINES / 2, 1, COLS - 2, "Enlarge terminal for sessions · Esc cancels", 3, 0);
		}
		curs_set(0);
		attrset(A_NORMAL);
		frame_begin();
		wnoutrefresh(stdscr);
		doupdate();
		frame_end();
		wint_t key;
		int kind = read_key(&key, 250);
		if (kind == ERR) {
			continue;
		}
		if (kind == KEY_CODE_YES && key == KEY_RESIZE) {
			title_resized();
			continue;
		}
		if (kind == KEY_CODE_YES && key == KEY_PASTE_BEGIN) {
			paste = 1;
			continue;
		}
		if (kind == KEY_CODE_YES && key == KEY_PASTE_END) {
			paste = 0;
			continue;
		}
		if (!paste &&
			((kind == OK && (key == 27 || key == 3 || key == 4)) ||
				(kind == KEY_CODE_YES &&
					(key == KEY_ESCAPE || key == KEY_INTERRUPT || key == KEY_EOF)))) {
			break;
		}
		if (!paste && kind == OK && key == 6) {
			stars_only = !stars_only;
			changed = 1;
			continue;
		}
		if (!paste && kind == OK && key == 2) {
			if (matched) {
				struct session_entry *entry = entries + matches[selected].index;
				if (session_star(entry, !entry->starred, star_error, sizeof(star_error))) {
					changed = 1;
				}
			}
			continue;
		}
		if (!paste && usable &&
			((kind == OK && key == '\r') || (kind == KEY_CODE_YES && key == KEY_ENTER))) {
			if (matched) {
				result = (int)matches[selected].index;
				break;
			}
		} else if (!paste &&
			((kind == KEY_CODE_YES && (key == KEY_DOWN || key == KEY_NEWLINE)) ||
				(kind == OK && key == 10))) {
			if (matched) {
				selected = (selected + 1) % matched;
			}
		} else if (!paste &&
			((kind == KEY_CODE_YES && key == KEY_UP) || (kind == OK && key == 11))) {
			if (matched) {
				selected = (selected + matched - 1) % matched;
			}
		} else if (!paste && kind == OK && key == '\t') {
			preview = !preview;
		} else if (!paste &&
			((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
				(kind == OK && (key == 8 || key == 127)))) {
			if (query_length) {
				query[--query_length] = 0;
				changed = 1;
			}
		} else if (!paste && kind == OK && key == 21) {
			query_length = 0;
			query[0] = 0;
			changed = 1;
		} else if (kind == OK && key >= 32 && wcwidth((wchar_t)key) >= 0 && query_length < 255) {
			query[query_length++] = (wchar_t)key;
			query[query_length] = 0;
			changed = 1;
		}
	}
	free(matches);
	modal_active = 0;
	dirty = 1;
	cursor_shape = -1;
	draw(1);

	return result;
}

static const char *resume_field(json_object *item, const char *name) {

	json_object *value = item ? json_object_object_get(item, name) : NULL;

	return json_object_is_type(value, json_type_string) ? json_object_get_string(value) : "";
}

static json_object *resume_call;

static void resume_diff_text(char kind, const char *text) {

	/* Saved replacements have no original file offsets. Empty newText is a
       deletion, not an added blank line. */
	while (*text) {
		size_t n = strcspn(text, "\n");
		char *copy = malloc(n + 2);
		if (!copy) {
			return;
		}
		memcpy(copy, text, n);
		copy[n] = '\n';
		copy[n + 1] = 0;
		ui_diff_line(kind, 0, copy, n + 1);
		free(copy);
		text += n;
		if (*text == '\n') {
			text++;
		} else {
			break;
		}
	}
}

static void resume_tool_preview(json_object *call, json_object *answer) {

	const char *name = resume_field(call, "name");
	const char *arguments = resume_field(call, "arguments");
	json_object *args = strlen(arguments) <= 4 * 1024 * 1024 ? json_tokener_parse(arguments) : NULL;
	if (!strcmp(name, "edit") && !*resume_field(answer, "error")) {
		json_object *edits = args ? json_object_object_get(args, "edits") : NULL;
		for (size_t i = 0;
			json_object_is_type(edits, json_type_array) && i < json_object_array_length(edits);
			i++) {
			json_object *edit = json_object_array_get_idx(edits, i);
			char label[96];
			snprintf(label, sizeof(label), "Change %zu · saved snippet (file lines unavailable)\n",
				i + 1);
			append_styled(label, 74); /* saved-snippet label; rendered by the current theme */
			resume_diff_text('-', resume_field(edit, "oldText"));
			resume_diff_text('+', resume_field(edit, "newText"));
		}
	} else if (!strcmp(name, "write") && !*resume_field(answer, "error")) {
		const char *content = resume_field(args, "content");
		if (*content) {
			ui_write_preview(content);
		}
	} else {
		const char *preview = resume_field(answer, "error");
		if (!*preview) {
			preview = resume_field(answer, "text");
		}
		if (!*preview) {
			preview = resume_field(answer, "output");
		}
		if (*preview) {
			if (!strcmp(name, "read")) {
				ui_read_preview(preview);
			} else {
				append_styled(preview, 5);
				if (preview[strlen(preview) - 1] != '\n') {
					append_styled("\n", 5);
				}
			}
		}
	}
	if (args) {
		json_object_put(args);
	}
}

static json_object *(*tool_renderer)(json_object *call, const char *heading, const char *detail);

void ui_set_tool_renderer(
	json_object *(*render)(json_object *call, const char *heading, const char *detail)) {

	tool_renderer = render;
}

static void resume_tool(json_object *item) {

	const char *type = resume_field(item, "type");
	if (!strcmp(type, "function_call")) {
		resume_call = item;
		const char *arguments = resume_field(item, "arguments");
		json_object *args = strlen(arguments) <= 1024 * 1024 ? json_tokener_parse(arguments) : NULL;
		const char *keys[] = {"path", "command", "query", "url"}, *detail = "";
		for (size_t i = 0; args && i < sizeof(keys) / sizeof(*keys); i++) {
			if (*(detail = resume_field(args, keys[i]))) {
				break;
			}
		}
		char short_detail[512];
		snprintf(short_detail, sizeof(short_detail), "%.500s", detail);
		if (args && !strcmp(resume_field(item, "name"), "bash_poll")) {
			snprintf(short_detail, sizeof(short_detail), "%lld",
				(long long)json_object_get_int64(json_object_object_get(args, "job_id")));
		}
		for (char *p = short_detail; *p; p++) {
			if ((unsigned char)*p < 32) {
				*p = ' ';
			}
		}
		/* tool_render handlers draw restored cards too (Pi's registerToolRenderer). */
		json_object *render =
			tool_renderer ? tool_renderer(item, resume_field(item, "name"), short_detail) : NULL;
		if (render && *resume_field(render, "title")) {
			ui_tool_title(resume_field(render, "title"));
		}
		ui_tool_begin(resume_field(item, "name"), short_detail);
		ui_tool_invocation(item);
		json_object *lines = render ? json_object_object_get(render, "lines") : NULL;
		for (size_t i = 0; lines && i < json_object_array_length(lines); i++) {
			json_object *line = json_object_array_get_idx(lines, i);
			if (json_object_is_type(line, json_type_string)) {
				ui_tool_line(json_object_get_string(line));
			}
		}
		if (render) {
			json_object_put(render);
		}
		if (args) {
			json_object_put(args);
		}
	} else if (!strcmp(type, "function_call_output")) {
		const char *output = resume_field(item, "output");
		json_object *answer = strlen(output) <= 4 * 1024 * 1024 ? json_tokener_parse(output) : NULL;
		if (resume_call && answer) {
			resume_tool_preview(resume_call, answer);
		}
		/* The same rule as the live card: errors, timeouts and non-zero exits fail. */
		char summary[768] = "Saved result is invalid";
		int failed = !answer || tool_answer_failed(answer, summary, sizeof(summary));
		tool_historical = 1; /* Suppress timing, not grouping or status rendering. */
		if (answer) {
			ui_tool_answer(answer);
		}
		ui_tool_end(failed ? summary : "", failed);
		if (answer) {
			json_object_put(answer);
		}
		resume_call = NULL;
	}
}

void ui_restore_history(json_object *history) {

	/* Batch the replay into one repaint; never replay tool execution or stream deltas. */
	int was_active = active;
	active = 0;
	ui_clear();
	for (size_t i = 0; i < queued_count; i++) {
		free(queued[i]);
	}
	queued_count = 0;
	char *text = malloc(LOG_SIZE);
	/* A turn's length, as shown live: from your message to the last item
       saved before the next one (saved times are whole seconds). */
	time_t turn_start = 0, turn_last = 0;
	if (text) {
		for (size_t i = 0; i < json_object_array_length(history); i++) {
			json_object *item = json_object_array_get_idx(history, i);
			time_t saved = sessions_item_time(item);
			if (history_is_hidden(item)) {
				if (saved) {
					turn_last = saved;
				}
				continue;
			}
			session_item_text(item, text, LOG_SIZE);
			const char *role = json_object_get_string(json_object_object_get(item, "role"));
			int user = *text && role && !strcmp(role, "user");
			if (user) {
				if (turn_start && turn_last > turn_start) {
					turn_done_line((long)(turn_last - turn_start), turn_last);
				}
				turn_start = turn_last = saved;
			} else if (saved) {
				turn_last = saved;
			}
			if (!*text) {
				if (!strcmp(resume_field(item, "type"), "reasoning")) {
					json_object *summary = json_object_object_get(item, "summary");
					for (size_t part = 0; json_object_is_type(summary, json_type_array) &&
						part < json_object_array_length(summary);
						part++) {
						json_object *entry = json_object_array_get_idx(summary, part);
						if (!strcmp(resume_field(entry, "type"), "summary_text")) {
							ui_thinking(resume_field(entry, "text"), 1);
						}
					}
					ui_thinking("", 1);
				} else {
					resume_tool(item);
				}
				continue;
			}
			if (user) {
				const char *via = resume_field(json_object_object_get(item, "_phi_origin"), "via");
				if (*via) {
					char label[180];
					snprintf(label, sizeof(label), "\n[via %.128s]\n", via);
					append_styled(label, 2);
				}
				ui_user(text);
			} else {
				ui_assistant(text);
				ui_append("\n\n");
			}
		}
	}
	if (turn_start && turn_last > turn_start) {
		turn_done_line((long)(turn_last - turn_start), turn_last);
	}
	free(text);
	/* CWD may have changed to the session's saved directory. Memory stays a startup snapshot. */
	welcome_set_path();
	active = was_active;
	dirty = 1;
	draw(1);
#ifdef __GLIBC__
	/* Parsing a long saved session leaves reusable heap pages behind. Return
       unused pages once, after restore, not on every frame or streamed token. */
	malloc_trim(0);
#endif
}
