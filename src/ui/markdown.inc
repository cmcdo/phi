/* Small display-only Markdown subset. No AST, dependencies, or transcript copy. */
#include <ctype.h>

struct md_canvas {
	WINDOW *pad;
	int width, y, x, origin_y, origin_x, indent, rule, word_wrap, tab_origin;
	int card, frame, outer_width;
	/* Optional visible-text projection for wrapping across inline styles. */
	wchar_t *capture;
	size_t capture_length, capture_capacity;
	const wchar_t *wrap_text;
	size_t wrap_at, wrap_length;
	char footer[32]; /* tool.duration "footer": the card's last row */
};

/* needle in text[0..n), or NULL. */
static const char *md_find(const char *text, size_t n, const char *needle) {

	size_t size = strlen(needle);
	for (size_t i = 0; size <= n && i <= n - size; i++) {
		if (!memcmp(text + i, needle, size)) {
			return text + i;
		}
	}

	return NULL;
}

static void md_row(struct md_canvas *c, int background) {

	int row = c->origin_y + c->y;
	/* Viewport pads still parse earlier lines for layout, but painting them
       would make thousands of failed ncurses calls on a long transcript. */
	if (!c->pad || row < 0 || row >= getmaxy(c->pad)) {
		return;
	}
	if (c->card) {
		wattrset(c->pad, COLOR_PAIR(8));
		mvwhline(c->pad, row, 0, ' ', c->outer_width);
		if (c->frame != TOOL_BORDER_NONE) {
			wattrset(c->pad, COLOR_PAIR(50));
			mvwaddwstr(c->pad, row, 0, L"│");
			if (c->frame == TOOL_BORDER_ROUNDED) {
				mvwaddwstr(c->pad, row, c->outer_width - 1, L"│");
			}
		}
	}
	if (background) {
		wattrset(c->pad, COLOR_PAIR(background));
		int start = c->rule ? c->indent - 1 : 0;
		mvwhline(c->pad, row, c->origin_x + start, ' ', c->width - start);
	}
	if (c->rule) {
		wattrset(c->pad, COLOR_PAIR(c->rule == 2 ? 53 : 50));
		mvwaddwstr(c->pad, row, c->origin_x + c->indent - 2, L"│");
	}
}

static void md_char(struct md_canvas *c, wchar_t wc, attr_t attr, int background) {

	if (wc >= GLYPH_FIRST && wc <= GLYPH_LAST) {
		const char *glyphs[] = {
			skin.card, skin.reply, skin.output, skin.fold, skin.prompt, skin.output};
		const char *glyph = glyphs[wc - GLYPH_FIRST];
		if (!*glyph) {
			return;
		}
		wchar_t wide[16];
		size_t count = mbstowcs(wide, glyph, 15);
		if (count == (size_t)-1) {
			return;
		}
		if (wc == GLYPH_LAST) { /* spaces as wide as the output glyph */
			int cells = skin_width(glyph) + 1;
			while (cells-- > 0) {
				md_char(c, L' ', attr, background);
			}
			return;
		}
		for (size_t i = 0; i < count; i++) {
			md_char(c, wide[i], attr, background);
		}
		md_char(c, L' ', attr, background);
		return;
	}
	if (wc == L'\t') {
		int count = 4 - (c->x - c->tab_origin) % 4;
		while (count--) {
			md_char(c, L' ', attr, background);
		}
		return;
	}
	int cells = wc >= L' ' && wc <= L'~' ? 1 : wcwidth(wc);
	if (cells < 0) {
		return;
	}
	if (c->capture && c->capture_length < c->capture_capacity) {
		c->capture[c->capture_length++] = wc;
	}
	if (c->wrap_text && c->wrap_at < c->wrap_length) {
		c->wrap_at++;
	}
	if (cells > 0 && (c->x + cells > c->width || c->x == c->width)) {
		c->y++;
		c->x = 0;
	}
	if (!c->x) {
		md_row(c, background);
		c->x = c->indent;
	}
	int row = c->origin_y + c->y;
	if (c->pad && row >= 0 && row < getmaxy(c->pad)) {
		wchar_t text[2] = {wc, 0};
		wattrset(c->pad, attr);
		if (!cells && c->x > c->indent) {
			ui_cell_mark(c->pad, row, c->origin_x + c->x, wc);
		} else {
			mvwaddwstr(c->pad, row, c->origin_x + c->x, text);
		}
	}
	c->x += cells;
}

/* Most transcript bytes are ASCII. Avoid the locale converter for them only
   when its state is initial; non-ASCII and stateful encodings use mbrtowc. */
static size_t md_decode(wchar_t *wc, const char *text, size_t n, mbstate_t *state) {

	if (n && (unsigned char)*text < 128 && *text && mbsinit(state)) {
		*wc = (unsigned char)*text;
		return 1;
	}

	return mbrtowc(wc, text, n, state);
}

static void md_text(struct md_canvas *c, const char *text, size_t n, attr_t attr, int background) {

	mbstate_t state = {0};
	for (size_t at = 0; at < n;) {
		wchar_t wc;
		size_t bytes = md_decode(&wc, text + at, n - at, &state);
		if (bytes == (size_t)-2) {
			break; /* Incomplete streaming UTF-8. */
		}
		if (bytes == (size_t)-1 || !bytes) {
			wc = L'?';
			bytes = 1;
			memset(&state, 0, sizeof(state));
		}
		if (c->word_wrap) {
			if (iswspace(wc) && c->x >= c->width) {
				if (c->wrap_text && c->wrap_at < c->wrap_length) {
					c->wrap_at++;
				}
				at += bytes;
				continue;
			}
			int start = c->wrap_text ? (!c->wrap_at || iswspace(c->wrap_text[c->wrap_at - 1]))
									 : (!at || isspace((unsigned char)text[at - 1]));
			if (!iswspace(wc) && start) {
				int cells = 0;
				mbstate_t probe = {0};
				if (c->wrap_text) {
					for (size_t next = c->wrap_at;
						next < c->wrap_length && !iswspace(c->wrap_text[next]); next++) {
						int span = wcwidth(c->wrap_text[next]);
						if (span > 0) {
							cells += span;
						}
						if (cells > c->width - c->indent) {
							break;
						}
					}
				} else {
					for (size_t next = at; next < n;) {
						wchar_t ch;
						size_t step = md_decode(&ch, text + next, n - next, &probe);
						if (!step || step == (size_t)-1 || step == (size_t)-2 || iswspace(ch)) {
							break;
						}
						int span = ch >= L' ' && ch <= L'~' ? 1 : wcwidth(ch);
						if (span > 0) {
							cells += span;
						}
						next += step;
						if (cells > c->width - c->indent) {
							break;
						}
					}
				}
				if (cells <= c->width - c->indent && c->x > c->indent && c->x + cells > c->width) {
					c->y++;
					c->x = 0;
				}
			}
		}
		md_char(c, wc, attr, background);
		at += bytes;
	}
}

