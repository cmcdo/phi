#define DIFF_MAX_LINES 200
#define DIFF_MAX_BYTES 32768
#define DIFF_MAX_LINE_BYTES 2048
#define DIFF_CONTEXT_LINES 2
#define DIFF_MAX_HUNKS 256

struct replacement {
	size_t start;
	size_t length;
	const char *text;
	size_t size;
};

/* Bounded line-wise replacement previews, derived from actual before/after
   buffers. No full-file diff matrix or extra file copies. */
typedef void (*diff_emit_fn)(char kind, size_t line, const char *text, size_t size,
	size_t highlight_start, size_t highlight_end, void *context);

static size_t diff_line_end(const char *text, size_t size, size_t at) {

	while (at < size && text[at] != '\n') {
		at++;
	}

	return at < size ? at + 1 : at;
}

struct diff_hunk {
	size_t start, end;
	size_t new_start, new_end;
	size_t old_line, new_line;
};

struct diff_budget {
	size_t lines, bytes;
};

static int diff_emit_range(const char *text, size_t start, size_t end, size_t line, char kind,
	struct diff_budget *budget, diff_emit_fn emit, void *context) {

	while (start < end) {
		size_t next = diff_line_end(text, end, start), size = next - start;
		if (budget->lines >= DIFF_MAX_LINES || budget->bytes + size > DIFF_MAX_BYTES ||
			size > DIFF_MAX_LINE_BYTES) {
			return 1;
		}
		emit(kind, line++, text + start, size, 0, 0, context);
		budget->lines++;
		budget->bytes += size;
		start = next;
	}

	return 0;
}

static int diff_emit_pair(const char *old, size_t old_size, size_t old_line, const char *new,
	size_t new_size, size_t new_line, struct diff_budget *budget, diff_emit_fn emit,
	void *context) {

	if (budget->lines + 2 > DIFF_MAX_LINES ||
		budget->bytes + old_size + new_size > DIFF_MAX_BYTES || old_size > DIFF_MAX_LINE_BYTES ||
		new_size > DIFF_MAX_LINE_BYTES) {
		return 1;
	}
	size_t first = 0, a = old_size, b = new_size;
	while (first < a && first < b && old[first] == new[first]) {
		first++;
	}
	while (first &&
		((first < old_size && ((unsigned char)old[first] & 0xc0) == 0x80) ||
			(first < new_size && ((unsigned char)new[first] & 0xc0) == 0x80))) {
		first--;
	}
	while (a > first && b > first && old[a - 1] == new[b - 1]) {
		a--;
		b--;
	}
	while (a < old_size && b < new_size && ((unsigned char)old[a] & 0xc0) == 0x80) {
		a++;
		b++;
	}
	emit('-', old_line, old, old_size, first, a, context);
	emit('+', new_line, new, new_size, first, b, context);
	budget->lines += 2;
	budget->bytes += old_size + new_size;

	return 0;
}

