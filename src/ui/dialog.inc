/* Extension dialogs (Pi's ctx.ui confirm/select/input/editor) and notices.
   Each dialog is a centred box over the conversation, drawn like the resume
   picker, with its own key loop; the extension waits for the answer.
   Included by ui.c. */

struct dialog_box {
	int x, y, width, height;
};

static struct dialog_box dialog_frame(const char *title, int wanted_height) {

	struct dialog_box box;
	box.width = COLS - 8 > 76 ? 76 : COLS - 8;
	box.height = wanted_height > LINES - 2 ? LINES - 2 : wanted_height;
	box.x = (COLS - box.width) / 2;
	box.y = (LINES - box.height) / 2;
	draw(1);
	welcome_scroll = 0;
	welcome_clip = -1;
	for (int row = 0; row < box.height; row++) {
		attrset(A_NORMAL);
		mvhline(box.y + row, box.x, ' ', box.width);
	}
	attrset(COLOR_PAIR(5));
	mvhline(box.y, box.x, ACS_HLINE, box.width);
	mvhline(box.y + box.height - 1, box.x, ACS_HLINE, box.width);
	mvvline(box.y, box.x, ACS_VLINE, box.height);
	mvvline(box.y, box.x + box.width - 1, ACS_VLINE, box.height);
	mvaddch(box.y, box.x, ACS_ULCORNER);
	mvaddch(box.y, box.x + box.width - 1, ACS_URCORNER);
	mvaddch(box.y + box.height - 1, box.x, ACS_LLCORNER);
	mvaddch(box.y + box.height - 1, box.x + box.width - 1, ACS_LRCORNER);
	char heading[160];
	snprintf(heading, sizeof(heading), " %.140s ", title && *title ? title : "Extension");
	welcome_text(box.y, box.x + 2, box.width - 4, heading, 28, 1);

	return box;
}

static void dialog_show(void) {

	attrset(A_NORMAL);
	frame_begin();
	wnoutrefresh(stdscr);
	doupdate();
	frame_end();
}

/* Word-wrap text into the box from row; returns the next free row. */
static int dialog_wrap(int row, int x, int width, int last_row, const char *text, int pair) {

	wchar_t wide[4096];
	size_t n = mbstowcs(wide, text ? text : "", 4095);
	if (n == (size_t)-1) {
		welcome_text(row, x, width, text, pair, 0);
		return row + 1;
	}
	size_t at = 0;
	while (at < n && row <= last_row) {
		size_t end = at, cut = at;
		int cells = 0;
		while (end < n && wide[end] != L'\n') {
			int w = wcwidth(wide[end]);
			if (cells + (w < 0 ? 1 : w) > width) {
				break;
			}
			cells += w < 0 ? 1 : w;
			if (wide[end] == L' ') {
				cut = end;
			}
			end++;
		}
		if (end < n && wide[end] != L'\n' && cut > at) {
			end = cut;
		}
		char line[4096];
		wchar_t saved = wide[end];
		wide[end] = 0;
		size_t bytes = wcstombs(line, wide + at, sizeof(line) - 1);
		wide[end] = saved;
		if (bytes != (size_t)-1) {
			line[bytes] = 0;
			welcome_text(row, x, width, line, pair, 0);
		}
		row++;
		at = end;
		if (at < n && (wide[at] == L'\n' || wide[at] == L' ')) {
			at++;
		}
	}

	return row;
}

/* One key, with the resize/paste/timeout handling every dialog needs.
   Returns ERR when the deadline passed. */
static int dialog_key(wint_t *key, double deadline, int *paste) {

	for (;;) {
		if (history_view_paste(-1)) {
			*paste = 1;
		}
		if (external_cancelled || (deadline > 0 && now() >= deadline)) {
			return ERR;
		}
		int kind = read_key(key, 100);
		if (kind == ERR) {
			if (idle_callback) {
				idle_callback();
			}
			continue;
		}
		if (kind == KEY_CODE_YES && *key == KEY_RESIZE) {
			title_resized();
			return KEY_CODE_YES;
		}
		if (kind == KEY_CODE_YES && *key == KEY_PASTE_BEGIN) {
			*paste = 1;
			continue;
		}
		if (kind == KEY_CODE_YES && *key == KEY_PASTE_END) {
			*paste = 0;
			continue;
		}
		return kind;
	}
}