#include "tool-layout.inc"

static int md_word(unsigned char c) {

	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		c >= 128;
}

static int md_space(char c) {

	return c == ' ' || c == '\t' || c == '\r';
}

static void md_inline(struct md_canvas *c, const char *text, size_t n, attr_t attr, int depth);

static void md_inline_body(
	struct md_canvas *c, const char *text, size_t n, attr_t attr, int depth) {

	if (depth > 8) {
		md_text(c, text, n, attr, 0);
		return;
	}
	size_t plain = 0, at = 0, budget = n * 4;
	while (at < n) {
		char mark = text[at];
		/* Maths stays as written: \( \) \[ \] and $...$ spans are not Markdown. */
		if ((mark == '\\' && at + 1 < n && (text[at + 1] == '(' || text[at + 1] == '[')) ||
			(mark == '$' && at + 1 < n && text[at + 1] != ' ')) {
			size_t open = mark == '$' && text[at + 1] == '$' ? 2 : mark == '$' ? 1 : 2;
			const char *close = mark == '$' ? (open == 2 ? "$$" : "$")
				: text[at + 1] == '('       ? "\\)"
											: "\\]";
			const char *found =
				at + open < n ? md_find(text + at + open, n - at - open, close) : NULL;
			if (found && found > text + at + open) {
				size_t stop = (size_t)(found - text) + strlen(close);
				md_text(c, text + plain, at - plain, attr, 0);
				md_text(c, text + at, stop - at, attr, 0);
				at = stop;
				plain = at;
				continue;
			}
		}
		if (mark == '\\' && at + 1 < n && strchr("\\`*_{}[]()#+-.!>|", text[at + 1])) {
			md_text(c, text + plain, at - plain, attr, 0);
			md_text(c, text + at + 1, 1, attr, 0);
			at += 2;
			plain = at;
			continue;
		}
		/* Completed inline web links render as labels. Keep incomplete or
           unsupported syntax literal while streaming; never alter history. */
		if (mark == '[' && !(at && text[at - 1] == '!')) {
			size_t close = at + 1;
			while (close < n && text[close] != ']' && budget) {
				close++;
				budget--;
			}
			if (close > at + 1 && close + 2 < n && text[close + 1] == '(') {
				size_t url = close + 2, end = url;
				int nesting = 1;
				int web = (n - url >= 7 && !memcmp(text + url, "http://", 7)) ||
					(n - url >= 8 && !memcmp(text + url, "https://", 8));
				while (web && end < n && nesting && budget) {
					char ch = text[end];
					if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') {
						break;
					}
					if (ch == '(') {
						nesting++;
					}
					if (ch == ')') {
						nesting--;
					}
					end++;
					budget--;
				}
				if (web && !nesting) {
					md_text(c, text + plain, at - plain, attr, 0);
					md_inline(c, text + at + 1, close - at - 1,
						(attr & ~A_COLOR) | COLOR_PAIR(28) | A_UNDERLINE, depth + 1);
					at = plain = end;
					continue;
				}
			}
			if (!budget) {
				md_text(c, text + plain, n - plain, attr, 0);
				return;
			}
		}
		if (mark != '*' && mark != '_' && mark != '`') {
			at++;
			continue;
		}
		size_t run = 1;
		while (at + run < n && text[at + run] == mark) {
			run++;
		}
		if ((mark != '`' && run > 3) || at + run == n ||
			(mark != '`' && md_space(text[at + run])) ||
			(mark == '_' && at && md_word((unsigned char)text[at - 1]))) {
			at += run;
			continue;
		}
		size_t end = at + run;
		while (end < n) {
			if (!budget--) {
				md_text(c, text + plain, n - plain, attr, 0);
				return;
			}
			if (text[end] == '\\' && mark != '`' && end + 1 < n) {
				end += 2;
				continue;
			}
			if (text[end] != mark) {
				end++;
				continue;
			}
			size_t close_run = 1;
			while (end + close_run < n && text[end + close_run] == mark) {
				close_run++;
			}
			if (close_run == run && end > at + run && (mark == '`' || !md_space(text[end - 1])) &&
				!(mark == '_' && end + run < n && md_word((unsigned char)text[end + run]))) {
				break;
			}
			end += close_run;
		}
		if (end == n) {
			at += run;
			continue;
		}
		md_text(c, text + plain, at - plain, attr, 0);
		attr_t style = attr;
		if (mark == '`') {
			md_text(c, text + at + run, end - at - run, COLOR_PAIR(66), 0); /* inline_code */
		} else {
			if (run >= 2) {
				style |= A_BOLD;
			}
#ifdef A_ITALIC
			if (run == 1 || run == 3) {
				style |= A_ITALIC;
			}
#else
			if (run == 1 || run == 3) {
				style |= A_UNDERLINE;
			}
#endif
			md_inline(c, text + at + run, end - at - run, style, depth + 1);
		}
		at = end + run;
		plain = at;
	}
	md_text(c, text + plain, n - plain, attr, 0);
}

/* Measure the rendered word, not each bold/link/code span independently.
   A style boundary inside a word must never introduce a wrapping boundary. */
static void md_inline(struct md_canvas *c, const char *text, size_t n, attr_t attr, int depth) {

	if (!depth && c->word_wrap && !c->wrap_text && !memchr(text, '\t', n) &&
		(memchr(text, '*', n) || memchr(text, '_', n) || memchr(text, '`', n) ||
			memchr(text, '[', n) || memchr(text, '\\', n))) {
		wchar_t *visible = malloc((n + 1) * sizeof(*visible));
		if (visible) {
			struct md_canvas measure = {
				.width = LOG_SIZE * 4, .capture = visible, .capture_capacity = n + 1};
			md_inline_body(&measure, text, n, attr, 0);
			if (measure.capture_length < n + 1) {
				c->wrap_text = visible;
				c->wrap_at = 0;
				c->wrap_length = measure.capture_length;
			}
			md_inline_body(c, text, n, attr, depth);
			c->wrap_text = NULL;
			c->wrap_at = c->wrap_length = 0;
			free(visible);
			return;
		}
	}
	md_inline_body(c, text, n, attr, depth);
}

#include "syntax.inc"
#include "table.inc"

