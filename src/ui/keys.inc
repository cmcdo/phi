/* What each key does: the command palette, wheel scrolling, editing a
   queued message, the key dispatcher, and keys and completions handled by
   extensions. Included by ui.c after editor.inc. */

static int palette_matches(size_t command, const wchar_t *query, size_t query_length) {

	const struct phi_command *item = ui_command_at(command);
	if (!item || !editor_command_allowed(item)) {
		return 0;
	}
	char text[256];
	snprintf(text, sizeof(text), "%s %s", item->name, command_description(item, editor_workspace));
	size_t at = 0, previous = 0;
	int score = 1;
	for (size_t q = 0; q < query_length; q++) {
		int wanted = towlower(query[q]), found = 0;
		for (; text[at]; at++) {
			if (tolower((unsigned char)text[at]) == wanted) {
				score += 10;
				if (at < strlen(item->name)) {
					score += 30;
				}
				if (!at || text[at - 1] == '/' || text[at - 1] == ' ') {
					score += 20;
				}
				if (q && at == previous + 1) {
					score += 15;
				}
				previous = at++;
				found = 1;
				break;
			}
		}
		if (!found) {
			return 0;
		}
	}

	return score;
}

/* Returns the edit action to take after a selection (submit for commands that
   take no argument, otherwise leave the command in the draft). */
static int command_menu(void) {

	wchar_t query[96] = {0};
	size_t query_length = 0, selected = 0;
	modal_active = 1;
	for (;;) {
		size_t matches[UI_COMMAND_LIMIT], count = 0;
		int scores[UI_COMMAND_LIMIT];
		for (size_t i = 0; i < ui_command_count(); i++) {
			int score = palette_matches(i, query, query_length);
			if (!score) {
				continue;
			}
			size_t pos = count++;
			while (pos && scores[pos - 1] < score) {
				scores[pos] = scores[pos - 1];
				matches[pos] = matches[pos - 1];
				pos--;
			}
			scores[pos] = score;
			matches[pos] = i;
		}
		if (selected >= count) {
			selected = count ? count - 1 : 0;
		}
		if (idle_callback) {
			idle_callback();
		}
		draw(1);
		welcome_scroll = 0;
		welcome_clip = -1;
		int width = COLS - 8, height = (int)count + 6;
		if (width > 72) {
			width = 72;
		}
		if (height > 24) {
			height = 24;
		}
		if (height < 8) {
			height = 8;
		}
		if (height > LINES - 2) {
			height = LINES - 2;
		}
		int usable = width >= 36 && height >= 8;
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
			welcome_text(y, x + 2, 17, " Command palette ", 28, 1);
			char search[128] = "Search: ";
			wcstombs(search + 8, query, sizeof(search) - 9);
			welcome_text(y + 2, x + 2, width - 4, search, 3, 0);
			size_t rows = (size_t)(height - 5), first = selected >= rows ? selected - rows + 1 : 0;
			for (size_t row = 0; row < rows && first + row < count; row++) {
				size_t i = matches[first + row];
				char line[256];
				const struct phi_command *item = ui_command_at(i);
				if (!item) {
					continue;
				}
				snprintf(line, sizeof(line), "%-14s %s", item->name,
					command_description(item, editor_workspace));
				welcome_text(y + 3 + (int)row, x + 2, width - 4, line,
					first + row == selected ? 28 : 3, first + row == selected);
			}
			if (!count) {
				welcome_text(y + 3, x + 2, width - 4, "No matching commands", 3, 0);
			}
			welcome_text(y + height - 2, x + 2, width - 4,
				"Enter select · Esc close · Ctrl+N/P move · click select", 3, 0);
		} else {
			welcome_text(
				LINES / 2, 1, COLS - 2, "Enlarge terminal for command palette · Esc cancels", 3, 0);
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
		if ((kind == OK && (key == 27 || key == 3)) ||
			(kind == KEY_CODE_YES && (key == KEY_ESCAPE || key == KEY_INTERRUPT))) {
			break;
		}
		if (usable && kind == KEY_CODE_YES && key == KEY_MOUSE) {
			MEVENT event;
			if (getmouse(&event) == OK) {
				size_t rows = (size_t)(height - 5),
					   first = selected >= rows ? selected - rows + 1 : 0;
				int row = event.y - (LINES - height) / 2 - 3, x = (COLS - width) / 2;
				if (event.bstate & BUTTON4_PRESSED) {
					if (selected) {
						selected--;
					}
				} else if (event.bstate & BUTTON5_PRESSED) {
					if (selected + 1 < count) {
						selected++;
					}
				} else if (event.x > x && event.x < x + width - 1 && row >= 0 &&
					(size_t)row < rows && first + (size_t)row < count &&
					(event.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED))) {
					selected = first + (size_t)row;
					kind = OK;
					key = '\r';
				}
			}
		}
		if (usable && count &&
			((kind == OK && key == '\r') || (kind == KEY_CODE_YES && key == KEY_ENTER))) {
			size_t i = matches[selected];
			const char *name = ui_command_at(i)->name;
			size_t n = strlen(name);
			if (!input_reserve(n + (ui_command_at(i)->argument ? 1 : 0))) {
				break;
			}
			for (size_t j = 0; j < n; j++) {
				input[j] = (unsigned char)name[j];
			}
			length = n;
			if (ui_command_at(i)->argument) {
				input[length++] = L' ';
			}
			input[length] = 0;
			cursor = length;
			normal_mode = visual_mode = delete_pending = input_view = 0;
			modal_active = 0;
			dirty = 1;
			cursor_shape = -1;
			draw(1);
			return ui_command_at(i)->argument ? 0 : 1;
		}
		if ((kind == KEY_CODE_YES && (key == KEY_DOWN || key == KEY_NEWLINE)) ||
			(kind == OK && (key == 10 || key == 14))) {
			if (count) {
				selected = (selected + 1) % count;
			}
			continue;
		}
		if ((kind == KEY_CODE_YES && key == KEY_UP) || (kind == OK && (key == 11 || key == 16))) {
			if (count) {
				selected = (selected + count - 1) % count;
			}
			continue;
		}
		if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
			(kind == OK && (key == 127 || key == 8))) {
			if (query_length) {
				query[--query_length] = 0;
			}
			selected = 0;
			continue;
		}
		if (kind == OK && key >= 32 && key < 127 &&
			query_length + 1 < sizeof(query) / sizeof(*query)) {
			query[query_length++] = (wchar_t)key;
			query[query_length] = 0;
			selected = 0;
		}
	}
	modal_active = 0;
	dirty = 1;
	cursor_shape = -1;
	draw(1);

	return 0;
}