static int dialog_cancel_key(int kind, wint_t key) {

	return (kind == OK && (key == 27 || key == 3 || key == 4)) ||
		(kind == KEY_CODE_YES && (key == KEY_ESCAPE || key == KEY_INTERRUPT || key == KEY_EOF));
}

static double dialog_deadline(int timeout_ms) {

	return timeout_ms > 0 ? now() + timeout_ms / 1000.0 : 0;
}

/* Yes/No, starting on Yes unless default_yes is 0. Returns 1 yes, 0 no,
   -1 cancelled or timed out. */
int ui_confirm(const char *title, const char *message, int timeout_ms, int default_yes) {

	if (!active) {
		return -1;
	}
	int choice = !!default_yes, paste = 0, result = -2;
	double deadline = dialog_deadline(timeout_ms);
	modal_active = 1;
	while (result == -2) {
		struct dialog_box box = dialog_frame(title, 9);
		dialog_wrap(box.y + 2, box.x + 3, box.width - 6, box.y + box.height - 4, message, 27);
		int row = box.y + box.height - 3;
		welcome_text(row, box.x + 3, 7, choice ? " Yes " : "  Yes ", choice ? 63 : 3, choice);
		welcome_text(row, box.x + 11, 6, !choice ? " No " : "  No ", !choice ? 63 : 3, !choice);
		welcome_text(row, box.x + box.width - 30, 27, "y/n · ←→ · enter · esc", 3, 0);
		curs_set(0);
		dialog_show();
		wint_t key;
		int kind = dialog_key(&key, deadline, &paste);
		if (kind == ERR) {
			result = -1;
			break;
		}
		if (paste) {
			continue;
		}
		if (dialog_cancel_key(kind, key)) {
			result = -1;
		} else if (kind == OK && (key == 'y' || key == 'Y')) {
			result = 1;
		} else if (kind == OK && (key == 'n' || key == 'N')) {
			result = 0;
		} else if ((kind == KEY_CODE_YES && (key == KEY_LEFT || key == KEY_RIGHT)) ||
			(kind == OK && (key == 'h' || key == 'l' || key == '\t'))) {
			choice = !choice;
		} else if ((kind == OK && key == '\r') || (kind == KEY_CODE_YES && key == KEY_ENTER)) {
			result = choice;
		}
	}
	modal_active = 0;
	dirty = 1;
	cursor_shape = -1;
	draw(1);

	return result;
}

