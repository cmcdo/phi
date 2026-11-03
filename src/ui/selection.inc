/* Selecting conversation text with the mouse: tracking, highlight, the
   selected text and copying it. Included by ui.c before the draw code. */

static void transcript_selection_tracking(int enabled) {

	mmask_t mask = ALL_MOUSE_EVENTS;
#ifdef REPORT_MOUSE_POSITION
	if (enabled) {
		mask |= REPORT_MOUSE_POSITION;
	}
#endif
	mousemask(mask, NULL);
	/* Ncurses requests ordinary press/release tracking (1000) even when the
       REPORT_MOUSE_POSITION bit is present on some terminfo entries.  Ask the
       terminal explicitly for button-motion tracking only while dragging, so
       idle pointer movement never wakes Phi.  Ncurses still decodes the SGR
       mouse reports because its normal 1006 mode remains enabled. */
	if (active && isatty(STDOUT_FILENO)) {
		fputs(enabled ? "\033[?1002h" : "\033[?1002l", stdout);
		fflush(stdout);
	}
}

static void transcript_selection_clear(void) {

	if (!transcript_selection.visible && !transcript_selection.dragging) {
		return;
	}
	transcript_selection = (struct transcript_selection_state){0};
	transcript_selection_tracking(0);
	dirty = 1;
}

static int transcript_point_from_screen(int y, int x, int clamp, int *row, int *col) {

	if (conversation_copy_rows <= 0 || conversation_width <= 0 || conversation_pad_rows <= 0) {
		return 0;
	}
	y -= ui_chat_y;
	x -= ui_chat_x;
	int first_y = conversation_destination;
	int last_y = conversation_destination + conversation_copy_rows - 1;
	int first_x = transcript_left(ui_chat_columns ? ui_chat_columns : COLS);
	int last_x = first_x + conversation_width - 1;
	if (!clamp && (y < first_y || y > last_y || x < first_x || x > last_x)) {
		return 0;
	}
	if (y < first_y) {
		y = first_y;
	}
	if (y > last_y) {
		y = last_y;
	}
	if (x < first_x) {
		x = first_x;
	}
	if (x > last_x) {
		x = last_x;
	}
	*row = conversation_source + y - first_y;
	if (*row >= conversation_pad_rows) {
		*row = conversation_pad_rows - 1;
	}
	*col = x - first_x;

	return 1;
}

static int transcript_position_after(int row_a, int col_a, int row_b, int col_b) {

	return row_a > row_b || (row_a == row_b && col_a > col_b);
}

static void transcript_selection_bounds(
	int *first_row, int *first_col, int *last_row, int *last_col) {

	*first_row = transcript_selection.anchor_row;
	*first_col = transcript_selection.anchor_col;
	*last_row = transcript_selection.row;
	*last_col = transcript_selection.col;
	if (transcript_position_after(*first_row, *first_col, *last_row, *last_col)) {
		int row = *first_row, col = *first_col;
		*first_row = *last_row;
		*first_col = *last_col;
		*last_row = row;
		*last_col = col;
	}
}

/* Rows are transcript rows; pad row 0 shows transcript row `first`. */
static void transcript_selection_highlight(WINDOW *pad, int first) {

	if (!pad || !transcript_selection.visible) {
		return;
	}
	int first_row, first_col, last_row, last_col;
	transcript_selection_bounds(&first_row, &first_col, &last_row, &last_col);
	if (first_row < 0) {
		first_row = 0;
	}
	if (last_row >= conversation_pad_rows) {
		last_row = conversation_pad_rows - 1;
	}
	for (int row = first_row; row <= last_row; row++) {
		int begin = row == first_row ? first_col : 0;
		int end = row == last_row ? last_col : conversation_width - 1;
		if (begin < 0) {
			begin = 0;
		}
		if (end >= conversation_width) {
			end = conversation_width - 1;
		}
		if (end >= begin) {
			mvwchgat(pad, row - first, begin, end - begin + 1, A_REVERSE, 0, NULL);
		}
	}
}

struct selection_buffer {
	char *text;
	size_t used, capacity;
};

static int selection_buffer_append(struct selection_buffer *buffer, const char *text, size_t len) {

	if (!len) {
		return 1;
	}
	if (buffer->used + len + 1 > buffer->capacity) {
		size_t next = buffer->capacity ? buffer->capacity : 256;
		while (next < buffer->used + len + 1) {
			if (next > 1024 * 1024) {
				return 0;
			}
			next *= 2;
		}
		char *grown = realloc(buffer->text, next);
		if (!grown) {
			return 0;
		}
		buffer->text = grown;
		buffer->capacity = next;
	}
	memcpy(buffer->text + buffer->used, text, len);
	buffer->used += len;
	buffer->text[buffer->used] = 0;

	return 1;
}

/* Extract rendered text, rather than raw Markdown/tool markup, so clipboard
   contents match the highlighted transcript. */
