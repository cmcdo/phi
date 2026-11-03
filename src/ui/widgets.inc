/* Extension widgets (Pi's setWidget/setHeader/setFooter/setWorkingMessage/
   setTitle): short lines an extension pins above or below the prompt, at
   the top of the screen, or in place of the footer. Each belongs to the
   extension that set it and goes when that extension unloads. Included by
   ui.c before draw(). */

static void ui_native_widget_forget(const char *owner);
static void welcome_text(int y, int x, int available, const char *text, int pair, int bold);
static void append_styled(const char *text, int user);

/* Markup in extension text: {accent} {muted} {error} {success} {text} and
   {b} switch the style, {/} restores it, {{ is a literal brace. Anything
   else, such as an unknown tag, is shown as written. */
enum {
	MARK_BASE,
	MARK_ACCENT,
	MARK_MUTED,
	MARK_ERROR,
	MARK_SUCCESS,
	MARK_TEXT,
	MARK_BOLD,
	MARK_BRACE
};

static int markup_tag(const char *p, size_t *size) {

	static const char *const tags[] = {
		"{/}", "{accent}", "{muted}", "{error}", "{success}", "{text}", "{b}", "{{"};
	for (int i = 0; i < 8; i++) {
		size_t n = strlen(tags[i]);
		if (!strncmp(p, tags[i], n)) {
			*size = n;
			return i;
		}
	}

	return -1;
}

/* Walk text as runs: each run is plain text in one style. */
static void markup_runs(
	const char *text, void (*run)(const char *text, size_t n, int mark, void *data), void *data) {

	int mark = MARK_BASE;
	const char *start = text, *p = text;
	while (*p) {
		size_t size;
		int tag = *p == '{' ? markup_tag(p, &size) : -1;
		if (tag < 0) {
			p++;
			continue;
		}
		if (p > start) {
			run(start, (size_t)(p - start), mark, data);
		}
		if (tag == MARK_BRACE) {
			run("{", 1, mark, data);
		} else {
			mark = tag;
		}
		p += size;
		start = p;
	}
	if (p > start) {
		run(start, (size_t)(p - start), mark, data);
	}
}

struct markup_screen {
	int y, x, end, pair, bold;
};

static void markup_screen_run(const char *text, size_t n, int mark, void *data) {

	struct markup_screen *s = data;
	static const int pairs[] = {0, 28, 3, 53, 51, 27, 0};
	char piece[512];
	if (n >= sizeof(piece)) {
		n = sizeof(piece) - 1;
	}
	memcpy(piece, text, n);
	piece[n] = 0;
	int width = skin_width(piece);
	if (width < 0) {
		width = (int)n;
	}
	int pair = mark == MARK_BASE || mark == MARK_BOLD ? s->pair : pairs[mark];
	welcome_text(s->y, s->x, s->end - s->x, piece, pair, s->bold || mark == MARK_BOLD);
	s->x += width;
}

/* welcome_text with markup; plain text takes the plain path. Returns the
   column after the text. */
static int markup_text(int y, int x, int available, const char *text, int pair, int bold) {

	if (!strchr(text, '{')) {
		welcome_text(y, x, available, text, pair, bold);
		int width = skin_width(text);
		return x + (width < 0 ? 0 : width < available ? width : available);
	}
	struct markup_screen s = {y, x, x + available, pair, bold};
	markup_runs(text, markup_screen_run, &s);

	return s.x < x + available ? s.x : x + available;
}

/* Transcript styles for card text: base, accent, muted, error, success, text, bold. */
static void markup_transcript_run(const char *text, size_t n, int mark, void *data) {

	int base = *(int *)data;
	static const int styles[] = {0, 41, 64, 7, 65, 5, 66};
	char piece[1024];
	while (n) {
		size_t take = n < sizeof(piece) - 1 ? n : sizeof(piece) - 1;
		memcpy(piece, text, take);
		piece[take] = 0;
		append_styled(piece,
			mark == MARK_BASE                      ? base
				: base == 14 && mark == MARK_MUTED ? 14
												   : styles[mark]);
		text += take;
		n -= take;
	}
}

static void markup_append(const char *text, int base) {

	if (!strchr(text, '{')) {
		append_styled(text, base);
		return;
	}
	markup_runs(text, markup_transcript_run, &base);
}

#define WIDGET_LIMIT 12
#define WIDGET_LINES 8
#define HEADER_LINES 4

struct widget {
	char owner[49], key[33];
	int below;
	size_t count;
	char lines[WIDGET_LINES][256];
};
static struct widget widgets[WIDGET_LIMIT];
static size_t widget_count;
static char header_owner[49], header_lines[HEADER_LINES][256], footer_owner[49], footer_text[256];
static size_t header_count;
static char composer_owner[128], composer_label[256], composer_hint[256];

int ui_composer(const char *owner, const char *label, const char *hint) {

	if (owner && *composer_owner && strcmp(composer_owner, owner)) {
		return 0;
	}
	if (!owner || (!label && !hint)) {
		composer_owner[0] = composer_label[0] = composer_hint[0] = 0;
	} else {
		if (strlen(owner) >= sizeof(composer_owner) ||
			(label && strlen(label) >= sizeof(composer_label)) ||
			(hint && strlen(hint) >= sizeof(composer_hint))) {
			return 0;
		}
		snprintf(composer_owner, sizeof(composer_owner), "%s", owner);
		snprintf(composer_label, sizeof(composer_label), "%s", label ? label : "");
		snprintf(composer_hint, sizeof(composer_hint), "%s", hint ? hint : "");
		for (char *p = composer_label; *p; p++) {
			if ((unsigned char)*p < 32 || *p == 127) {
				*p = ' ';
			}
		}
		for (char *p = composer_hint; *p; p++) {
			if ((unsigned char)*p < 32 || *p == 127) {
				*p = ' ';
			}
		}
	}
	dirty = 1;

	return 1;
}