/* A list; typing filters it. Returns the chosen index, or -1. */
int ui_select(const char *title, const char *const *options, size_t count, int timeout_ms) {

	if (!active || !count) {
		return -1;
	}
	size_t *shown = malloc(count * sizeof(*shown));
	if (!shown) {
		return -1;
	}
	char filter[256] = "";
	size_t matched = 0, selected = 0, first = 0;
	int paste = 0, result = -2, changed = 1;
	double deadline = dialog_deadline(timeout_ms);
	modal_active = 1;
	while (result == -2) {
		if (changed) {
			matched = 0;
			for (size_t i = 0; i < count; i++) {
				char lower_option[512], lower_filter[256];
				snprintf(lower_option, sizeof(lower_option), "%s", options[i]);
				snprintf(lower_filter, sizeof(lower_filter), "%s", filter);
				for (char *c = lower_option; *c; c++) {
					*c = (char)tolower((unsigned char)*c);
				}
				for (char *c = lower_filter; *c; c++) {
					*c = (char)tolower((unsigned char)*c);
				}
				if (!*filter || strstr(lower_option, lower_filter)) {
					shown[matched++] = i;
				}
			}
			selected = first = 0;
			changed = 0;
		}
		int rows = (int)(count < 12 ? count : 12);
		struct dialog_box box = dialog_frame(title, rows + 7);
		welcome_text(box.y + 2, box.x + 3, box.width - 6, *filter ? filter : "Type to filter…",
			*filter ? 27 : 3, 0);
		attrset(COLOR_PAIR(5));
		mvhline(box.y + 3, box.x + 1, ACS_HLINE, box.width - 2);
		int list_rows = box.height - 7;
		if (selected < first) {
			first = selected;
		}
		if (selected >= first + (size_t)list_rows) {
			first = selected - (size_t)list_rows + 1;
		}
		if (!matched) {
			welcome_text(box.y + 4, box.x + 3, box.width - 6, "No matches", 3, 0);
		}
		for (size_t i = first; i < matched && i < first + (size_t)list_rows; i++) {
			int row = box.y + 4 + (int)(i - first), chosen = i == selected;
			if (chosen) {
				attrset(COLOR_PAIR(2));
				mvhline(row, box.x + 1, ' ', box.width - 2);
			}
			welcome_text(row, box.x + 2, 2, chosen ? "›" : " ", chosen ? 63 : 3, 1);
			welcome_text(row, box.x + 4, box.width - 6, options[shown[i]], chosen ? 2 : 27, chosen);
		}
		welcome_text(box.y + box.height - 2, box.x + 3, box.width - 6,
			"↑↓ or ctrl+j/k · enter choose · esc cancel", 3, 0);
		curs_set(0);
		dialog_show();
		wint_t key;
		int kind = dialog_key(&key, deadline, &paste);
		if (kind == ERR) {
			result = -1;
			break;
		}
		if (dialog_cancel_key(kind, key) && !paste) {
			result = -1;
		} else if (!paste &&
			((kind == OK && key == '\r') || (kind == KEY_CODE_YES && key == KEY_ENTER))) {
			if (matched) {
				result = (int)shown[selected];
			}
		} else if ((kind == KEY_CODE_YES && (key == KEY_DOWN || key == KEY_NEWLINE)) ||
			(kind == OK && key == 10)) {
			if (matched) {
				selected = (selected + 1) % matched;
			}
		} else if ((kind == KEY_CODE_YES && key == KEY_UP) || (kind == OK && key == 11)) {
			if (matched) {
				selected = (selected + matched - 1) % matched;
			}
		} else if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
			(kind == OK && (key == 8 || key == 127))) {
			size_t n = strlen(filter);
			while (n && ((unsigned char)filter[n - 1] & 0xC0) == 0x80) {
				n--;
			}
			if (n) {
				filter[n - 1] = 0;
				changed = 1;
			}
		} else if (kind == OK && key == 21) {
			filter[0] = 0;
			changed = 1;
		} else if (kind == OK && key >= 32 && wcwidth((wchar_t)key) >= 0) {
			char bytes[8];
			mbstate_t state;
			memset(&state, 0, sizeof(state));
			size_t n = wcrtomb(bytes, (wchar_t)key, &state), used_bytes = strlen(filter);
			if (n != (size_t)-1 && used_bytes + n < sizeof(filter)) {
				memcpy(filter + used_bytes, bytes, n);
				filter[used_bytes + n] = 0;
				changed = 1;
			}
		}
	}
	free(shown);
	modal_active = 0;
	dirty = 1;
	cursor_shape = -1;
	draw(1);

	return result;
}