/* Layout twice: one pass counts rows, one paints. Style 3 is assistant prose.
   With first_row > 0 the pad holds only a window of the transcript: row
   first_row lands on pad row 0 and writes outside the pad are ignored by
   ncurses, so a screen-sized pad can show any part of a long transcript. */
/* A tool that is still running (details folded) shows one line instead of
   its output, so nothing appears and then vanishes when it folds:
   "Running ls · last output line". Ctrl+O shows the live card. */
/* A tool call while it is written and while it runs: the finished card's
   heading, then its body under the output glyph, on the card's band. The
   pending card (pending.inc) and the running card both draw with these,
   so a call keeps one shape from its first character until it folds. */

/* The heading text from tool.title, as the finished card shows it. With no
   detail, separators left dangling ("Bash · ", "Bash()") are trimmed. */
static void tool_title_text(const char *name, const char *detail, char *out, size_t size) {

	const char *label = !strcmp(name, "bash_poll") ? "bash output"
		: !strcmp(name, "bash_start")              ? "bash"
												   : name;
	const char *template = skin_title_for(&skin, name);
	if (!*template) {
		snprintf(out, size, "%s%s%s", label, *detail ? " " : "", detail);
		return;
	}
	size_t filled = 0;
	out[0] = 0;
	for (const char *p = template; *p && filled + 1 < size;) {
		const char *field = NULL;
		int capital = 0, skip = 0;
		if (!strncmp(p, "{name}", 6)) {
			field = name;
			skip = 6;
		} else if (!strncmp(p, "{Name}", 6)) {
			field = name;
			capital = 1;
			skip = 6;
		} else if (!strncmp(p, "{label}", 7)) {
			field = label;
			skip = 7;
		} else if (!strncmp(p, "{detail}", 8)) {
			field = detail;
			skip = 8;
		}
		if (!field) {
			out[filled++] = *p++;
			out[filled] = 0;
			continue;
		}
		int n = snprintf(out + filled, size - filled, "%s", field);
		if (capital && n > 0) {
			out[filled] = (char)toupper((unsigned char)out[filled]);
		}
		filled += n > 0 && (size_t)n < size - filled ? (size_t)n : 0;
		p += skip;
	}
	if (*detail) {
		return;
	}
	size_t n = strlen(out);
	if (n >= 2 && strchr(")]", out[n - 1]) && strchr("([", out[n - 2])) {
		out[n -= 2] = 0;
	}
	while (n && strchr(" ·:-|,", out[n - 1])) {
		out[--n] = 0;
	}
	while (n >= 2 && !strncmp(out + n - 2, "\xc2\xb7", 2)) {
		out[n -= 2] = 0;
		while (n && out[n - 1] == ' ') {
			out[--n] = 0;
		}
	}
}

/* Text clipped to the rest of the row, ending in "…" when it is cut. */
static void md_clip(struct md_canvas *c, const char *text, size_t n, attr_t attr) {

	int room = c->width - c->x;
	size_t bytes = 0;
	int cells = 0;
	mbstate_t state = {0};
	while (bytes < n) {
		wchar_t wc;
		size_t step = mbrtowc(&wc, text + bytes, n - bytes, &state);
		if (!step || step == (size_t)-1 || step == (size_t)-2) {
			break;
		}
		int span = wc == L'\t' ? 4 : wcwidth(wc);
		if (span < 0) {
			span = 0;
		}
		if (cells + span > room - (bytes + step < n ? 1 : 0)) {
			break;
		}
		cells += span;
		bytes += step;
	}
	md_text(c, text, bytes, attr, 0);
	if (bytes < n && c->x < c->width) {
		md_char(c, L'…', COLOR_PAIR(49), 0);
	}
}

/* The heading row: the card glyph, the title, then a muted note. */
static void md_live_heading(
	struct md_canvas *c, int full_width, const char *title, const char *note, int note_pair) {

	md_tool_open(c, full_width);
	md_text(c, GLYPH_CARD, strlen(GLYPH_CARD), COLOR_PAIR(49), 0);
	if (!c->x) {
		md_row(c, 0);
		c->x = c->indent;
	}
	int note_cells = note && *note ? skin_width(note) + 1 : 0;
	size_t n = strlen(title);
	int keep = c->width;
	if (note_cells && c->width - c->x > note_cells + 8) {
		c->width -= note_cells;
	}
	md_clip(c, title, n, COLOR_PAIR(9) | (skin.tool_heading_bold ? A_BOLD : 0));
	c->width = keep;
	if (note_cells && c->x + note_cells <= c->width) {
		md_char(c, L' ', COLOR_PAIR(49), 0);
		md_text(c, note, strlen(note), COLOR_PAIR(note_pair), 0);
	}
}

/* Start a body row: the output glyph on the first, its hang after. */
static void md_live_row(struct md_canvas *c, int first) {

	if (c->x) {
		c->y++;
		c->x = 0;
	}
	if (!c->x) {
		md_row(c, 0);
		c->x = c->indent;
	}
	if (*skin.output) {
		md_text(c, first ? GLYPH_OUTPUT : GLYPH_HANG, 3, COLOR_PAIR(49), 0);
	} else {
		md_text(c, "  ", 2, COLOR_PAIR(49), 0);
	}
}

/* Up to rows rows of a bash command after "$ ", wrapped at spaces, with
   shell colours; "…" when more would follow. */
static void md_live_command(struct md_canvas *c, const char *command, int rows) {

	struct shell_state shell = {0};
	size_t at = 0, n = strlen(command);
	for (int row = 0; row < rows && at < n; row++) {
		md_live_row(c, row == 0);
		md_text(c, row ? "  " : "$ ", 2, COLOR_PAIR(49), 0);
		int room = c->width - c->x;
		/* The longest run of whole words that fits, or a hard cut. */
		size_t end = at, fit = at, cells = 0;
		while (end < n && command[end] != '\n') {
			if (((unsigned char)command[end] & 0xc0) != 0x80) {
				if ((int)cells >= room) {
					break;
				}
				cells++;
			}
			end++;
			if (end == n || command[end] == ' ' || command[end] == '\n') {
				fit = end;
			}
		}
		if (end < n && command[end] != '\n' && fit > at) {
			end = fit;
		}
		int last = row == rows - 1 && end < n;
		if (last && end > at + 1) {
			end--; /* room for the "…" */
		}
		md_shell(c, command + at, end - at, 0, &shell);
		if (last) {
			md_char(c, L'…', COLOR_PAIR(49), 0);
		}
		at = end;
		while (at < n && (command[at] == ' ' || command[at] == '\n')) {
			at++;
		}
	}
}

/* While a tool runs: its heading, the command (bash), and its latest output
   line, muted. */