/* 1 = submit, 2 = quit, 3 = cancel current work. */
static int complete_extension(void);

/* Lines for one wheel event (scroll.lines, default 3).
   "auto" follows pi's WheelScrollAccelerator: a lone notch moves one line, and a spin
   speeds up with its pace: notches 100 ms apart move 1 line each, 50 ms 2,
   20 ms 5, at most 6. Events under 5 ms apart are one physical notch (or a
   high-resolution wheel) and move a line each; a 200 ms pause or a change
   of direction starts over. Local macOS terminals accelerate already, and
   so do touchpads in terminals such as foot, which send an event per line:
   auto made those lurch, so it is opt-in. */
static int wheel_lines(int direction) {

	static double last = -1, average, carry;
	static int last_direction;
	wheel_pending = 1;
	if (skin.scroll_lines) {
		return skin.scroll_lines;
	}
#ifdef __APPLE__
	if (!getenv("SSH_CONNECTION") && !getenv("SSH_CLIENT") && !getenv("SSH_TTY")) {
		return 1;
	}
#endif
	double at = now(), gap = (at - last) * 1000;
	int same = last >= 0 && direction == last_direction && gap <= 200;
	last = at;
	last_direction = direction;
	if (!same) {
		average = carry = 0;
		return 1;
	}
	if (gap < 5) {
		return 1;
	}
	average = average ? (average + gap) / 2 : gap;
	double lines = 100 / average;
	if (lines < 1) {
		lines = 1;
	}
	if (lines > 6) {
		lines = 6;
	}
	lines += carry;
	int whole = (int)lines;
	carry = lines - whole;

	return whole;
}

/* Up on an empty prompt while a turn runs: the newest queued message comes
   back into the prompt, out of the queue, to change or delete; Enter sends
   it again. */
