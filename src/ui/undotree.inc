/* U in NORMAL mode: the draft's undo tree (undo.inc). Each state is a row,
   indented where it starts a new branch; moving through them shows each in
   the prompt. j/k or ↑/↓ move, Enter keeps the state, Esc/q goes back to
   where you were. Included by ui.c after dialog.inc. */

struct undo_row {
	int node, column;
};

static int undo_seq_order(const void *a, const void *b) {

	unsigned x = undo_nodes[*(const int *)a].seq, y = undo_nodes[*(const int *)b].seq;

	return x < y ? -1 : x > y;
}

/* Depth-first from each root; a child after the first moves a column right. */
static void undo_rows_from(int node, int column, struct undo_row *rows, int *count) {

	rows[(*count)++] = (struct undo_row){node, column};
	int children[UNDO_NODES], n = 0;
	for (int i = 0; i < UNDO_NODES; i++) {
		if (undo_nodes[i].text && undo_nodes[i].parent == node) {
			children[n++] = i;
		}
	}
	qsort(children, (size_t)n, sizeof(*children), undo_seq_order);
	for (int i = 0; i < n; i++) {
		undo_rows_from(children[i], column + (i > 0), rows, count);
	}
}

static void undo_row_label(const struct undo_row *row, char *out, size_t size) {

	const struct undo_node *state = &undo_nodes[row->node];
	double age = now() - state->when;
	char when[16];
	if (age < 60) {
		snprintf(when, sizeof(when), "%ds", (int)age);
	} else if (age < 3600) {
		snprintf(when, sizeof(when), "%dm", (int)(age / 60));
	} else {
		snprintf(when, sizeof(when), "%dh", (int)(age / 3600));
	}
	char preview[160] = "";
	size_t filled = 0;
	for (size_t i = 0; i < state->length && filled + 8 < sizeof(preview) && i < 60; i++) {
		wchar_t ch = state->text[i] == L'\n' ? L'⏎' : state->text[i];
		char bytes[8];
		mbstate_t mb = {0};
		size_t n = wcrtomb(bytes, ch, &mb);
		if (n == (size_t)-1) {
			continue;
		}
		memcpy(preview + filled, bytes, n);
		filled += n;
	}
	preview[filled] = 0;
	if (state->length > 60 && filled + 4 < sizeof(preview)) {
		strcat(preview, "…");
	}
	int indent = row->column > 12 ? 12 : row->column;
	snprintf(out, size, "%*s%s %-3u %4s  %s", indent * 2, "", row->node == undo_current ? "●" : "○",
		state->seq, when, *preview ? preview : "(empty)");
}

static void undo_tree_view(void) {

	if (undo_current < 0 || !active) {
		return;
	}
	static struct undo_row rows[UNDO_NODES];
	int count = 0;
	int roots[UNDO_NODES], root_count = 0;
	for (int i = 0; i < UNDO_NODES; i++) {
		if (undo_nodes[i].text &&
			(undo_nodes[i].parent < 0 || !undo_nodes[undo_nodes[i].parent].text)) {
			roots[root_count++] = i;
		}
	}
	qsort(roots, (size_t)root_count, sizeof(*roots), undo_seq_order);
	for (int i = 0; i < root_count; i++) {
		undo_rows_from(roots[i], 0, rows, &count);
	}
	int original = undo_current, selected = 0, first = 0, paste = 0, done = 0;
	for (int i = 0; i < count; i++) {
		if (rows[i].node == undo_current) {
			selected = i;
		}
	}
	modal_active = 1;
	while (!done) {
		int list_rows = count < 14 ? count : 14;
		struct dialog_box box = dialog_frame("Undo tree", list_rows + 4);
		list_rows = box.height - 4;
		if (selected < first) {
			first = selected;
		}
		if (selected >= first + list_rows) {
			first = selected - list_rows + 1;
		}
		for (int i = first; i < count && i < first + list_rows; i++) {
			int row = box.y + 1 + (i - first), chosen = i == selected;
			char label[256];
			undo_row_label(&rows[i], label, sizeof(label));
			if (chosen) {
				attrset(COLOR_PAIR(2));
				mvhline(row, box.x + 1, ' ', box.width - 2);
			}
			welcome_text(row, box.x + 3, box.width - 6, label,
				chosen                         ? 2
					: rows[i].node == original ? 28
											   : 27,
				chosen);
		}
		welcome_text(box.y + box.height - 2, box.x + 3, box.width - 6,
			"j/k move (shown in the prompt) · enter keep · esc back", 3, 0);
		curs_set(0);
		dialog_show();
		wint_t key;
		int kind = dialog_key(&key, 0, &paste);
		if (kind == ERR || paste) {
			continue;
		}
		if (dialog_cancel_key(kind, key) || (kind == OK && key == 'q')) {
			undo_go(original);
			done = 1;
		} else if ((kind == OK && key == '\r') || (kind == KEY_CODE_YES && key == KEY_ENTER)) {
			done = 1;
		} else if ((kind == OK && (key == 'j' || key == 14)) ||
			(kind == KEY_CODE_YES && key == KEY_DOWN)) {
			if (selected + 1 < count) {
				undo_go(rows[++selected].node);
			}
		} else if ((kind == OK && (key == 'k' || key == 16)) ||
			(kind == KEY_CODE_YES && key == KEY_UP)) {
			if (selected > 0) {
				undo_go(rows[--selected].node);
			}
		}
	}
	history_action = 1;
	modal_active = 0;
	dirty = 1;
	cursor_shape = -1;
	draw(1);
}