static void md_running_card(struct md_canvas *c) {

	/* The latest non-empty output line, without glyph slots or controls. */
	size_t end = used;
	while (end > tool_body_start && (transcript[end - 1] == '\n' || transcript[end - 1] == '\r')) {
		end--;
	}
	size_t start = end;
	while (start > tool_body_start && transcript[start - 1] != '\n') {
		start--;
	}
	char last[256];
	size_t kept = 0;
	for (size_t i = start; i < end && kept + 4 < sizeof(last); i++) {
		unsigned char byte = (unsigned char)transcript[i];
		if (byte == 0xee && i + 2 < end && (unsigned char)transcript[i + 1] == 0x80 &&
			(unsigned char)transcript[i + 2] >= 0x80 && (unsigned char)transcript[i + 2] <= 0x86) {
			i += 2;
			continue;
		}
		if (byte < 32 && byte != '\t') {
			continue;
		}
		last[kept++] = (char)byte;
	}
	while (kept && ((unsigned char)last[kept - 1] & 0xc0) == 0x80) {
		/* never end inside a UTF-8 character */
		size_t lead = kept - 1;
		while (lead && ((unsigned char)last[lead] & 0xc0) == 0x80) {
			lead--;
		}
		unsigned char first = (unsigned char)last[lead];
		size_t need = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : first >= 0xc0 ? 2 : 1;
		if (kept - lead >= need) {
			break;
		}
		kept = lead;
	}
	last[kept] = 0;
	int full_width = c->width;
	int shell = tool_shell(tool_live_name);
	char title[640], cwd[4096] = "";
	if (skin.tool_path == TOOL_PATH_RELATIVE && !getcwd(cwd, sizeof(cwd))) {
		cwd[0] = 0;
	}
	tool_title_text(tool_live_name, shell ? "" : md_tool_path(tool_live_name, tool_detail, cwd),
		title, sizeof(title));
	md_live_heading(c, full_width, title, "…", 49);
	int body = 0;
	if (shell && *tool_detail) {
		md_live_command(c, tool_detail, 4);
		body = 1;
	}
	if (kept) {
		md_live_row(c, !body);
		md_clip(c, last, kept, COLOR_PAIR(49));
	}
	md_tool_close(c, full_width);
}

/* Output inside a card (not headings, diffs or captions), for tool.max_lines. */
static int md_output_style(int style) {

	return style == 5 || style == 10 || style == 13 || style == 17 || style == 21 ||
		(style >= 25 && style <= 40) || style == 42;
}

/* Output lines of the card whose heading is at start, and whether it is a
   diff (never capped). */
static int md_card_lines(size_t start, int *diff) {

	int lines = 0, last_newline = 1;
	*diff = 0;
	for (size_t j = start; j < used; j++) {
		int style = transcript_style[j];
		if (j > start &&
			(style == 73 || (style == 71 && transcript_style[j - 1] != 71) || style == 1 ||
				style == 3 || style == 19 || style == 22 || style == 78 || style == 80)) {
			break; /* the next block */
		}
		if (style == 8 || style == 9 || style == 11 || style == 12 || style == 15 || style == 16 ||
			(style >= 67 && style <= 69) || (style >= 75 && style <= 77)) {
			*diff = 1;
		}
		if (!md_output_style(style)) {
			continue;
		}
		if (transcript[j] == '\n') {
			lines++;
			last_newline = 1;
		} else {
			last_newline = 0;
		}
	}

	return lines + !last_newline;
}