static char working_message[128], working_detail[128], working_tip[160];
static int working_hidden;

static void copy_lines(
	char (*to)[256], size_t limit, const char *const *lines, size_t count, size_t *used_count) {

	*used_count = 0;
	for (size_t i = 0; i < count && i < limit; i++) {
		snprintf(to[i], 256, "%s", lines[i] ? lines[i] : "");
		for (char *c = to[i]; *c; c++) {
			if ((unsigned char)*c < 32) {
				*c = ' ';
			}
		}
		(*used_count)++;
	}
}

/* count 0 removes the widget. Returns 0 when no slot is free. */
int ui_widget(
	const char *owner, const char *key, const char *const *lines, size_t count, int below) {

	size_t slot = widget_count;
	for (size_t i = 0; i < widget_count; i++) {
		if (!strcmp(widgets[i].owner, owner) && !strcmp(widgets[i].key, key)) {
			slot = i;
		}
	}
	if (!count) {
		if (slot < widget_count) {
			memmove(
				widgets + slot, widgets + slot + 1, (widget_count - slot - 1) * sizeof(*widgets));
			widget_count--;
			dirty = 1;
		}
		return 1;
	}
	if (slot == widget_count) {
		if (widget_count == WIDGET_LIMIT) {
			return 0;
		}
		widget_count++;
	}
	struct widget *w = &widgets[slot];
	snprintf(w->owner, sizeof(w->owner), "%s", owner);
	snprintf(w->key, sizeof(w->key), "%s", key);
	w->below = below;
	copy_lines(w->lines, WIDGET_LINES, lines, count, &w->count);
	dirty = 1;

	return 1;
}

void ui_header(const char *owner, const char *const *lines, size_t count) {

	snprintf(header_owner, sizeof(header_owner), "%s", count ? owner : "");
	copy_lines(header_lines, HEADER_LINES, lines, count, &header_count);
	dirty = 1;
}

void ui_footer(const char *owner, const char *text) {

	snprintf(footer_owner, sizeof(footer_owner), "%s", text ? owner : "");
	snprintf(footer_text, sizeof(footer_text), "%s", text ? text : "");
	dirty = 1;
}

void ui_working(const char *message, const char *detail, const char *tip, int visible) {

	snprintf(working_message, sizeof(working_message), "%s", message ? message : "");
	snprintf(working_detail, sizeof(working_detail), "%s", detail ? detail : "");
	snprintf(working_tip, sizeof(working_tip), "%s", tip ? tip : "");
	for (char *c = working_detail; *c; c++) {
		if ((unsigned char)*c < 32) {
			*c = ' ';
		}
	}
	for (char *c = working_tip; *c; c++) {
		if ((unsigned char)*c < 32) {
			*c = ' ';
		}
	}
	working_hidden = !visible;
	dirty = 1;
}

void ui_title(const char *title) {

	/* OSC 2: the window title. Control characters would end the sequence. */
	char safe[256];
	snprintf(safe, sizeof(safe), "%s", title ? title : "");
	for (char *c = safe; *c; c++) {
		if ((unsigned char)*c < 32 || *c == 127) {
			*c = ' ';
		}
	}
	fprintf(stdout, "\033]2;%s\007", safe);
	fflush(stdout);
}

/* An extension unloaded: remove what it placed. */
void ui_forget_owner(const char *owner) {

	ui_native_widget_forget(owner);
	ui_native_transition_forget(owner);
	if (!strcmp(composer_owner, owner)) {
		ui_composer(owner, NULL, NULL);
	}
	for (size_t i = widget_count; i-- > 0;) {
		if (!strcmp(widgets[i].owner, owner)) {
			memmove(widgets + i, widgets + i + 1, (widget_count - i - 1) * sizeof(*widgets));
			widget_count--;
		}
	}
	if (!strcmp(header_owner, owner)) {
		header_owner[0] = 0;
		header_count = 0;
	}
	if (!strcmp(footer_owner, owner)) {
		footer_owner[0] = 0;
		footer_text[0] = 0;
	}
	dirty = 1;
}

static int widget_rows(int below) {

	int rows = 0;
	for (size_t i = 0; i < widget_count; i++) {
		if (widgets[i].below == below) {
			rows += (int)widgets[i].count;
		}
	}

	return rows;
}

static void widget_draw(int row, int below, int cols) {

	for (size_t i = 0; i < widget_count; i++) {
		if (widgets[i].below != below) {
			continue;
		}
		for (size_t l = 0; l < widgets[i].count; l++) {
			markup_text(row++, skin.margin, cols - 2 * skin.margin, widgets[i].lines[l], 3, 0);
		}
	}
}

static int header_rows(void) {

	return skin.header_visible ? (header_count ? (int)header_count : 1) : 0;
}

static void header_draw(int cols) {

	if (!skin.header_visible) {
		return;
	}
	if (!header_count) {
		chrome_header_row(cols);
		return;
	}
	for (size_t l = 0; l < header_count; l++) {
		markup_text((int)l, skin.margin, cols - 2 * skin.margin, header_lines[l], 27, !l);
	}
}
