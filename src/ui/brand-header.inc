/* Shared theme masthead: text/art only, no providers or editor state. */
static int brand_header_rows(const struct skin *s) {

	int text = (*s->welcome_title || s->welcome_line_count) ? 1 + s->welcome_line_count : 0;

	return text > s->art_count ? text : s->art_count;
}

static void brand_compact_draw(const struct skin *s, int top, int left, int width, int accent,
	int muted, void (*expand)(const char *, char *, size_t, void *),
	void (*paint)(int, int, int, const char *, int, int, void *), void *context) {

	if (width < 1) {
		return;
	}
	char lhs[1024], rhs[1024];
	expand(s->header_left, lhs, sizeof(lhs), context);
	expand(s->header_right, rhs, sizeof(rhs), context);
	int right = skin_width(rhs);
	if (right < 0) {
		right = 0;
	}
	int show = right > 0 && right + 10 <= width;
	paint(top, left, show ? width - right - 2 : width, lhs, accent, 1, context);
	if (show) {
		paint(top, left + width - right, right, rhs, muted, 0, context);
	}
}

static void brand_header_draw(const struct skin *s, int top, int left, int width, int height,
	int accent, int text_pair, int muted, const char *(*art)(int, void *),
	void (*expand)(const char *, char *, size_t, void *),
	void (*paint)(int, int, int, const char *, int, int, void *), void *context) {

	if (height <= 0 || width <= 0) {
		return;
	}
	for (int i = 0; i < s->art_count && i < height; i++) {
		paint(top + i, left, width, art(i, context), accent, 0, context);
	}
	int x = left;
	if (s->art_count && s->art_width + 12 < left + width + 2) {
		x += s->art_width + 2;
	}
	int available = left + width - x;
	char line[4608];
	expand(s->welcome_title, line, 512, context);
	paint(top, x, available, line, accent, 1, context);
	for (int i = 0; i < s->welcome_line_count && i + 1 < height; i++) {
		expand(s->welcome_lines[i], line, sizeof(line), context);
		paint(top + 1 + i, x, available, line, i ? muted : text_pair, 0, context);
	}
}
