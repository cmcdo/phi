/* GFM-style pipe tables, bounded columns; cells point into the transcript. */
#define TABLE_COLS 16

struct md_cell {
	const char *text;
	size_t length;
};

static void table_trim(struct md_cell *cell) {

	while (cell->length && md_space(cell->text[0])) {
		cell->text++;
		cell->length--;
	}
	while (cell->length && md_space(cell->text[cell->length - 1])) {
		cell->length--;
	}
}

static int table_row(size_t at, struct md_cell *cells, size_t *after, int *complete) {

	size_t end = at;
	while (end < used && transcript_style[end] == 3 && transcript[end] != '\n') {
		end++;
	}
	*complete = end < used && transcript_style[end] == 3 && transcript[end] == '\n';
	*after = end + (*complete ? 1 : 0);
	size_t first = at;
	while (first < end && md_space(transcript[first])) {
		first++;
	}
	while (end > first && md_space(transcript[end - 1])) {
		end--;
	}
	if (first == end) {
		return 0;
	}
	int pipes = 0, count = 0;
	if (transcript[first] == '|') {
		first++;
		pipes++;
	}
	size_t start = first, ticks = 0;
	for (size_t i = first; i < end; i++) {
		if (transcript[i] == '\\' && i + 1 < end) {
			i++;
			continue;
		}
		if (transcript[i] == '`') {
			size_t run = 1;
			while (i + run < end && transcript[i + run] == '`') {
				run++;
			}
			if (ticks == run) {
				ticks = 0;
			} else if (!ticks) {
				ticks = run;
			}
			i += run - 1;
			continue;
		}
		if (transcript[i] != '|' || ticks) {
			continue;
		}
		if (count == TABLE_COLS) {
			return 0;
		}
		cells[count] = (struct md_cell){transcript + start, i - start};
		table_trim(&cells[count++]);
		pipes++;
		start = i + 1;
	}
	if (start < end) {
		if (count == TABLE_COLS) {
			return 0;
		}
		cells[count] = (struct md_cell){transcript + start, end - start};
		table_trim(&cells[count++]);
	}

	return pipes ? count : 0;
}

/* 0 = left, 1 = right, 2 = centre. */
static int table_separator(struct md_cell cell, int *alignment) {

	size_t start = 0, end = cell.length;
	int left = start < end && cell.text[start] == ':';
	if (left) {
		start++;
	}
	int right = start < end && cell.text[end - 1] == ':';
	if (right) {
		end--;
	}
	if (end - start < 3) {
		return 0;
	}
	for (size_t i = start; i < end; i++) {
		if (cell.text[i] != '-') {
			return 0;
		}
	}
	*alignment = right ? (left ? 2 : 1) : 0;

	return 1;
}

static int table_cell_width(struct md_cell cell) {

	struct md_canvas measure = {.width = LOG_SIZE * 4};
	md_inline(&measure, cell.text, cell.length, A_NORMAL, 0);

	return measure.x;
}

static void table_paint_row(struct md_canvas *c, struct md_cell *cells, int count,
	const int *widths, const int *alignment, int header) {

	int height = 1, x = c->indent; /* tables hang with the reply */
	for (int i = 0; i < count; i++) {
		struct md_canvas cell = {.pad = c->pad,
			.width = widths[i],
			.origin_y = c->origin_y + c->y,
			.origin_x = c->origin_x + x,
			.word_wrap = 1};
		int natural = table_cell_width(cells[i]);
		if (natural < widths[i]) {
			int shift = alignment[i] == 1 ? widths[i] - natural
				: alignment[i] == 2       ? (widths[i] - natural) / 2
										  : 0;
			cell.origin_x += shift;
			cell.width -= shift;
		}
		md_inline(&cell, cells[i].text, cells[i].length, header ? A_BOLD : A_NORMAL, 0);
		if (cell.y + 1 > height) {
			height = cell.y + 1;
		}
		x += widths[i] + 3;
	}
	if (c->pad) {
		x = c->indent;
		for (int i = 0; i + 1 < count; i++) {
			x += widths[i];
			wattrset(c->pad, COLOR_PAIR(50));
			for (int y = 0; y < height; y++) {
				mvwaddwstr(c->pad, c->origin_y + c->y + y, c->origin_x + x + 1, L"│");
			}
			x += 3;
		}
	}
	c->y += height;
	c->x = 0;
}

static void table_paint_rule(struct md_canvas *c, int count, const int *widths) {

	for (int i = 0; i < count; i++) {
		for (int j = 0; j < widths[i]; j++) {
			md_char(c, L'─', COLOR_PAIR(50), 0);
		}
		if (i + 1 < count) {
			md_char(c, L'─', COLOR_PAIR(50), 0);
			md_char(c, L'┼', COLOR_PAIR(50), 0);
			md_char(c, L'─', COLOR_PAIR(50), 0);
		}
	}
	c->y++;
	c->x = 0;
}

static int md_table(struct md_canvas *c, size_t at, size_t *after) {

	struct md_cell header[TABLE_COLS], cells[TABLE_COLS];
	int complete, count = table_row(at, header, after, &complete);
	if (!count || !complete || c->width - c->indent < count * 3 + (count - 1) * 3) {
		return 0;
	}
	size_t body;
	int delimiter_count = table_row(*after, cells, &body, &complete);
	if (delimiter_count != count || !complete) {
		return 0;
	}
	int widths[TABLE_COLS], alignment[TABLE_COLS];
	for (int i = 0; i < count; i++) {
		if (!table_separator(cells[i], &alignment[i])) {
			return 0;
		}
		widths[i] = table_cell_width(header[i]);
		if (widths[i] < 3) {
			widths[i] = 3;
		}
	}
	size_t scan = body, next;
	while (scan < used && table_row(scan, cells, &next, &complete) == count) {
		for (int i = 0; i < count; i++) {
			int width = table_cell_width(cells[i]);
			if (width > widths[i]) {
				widths[i] = width;
			}
		}
		scan = next;
		if (!complete) {
			break;
		}
	}
	int total = (count - 1) * 3;
	for (int i = 0; i < count; i++) {
		total += widths[i];
	}
	while (total > c->width - c->indent) {
		int largest = 0;
		for (int i = 1; i < count; i++) {
			if (widths[i] > widths[largest]) {
				largest = i;
			}
		}
		widths[largest]--;
		total--;
	}
	table_paint_row(c, header, count, widths, alignment, 1);
	if (skin.table_rules != TABLE_RULES_NONE) {
		table_paint_rule(c, count, widths);
	}
	int first_row = 1;
	while (body < scan) {
		int cols = table_row(body, cells, &next, &complete);
		if (cols != count) {
			break;
		}
		if (!first_row && skin.table_rules == TABLE_RULES_ROWS) {
			table_paint_rule(c, count, widths);
		}
		table_paint_row(c, cells, count, widths, alignment, 0);
		first_row = 0;
		body = next;
	}
	*after = body;

	return 1;
}