/* A text field (multiline: an editor). Returns malloc'd text, or NULL. */
char *ui_ask_text(
	const char *title, const char *placeholder, const char *value, int multiline, int timeout_ms) {

	if (!active) {
		return NULL;
	}
	size_t capacity = 8192, count = 0, at = 0;
	wchar_t *text = calloc(capacity, sizeof(*text));
	if (!text) {
		return NULL;
	}
	if (value) {
		size_t n = mbstowcs(text, value, capacity - 1);
		count = at = n == (size_t)-1 ? 0 : n;
		text[count] = 0;
	}
	int paste = 0, result = -2;
	double deadline = dialog_deadline(timeout_ms);
	modal_active = 1;
	while (result == -2) {
		int field_rows = multiline ? (LINES - 12 > 12 ? 12 : LINES - 12 < 3 ? 3 : LINES - 12) : 1;
		struct dialog_box box = dialog_frame(title, field_rows + 6);
		int width = box.width - 6, row0 = box.y + 2;
		/* Lay the text out in rows; find the cursor. */
		int row = 0, col = 0, cursor_row = 0, cursor_col = 0, top_row;
		for (size_t i = 0; i <= count; i++) {
			if (i == at) {
				cursor_row = row;
				cursor_col = col;
			}
			if (i == count) {
				break;
			}
			int w = text[i] == L'\n' ? 0 : wcwidth(text[i]);
			if (text[i] == L'\n' || col + (w < 0 ? 1 : w) > width) {
				row++;
				col = 0;
				if (text[i] == L'\n') {
					continue;
				}
			}
			col += w < 0 ? 1 : w;
		}
		top_row = cursor_row >= field_rows ? cursor_row - field_rows + 1 : 0;
		if (!count && placeholder) {
			welcome_text(row0, box.x + 3, width, placeholder, 3, 0);
		}
		attrset(COLOR_PAIR(27));
		row = col = 0;
		for (size_t i = 0; i < count; i++) {
			int w = text[i] == L'\n' ? 0 : wcwidth(text[i]);
			if (text[i] == L'\n' || col + (w < 0 ? 1 : w) > width) {
				row++;
				col = 0;
				if (text[i] == L'\n') {
					continue;
				}
			}
			if (row >= top_row && row < top_row + field_rows) {
				mvaddnwstr(row0 + row - top_row, box.x + 3 + col, text + i, 1);
			}
			col += w < 0 ? 1 : w;
		}
		welcome_text(box.y + box.height - 2, box.x + 3, width,
			multiline ? "ctrl+s save · enter new line · esc cancel" : "enter submit · esc cancel",
			3, 0);
		curs_set(1);
		move(row0 + cursor_row - top_row, box.x + 3 + cursor_col);
		dialog_show();
		wint_t key;
		int kind = dialog_key(&key, deadline, &paste);
		if (kind == ERR) {
			result = -1;
			break;
		}
		int enter = (kind == OK && (key == '\r' || key == '\n')) ||
			(kind == KEY_CODE_YES && key == KEY_ENTER);
		if (!paste && dialog_cancel_key(kind, key)) {
			result = -1;
		} else if (!paste && ((enter && !multiline) || (kind == OK && key == 19))) {
			result = 1; /* Ctrl+S */
		} else if (kind == KEY_CODE_YES && key == KEY_LEFT) {
			if (at) {
				at--;
			}
		} else if (kind == KEY_CODE_YES && key == KEY_RIGHT) {
			if (at < count) {
				at++;
			}
		} else if ((kind == KEY_CODE_YES && key == KEY_HOME) || (kind == OK && key == 1)) {
			while (at && text[at - 1] != L'\n') {
				at--;
			}
		} else if ((kind == KEY_CODE_YES && key == KEY_END) || (kind == OK && key == 5)) {
			while (at < count && text[at] != L'\n') {
				at++;
			}
		} else if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
			(kind == OK && (key == 8 || key == 127))) {
			if (at) {
				memmove(text + at - 1, text + at, (count - at + 1) * sizeof(*text));
				at--;
				count--;
			}
		} else if (kind == OK && key == 21) { /* Ctrl+U: to the start of the line */
			size_t start = at;
			while (start && text[start - 1] != L'\n') {
				start--;
			}
			memmove(text + start, text + at, (count - at + 1) * sizeof(*text));
			count -= at - start;
			at = start;
		} else if (kind == OK && key == 23) { /* Ctrl+W: the word before the cursor */
			size_t start = at;
			while (start && text[start - 1] == L' ') {
				start--;
			}
			while (start && text[start - 1] != L' ' && text[start - 1] != L'\n') {
				start--;
			}
			memmove(text + start, text + at, (count - at + 1) * sizeof(*text));
			count -= at - start;
			at = start;
		} else if ((kind == OK &&
					   ((key >= 32 && wcwidth((wchar_t)key) >= 0) ||
						   (multiline && (enter || key == '\t')))) &&
			count + 2 < capacity) {
			wchar_t ch = enter ? L'\n' : key == '\t' ? L' ' : (wchar_t)key;
			if (!multiline && (ch == L'\n')) {
				continue;
			}
			memmove(text + at + 1, text + at, (count - at + 1) * sizeof(*text));
			text[at++] = ch;
			count++;
		}
	}
	modal_active = 0;
	dirty = 1;
	cursor_shape = -1;
	draw(1);
	char *out = NULL;
	if (result == 1) {
		size_t bytes = wcstombs(NULL, text, 0);
		out = bytes == (size_t)-1 ? NULL : malloc(bytes + 1);
		if (out) {
			wcstombs(out, text, bytes + 1);
		}
	}
	free(text);

	return out;
}