static int queued_edit(void) {

	if (!working || !queued_count || length) {
		return 0;
	}
	char *message = queued[--queued_count];
	ui_set_draft(message);
	free(message);
	dirty = 1;

	return 1;
}

static int edit_key_raw(int kind, wint_t key) {

	replace_changed = 0;
	if (!(kind == KEY_CODE_YES && key == KEY_MOUSE) && transcript_selection.visible) {
		transcript_selection_clear();
	}
	if (kind == KEY_CODE_YES && key == KEY_ESCAPE) {
		kind = OK;
		key = 27;
	}
	if (kind == KEY_CODE_YES && key == KEY_EOF) {
		kind = OK;
		key = 4;
	}
	if (kind == KEY_CODE_YES && key == KEY_INTERRUPT) {
		kind = OK;
		key = 3;
	}
	dirty = 1;
	if (kind == KEY_CODE_YES && key == KEY_RESIZE) {
		title_resized();
		/* Repaint everything: after a reattach (phi-host resizes to make us
           redraw) the terminal no longer shows what curses remembers, and a
           partial repaint would leave stale or repeated lines. */
		clearok(curscr, TRUE);
		return 0;
	}
	/* Consume leaving INSERT here, before any stop-counting or modal command
       logic. This Escape can never also become a NORMAL stop request. */
	if (kind == OK && key == 27 && !pasting && !normal_mode) {
		esc_armed = 0;
		esc_armed_at = 0;
		ctrl_c_armed = left_armed = 0;
		vim_pending_clear();
		g_pending = find_pending = delete_pending = visual_mode = 0;
		normal_mode = 1;
		if (cursor > line_start(cursor)) {
			cursor--;
		}
		normal_cursor();
		return 0;
	}
	/* An r argument is input, not a command: Enter must not submit, and
       letters such as u/f/v must not run their ordinary Normal commands.
       Resize/focus events above/below leave the pending argument intact. */
	if (replace_pending && normal_mode &&
		!(kind == KEY_CODE_YES && (key == KEY_FOCUS_IN || key == KEY_FOCUS_OUT))) {
		size_t count = replace_count;
		vim_pending_clear();
		if (kind == OK && key == 27) {
			visual_mode = 0;
			if (working) {
				esc_armed = 1;
				esc_armed_at = now();
			}
			return 0;
		}
		if ((kind == OK &&
				(key == L'\r' || key == L'\n' || (key >= 32 && wcwidth((wchar_t)key) >= 0))) ||
			(kind == KEY_CODE_YES && (key == KEY_ENTER || key == KEY_NEWLINE))) {
			wchar_t ch = kind == KEY_CODE_YES ? L'\n' : (wchar_t)key;
			if (!visual_mode || (ch != L'\r' && ch != L'\n')) {
				replace_character(ch, count);
			} else {
				visual_mode = 0;
			}
			return 0;
		}
		if (kind == OK && key == L'\t') {
			visual_mode = 0;
			return 0;
		}
	}
	if ((kind == OK && key < 32) ||
		(kind == KEY_CODE_YES && key != KEY_FOCUS_IN && key != KEY_FOCUS_OUT)) {
		vim_count = 0;
	}
	if (!(kind == OK && key == 3) &&
		!(kind == KEY_CODE_YES && (key == KEY_FOCUS_IN || key == KEY_FOCUS_OUT))) {
		ctrl_c_armed = 0;
	}
	if (!(kind == KEY_CODE_YES &&
			(key == KEY_LEFT || key == KEY_FOCUS_IN || key == KEY_FOCUS_OUT))) {
		left_armed = 0;
	}
	if (!(kind == OK && key == 27) &&
		!(kind == KEY_CODE_YES && (key == KEY_FOCUS_IN || key == KEY_FOCUS_OUT))) {
		esc_armed = 0;
	}
	if (!pasting && kind == OK && key == 26) {
		undo_step(-1);
		return 0;
	}
	if (!pasting && kind == OK && key == 25) {
		undo_step(1);
		return 0;
	}
	if (!pasting && kind == OK && key == 7) {
		external_editor();
		return 0;
	}
	if (!pasting && kind == KEY_CODE_YES && key == KEY_COMMAND_MENU) {
		return command_menu();
	}
	if (!pasting && kind == OK && key == 6) {
		conversation_search();
		return 0;
	}
	if (!pasting && kind == OK && key == 15) {
		/* Open cards (tool.expanded) with a line cap: Ctrl+O shows all
           their output, as pi; otherwise it folds and unfolds them. */
		if (pending_active) {
			tool_full = !tool_full;
			tool_details = tool_full; /* keep the completed write open too */
			transcript_revision++;
			dirty = 1;
			draw(1);
		} else if (skin.tool_expanded && skin_caps_any(&skin) && tool_details) {
			tool_full = !tool_full;
			transcript_revision++;
			dirty = 1;
		} else {
			presentation_batch++;
			tool_card_toggle();
			tool_full = tool_details;
			transcript_revision++;
			dirty = 1;
			presentation_batch--;
			draw(1);
		}
		return 0;
	}
	if (!pasting && ((kind == OK && key == 22) || (kind == KEY_CODE_YES && key == KEY_CLIPBOARD))) {
		if (normal_mode) {
			visual_mode = visual_mode == 2 ? 0 : 2;
			visual_anchor = cursor;
			delete_pending = 0;
		} else {
			paste_clipboard();
		}
		return 0;
	}
	if (kind == OK && key == 27 && !pasting && working && normal_mode) {
		if (esc_armed && now() - esc_armed_at < 2.5) {
			esc_armed = 0;
			return 3;
		}
		esc_armed = 1;
		esc_armed_at = now();
		/* Only NORMAL-mode Escape presses count toward cancellation.
           Leaving INPUT is a mode switch, not the first stop request. */
	}
	if (kind == OK && key == 3 && !pasting) {
		delete_pending = visual_mode = 0;
		if (working) {
			ctrl_c_armed = 1;
			return 3;
		}
		if (ctrl_c_armed) {
			ctrl_c_armed = 0;
			return 2;
		}
		ctrl_c_armed = 1;
		return 0;
	}
	if (kind == KEY_CODE_YES && (key == KEY_FOCUS_IN || key == KEY_FOCUS_OUT)) {
		focused = key == KEY_FOCUS_IN;
		cursor_shape = -1;
		return 0;
	}
	if (kind == KEY_CODE_YES && key == KEY_PASTE_BEGIN) {
		if (visual_mode) {
			visual_action(L'c');
		}
		vim_pending_clear();
		pasting = 1;
		paste_cr = 0;
		delete_pending = 0;
		normal_mode = 0;
		return 0;
	}
	if (kind == KEY_CODE_YES && key == KEY_PASTE_END) {
		pasting = paste_cr = 0;
		return 0;
	}
	if (pasting) {
		if (kind != OK) {
			return 0;
		}
		if (key == '\r') {
			insert_char(L'\n');
			paste_cr = 1;
		} else {
			if (key == '\n') {
				if (!paste_cr) {
					insert_char(L'\n');
				}
			} else if (key == '\t') {
				insert_char(L' ');
			} else if (key >= 32 && wcwidth((wchar_t)key) >= 0) {
				insert_char((wchar_t)key);
			}
			paste_cr = 0;
		}
		return 0;
	}
	if (!normal_mode && kind == OK && (key == 21 || key == 23)) {
		size_t first = cursor;
		if (key == 21) {
			first = line_start(cursor);
		} else {
			while (first && iswspace(input[first - 1]) && input[first - 1] != L'\n') {
				first--;
			}
			while (first && !iswspace(input[first - 1])) {
				first--;
			}
		}
		if (first < cursor) {
			memmove(input + first, input + cursor, (length - cursor + 1) * sizeof(*input));
			length -= cursor - first;
			cursor = first;
		}
		return 0;
	}
	int command_action = command_key(kind, key);
	if (command_action >= 0) {
		return command_action;
	}
	if (kind == OK && key == '\t' && complete_path()) {
		return 0;
	}
	if (kind == OK && key == '\t' && complete_extension()) {
		return 0;
	}
	if (kind == KEY_CODE_YES) {
		if (key != KEY_FOCUS_IN && key != KEY_FOCUS_OUT) {
			vim_pending_clear();
		}
		delete_pending = 0;
		switch (key) {
		case KEY_BTAB:
			cycle_effort();
			break;
		case KEY_LEFT:
			if (cursor) {
				cursor--;
			} else if (!editor_host_redraw && !length && (normal_mode || !chat_turns) &&
				getenv("PHI_CHAT_ID")) {
				if (chat_turns && !chat_backgrounded &&
					!(left_armed && now() - left_armed_at < 2.5)) {
					left_armed = 1;
					left_armed_at = now();
					break;
				}
				/* Ask the viewer to detach (it shows the board). With nothing
                       said, the chat closes instead of hiding in the background. */
				if (chat_turns) {
					chat_backgrounded = 1;
				}
				left_armed = 0;
				normal_mode = 0; /* Return to an editable prompt when reopened. */
				fputs(chat_turns ? "\033]777;phi-detach\007" : "\033]777;phi-close\007", stdout);
				fflush(stdout);
				if (!chat_turns) {
					return 2;
				}
			}
			break;
		case KEY_RIGHT:
			if (cursor < length) {
				cursor++;
			}
			break;
		case KEY_HOME:
			cursor = line_start(cursor);
			break;
		case KEY_END:
			cursor = line_end(cursor);
			break;
		case KEY_UP:
			if (queued_edit()) {
				break; /* a queued message comes back to edit */
			}
			if (!visual_vertical(-1)) {
				(void)recall_move(-1);
			}
			break;
		case KEY_DOWN:
			if (!visual_vertical(1)) {
				(void)recall_move(1);
			}
			break;
		case KEY_NEWLINE:
			if (visual_mode) {
				visual_action(L'c');
			}
			normal_mode = 0;
			insert_char(L'\n');
			break;
		case KEY_ENTER:
			return draft_has_text() ? 1 : 0;
		case KEY_BACKSPACE:
			if (visual_mode) {
				visual_action(L'd');
				break;
			}
			if (cursor) {
				memmove(input + cursor - 1, input + cursor, (length - cursor + 1) * sizeof(*input));
				cursor--;
				length--;
			}
			break;
		case KEY_DC:
			if (visual_mode) {
				visual_action(L'd');
				break;
			}
			if (cursor < length) {
				memmove(input + cursor, input + cursor + 1, (length - cursor) * sizeof(*input));
				length--;
			}
			break;
		case KEY_PPAGE:
			if (used && scroll_offset >= conversation_maximum &&
				scrollback_open(-(conversation_height > 1 ? conversation_height - 1 : 1))) {
				break;
			}
			scroll_offset += conversation_height > 1 ? conversation_height - 1 : 1;
			break;
		case KEY_NPAGE: {
			int page = conversation_height > 1 ? conversation_height - 1 : 1;
			scroll_offset = scroll_offset > page ? scroll_offset - page : 0;
			break;
		}
		case KEY_MOUSE: {
			MEVENT event;
			if (getmouse(&event) == OK) {
				if (ui_frame_suspended) {
					break; /* No invisible/stale hit targets. */
				}
				if (ui_layout_mouse(&event)) {
					break;
				}
				if (ui_surface_mouse(&event)) {
					break;
				}
				MEVENT local = event;
				local.x -= ui_chat_x;
				local.y -= ui_chat_y;
				if (cinema_mouse(&local)) {
					break;
				}
				if (event.bstate & BUTTON4_PRESSED) {
					if (scrollback_active()) {
						scrollback_move(-wheel_lines(1));
						dirty = 1;
						draw(1);
						break;
					}
					if (!transcript_selection.dragging && used &&
						scroll_offset >= conversation_maximum && scrollback_open(-wheel_lines(1))) {
						break;
					}
					if (!transcript_selection.dragging) {
						scroll_offset += wheel_lines(1);
					}
				}
#ifdef BUTTON5_PRESSED
				else if (event.bstate & BUTTON5_PRESSED) {
					if (scrollback_active()) {
						scrollback_move(wheel_lines(-1));
						dirty = 1;
						draw(1);
						break;
					}
					if (!transcript_selection.dragging) {
						int lines = wheel_lines(-1);
						scroll_offset = scroll_offset > lines ? scroll_offset - lines : 0;
					}
				}
#endif
#ifdef BUTTON1_RELEASED
				else if ((event.bstate & BUTTON1_RELEASED) && transcript_selection.dragging) {
					transcript_selection_finish(event.y, event.x);
				}
#endif
#ifdef BUTTON1_CLICKED
				else if (event.bstate & BUTTON1_CLICKED) {
					transcript_selection_start(event.y, event.x);
					transcript_selection_finish(event.y, event.x);
				}
#endif
				else if (transcript_selection.dragging) {
					transcript_selection_update(event.y, event.x);
				} else if (event.bstate & BUTTON1_PRESSED) {
					transcript_selection_start(event.y, event.x);
				}
			}
			break;
		}
		default:
			break;
		}
		if (normal_mode) {
			normal_cursor();
		}
	} else if (key == 4) {
		return working ? 0 : 2;
	} else if (key == 27) {
		vim_pending_clear();
		g_pending = find_pending = 0;
		delete_pending = 0;
		if (visual_mode) {
			visual_mode = 0;
		} else if (!normal_mode) {
			normal_mode = 1;
			if (cursor > line_start(cursor)) {
				cursor--;
			}
			normal_cursor();
		}
	} else if (key == '\r') {
		delete_pending = 0;
		return draft_has_text() ? 1 : 0;
	} else if (key == '\n' && !normal_mode) {
		insert_char(L'\n');
	} else if (normal_mode) {
		normal_key(key);
	} else if (key == 127 || key == 8) {
		if (cursor) {
			memmove(input + cursor - 1, input + cursor, (length - cursor + 1) * sizeof(*input));
			cursor--;
			length--;
		}
	} else if (key >= 32 && wcwidth((wchar_t)key) >= 0) {
		insert_char((wchar_t)key);
	}

	return 0;
}

