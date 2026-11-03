/* Declarative chrome: content, style and placement are independent of meaning.
   The host draws protected input/approval controls afterwards. No element IDs
   have built-in behavior; omitting an element means it does not exist. */
static void chrome_draw(const struct chrome_elements *elements, int left, int top, int width,
	int height, int normal_pair, int muted_pair, int accent_pair,
	void (*expand)(const char *, char *, size_t, void *), void *context) {

	if (width < 1 || height < 1) {
		return;
	}
	int right = left + width;
	if (right > COLS - 1) {
		right = COLS - 1;
	}
	for (int i = 0; i < elements->count; i++) {
		const struct chrome_element *e = &elements->items[i];
		char expanded[4096], text[4352];
		expand(e->text, expanded, sizeof(expanded), context);
		snprintf(
			text, sizeof(text), "%s%s%s", e->glyph, *e->glyph && *expanded ? " " : "", expanded);
		wchar_t chars[2048];
		int widths[2048], count = 0, cells = 0;
		mbstate_t state = {0};
		const char *p = text;
		size_t bytes = strlen(text);
		while (bytes && count < 2048) {
			wchar_t wc;
			size_t n = mbrtowc(&wc, p, bytes, &state);
			if (n == (size_t)-1 || n == (size_t)-2) {
				n = 1;
				wc = L'?';
				memset(&state, 0, sizeof(state));
			}
			if (!n) {
				break;
			}
			p += n;
			bytes -= n;
			int w = wcwidth(wc);
			if (wc < 32 || wc == 127) {
				wc = L' ';
				w = 1;
			} else if (w < 0 || (!count && !w)) {
				wc = L'?';
				w = 1;
			}
			if (e->width && cells + w > e->width) {
				break;
			}
			chars[count] = wc;
			widths[count++] = w;
			cells += w;
		}
		int x = left, y = top;
		if (e->anchor == 1 || e->anchor == 3) {
			x = right - cells;
		}
		if (e->anchor == 2 || e->anchor == 3) {
			y = top + height - 1;
		}
		if (e->anchor == 4) {
			x = left + (width - cells) / 2;
			y = top + height / 2;
		}
		x += e->x;
		y += e->y;
		if (y < top || y >= top + height || y < 0 || y >= LINES) {
			continue;
		}
		int pair = e->style == 1 ? muted_pair : e->style == 2 ? accent_pair : normal_pair;
		attrset(COLOR_PAIR(pair) | (e->style == 3 ? A_BOLD : 0));
		for (int c = 0; c < count; c++) {
			int w = widths[c];
			if (x >= left && x + w <= right && x >= 0) {
				wchar_t glyph[2] = {chars[c], 0};
				mvaddwstr(y, x, glyph);
			}
			x += w;
			if (x >= right) {
				break;
			}
		}
	}
	attrset(A_NORMAL);
}
