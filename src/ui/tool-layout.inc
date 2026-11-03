/* Display-only tool-card geometry and headings. Called by both layout passes;
   no transcript mutation, file reads, tool execution or extension callbacks. */
static void md_tool_rule(struct md_canvas *c, int bottom) {

	int row = c->origin_y + c->y;
	if (!c->pad || row < 0 || row >= getmaxy(c->pad)) {
		return;
	}
	wattrset(c->pad, COLOR_PAIR(50));
	mvwaddwstr(c->pad, row, 0, bottom ? L"╰" : L"╭");
	for (int col = 1; col < c->outer_width - 1; col++) {
		mvwaddwstr(c->pad, row, col, L"─");
	}
	mvwaddwstr(c->pad, row, c->outer_width - 1, bottom ? L"╯" : L"╮");
}

/* tool.padding_y: blank rows of the card's band. */
static void md_tool_pad(struct md_canvas *c) {

	for (int i = 0; i < skin.tool_padding_y; i++) {
		md_row(c, 0);
		c->y++;
	}
}

static void md_tool_close(struct md_canvas *c, int full_width) {

	if (!c->card) {
		c->footer[0] = 0;
		return;
	}
	if (c->x) {
		c->y++;
		c->x = 0;
	}
	if (c->footer[0]) {
		/* After a blank row, as pi shows it. */
		md_row(c, 0);
		c->y++;
		md_text(c, c->footer, strlen(c->footer), COLOR_PAIR(49), 0);
		c->y++;
		c->x = 0;
		c->footer[0] = 0;
	}
	md_tool_pad(c);
	if (c->frame == TOOL_BORDER_ROUNDED) {
		md_tool_rule(c, 1);
		c->y++;
	}
	c->origin_x = c->card = c->frame = c->indent = c->rule = c->tab_origin = 0;
	c->width = full_width;
}

/* After a card: close it, then tool.spacing blank rows. */
static void md_card_end(struct md_canvas *c, int full_width) {

	md_tool_close(c, full_width);
	if (c->x) {
		c->y++;
		c->x = 0;
	}
	c->y += skin.tool_spacing;
}

static void md_tool_open(struct md_canvas *c, int full_width) {

	md_tool_close(c, full_width);
	if (c->x) {
		c->y++;
		c->x = 0;
	}
	int outer =
		skin.tool_max_width && skin.tool_max_width < full_width ? skin.tool_max_width : full_width;
	int frame = outer >= 6 ? skin.tool_border : TOOL_BORDER_NONE;
	int left = frame == TOOL_BORDER_LEFT ? 2 : frame == TOOL_BORDER_ROUNDED ? 1 : 0;
	int right = frame == TOOL_BORDER_ROUNDED ? 1 : 0;
	int padding = skin.tool_padding, room = outer - left - right;
	if (2 * padding + 2 > room) {
		padding = room > 2 ? (room - 2) / 2 : 0;
	}
	c->card = 1;
	c->frame = frame;
	c->outer_width = outer;
	c->origin_x = left + padding;
	c->width = room - 2 * padding;
	c->indent = c->rule = c->tab_origin = c->word_wrap = 0;
	if (frame == TOOL_BORDER_ROUNDED) {
		md_tool_rule(c, 0);
		c->y++;
	}
	md_tool_pad(c);
}

/* Clip a caption by terminal cells, never by UTF-8 bytes. */
static void md_tool_detail(struct md_canvas *c, const char *name, const char *text) {

	size_t n = strlen(text), bytes = 0;
	/* tool.detail "title": the detail in the heading's colour, as pi. */
	attr_t attr = skin.tool_detail == TOOL_DETAIL_TITLE
		? COLOR_PAIR(9) | (skin.tool_heading_bold ? A_BOLD : 0)
		: COLOR_PAIR(49);
	int reserve =
		skin_duration_for(&skin, name) == TOOL_DURATION_HEADING || skin.tool_change_counts ? 9 : 0;
	int room = c->width >= 40 ? c->width - c->x - reserve : c->width;
	if (c->width < 40) {
		md_text(c, text, n, attr, 8);
		return;
	}
	if (skin.tool_heading_wrap) { /* wraps at spaces, as pi */
		c->word_wrap = 1;
		md_text(c, text, n, attr, 8);
		c->word_wrap = 0;
		return;
	}
	int cells = 0;
	mbstate_t state = {0};
	while (bytes < n && room > 0) {
		wchar_t wc;
		size_t step = md_decode(&wc, text + bytes, n - bytes, &state);
		if (!step || step == (size_t)-1 || step == (size_t)-2) {
			break;
		}
		int span = wcwidth(wc);
		if (span < 0) {
			span = 0;
		}
		if (cells + span > room - (bytes + step < n ? 1 : 0)) {
			break;
		}
		cells += span;
		bytes += step;
	}
	md_text(c, text, bytes, attr, 8);
	if (bytes < n && room > 0) {
		md_char(c, L'…', COLOR_PAIR(49), 8);
	}
}