/* Extension hooks (phase 5c): shortcuts and terminal_input, and Tab
   completion when Phi's own path completion has nothing. */
static int (*key_hook)(const char *name);
static json_object *(*completion_hook)(const char *text, const char *word);

void ui_set_extension_hooks(
	int (*keys)(const char *name), json_object *(*complete)(const char *text, const char *word)) {

	key_hook = keys;
	completion_hook = complete;
}

/* A readable name for a key: "a", "ctrl+t", "f5", "up", "enter", ... */
static int extension_key_name(int kind, wint_t key, char *name, size_t size) {

	static const struct {
		int code;
		const char *name;
	} special[] = {{KEY_UP, "up"}, {KEY_DOWN, "down"}, {KEY_LEFT, "left"}, {KEY_RIGHT, "right"},
		{KEY_HOME, "home"}, {KEY_END, "end"}, {KEY_PPAGE, "pageup"}, {KEY_NPAGE, "pagedown"},
		{KEY_DC, "delete"}, {KEY_BACKSPACE, "backspace"}, {KEY_ENTER, "enter"},
		{KEY_BTAB, "shift+tab"}};

	if (kind == KEY_CODE_YES) {
		if (key >= KEY_F(1) && key <= KEY_F(12)) {
			snprintf(name, size, "f%d", (int)(key - KEY_F0));
			return 1;
		}
		for (size_t i = 0; i < sizeof(special) / sizeof(*special); i++) {
			if ((int)key == special[i].code) {
				snprintf(name, size, "%s", special[i].name);
				return 1;
			}
		}
		return 0;
	}
	if (kind != OK) {
		return 0;
	}
	if (key == 27) {
		snprintf(name, size, "escape");
	} else if (key == '\r') {
		snprintf(name, size, "enter");
	} else if (key == '\t') {
		snprintf(name, size, "tab");
	} else if (key == 127 || key == 8) {
		snprintf(name, size, "backspace");
	} else if (key == 28) {
		snprintf(name, size, "ctrl+\\");
	} else if (key >= 1 && key <= 26) {
		snprintf(name, size, "ctrl+%c", 'a' + (int)key - 1);
	} else if (key >= 32) {
		mbstate_t state;
		memset(&state, 0, sizeof(state));
		char bytes[8];
		size_t n = wcrtomb(bytes, (wchar_t)key, &state);
		if (n == (size_t)-1 || n + 1 > size) {
			return 0;
		}
		memcpy(name, bytes, n);
		name[n] = 0;
	} else {
		return 0;
	}

	return 1;
}

