/* Board-local view state: filtering never mutates a chat or its draft. */
static int board_group(const struct chat_row *row) {

	for (int g = 0; g < 4; g++) {
		if (!strcmp(row->group, groups[g][0])) {
			return g;
		}
	}

	return -1;
}

static int board_matches(const struct view *v, size_t i) {

	return !v->query[0] || (v->scores && v->scores[i] >= 0);
}

static int board_visible(const struct view *v, size_t i) {

	int g = board_group(&v->rows[i]);

	return g >= 0 && board_matches(v, i) && !(v->collapsed & (1u << g));
}

static void board_filter_update(struct view *v, int choose) {

	if (!v->query[0]) {
		return;
	}
	if (!v->recent) {
		v->recent = calloc(v->count, sizeof(*v->recent));
	}
	if (!v->scores) {
		v->scores = malloc(v->count * sizeof(*v->scores));
	}
	if (v->count && (!v->recent || !v->scores)) {
		v->query[0] = 0;
		snprintf(v->notice, sizeof(v->notice), "Cannot allocate chat filter; showing all chats.");
		return;
	}
	char query[1024];
	if (wcstombs(query, v->query, sizeof(query)) == (size_t)-1) {
		v->query[0] = 0;
		snprintf(v->notice, sizeof(v->notice), "Cannot encode filter; showing all chats.");
		return;
	}
	int best = -1;
	size_t selected = v->cursor, matched = 0;
	for (size_t i = 0; i < v->count; i++) {
		struct chat_row *row = &v->rows[i];
		int score = session_text_score(row->title, query);
		if (score >= 0) {
			score += 1000;
		} else {
			if (!v->recent[i][0]) {
				struct session_entry entry = {0};
				char error[256];
				char text[65537] = "";
				if (*row->session_file &&
					session_from_path(row->session_file, &entry, error, sizeof(error))) {
					(void)session_recent_text(&entry, text, sizeof(text), error, sizeof(error));
				}
				size_t bytes = strlen(text), start = bytes > 4095 ? bytes - 4095 : 0;
				while (start < bytes && ((unsigned char)text[start] & 0xc0) == 0x80) {
					start++;
				}
				memcpy(v->recent[i] + 1, text + start, bytes - start + 1);
				v->recent[i][0] = 1;
			}
			char text[4608];
			snprintf(text, sizeof(text), "%s %s", row->detail, v->recent[i] + 1);
			score = session_text_score(text, query);
			if (score > 999) {
				score = 999;
			}
		}
		v->scores[i] = score;
		if (score >= 0) {
			matched++;
		}
		if (score > best && board_visible(v, i)) {
			best = score;
			selected = i;
		}
	}
	if (choose || v->cursor >= v->count || !board_visible(v, v->cursor)) {
		v->cursor = v->anchor = selected;
	}
	if (strlen(query) > 120) {
		size_t end = 120;
		while (end && ((unsigned char)query[end] & 0xc0) == 0x80) {
			end--;
		}
		query[end] = 0;
	}
	snprintf(v->notice, sizeof(v->notice), "Filter /%.120s · %zu matches · / edit · Esc clears",
		query, matched);
}

static void board_filter_key(struct view *v, int special, wint_t key) {

	if (v->paste_block && (special || key < 32 || key == 127)) {
		return;
	}
	if ((special && (key == KEY_UP || key == KEY_DOWN)) || (!special && (key == 14 || key == 16))) {
		move_cursor(v, (special ? key == KEY_UP : key == 16) ? -1 : 1);
		return;
	}
	size_t n = wcslen(v->query);
	if ((!special && (key == 27 || key == 3))) {
		v->query[0] = 0;
		v->mode = NORMAL;
		v->notice[0] = 0;
		return;
	}
	if ((!special && (key == '\r' || key == '\n')) || (special && key == KEY_ENTER)) {
		v->mode = NORMAL;
		return;
	}
	if ((special && key == KEY_BACKSPACE) || (!special && (key == 127 || key == 8))) {
		if (n) {
			v->query[n - 1] = 0;
		}
	} else if (!special && key == 21) {
		v->query[0] = 0;
	} else if (!special && key >= 32 && iswprint(key) &&
		n + 1 < sizeof(v->query) / sizeof(*v->query)) {
		v->query[n] = (wchar_t)key;
		v->query[n + 1] = 0;
	} else {
		return;
	}
	if (!v->query[0]) {
		snprintf(v->notice, sizeof(v->notice),
			"Filter / · titles and recent messages · Enter keeps · Esc clears");
	}
	board_filter_update(v, 1);
}

static void board_next_heading(struct view *v, int direction) {

	int start = v->heading ? v->heading - 1 : direction > 0 ? -1 : 4;
	for (int step = 1; step <= 4; step++) {
		int g = (start + direction * step + 8) % 4;
		for (size_t i = 0; i < v->count; i++) {
			if (board_group(&v->rows[i]) == g && board_matches(v, i)) {
				v->heading = g + 1;
				return;
			}
		}
	}
}

static int board_heading_mouse(struct view *v, const MEVENT *mouse) {

	if (v->mode != NORMAL) {
		return 0;
	}
	for (int g = 0; g < 4; g++) {
		if (v->heading_y[g] >= 0 && mouse->y == v->heading_y[g]) {
			v->heading = g + 1;
			v->pending_g = v->pending_d = 0;
			if (mouse->bstate & (BUTTON1_CLICKED | BUTTON1_RELEASED)) {
				v->collapsed ^= 1u << g;
			}
			return 1;
		}
	}
	if (mouse->bstate & REPORT_MOUSE_POSITION) {
		if (v->heading) {
			v->pending_g = v->pending_d = 0;
		}
		v->heading = 0;
	}

	return 0;
}
