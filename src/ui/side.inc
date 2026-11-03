/* Side requests' replies (api->complete_async with "show"), such as a
   /btw question asked while a turn runs. Each streams into a panel above
   the prompt, apart from the conversation, so it never splits the reply
   being written. Finished, it becomes a card in the conversation: at once
   when idle, after the turn otherwise. Display only: neither question nor
   answer enters the model's history. Included by ui.c after markdown.inc. */

static void ui_card_markdown(const char *title, const char *text); /* dialog.inc */

#define SIDE_PANELS 4

static struct side_panel {
	int id, done, failed;
	char title[128];
	char *text;
	size_t length;
} side_panels[SIDE_PANELS];

static struct side_panel *side_find(int id) {

	for (int i = 0; i < SIDE_PANELS; i++) {
		if (side_panels[i].id && side_panels[i].id == id) {
			return &side_panels[i];
		}
	}

	return NULL;
}

static void side_append(struct side_panel *panel, const char *text, size_t n) {

	char *grown = realloc(panel->text, panel->length + n + 1);
	if (!grown) {
		return;
	}
	memcpy(grown + panel->length, text, n);
	panel->length += n;
	grown[panel->length] = 0;
	panel->text = grown;
}

static void side_release(struct side_panel *panel) {

	free(panel->text);
	*panel = (struct side_panel){0};
}

/* Finished panels become cards, oldest first. While a turn runs they wait,
   so a card never lands inside the reply being written. */
static void side_flush(void) {

	if (working) {
		return;
	}
	for (;;) {
		struct side_panel *oldest = NULL;
		for (int i = 0; i < SIDE_PANELS; i++) {
			if (side_panels[i].id && side_panels[i].done &&
				(!oldest || side_panels[i].id < oldest->id)) {
				oldest = &side_panels[i];
			}
		}
		if (!oldest) {
			return;
		}
		char title[160];
		snprintf(title, sizeof(title), "%s%s", oldest->title, oldest->failed ? " · failed" : "");
		if (oldest->failed || !oldest->text || !*oldest->text) {
			ui_card(title, oldest->text && *oldest->text ? oldest->text : "(no answer)");
		} else {
			ui_card_markdown(title, oldest->text);
		}
		side_release(oldest);
		dirty = 1;
	}
}

void ui_side_begin(int id, const char *title) {

	struct side_panel *panel = NULL;
	for (int i = 0; i < SIDE_PANELS && !panel; i++) {
		if (!side_panels[i].id) {
			panel = &side_panels[i];
		}
	}
	if (!panel) { /* the oldest still running gives way */
		panel = &side_panels[0];
		for (int i = 1; i < SIDE_PANELS; i++) {
			if (side_panels[i].id < panel->id) {
				panel = &side_panels[i];
			}
		}
		side_release(panel);
	}
	panel->id = id;
	snprintf(panel->title, sizeof(panel->title), "%s", title && *title ? title : "side");
	for (char *c = panel->title; *c; c++) {
		if ((unsigned char)*c < 32) {
			*c = ' ';
		}
	}
	dirty = 1;
}

void ui_side_text(int id, const char *text) {

	struct side_panel *panel = side_find(id);
	if (!panel || !text) {
		return;
	}
	side_append(panel, text, strlen(text));
	dirty = 1;
}

void ui_side_end(int id, const char *text, const char *error) {

	struct side_panel *panel = side_find(id);
	if (!panel) {
		return;
	}
	/* The final text is the whole answer; streamed pieces were a preview. */
	if (error && *error) {
		free(panel->text);
		panel->text = NULL;
		panel->length = 0;
		side_append(panel, error, strlen(error));
		panel->failed = 1;
	} else if (text && *text) {
		free(panel->text);
		panel->text = NULL;
		panel->length = 0;
		side_append(panel, text, strlen(text));
	}
	panel->done = 1;
	dirty = 1;
	side_flush();
}

/* The panel shown: the newest. */
static struct side_panel *side_current(void) {

	struct side_panel *newest = NULL;
	for (int i = 0; i < SIDE_PANELS; i++) {
		if (side_panels[i].id && (!newest || side_panels[i].id > newest->id)) {
			newest = &side_panels[i];
		}
	}

	return newest;
}

/* The text into c line by line, with its inline markdown (bold, `code`). */
static void side_text(struct md_canvas *c, const char *text, size_t n) {

	for (size_t at = 0; at < n;) {
		size_t end = at;
		while (end < n && text[end] != '\n') {
			end++;
		}
		md_inline(c, text + at, end - at, COLOR_PAIR(24), 8);
		if (end < n) {
			c->y++;
			c->x = 0;
		}
		at = end + 1;
	}
}

/* Rows of text, wrapped at width, laid into a pad (rows used in *count). */
static WINDOW *side_layout(const char *text, size_t n, int width, int *count) {

	struct md_canvas probe = {.pad = NULL, .width = width, .word_wrap = 1};
	side_text(&probe, text, n);
	int rows = probe.y + 1;
	WINDOW *pad = newpad(rows, width);
	if (!pad) {
		*count = 0;
		return NULL;
	}
	wbkgd(pad, COLOR_PAIR(8) | ' ');
	struct md_canvas canvas = {.pad = pad, .width = width, .word_wrap = 1};
	side_text(&canvas, text, n);
	*count = rows;

	return pad;
}

/* Height of the panel (0: none): a heading and up to 6 rows of text. */
static int side_rows(int cols, int screen_rows) {

	struct side_panel *panel = side_current();
	if (!panel || cols < 40) {
		return 0;
	}
	int width = pending_text_width(cols), count = 0;
	WINDOW *pad = panel->length ? side_layout(panel->text, panel->length, width, &count) : NULL;
	if (pad) {
		delwin(pad);
	}
	int body = count < 1 ? 1 : count > 6 ? 6 : count;
	int limit = screen_rows / 3;
	int total = 1 + body;

	return total > limit ? (limit >= 2 ? limit : 0) : total;
}

static void side_draw(int row, int rows, int cols) {

	struct side_panel *panel = side_current();
	if (!panel || rows <= 0) {
		return;
	}
	int width = cols - 2 * skin.margin;
	if (width > 96) {
		width = 96;
	}
	int left = skin.margin + 1, end = skin.margin + width - 1;
	for (int y = row; y < row + rows; y++) {
		attrset(COLOR_PAIR(8));
		mvhline(y, left + 1, ' ', width - 2);
		attrset(COLOR_PAIR(50));
		mvaddwstr(y, left, L"│");
	}
	welcome_text(row, left + 2, end - left - 2, panel->title, 9, 1);
	int x = left + 3 + skin_width(panel->title);
	const char *state = panel->failed ? "· failed"
		: panel->done                 ? "· done · shown below after this turn"
									  : "· answering…";
	if (x < end - 2) {
		welcome_text(row, x, end - x, state, panel->failed ? 62 : 49, 0);
	}
	int text_width = pending_text_width(cols), count = 0;
	WINDOW *pad =
		panel->length ? side_layout(panel->text, panel->length, text_width, &count) : NULL;
	if (!pad) {
		if (rows > 1) {
			welcome_text(row + 1, left + 2, end - left - 2, "…", 49, 0);
		}
		attrset(A_NORMAL);
		return;
	}
	/* The newest lines while it is written. */
	int body = rows - 1, first = count > body ? count - body : 0;
	if (body > 0) {
		copywin(pad, stdscr, first, 0, row + 1, left + 2, row + body, left + 1 + text_width, 0);
	}
	delwin(pad);
	attrset(A_NORMAL);
}