/* Tab with extensions: complete the word before the cursor. One result is
   inserted; several open a pick list. */
static int complete_extension(void) {

	if (!completion_hook) {
		return 0;
	}
	size_t start = cursor;
	while (start && !iswspace((wint_t)input[start - 1])) {
		start--;
	}
	wchar_t saved = input[cursor];
	input[cursor] = 0;
	size_t text_bytes = wcstombs(NULL, input, 0), word_bytes = wcstombs(NULL, input + start, 0);
	char *text = text_bytes == (size_t)-1 ? NULL : malloc(text_bytes + 1);
	char *word = word_bytes == (size_t)-1 ? NULL : malloc(word_bytes + 1);
	if (text) {
		wcstombs(text, input, text_bytes + 1);
	}
	if (word) {
		wcstombs(word, input + start, word_bytes + 1);
	}
	input[cursor] = saved;
	if (!text || !word) {
		free(text);
		free(word);
		return 0;
	}
	json_object *items = completion_hook(text, word);
	free(text);
	free(word);
	size_t count =
		json_object_is_type(items, json_type_array) ? json_object_array_length(items) : 0;
	if (!count) {
		if (items) {
			json_object_put(items);
		}
		return 0;
	}
	size_t chosen = 0;
	if (count > 1) {
		const char *labels[64];
		if (count > 64) {
			count = 64;
		}
		for (size_t i = 0; i < count; i++) {
			json_object *item = json_object_array_get_idx(items, i);
			labels[i] = *field(item, "label") ? field(item, "label") : field(item, "value");
		}
		int picked = ui_select("Complete", labels, count, 0);
		if (picked < 0) {
			json_object_put(items);
			return 1;
		}
		chosen = (size_t)picked;
	}
	const char *value = field(json_object_array_get_idx(items, chosen), "value");
	size_t n;
	wchar_t *wide = wide_decode(value, &n);
	if (wide && n <= SIZE_MAX - length && input_reserve(length - (cursor - start) + n)) {
		memmove(input + start + n, input + cursor, (length - cursor + 1) * sizeof(*input));
		memcpy(input + start, wide, n * sizeof(*input));
		length = length - (cursor - start) + n;
		cursor = start + n;
		dirty = 1;
	}
	free(wide);
	json_object_put(items);

	return 1;
}