/* Rows are transcript rows; pad row 0 shows transcript row `first`. */
static char *transcript_selection_text(WINDOW *pad, int first, size_t *length_out) {

	if (length_out) {
		*length_out = 0;
	}
	if (!pad || !transcript_selection.visible || conversation_width <= 0) {
		return NULL;
	}
	int first_row, first_col, last_row, last_col;
	transcript_selection_bounds(&first_row, &first_col, &last_row, &last_col);
	if (first_row < 0) {
		first_row = 0;
	}
	if (last_row >= conversation_pad_rows) {
		last_row = conversation_pad_rows - 1;
	}
	if (last_row < first_row) {
		return NULL;
	}
	wchar_t *line = calloc((size_t)conversation_width + 2, sizeof(*line));
	wchar_t *picked = calloc((size_t)conversation_width + 2, sizeof(*picked));
	if (!line || !picked) {
		free(line);
		free(picked);
		return NULL;
	}
	struct selection_buffer buffer = {0};
	for (int row = first_row; row <= last_row; row++) {
		memset(line, 0, ((size_t)conversation_width + 2) * sizeof(*line));
		int count = mvwinnwstr(pad, row - first, 0, line, conversation_width);
		if (count < 0) {
			count = 0;
		}
		int begin = row == first_row ? first_col : 0;
		int end = row == last_row ? last_col : conversation_width - 1;
		size_t picked_count = 0;
		int cell = 0;
		for (int i = 0; i < count; i++) {
			int cells = wcwidth(line[i]);
			if (cells < 0) {
				cells = 0;
			}
			int char_first = cell;
			int char_last = cells ? cell + cells - 1 : cell;
			/* A code block's language label (dim, code colours) is not code. */
			cchar_t shown;
			attr_t attributes = 0;
			short pair = 0;
			wchar_t glyph[CCHARW_MAX + 1];
			int label = mvwin_wch(pad, row - first, char_first, &shown) != ERR &&
				getcchar(&shown, glyph, &attributes, &pair, NULL) != ERR && pair == 6 &&
				(attributes & A_DIM);
			if (!label && char_last >= begin && char_first <= end) {
				picked[picked_count++] = line[i];
			}
			cell += cells;
			if (cell > end && cells) {
				break;
			}
		}
		while (picked_count &&
			(picked[picked_count - 1] == L' ' || picked[picked_count - 1] == L'\t')) {
			picked_count--;
		}
		mbstate_t state = {0};
		for (size_t i = 0; i < picked_count; i++) {
			char encoded[MB_LEN_MAX];
			size_t bytes = wcrtomb(encoded, picked[i], &state);
			if (bytes == (size_t)-1) {
				memset(&state, 0, sizeof(state));
				encoded[0] = '?';
				bytes = 1;
			}
			if (!selection_buffer_append(&buffer, encoded, bytes)) {
				goto failure;
			}
		}
		if (row != last_row && !selection_buffer_append(&buffer, "\n", 1)) {
			goto failure;
		}
	}
	free(line);
	free(picked);
	if (!buffer.text) {
		buffer.text = strdup("");
	}
	if (length_out) {
		*length_out = buffer.used;
	}
	return buffer.text;
failure:
	free(line);
	free(picked);
	free(buffer.text);

	return NULL;
}

static int transcript_selection_copy(void) {

	if (!transcript_selection.visible || conversation_pad_rows <= 0 || conversation_width <= 0) {
		return 0;
	}
	if (scrollback_active()) {
		size_t len = 0;
		char *text = transcript_selection_text(scrollback_page(), 0, &len);
		int ok = text && len && clipboard_copy(text, len);
		free(text);
		return ok;
	}
	/* Lay out only the selected rows, not the whole transcript. */
	int first_row, first_col, last_row, last_col;
	transcript_selection_bounds(&first_row, &first_col, &last_row, &last_col);
	if (first_row < 0) {
		first_row = 0;
	}
	if (last_row >= conversation_pad_rows) {
		last_row = conversation_pad_rows - 1;
	}
	if (last_row < first_row) {
		return 0;
	}
	WINDOW *pad = newpad(last_row - first_row + 1, conversation_width);
	if (!pad) {
		return 0;
	}
	wbkgd(pad, COLOR_PAIR(27) | ' ');
	layout_rows(pad, conversation_width, first_row);
	size_t len = 0;
	char *text = transcript_selection_text(pad, first_row, &len);
	delwin(pad);
	int ok = text && len && clipboard_copy(text, len);
	free(text);

	return ok;
}

static void transcript_selection_start(int y, int x) {

	int row, col;
	if (!transcript_point_from_screen(y, x, 0, &row, &col)) {
		return;
	}
	transcript_selection = (struct transcript_selection_state){.visible = 1,
		.dragging = 1,
		.anchor_row = row,
		.anchor_col = col,
		.row = row,
		.col = col,
		.mouse_y = y,
		.mouse_x = x};
	transcript_selection_tracking(1);
	dirty = 1;
}

static void transcript_selection_update(int y, int x) {

	if (!transcript_selection.dragging) {
		return;
	}
	int row, col;
	transcript_selection.mouse_y = y;
	transcript_selection.mouse_x = x;
	if (transcript_point_from_screen(y, x, 1, &row, &col) &&
		(row != transcript_selection.row || col != transcript_selection.col)) {
		transcript_selection.row = row;
		transcript_selection.col = col;
		dirty = 1;
	}
}

static void transcript_selection_finish(int y, int x) {

	if (!transcript_selection.dragging) {
		return;
	}
	transcript_selection_update(y, x);
	transcript_selection.dragging = 0;
	transcript_selection_tracking(0);
	transcript_selection_copy();
	dirty = 1;
}

static void transcript_selection_autoscroll(void) {

	if (!transcript_selection.dragging || conversation_copy_rows <= 0 || scrollback_active()) {
		return;
	}
	double stamp = now();
	if (transcript_selection.last_scroll && stamp - transcript_selection.last_scroll < 0.04) {
		return;
	}
	int changed = 0;
	if (transcript_selection.mouse_y <= ui_chat_y + conversation_destination &&
		scroll_offset < conversation_maximum) {
		scroll_offset++;
		changed = 1;
	} else if (transcript_selection.mouse_y >=
			ui_chat_y + conversation_destination + conversation_copy_rows - 1 &&
		scroll_offset > 0) {
		scroll_offset--;
		changed = 1;
	}
	if (!changed) {
		return;
	}
	transcript_selection.last_scroll = stamp;
	dirty = 1;
	draw(1);
	transcript_selection_update(transcript_selection.mouse_y, transcript_selection.mouse_x);
}