static void feedback_append(const char *text, int level) {

	int style = level == 2 ? 7 : level == 1 ? 41 : 14;
	if (used && transcript[used - 1] != '\n') {
		append_styled("\n", 0);
	}
	append_styled("\n", 0);
	append_styled(level == 2 ? "error: " : level == 1 ? "warning: " : "· ", style);
	markup_append(text, style);
	append_styled("\n", 0);
}

/* Compatibility feedback adapter, not a notification system. Ordinary
   bounded transcript text only; no overlay, history store or expiry timer. */
void ui_notice(const char *text, int level) {

	if (!text || !*text) {
		return;
	}
	if (editor_host_feedback) {
		editor_host_feedback(text, editor_host_context);
		dirty = 1;
		return;
	}
	while (*text == '\n' || *text == '\r') {
		text++;
	}
	if (!*text) {
		return;
	}
	int style = level == 2 ? 7 : level == 1 ? 41 : 14;
	/* Keep out-of-band feedback outside a live tool's hideable/foldable
       span. Render temporarily, append through the bounded cache, then move
       the feedback before the surviving live heading. No queue is retained. */
	int live = tool_active && !styled_capture;
	struct styled_capture feedback = {0};
	struct styled_capture *previous = styled_capture;
	presentation_batch++;
	if (live) {
		styled_capture = &feedback;
	}
	feedback_append(text, level);
	if (live) {
		styled_capture = previous;
		if (!feedback.failed && feedback.used) {
			tool_active = 0; /* do not add tool-body glyphs to ordinary text */
			append_styled(feedback.text, style);
			size_t kept = feedback.used < used ? feedback.used : used;
			memcpy(transcript_style + used - kept, feedback.styles + feedback.used - kept, kept);
			tool_active = 1;
			size_t start = tool_header_start, tail = used - kept;
			if (start < tail) {
				memmove(transcript + start + kept, transcript + start, tail - start);
				memmove(transcript_style + start + kept, transcript_style + start, tail - start);
				memcpy(transcript + start, feedback.text + feedback.used - kept, kept);
				memcpy(transcript_style + start, feedback.styles + feedback.used - kept, kept);
				semantic_messages_shift(start, start, (ptrdiff_t)kept);
				for (size_t i = 0; i < tool_card_count; i++) {
					if (tool_cards[i].start >= start) {
						tool_cards[i].start += kept;
						tool_cards[i].end += kept;
					}
				}
				tool_header_start += kept;
				tool_heading_end += kept;
				tool_body_start += kept;
				if (assistant_open && assistant_start >= start) {
					assistant_start += kept;
				}
			} else {
				tool_active = 0; /* only feedback survived cache eviction */
			}
			transcript_revision++;
		}
		int failed = feedback.failed;
		free(feedback.text);
		free(feedback.styles);
		if (failed) {
			/* Allocation failure degrades presentation to unfurled text,
               never execution/cancellation or a hidden diagnostic. */
			tool_active = tool_heading_valid = 0;
			feedback_append(text, level);
		}
	}
	presentation_batch--;
	/* Use the normal frame loop: never re-enter painting from an error
       handler or repaint over a protected prompt from an idle callback. */
	dirty = 1;
}