char *ui_editor_text(void) {

	size_t bytes = wcstombs(NULL, input, 0);
	char *text = bytes == (size_t)-1 ? NULL : malloc(bytes + 1);
	if (text) {
		wcstombs(text, input, bytes + 1);
	}

	return text;
}

void ui_insert_text(const char *text) {

	size_t n;
	wchar_t *wide = wide_decode(text, &n);
	if (!wide) {
		return;
	}
	if (n > SIZE_MAX - length || !input_reserve(length + n)) {
		free(wide);
		return;
	}
	wchar_t *before = wide_copy(input, length);
	if (!before) {
		free(wide);
		return;
	}
	size_t before_length = length, before_cursor = cursor;
	memmove(input + cursor + n, input + cursor, (length - cursor + 1) * sizeof(*input));
	memcpy(input + cursor, wide, n * sizeof(*input));
	length += n;
	cursor += n;
	if (n) {
		vim_pending_clear();
		last_replace_count = 0;
		replace_changed = 0;
		undo_record(before, before_length, before_cursor);
	}
	free(before);
	free(wide);
	recovery_save();
	dirty = 1;
}

int ui_tools_expanded(void) {

	return tool_details;
}

void ui_set_tools_expanded(int expanded) {

	if (!!expanded != !!tool_details) {
		tool_card_toggle();
	}
	tool_details = !!expanded;
}