static const char *md_tool_path(const char *name, const char *detail, const char *cwd) {

	/* Commands, URLs and extension-specific arguments are not file paths. */
	if (strcmp(name, "read") && strcmp(name, "write") && strcmp(name, "edit")) {
		return detail;
	}
	if (skin.tool_path == TOOL_PATH_BASENAME) {
		const char *slash = strrchr(detail, '/');
		return slash && slash[1] ? slash + 1 : detail;
	}
	if (skin.tool_path == TOOL_PATH_RELATIVE && *cwd) {
		size_t n = strlen(cwd);
		if (!strcmp(cwd, "/") && detail[0] == '/' && detail[1]) {
			return detail + 1;
		}
		if (!strncmp(detail, cwd, n) && detail[n] == '/' && detail[n + 1]) {
			return detail + n + 1;
		}
	}

	return detail;
}

/* Style 71 holds tab-delimited, sanitized name/label/detail/override fields.
   The fallback heading following it remains available for custom tool_render
   titles; ordinary headings are rendered afresh from the current skin. */
static int md_tool_heading(
	struct md_canvas *c, const char *metadata, size_t n, int glyph_style, const char *cwd) {

	char buffer[1024];
	if (n >= sizeof(buffer)) {
		return 0;
	}
	memcpy(buffer, metadata, n);
	buffer[n] = 0;
	char *fields[4] = {buffer};
	for (int i = 1; i < 4; i++) {
		char *tab = strchr(fields[i - 1], '\t');
		if (!tab) {
			return 0;
		}
		*tab = 0;
		fields[i] = tab + 1;
	}
	if (strcmp(fields[3], "0")) {
		return 0; /* an extension owns its custom heading */
	}
	const char *name = fields[0], *label = fields[1];
	const char *detail = md_tool_path(name, fields[2], cwd);
	c->indent = c->rule = c->word_wrap = 0;
	md_row(c, 8);
	if (strcmp(name, "edit")) {
		md_text(c, GLYPH_CARD, 3,
			COLOR_PAIR(glyph_style == 43 ? 51
					: glyph_style == 44  ? 7
										 : 49),
			8);
	}
	attr_t title = COLOR_PAIR(9) | (skin.tool_heading_bold ? A_BOLD : 0);
	const char *template = skin_title_for(&skin, name);
	if (!*template) {
		md_text(c, label, strlen(label), title, 8);
		if (*detail) {
			md_char(c, L' ', COLOR_PAIR(49), 8);
			md_tool_detail(c, name, detail);
		}
		return 1;
	}
	const char *p = template;
	while (*p) {
		const char *end = *p == '{' ? strchr(p, '}') : NULL;
		size_t size = end ? (size_t)(end - p + 1) : 0;
		const char *field = NULL;
		int capital = 0, caption = 0;
		if (size == 6 && !strncmp(p, "{name}", 6)) {
			field = name;
		} else if (size == 6 && !strncmp(p, "{Name}", 6)) {
			field = name;
			capital = 1;
		} else if (size == 7 && !strncmp(p, "{label}", 7)) {
			field = label;
		} else if (size == 8 && !strncmp(p, "{detail}", 8)) {
			field = detail;
			caption = 1;
		}
		if (!field) {
			/* Draw a complete literal run so multi-byte separators stay intact. */
			const char *next = strchr(p + 1, '{');
			size_t len = next ? (size_t)(next - p) : strlen(p);
			md_text(c, p, len, skin.tool_detail == TOOL_DETAIL_TITLE ? title : COLOR_PAIR(49), 8);
			p += len;
			continue;
		}
		if (caption) {
			md_tool_detail(c, name, field);
		} else if (capital && *field) {
			md_char(c, (wchar_t)toupper((unsigned char)*field), title, 8);
			md_text(c, field + 1, strlen(field + 1), title, 8);
		} else {
			md_text(c, field, strlen(field), title, 8);
		}
		p += size;
	}

	return 1;
}

static int md_diff_pair(int style) {

	int removed = style == 8 || style == 11 || style == 15 || style == 67 || style == 75;
	int added = style == 9 || style == 12 || style == 16 || style == 68 || style == 76;
	if (!removed && !added) {
		return 13;
	}
	if (skin.diff_style == DIFF_STYLE_BACKGROUND) {
		return removed ? 11 : 12;
	}
	if (style == 67 || style == 68) {
		return 13;
	}
	if (skin.diff_style == DIFF_STYLE_MARKERS && style != 75 && style != 76) {
		return 8;
	}

	return removed ? 31 : 48;
}