static int edit_diff(const char *before, size_t before_size, const char *after, size_t after_size,
	const struct replacement *changes, size_t count, diff_emit_fn emit, void *context) {

	size_t removed = 0, added = 0, old_scan = 0, new_scan = 0;
	size_t old_line = 1, new_line = 1;
	struct diff_hunk hunks[DIFF_MAX_HUNKS];
	size_t hunk_count = 0;
	if (count > DIFF_MAX_HUNKS) {
		return 1;
	}
	for (size_t i = 0; i < count;) {
		size_t start = changes[i].start;
		while (start && before[start - 1] != '\n') {
			start--;
		}
		size_t end = diff_line_end(before, before_size, changes[i].start + changes[i].length - 1);
		size_t new_start = start - removed + added;
		size_t j = i, new_end;
		for (;;) {
			while (j < count && changes[j].start < end) {
				removed += changes[j].length;
				added += changes[j].size;
				size_t last =
					diff_line_end(before, before_size, changes[j].start + changes[j].length - 1);
				if (last > end) {
					end = last;
				}
				j++;
			}
			new_end = end - removed + added;
			if (new_end > after_size) {
				return 1;
			}
			/* Removing a newline joins the following line into this hunk. */
			if (end < before_size && new_end > new_start && after[new_end - 1] != '\n') {
				end = diff_line_end(before, before_size, end);
				continue;
			}
			break;
		}
		while (old_scan < start) {
			if (before[old_scan++] == '\n') {
				old_line++;
			}
		}
		while (new_scan < new_start) {
			if (after[new_scan++] == '\n') {
				new_line++;
			}
		}
		/* Trim equal whole lines from the replacement's supplied context. */
		while (start < end && new_start < new_end) {
			size_t old_next = diff_line_end(before, end, start);
			size_t new_next = diff_line_end(after, new_end, new_start);
			if (old_next - start != new_next - new_start ||
				memcmp(before + start, after + new_start, old_next - start)) {
				break;
			}
			for (size_t p = start; p < old_next; p++) {
				if (before[p] == '\n') {
					old_line++;
				}
			}
			for (size_t p = new_start; p < new_next; p++) {
				if (after[p] == '\n') {
					new_line++;
				}
			}
			start = old_next;
			new_start = new_next;
		}
		size_t display_end = end, display_new_end = new_end;
		while (display_end > start && display_new_end > new_start) {
			size_t old_start = display_end - 1;
			size_t new_line_start = display_new_end - 1;
			while (old_start > start && before[old_start - 1] != '\n') {
				old_start--;
			}
			while (new_line_start > new_start && after[new_line_start - 1] != '\n') {
				new_line_start--;
			}
			if (display_end - old_start != display_new_end - new_line_start ||
				memcmp(before + old_start, after + new_line_start, display_end - old_start)) {
				break;
			}
			display_end = old_start;
			display_new_end = new_line_start;
		}
		if (start < display_end || new_start < display_new_end) {
			hunks[hunk_count++] = (struct diff_hunk){
				start, display_end, new_start, display_new_end, old_line, new_line};
		}
		old_scan = start;
		new_scan = new_start;
		while (old_scan < end) {
			if (before[old_scan++] == '\n') {
				old_line++;
			}
		}
		while (new_scan < new_end) {
			if (after[new_scan++] == '\n') {
				new_line++;
			}
		}
		i = j;
	}
	struct diff_budget budget = {0};
	size_t shown_until = 0;
	for (size_t i = 0; i < hunk_count; i++) {
		struct diff_hunk h = hunks[i];
		size_t context_start = h.new_start, context_line = h.new_line;
		for (int n = 0; n < DIFF_CONTEXT_LINES && context_start > shown_until; n++) {
			context_start--;
			while (context_start > shown_until && after[context_start - 1] != '\n') {
				context_start--;
			}
			if (context_line > 1) {
				context_line--;
			}
		}
		if (i && context_start > shown_until &&
			diff_emit_range("...\n", 0, 4, 0, '@', &budget, emit, context)) {
			return 1;
		}
		if (diff_emit_range(
				after, context_start, h.new_start, context_line, ' ', &budget, emit, context)) {
			return 1;
		}
		if (h.start < h.end && h.new_start < h.new_end &&
			diff_line_end(before, h.end, h.start) == h.end &&
			diff_line_end(after, h.new_end, h.new_start) == h.new_end) {
			if (diff_emit_pair(before + h.start, h.end - h.start, h.old_line, after + h.new_start,
					h.new_end - h.new_start, h.new_line, &budget, emit, context)) {
				return 1;
			}
		} else if (diff_emit_range(
					   before, h.start, h.end, h.old_line, '-', &budget, emit, context) ||
			diff_emit_range(
				after, h.new_start, h.new_end, h.new_line, '+', &budget, emit, context)) {
			return 1;
		}
		size_t context_end = h.new_end;
		size_t boundary = i + 1 < hunk_count ? hunks[i + 1].new_start : after_size;
		for (int n = 0; n < DIFF_CONTEXT_LINES && context_end < boundary; n++) {
			context_end = diff_line_end(after, boundary, context_end);
		}
		size_t trailing_line = h.new_line;
		for (size_t p = h.new_start; p < h.new_end; p++) {
			if (after[p] == '\n') {
				trailing_line++;
			}
		}
		if (diff_emit_range(
				after, h.new_end, context_end, trailing_line, ' ', &budget, emit, context)) {
			return 1;
		}
		shown_until = context_end;
	}

	return 0;
}