static void paste_checkpoint(void) {

	if (pasting && paste_recovery_pending && now() - paste_saved_at >= 0.25) {
		recovery_save();
		paste_saved_at = now();
		paste_recovery_pending = 0;
	}
}

static void paste_snapshot_finish(void) {

	if (paste_before && (length != paste_before_length || wmemcmp(input, paste_before, length))) {
		/* Even a one-character paste is its own undo operation. */
		if (undo_current >= 0) {
			undo_nodes[undo_current].typed = 0;
		}
		undo_record(paste_before, paste_before_length, paste_before_cursor);
		if (undo_current >= 0) {
			undo_nodes[undo_current].typed = 0;
		}
	}
	paste_snapshot_clear();
	recovery_save();
}

static int edit_key(int kind, wint_t key) {

	if (kind == KEY_CODE_YES && key == KEY_PASTE_END) {
		history_view_paste(0);
	}
	if (history_view_paste(-1) && !history_view_active()) {
		return 0;
	}
	if (history_view_active()) {
		if (working && !history_view_paste(-1) &&
			((kind == OK && key == 3) || (kind == KEY_CODE_YES && key == KEY_INTERRUPT))) {
			history_view_close();
			return edit_key_raw(kind, key);
		}
		if (history_view_key(kind, key)) {
			dirty = 1;
			draw(1);
		}
		return 0;
	}
	/* The built-in INSERT -> NORMAL transition wins over extension hooks.
       Inspectors still consume Escape themselves rather than stopping work. */
	if (!normal_mode && !pasting && !modal_active && !scrollback_active() &&
		((kind == OK && key == 27) || (kind == KEY_CODE_YES && key == KEY_ESCAPE))) {
		return edit_key_raw(kind, key);
	}
	if (key_hook && !pasting && !modal_active) {
		char name[32];
		if (extension_key_name(kind, key, name, sizeof(name)) && key_hook(name)) {
			dirty = 1;
			return 0;
		}
	}
	if (scrollback_key(kind, key)) {
		dirty = 1;
		draw(1);
		return 0;
	}
	if (pasting) {
		size_t previous_length = length;
		int action = edit_key_raw(kind, key);
		if (length != previous_length) {
			paste_recovery_pending = 1;
		}
		if (!pasting) {
			paste_snapshot_finish();
		} else {
			paste_checkpoint();
		}
		return action;
	}
	/* These keys change modes or the viewport, never composer contents. */
	if ((kind == OK && (key == 3 || key == 4 || key == 27)) ||
		(kind == KEY_CODE_YES &&
			(key == KEY_LEFT || key == KEY_RIGHT || key == KEY_HOME || key == KEY_END ||
				key == KEY_RESIZE || key == KEY_MOUSE || key == KEY_PPAGE || key == KEY_NPAGE ||
				key == KEY_BTAB || key == KEY_FOCUS_IN || key == KEY_FOCUS_OUT ||
				key == KEY_INTERRUPT || key == KEY_EOF || key == KEY_ESCAPE))) {
		int old_scroll = scroll_offset;
		int action = edit_key_raw(kind, key);
		if (scroll_offset != old_scroll) {
			dirty = 1;
			draw(1);
		}
		return action;
	}
	wchar_t *before = wide_copy(input, length);
	if (!before) {
		/* Never reinterpret a paste as live commands just because its undo
           snapshot could not be allocated. Cancellation takes the fast path above. */
		if (kind == KEY_CODE_YES && (key == KEY_PASTE_BEGIN || key == KEY_PASTE_END)) {
			size_t previous_length = length;
			int action = edit_key_raw(kind, key);
			if (pasting) {
				paste_saved_at = now();
				paste_recovery_pending = length != previous_length;
			}
			return action;
		}
		return 0;
	}
	size_t before_length = length, before_cursor = cursor;
	history_action = 0;
	int old_scroll = scroll_offset;
	int action = edit_key_raw(kind, key);
	if (pasting) {
		paste_snapshot_clear();
		paste_before = before;
		paste_before_length = before_length;
		paste_before_cursor = before_cursor;
		paste_saved_at = now();
		paste_recovery_pending = length != before_length;
		return action;
	}
	/* Input-driven viewport changes are latency-sensitive. Keep the normal
       frame budget for streamed output, not for wheel/PageUp/PageDown; a
       wheel spin still paints at most every SCROLL_FRAME (draw). */
	if (scroll_offset != old_scroll) {
		dirty = 1;
		draw(1);
	}
	int changed = length != before_length || memcmp(input, before, (length + 1) * sizeof(*input));
	if (changed) {
		recovery_save();
	}
	/* Vim gives even rX over an existing X its own undo step. */
	if (!history_action && (changed || replace_changed)) {
		if (!replace_changed) {
			last_replace_count = 0;
		}
		undo_record(before, before_length, before_cursor);
	}
	replace_changed = 0;
	free(before);

	return action;
}

/* Commands typed while a turn runs: 1 if the handler ran it now. */
static int (*turn_command)(const char *message);

void ui_set_turn_command(int (*handler)(const char *message)) {

	turn_command = handler;
}