void ui_scene(const char *name) {

	if (!active || working || pending_approval) {
		return;
	}
	if (!name || !*name) {
		ui_notice("Choose a user composition with welcome.scene: /scene NAME", 0);
		return;
	}
	struct skin *scene = calloc(1, sizeof(*scene));
	if (!scene) {
		return;
	}
	short colors[THEME_COUNT];
	char error[256];
	int status = theme_read_named(name, colors, scene, error, sizeof(error));
	if (status != 1 || !scene->scene_count) {
		skin_release(scene);
		free(scene);
		ui_notice(status < 0 ? error : "No scene found in that preset.", 2);
		return;
	}
	modal_active = 1;
	curs_set(0);
	for (;;) {
		erase();
		welcome_scroll = 0;
		welcome_clip = LINES - 2;
		welcome_scene_render(scene, LINES - 2, COLS);
		welcome_clip = -1;
		welcome_text(LINES - 1, skin.margin, COLS - 2 * skin.margin, "Esc to return", 3, 0);
		dialog_show();
		wint_t key = 0;
		int kind = read_key(&key, 100);
		if (kind == ERR) {
			if (idle_callback) {
				idle_callback();
			}
			continue;
		}
		if (kind == KEY_CODE_YES && key == KEY_RESIZE) {
			title_resized();
			continue;
		}
		if (dialog_cancel_key(kind, key) || (kind == OK && (key == '\n' || key == '\r'))) {
			break;
		}
	}
	skin_release(scene);
	free(scene);
	modal_active = 0;
	dirty = 1;
	draw(1);
}

/* A message an extension asked to show (send_message with display), drawn
   as a card titled with its type. */
void ui_card(const char *title, const char *text) {

	if (!text || !*text) {
		return;
	}
	presentation_batch++;
	while (used && (transcript[used - 1] == '\n' || transcript[used - 1] == '\r')) {
		used--;
	}
	transcript[used] = 0;
	if (used) {
		append_styled("\n\n", 0);
	}
	append_styled(GLYPH_CARD, 14);
	append_styled(title && *title ? title : "extension", 4);
	append_styled("\n", 4);
	extension_card_body(text, strlen(text), 5);
	presentation_batch--;
	draw(0);
}

/* A card whose text a model wrote (style 79): laid out as card text, with
   its inline markdown (**bold**, `code`) shown as in replies. */
static void ui_card_markdown(const char *title, const char *text) {

	size_t size = strlen(text);
	while (size && (text[size - 1] == '\n' || text[size - 1] == '\r')) {
		size--;
	}
	if (!size) {
		return;
	}
	presentation_batch++;
	while (used && (transcript[used - 1] == '\n' || transcript[used - 1] == '\r')) {
		used--;
	}
	transcript[used] = 0;
	if (used) {
		append_styled("\n\n", 0);
	}
	append_styled(GLYPH_CARD, 14);
	append_styled(title && *title ? title : "model", 4);
	append_styled("\n", 4);
	char *copy = strndup(text, size);
	if (copy) {
		append_styled(copy, 79);
		free(copy);
	}
	append_styled("\n", 79);
	presentation_batch--;
	draw(0);
}

/* A model reply streaming into a card (api->complete with "show"). */
void ui_stream_begin(const char *title) {

	while (used && (transcript[used - 1] == '\n' || transcript[used - 1] == '\r')) {
		used--;
	}
	transcript[used] = 0;
	if (used) {
		append_styled("\n\n", 0);
	}
	append_styled(GLYPH_CARD, 14);
	append_styled(title && *title ? title : "model", 4);
	append_styled("\n", 4);
}

void ui_stream_text(const char *text) {

	if (text && *text) {
		append_styled(text, 79);
	}
} /* a model's words */

void ui_stream_end(void) {

	if (used && transcript[used - 1] != '\n') {
		append_styled("\n", 79);
	}
	draw(1);
}