static int layout_rows(WINDOW *pad, int width, int first_row) {

	int full_width = width;
	int quiet_shell = !archive_layout && working && skin.tool_live && tool_active &&
		!tool_details && tool_shell(tool_live_name);
	size_t saved_used = used;
	/* The activity subline carries collapsed live shell work. Project the
       preceding transcript exactly as it looked before this call, including
       trimming trailing separators; retain all original bytes for Ctrl+O. */
	if (!archive_layout) {
		used = shell_projection_end(used, width);
	}
	int prose_width = reading_width && width > reading_width ? reading_width : width;
	struct md_canvas c = {.pad = pad, .width = prose_width, .origin_y = -first_row};
	size_t at = 0, fence_length = 0;
	char fence = 0;
	int python = 0, c_family = 0, math = 0; /* math: inside \[..\] (1) or $$..$$ (2) */
	char fence_label[40] = ""; /* a code block's language, shown at the end of its first line */
	char card_tool[32] = ""; /* the tool whose card is being laid out */
	int user_open = 0; /* inside your message (for user.padding_y) */
	int heading_gap = 0; /* blank rows still owed after a card's heading */
	int cap_skip = 0, cap_keep = 0, cap_total = 0, out_line = 0, cap_noted = 0; /* tool.max_lines */
	int diff_indent = 0, folded_card = 0;
	char layout_cwd[4096] = "";
	if (skin.tool_path == TOOL_PATH_RELATIVE && !getcwd(layout_cwd, sizeof(layout_cwd))) {
		layout_cwd[0] = 0;
	}
	struct python_state python_state = {0};
	struct c_syntax_state c_state = {0}, tool_c_state = {0};
	struct shell_state shell_state = {0};
	int shell = 0;
	while (at < used) {
		if (!archive_layout && tool_active && at == tool_header_start) {
			struct semantic_frame_cache *live = semantic_live_frame(full_width);
			if (live) {
				md_tool_close(&c, full_width);
				if (c.x) {
					c.y++;
					c.x = 0;
				}
				semantic_frame_paint(pad, first_row, c.y, live);
				c.y += live->rows;
				break;
			}
		}
		if (!archive_layout && tool_active && !tool_details && !tool_edit &&
			at == tool_header_start && tool_body_start >= at) {
			md_tool_close(&c, full_width);
			c.width = full_width;
			md_running_card(&c);
			md_card_end(&c, full_width); /* the gap it will have when it folds */
			break;
		}
		struct semantic_message *message =
			archive_layout ? NULL : semantic_message_frame(at, prose_width);
		if (message) {
			if (user_open && skin.user_padding_y) {
				if (c.x) {
					c.y++;
					c.x = 0;
				}
				for (int row = 0; row < skin.user_padding_y; row++) {
					md_row(&c, 2);
					c.y++;
				}
			}
			md_tool_close(&c, full_width);
			if (c.x) {
				c.y++;
				c.x = 0;
			}
			semantic_frame_paint(pad, first_row, c.y, &message->projection);
			c.y += message->projection.rows;
			c.width = prose_width;
			c.indent = c.rule = c.tab_origin = c.word_wrap = 0;
			at = message->end;
			user_open = 0;
			fence = 0;
			diff_indent = folded_card = heading_gap = 0;
			cap_skip = cap_keep = cap_total = out_line = cap_noted = 0;
			card_tool[0] = 0;
			continue;
		}
		int style = transcript_style[at];
		int thinking = style == 81;
		if (thinking && !skin.thinking_visible) {
			while (at < used && transcript_style[at] == 81) {
				at++;
			}
			continue;
		}
		if (style == 80) {
			while (at < used && transcript_style[at] == 80) {
				at++;
			}
			continue;
		}
		int tool = style >= 4 && style != 19 && style != 22 && !thinking;
		if (!c.card) {
			c.width = tool ? full_width : prose_width;
		}
		size_t end = at;
		while (end < used && transcript[end] != '\n' && transcript_style[end] == style) {
			end++;
		}
		const char *text = transcript + at;
		size_t n = end - at;
		int newline = end < used && transcript[end] == '\n' && transcript_style[end] == style;
		if (thinking) {
			style = 2; /* muted prose, not a tool-card style */
		}
		int skip = 0;
		if (user_open && style != 1 && style != 19) {
			/* The message ended: band rows below it, standing in for the
               blank line that follows, so the spacing stays the same. */
			user_open = 0;
			if (skin.user_padding_y) {
				if (c.x) {
					c.y++;
					c.x = 0;
				}
				for (int i = 0; i < skin.user_padding_y; i++) {
					md_row(&c, 2);
					c.y++;
				}
				if (style == 0 && !n && newline) {
					at = end + 1;
					continue;
				} /* that newline */
			}
		}
		struct tool_card *replacement = archive_layout ? NULL : semantic_tool_frame(at, full_width);
		if (replacement) {
			md_tool_close(&c, full_width);
			if (c.x) {
				c.y++;
				c.x = 0;
			}
			semantic_frame_paint(pad, first_row, c.y, &replacement->projection);
			c.y += replacement->projection.rows;
			c.width = prose_width;
			c.indent = c.rule = c.tab_origin = c.word_wrap = 0;
			at = replacement->end;
			fence = 0;
			diff_indent = folded_card = heading_gap = 0;
			cap_skip = cap_keep = cap_total = out_line = cap_noted = 0;
			card_tool[0] = 0;
			continue;
		}
		if (style == 71) {
			fence = 0;
			diff_indent = folded_card = 0;
			size_t name_length = strcspn(text, "\t");
			if (name_length > n) {
				name_length = n;
			}
			snprintf(card_tool, sizeof(card_tool), "%.*s", (int)name_length, text);
			heading_gap = skin.tool_heading_gap;
			/* tool.max_lines: shell output keeps its last lines, the rest
               their first; Ctrl+O (tool_full) shows everything. */
			cap_skip = cap_keep = cap_total = out_line = cap_noted = 0;
			int cap = skin_cap_for(&skin, card_tool);
			if (cap && (archive_layout || tool_details) && !tool_full) {
				int diff = 0, total = md_card_lines(at, &diff);
				if (!diff && total > cap) {
					cap_total = total;
					if (tool_shell(card_tool)) {
						cap_skip = total - cap;
					} else {
						cap_keep = cap;
					}
				}
			}
			md_tool_open(&c, full_width);
			if (md_tool_heading(&c, text, n, end < used ? transcript_style[end] : 14, layout_cwd)) {
				/* Skip the baked heading, but retain independently styled
                   timing/change counts and its terminating newline. */
				while (end < used && transcript[end] != '\n' && transcript_style[end] != 18 &&
					transcript_style[end] != 46 && transcript_style[end] != 47) {
					end++;
				}
			}
			at = end;
			continue;
		}
		if (style == 72) {
			md_tool_close(&c, full_width);
			folded_card = diff_indent = 0;
			at = end;
			continue;
		}
		if (style == 73) {
			while (end < used && transcript_style[end] == 73) {
				end++;
			}
			md_card_end(&c, full_width);
			at = end;
			continue;
		}
		if (style == 1 || style == 3 || style == 19 || style == 22 || style == 78) {
			card_tool[0] = 0;
		}
		/* tool.hide_output: this card shows only its heading. */
		if (c.card && *card_tool &&
			(style == 5 || style == 10 || style == 13 || style == 17 || style == 42)) {
			if (skin_hidden_for(&skin, card_tool)) {
				at = end + (newline ? 1 : 0);
				continue;
			}
		}
		/* tool.line_numbers false: no numbers beside write and read previews
           (diff numbers are diff.line_numbers). */
		if (!skin.tool_line_numbers &&
			(style == 13 ||
				(style == 10 && !diff_indent && n && strspn(text, "0123456789 ") == n))) {
			at = end;
			continue;
		}
		if (c.card && cap_total && md_output_style(style)) {
			char note[96];
			if (heading_gap && out_line >= cap_skip && !c.x) { /* the gap comes before the note */
				for (int i = 0; i < heading_gap; i++) {
					md_row(&c, 0);
					c.y++;
				}
				heading_gap = 0;
			}
			if (out_line < cap_skip) {
				if (newline) {
					out_line++;
				}
				at = end + (newline ? 1 : 0);
				continue;
			}
			if (cap_skip && !cap_noted) {
				snprintf(note, sizeof(note), "… (%d earlier lines, ctrl+o to expand)", cap_skip);
				if (c.x) {
					c.y++;
					c.x = 0;
				}
				md_row(&c, 0);
				c.x = c.indent;
				md_text(&c, note, strlen(note), COLOR_PAIR(49), 0);
				c.y++;
				c.x = 0;
				cap_noted = 1;
			}
			if (cap_keep && out_line >= cap_keep) {
				if (!cap_noted) {
					snprintf(note, sizeof(note), "… (%d more lines, %d total, ctrl+o to expand)",
						cap_total - cap_keep, cap_total);
					if (c.x) {
						c.y++;
						c.x = 0;
					}
					md_row(&c, 0);
					c.x = c.indent;
					md_text(&c, note, strlen(note), COLOR_PAIR(49), 0);
					c.y++;
					c.x = 0;
					cap_noted = 1;
				}
				at = end + (newline ? 1 : 0);
				continue;
			}
			if (newline) {
				out_line++;
			}
		}
		/* tool.captions false: no caption rows ("2 lines · 36 B") in cards. */
		if (!skin.tool_captions && c.card && style == 14 && !c.x && newline) {
			at = end + 1;
			continue;
		}
		if (thinking || style == 73 || style == 1 || style == 3 || style == 19) {
			heading_gap = 0;
		}
		/* tool.heading_gap: blank rows between a card's heading and its body. */
		if (heading_gap && c.card && !c.x && style != 18 && style != 46 && style != 47 &&
			(n || newline)) {
			for (int i = 0; i < heading_gap; i++) {
				md_row(&c, 0);
				c.y++;
			}
			heading_gap = 0;
		}
		if (style == 45 && !c.card && !c.x) {
			md_tool_open(&c, full_width);
			folded_card = 1;
		}
		if (c.card && (thinking || style == 1 || style == 3 || style == 19 || style == 22)) {
			md_tool_close(&c, full_width);
			folded_card = 0;
			c.width = prose_width;
		}
		char gutter[64];
		if (style >= 67 && style <= 69) {
			size_t first = 0, last = n;
			while (first < last && text[first] == ' ') {
				first++;
			}
			while (last > first && text[last - 1] == ' ') {
				last--;
			}
			size_t digits = last - first;
			size_t columns =
				digits > (size_t)skin.diff_gutter_width ? digits : (size_t)skin.diff_gutter_width;
			if (skin.diff_sign == DIFF_SIGN_BEFORE && columns + 2 < sizeof(gutter)) {
				/* "-2 x": the sign beside the number; its marker is skipped. */
				char sign = style == 67 ? '-' : style == 68 ? '+' : ' ';
				size_t shown = skin.diff_line_numbers ? digits : 0,
					   width_used = skin.diff_line_numbers ? columns : 0;
				memset(gutter, ' ', width_used + 2);
				if (shown) {
					memcpy(gutter + 1 + width_used - shown, text + first, shown);
				}
				gutter[width_used - shown] = sign;
				text = gutter;
				n = width_used + 2;
			} else if (!skin.diff_line_numbers) {
				n = 0;
			} else if (columns + 1 < sizeof(gutter)) {
				memset(gutter, ' ', columns + 1);
				memcpy(gutter + columns - digits, text + first, digits);
				text = gutter;
				n = columns + 1;
			}
		}
		if (skin.diff_sign == DIFF_SIGN_BEFORE && style >= 75 && style <= 77) {
			at = end + (newline ? 1 : 0);
			continue;
		}
		if (style == 74) {
			if (skin.diff_saved_label == DIFF_LABEL_NONE) {
				at = end + (newline ? 1 : 0);
				continue;
			}
			if (skin.diff_saved_label == DIFF_LABEL_COMPACT) {
				const char *suffix = md_find(text, n, " (file lines unavailable)");
				if (suffix) {
					n = (size_t)(suffix - text);
				}
			}
		}
		c.word_wrap = style == 1 || (style == 3 && !fence);
		c.tab_origin = 0;
		if (style == 46 || style == 47) {
			fence = 0;
			c.rule = c.indent = 0;
			if (skin.tool_change_counts) {
				md_text(&c, text, n, COLOR_PAIR(style == 46 ? 31 : 48), 0);
			}
		} else if (style == 43 || style == 44 || style == 45) {
			fence = 0;
			c.rule = 0;
			c.indent = style == 45 && !*skin.fold && c.width >= 4 ? 2 : 0;
			c.word_wrap = style == 45;
			/* Inside a card the summary keeps the card's background. */
			md_text(&c, text, n,
				COLOR_PAIR(style == 43 ? 51
						: style == 44  ? 7
						: c.card       ? 49
									   : 3),
				0);
		} else if (style == 78) {
			/* After a turn: "✻ Worked for 1m 12s". The transcript holds only
               the seconds; glyph and word come from the current skin. */
			fence = 0;
			c.indent = c.rule = 0;
			char *after = NULL;
			long seconds = strtol(text, &after, 10);
			long long finished =
				after && after < text + n && *after == '@' ? strtoll(after + 1, NULL, 10) : 0;
			if (skin.done_count) {
				char line[192], took[32], clock[48] = "";
				if (seconds < 60) {
					snprintf(took, sizeof(took), "%lds", seconds);
				} else if (seconds < 3600) {
					snprintf(took, sizeof(took), "%ldm %lds", seconds / 60, seconds % 60);
				} else {
					snprintf(took, sizeof(took), "%ldh %ldm", seconds / 3600, seconds / 60 % 60);
				}
				if (skin.done_clock && finished > 0) {
					time_t when = (time_t)finished;
					struct tm local;
					char hour[24];
					if (localtime_r(&when, &local) &&
						strftime(hour, sizeof(hour), "%I:%M %p", &local)) {
						snprintf(
							clock, sizeof(clock), " · done %s", hour[0] == '0' ? hour + 1 : hour);
					}
				}
				/* The glyph sits in the replies' glyph column, the words
                   under their text. */
				int hang = *skin.reply ? skin_width(skin.reply) + 1 : 0;
				int glyph = *skin.done_glyph ? skin_width(skin.done_glyph) + 1 : 0;
				char lead[32] = "";
				int gap = hang > glyph ? hang - glyph : glyph ? 0 : hang;
				if (gap > 8) {
					gap = 8;
				}
				snprintf(lead, sizeof(lead), "%s%s%*s", skin.done_glyph, glyph ? " " : "", gap, "");
				int written = snprintf(line, sizeof(line), "%s%s for %s%s", lead,
					skin.done[seconds % skin.done_count], took, clock);
				if (written > 0) {
					md_text(&c, line,
						(size_t)written < sizeof(line) ? (size_t)written : sizeof(line) - 1,
						COLOR_PAIR(3), 0);
				}
			}
		} else if (style == 22) {
			fence = 0;
			c.indent = c.rule = 0;
			md_text(&c, text, n, COLOR_PAIR(53) | A_BOLD, 0);
		} else if (style == 19) {
			c.indent = c.rule = 0;
			/* user.padding_y: band rows above the message (below: user_open). */
			if (!c.x) {
				for (int i = 0; i < skin.user_padding_y; i++) {
					md_row(&c, 2);
					c.y++;
				}
			}
			user_open = 1;
			if (!c.x) {
				md_row(&c, 2);
			}
			if (skin.user_glyph != USER_GLYPH_NONE) {
				md_text(&c, text, n,
					COLOR_PAIR(skin.user_glyph == USER_GLYPH_MUTED ? 4
							: skin.user_glyph == USER_GLYPH_TEXT   ? 2
																   : 63),
					2);
			}
		} else if (style == 18) {
			/* Timing belongs to its heading, not the far edge of the terminal. */
			int duration = skin_duration_for(&skin, card_tool);
			if (duration == TOOL_DURATION_FOOTER) {
				/* Held for the card's last row: " · 3ms" becomes "Took 3ms". */
				const char *took = text;
				size_t left = n;
				while (left &&
					(*took == ' ' || (unsigned char)*took == 0xc2 ||
						(unsigned char)*took == 0xb7)) {
					took++;
					left--;
				}
				snprintf(c.footer, sizeof(c.footer), "Took %.*s", (int)left, took);
			} else if (duration == TOOL_DURATION_HEADING && c.x + (int)n < c.width) {
				md_text(&c, text, n, COLOR_PAIR(49), 0);
			}
		} else if (style == 20) {
			/* Right-side metadata must never overwrite a path or output. */
			int cells = style == 20 ? 1 : (int)n;
			int inset = style == 20 ? 1 : 0;
			if (n && c.x + cells + inset + 2 <= c.width) {
				c.x = c.width - cells - inset;
				md_text(&c, text, n, COLOR_PAIR(style == 20 ? 51 : 49), 0);
			}
		} else if (style != 3) {
			fence = 0;
			int ansi = style >= 25 && style <= 40;
			int background = style == 41                   ? 62
				: ansi                                     ? 54 + (style - 25) % 8
				: style == 1                               ? 2
				: style == 2                               ? 4
				: style == 21                              ? 52
				: style == 14                              ? 49
				: style == 4 || style == 6                 ? 9
				: style == 7 || style == 23 || style == 24 ? 10
				: style == 8 || style == 9 || style == 11 || style == 12 || style == 15 ||
					style == 16 || (style >= 67 && style <= 69) || (style >= 75 && style <= 77)
				? md_diff_pair(style)
				: style == 10 || style == 13 || style == 70 || style == 74 ? 13
				: style == 64                                              ? 49
				: style == 65                                              ? 51
				: style >= 4                                               ? 8
																		   : 0;
			c.rule = c.width >= 5 && (style == 23 || style == 24) ? 2 : 0;
			c.indent = c.width >= skin.tool_indent + 2 && style >= 4 && style != 4
				? skin.tool_indent
				: 0; /* tool.indent */
			/* Diff gutters start at the card edge, not inside the ordinary
               tool-output indent. Continuations hang under the code. */
			if (style >= 67 && style <= 69) {
				diff_indent = (int)n +
					(skin.diff_sign == DIFF_SIGN_BEFORE ? 0
														: 2); /* number field, then change marker */
				c.indent = 0;
			} else if (style == 8 || style == 9 || (style == 10 && diff_indent) || style == 15 ||
				style == 16) {
				c.indent = diff_indent + 2 < c.width ? diff_indent : 0;
				c.tab_origin = c.indent;
			} else if (style == 70 || style == 74 || (style >= 75 && style <= 77)) {
				c.indent = 0;
			}
			/* Every card head (tool, notice, extension card) starts at the edge. */
			if (n >= 3 && !memcmp(text, GLYPH_CARD, 3)) {
				c.indent = 0;
			}
			/* Wrapped lines of your message hang under its text, past the marker. */
			if (style == 1 && width >= 5) {
				c.indent = skin.user_glyph == USER_GLYPH_NONE || !*skin.prompt
					? 0
					: skin_width(skin.prompt) + 1;
			}
			if (!c.x) {
				md_row(&c, background);
			}
			attr_t attr = background ? COLOR_PAIR(background) : A_NORMAL;
			if (style >= 67 && style <= 69) {
				attr = COLOR_PAIR(13); /* quiet number column */
			}
			/* 64-66: extension markup (muted, success, bold) in card text. */
			if ((style == 4 && (!c.card || skin.tool_heading_bold)) ||
				(style == 1 && skin.user_bold) || style == 23 || (style >= 33 && style <= 41) ||
				style == 66) {
				attr |= A_BOLD;
			}
#ifdef A_ITALIC
			if (thinking) {
				attr |= A_ITALIC;
			}
#endif
			if (style == 15 || style == 16) {
				attr |= A_BOLD;
			}
			if (style == 14 && c.width >= 40) {
				/* Details are a caption, not a second command transcript. */
				int room = c.width - c.x - 9, cells = 0;
				size_t bytes = 0;
				mbstate_t state = {0};
				while (bytes < n && room > 0) {
					wchar_t wc;
					size_t step = mbrtowc(&wc, text + bytes, n - bytes, &state);
					if (!step || step == (size_t)-1 || step == (size_t)-2) {
						break;
					}
					int span = wcwidth(wc);
					if (span < 0) {
						span = 0;
					}
					if (cells + span > room) {
						break;
					}
					cells += span;
					bytes += step;
				}
				md_text(&c, text, bytes, attr, background);
				if (bytes < n && room > 0) {
					md_char(&c, L'…', attr, background);
				}
			} else if (style == 17) {
				struct python_state line_state = {0};
				md_python(&c, text, n, background, &line_state);
			} else if (style == 42) {
				md_c(&c, text, n, background, &tool_c_state);
			} else if (style >= 11 && style <= 12 && n >= 8) {
				md_text(&c, text, 8, attr, background);
				struct python_state line_state = {0};
				md_python(&c, text + 8, n - 8, background, &line_state);
			} else {
				/* Phi's messages, errors and a model's words in a card wrap
                   at spaces; tool output wraps anywhere, as a terminal would. */
				c.word_wrap = thinking || style == 0 || style == 1 || style == 7 || style == 79;
				if (thinking) {
					md_inline(&c, text, n, attr, 0);
				} else if (style == 79) {
					md_inline(&c, text, n, attr, background);
				} else {
					md_text(&c, text, n, attr, background);
				}
				c.word_wrap = 0;
			}
		} else {
			/* With a reply glyph ("● "), the reply hangs under its text. */
			c.indent = *skin.reply && width >= 8 ? skin_width(skin.reply) + 1 : 0;
			c.rule = 0;
			size_t after;
			/* Display maths (\[ ... \] or $$ ... $$ opening a line) is shown as written. */
			size_t lead = 0;
			while (lead < n && text[lead] == ' ') {
				lead++;
			}
			int opens = !fence && !math && n - lead >= 2 &&
				(!memcmp(text + lead, "\\[", 2) || !memcmp(text + lead, "$$", 2));
			if (math || opens) {
				size_t from = lead;
				if (opens) {
					math = text[lead] == '$' ? 2 : 1;
					from = lead + 2;
				}
				if (md_find(text + from, n - from, math == 1 ? "\\]" : "$$")) {
					math = 0;
				}
				md_text(&c, text, n, A_NORMAL, 0);
				if (newline) {
					c.y++;
					c.x = 0;
				}
				at = end + (newline ? 1 : 0);
				continue;
			}
			if (!fence && !c.x && md_table(&c, at, &after)) {
				at = after;
				continue;
			}
			size_t indent = 0;
			while (indent < n && text[indent] == ' ') {
				indent++;
			}
			size_t run = 0;
			if (indent <= 3 && indent < n && (text[indent] == '`' || text[indent] == '~')) {
				while (indent + run < n && text[indent + run] == text[indent]) {
					run++;
				}
			}
			size_t tail = indent + run;
			while (tail < n && md_space(text[tail])) {
				tail++;
			}
			if (run >= 3 &&
				(!fence || (text[indent] == fence && run >= fence_length && tail == n))) {
				if (fence) {
					fence = 0;
					skip = 1;
				} else {
					fence = text[indent];
					fence_length = run;
					size_t language_end = tail;
					while (language_end < n && !md_space(text[language_end])) {
						language_end++;
					}
					size_t language_size = language_end - tail;
					python = (language_size == 6 && !memcmp(text + tail, "python", 6)) ||
						(language_size == 2 && !memcmp(text + tail, "py", 2)) ||
						(language_size == 7 && !memcmp(text + tail, "python3", 7));
					c_family = (language_size == 1 && !memcmp(text + tail, "c", 1)) ||
						(language_size == 3 && !memcmp(text + tail, "c++", 3)) ||
						(language_size == 2 && !memcmp(text + tail, "cc", 2)) ||
						(language_size == 3 && !memcmp(text + tail, "cpp", 3)) ||
						(language_size == 3 && !memcmp(text + tail, "cxx", 3)) ||
						(language_size == 1 && !memcmp(text + tail, "h", 1)) ||
						(language_size == 3 && !memcmp(text + tail, "hpp", 3));
					shell =
						(language_size == 4 &&
							(!memcmp(text + tail, "bash", 4) || !memcmp(text + tail, "fish", 4))) ||
						(language_size == 2 && (!memcmp(text + tail, "sh", 2))) ||
						(language_size == 3 && !memcmp(text + tail, "zsh", 3)) ||
						(language_size == 5 && !memcmp(text + tail, "shell", 5));
					python_state.quote = 0;
					c_state = (struct c_syntax_state){0};
					shell_state.quote = 0;
					/* No row of its own: a label line read like code, and was copied with it. */
					if (language_size >= sizeof(fence_label)) {
						language_size = 0;
					}
					/* "text" and the like say nothing worth a label. */
					static const char *const silent[] = {
						"text", "txt", "plain", "plaintext", "output", "console", "none"};
					for (size_t i = 0; i < sizeof(silent) / sizeof(*silent); i++) {
						if (strlen(silent[i]) == language_size &&
							!memcmp(text + tail, silent[i], language_size)) {
							language_size = 0;
						}
					}
					memcpy(fence_label, text + tail, language_size);
					fence_label[language_size] = 0;
					skip = 1;
				}
			} else if (fence) {
				md_row(&c, 6);
				int first_y = c.y;
				if (python) {
					md_python(&c, text, n, 6, &python_state);
				} else if (c_family) {
					md_c(&c, text, n, 6, &c_state);
				} else if (shell) {
					md_shell(&c, text, n, 6, &shell_state);
				} else {
					md_text(&c, text, n, COLOR_PAIR(6), 6);
				}
				/* The language, dim at the right of the first line when it
                   fits there. Copying skips it (A_DIM on pair 6). */
				int label_cells = *fence_label ? skin_width(fence_label) : 0;
				if (label_cells > 0 && c.y == first_y && c.x + 3 + label_cells < c.width) {
					while (c.x < c.width - label_cells - 1) {
						md_char(&c, L' ', COLOR_PAIR(6), 6);
					}
					md_text(&c, fence_label, strlen(fence_label), COLOR_PAIR(6) | A_DIM, 6);
				}
				fence_label[0] = 0;
			} else {
				size_t start = 0, hashes = 0;
				while (indent + hashes < n && text[indent + hashes] == '#') {
					hashes++;
				}
				attr_t attr = A_NORMAL;
				if (indent <= 3 && hashes && hashes <= 6 && indent + hashes < n &&
					text[indent + hashes] == ' ') {
					start = indent + hashes + 1;
					attr = A_BOLD;
				} else if (indent + 1 < n && strchr("-*+", text[indent]) &&
					text[indent + 1] == ' ') {
					md_text(&c, text, indent, attr, 0);
					md_text(&c, skin.bullet, strlen(skin.bullet), attr, 0);
					md_char(&c, L' ', attr, 0); /* markdown.bullet */
					start = indent + 2;
					c.indent = c.x; /* a wrapped item hangs under its text, not its bullet */
				} else {
					/* "1. " or "1) ": the same hang for numbered items. */
					size_t digits = indent;
					while (digits < n && digits - indent < 9 && text[digits] >= '0' &&
						text[digits] <= '9') {
						digits++;
					}
					if (digits > indent && digits + 1 < n &&
						(text[digits] == '.' || text[digits] == ')') && text[digits + 1] == ' ') {
						md_text(&c, text, digits + 2, attr, 0);
						start = digits + 2;
						c.indent = c.x;
					}
				}
				md_inline(&c, text + start, n - start, attr, 0);
			}
		}
		if (newline && !skip) {
			c.y++;
			c.x = 0;
			diff_indent = 0;
			if (folded_card) {
				md_tool_close(&c, full_width);
				folded_card = 0;
			}
		}
		at = end + (newline ? 1 : 0);
		/* A newline belonging to the next style is consumed next iteration. */
	}
	if (!archive_layout && c.card && tool_active && !quiet_shell) {
		/* The running card holds the rows it will have when it finishes
           (footer, padding, the gap after it), so nothing jumps then. */
		if (skin_duration_for(&skin, card_tool) == TOOL_DURATION_FOOTER) {
			snprintf(c.footer, sizeof(c.footer), "Running…");
		}
		md_card_end(&c, full_width);
	} else if (c.card && c.frame == TOOL_BORDER_ROUNDED) {
		md_tool_close(&c, full_width);
	}
	used = saved_used;

	return c.y + 1;
}

static int layout(WINDOW *pad, int width) {

	return layout_rows(pad, width, 0);
}

/* Reuse every ordinary theme/Markdown/card/code rule, but not live projections,
   active-tool state or native renderer callbacks. Restore all borrowed globals. */
static int layout_saved_rows(
	WINDOW *pad, int width, int first, const struct styled_capture *saved, size_t bytes) {

	char *live_text = transcript;
	unsigned char *live_styles = transcript_style;
	size_t live_used = used;
	int live_archive = archive_layout;
	transcript = saved->text;
	transcript_style = saved->styles;
	used = bytes < saved->used ? bytes : saved->used;
	archive_layout = 1;
	int rows = layout_rows(pad, width, first);
	transcript = live_text;
	transcript_style = live_styles;
	used = live_used;
	archive_layout = live_archive;

	return rows;
}
