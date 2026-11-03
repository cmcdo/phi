/* Transient tool intent, separate from committed transcript/history. */
static int pending_active, pending_ready, pending_truncated;
static int approval_choice; /* a skin's approval menu: 0 Yes, 1 No */
static char pending_name[96], pending_arguments[8193];
/* Retained streamed arguments (pending_arguments keeps an 8 KiB fallback),
   and their string fields decoded as far as they have arrived, in order.
   A key seen again (each of several edits) keeps its latest value. */
static char *pending_full, *pending_decoded;
#define PENDING_FIELDS 16

static struct {
	char key[32];
	size_t at, length;
} pending_fields[PENDING_FIELDS];

static int pending_field_count;

static void pending_snapshot(char *text, size_t size) {

	int width = COLS - 4;
	if (width > 96) {
		width = 96;
	}
	int cells = 0, limit = 2 * (width - 4);
	size_t filled = 0;
	const char *p = pending_arguments;
	mbstate_t state = {0};
	while (*p && cells < limit) {
		wchar_t wc;
		size_t n = mbrtowc(&wc, p, strlen(p), &state);
		if (n == (size_t)-1 || n == (size_t)-2) {
			state = (mbstate_t){0};
			p++;
			continue;
		}
		if (!n) {
			break;
		}
		int span = wcwidth(wc);
		if (span < 0) {
			if (filled + 1 >= size) {
				break;
			}
			text[filled++] = ' ';
			span = 1;
		} else {
			if (cells + span > limit || filled + n + 4 >= size) {
				break;
			}
			memcpy(text + filled, p, n);
			filled += n;
		}
		cells += span;
		p += n;
	}
	if (*p && filled + 3 < size) {
		memcpy(text + filled, "…", 3);
		filled += 3;
	}
	text[filled] = 0;
}

static size_t pending_utf8(char *out, unsigned code) {

	if (code < 0x80) {
		out[0] = (char)code;
		return 1;
	}
	if (code < 0x800) {
		out[0] = (char)(0xc0 | code >> 6);
		out[1] = (char)(0x80 | (code & 0x3f));
		return 2;
	}
	if (code < 0x10000) {
		out[0] = (char)(0xe0 | code >> 12);
		out[1] = (char)(0x80 | (code >> 6 & 0x3f));
		out[2] = (char)(0x80 | (code & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | code >> 18);
	out[1] = (char)(0x80 | (code >> 12 & 0x3f));
	out[2] = (char)(0x80 | (code >> 6 & 0x3f));
	out[3] = (char)(0x80 | (code & 0x3f));

	return 4;
}

static int pending_hex(const char *p, unsigned *code) {

	*code = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		int digit = c >= '0' && c <= '9' ? c - '0'
			: c >= 'a' && c <= 'f'       ? c - 'a' + 10
			: c >= 'A' && c <= 'F'       ? c - 'A' + 10
										 : -1;
		if (digit < 0) {
			return 0;
		}
		*code = *code << 4 | (unsigned)digit;
	}

	return 1;
}

/* Decode the arguments so far: still-arriving JSON, so the last string may
   be unfinished. Strings directly after "key": are recorded as fields. */
static void pending_decode(void) {

	pending_field_count = 0;
	const char *p = pending_full ? pending_full : pending_arguments;
	char *out = realloc(pending_decoded, strlen(p) + 1);
	if (!out) {
		return;
	}
	pending_decoded = out;
	size_t filled = 0;
	char key[32] = "";
	int value = 0; /* the next string is the value of key */
	while (*p) {
		if (*p != '"') {
			if (strchr("{[,}]", *p)) {
				value = 0;
			}
			p++;
			continue;
		}
		size_t start = filled;
		int closed = 0;
		for (p++; *p;) {
			if (*p == '"') {
				closed = 1;
				p++;
				break;
			}
			if (*p != '\\') {
				if ((unsigned char)*p >= 32) {
					out[filled++] = *p;
				}
				p++;
				continue;
			}
			if (!p[1]) {
				p++;
				break;
			}
			char escape = p[1];
			p += 2;
			if (escape == 'n') {
				out[filled++] = '\n';
			} else if (escape == 't') {
				out[filled++] = '\t';
			} else if (escape == 'u') {
				unsigned code, low;
				if (!pending_hex(p, &code)) {
					p += strlen(p);
					break;
				}
				p += 4;
				if (code >= 0xd800 && code < 0xdc00) {
					if (p[0] != '\\' || p[1] != 'u' || !pending_hex(p + 2, &low)) {
						p += strlen(p);
						break;
					}
					p += 6;
					code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
				}
				if (code >= 32) {
					filled += pending_utf8(out + filled, code);
				}
			} else if (escape != 'r' && escape != 'b' && escape != 'f') {
				out[filled++] = escape;
			}
		}
		const char *after = p;
		while (*after == ' ' || *after == '\n' || *after == '\t' || *after == '\r') {
			after++;
		}
		if (closed && !value && *after == ':') {
			size_t n = filled - start < sizeof(key) - 1 ? filled - start : sizeof(key) - 1;
			memcpy(key, out + start, n);
			key[n] = 0;
			filled = start;
			p = after + 1;
			value = 1;
			continue;
		}
		if (value) {
			int slot = 0;
			while (slot < pending_field_count && strcmp(pending_fields[slot].key, key)) {
				slot++;
			}
			if (slot < PENDING_FIELDS) {
				if (slot == pending_field_count) {
					pending_field_count++;
				}
				snprintf(pending_fields[slot].key, sizeof(pending_fields[slot].key), "%s", key);
				pending_fields[slot].at = start;
				pending_fields[slot].length = filled - start;
			}
		}
		out[filled++] = 0;
		value = 0;
	}
}

static const char *pending_value(const char *key, size_t *size) {

	for (int i = 0; i < pending_field_count; i++) {
		if (!strcmp(pending_fields[i].key, key)) {
			if (size) {
				*size = pending_fields[i].length;
			}
			return pending_decoded + pending_fields[i].at;
		}
	}

	return NULL;
}

/* The heading detail, as on the finished card: a path or a command. */
static const char *pending_detail(void) {

	const char *name = pending_name, *detail = NULL;
	if (!strcmp(name, "read") || !strcmp(name, "write") || !strcmp(name, "edit")) {
		detail = pending_value("path", NULL);
	} else if (tool_shell(name)) {
		detail = pending_value("command", NULL);
	}
	if (!detail && pending_field_count && pending_fields[0].length <= 200 &&
		!memchr(pending_decoded + pending_fields[0].at, '\n', pending_fields[0].length)) {
		detail = pending_decoded + pending_fields[0].at;
	}

	return detail;
}

/* The text being written (file content, an edit's new text), else NULL. */
static const char *pending_body(size_t *size) {

	static const char *const keys[] = {
		"content", "newText", "new_string", "patch", "input", "data"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
		const char *text = pending_value(keys[i], size);
		if (text) {
			return text;
		}
	}

	return NULL;
}

/* The short card shows up to PENDING_LINES body rows. Ctrl+O expands a
   received write/edit body; the approval menu, if any, remains at the end. */
#define PENDING_LINES 4

static int pending_is_shell(void) {

	return tool_shell(pending_name);
}

/* Text cells inside the card at this terminal width. */
static int pending_text_width(int cols) {

	int width = cols - 2 * skin.margin;
	if (width > 96) {
		width = 96;
	}

	return width - 4;
}

static int tool_syntax_for_path(const char *path);
static int text_width(int cols);

enum {
	PENDING_PLAIN,
	PENDING_SHELL,
	PENDING_PYTHON,
	PENDING_C
};

/* Highlight a line; clip the short view, wrap the expanded live body. */
static void pending_code_line(struct md_canvas *c, const char *text, size_t n, int kind) {

	int room = c->width - c->x;
	size_t bytes = tool_full ? n : 0;
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
	struct python_state python = {0};
	struct c_syntax_state c_state = {0};
	struct shell_state shell = {0};
	if (kind == PENDING_PYTHON) {
		md_python(c, text, bytes, 0, &python);
	} else if (kind == PENDING_C) {
		md_c(c, text, bytes, 0, &c_state);
	} else if (kind == PENDING_SHELL) {
		md_shell(c, text, bytes, 0, &shell);
	} else {
		md_text(c, text, bytes, COLOR_PAIR(24), 0);
	}
	if (bytes < n && c->x < c->width) {
		md_char(c, L'…', COLOR_PAIR(49), 0);
	}
}

/* The card, onto a canvas: the finished card's heading, then the command,
   the last lines written, or the other arguments, under the output glyph;
   then the approval menu. Drawn into a pad (pending_draw) and measured
   (pending_height) by the same code. */
static void pending_render(struct md_canvas *c, int full_width) {

	int menu = pending_approval && skin.approval_menu;
	size_t body_length = 0;
	const char *body = pending_body(&body_length);
	size_t lines = 0;
	for (size_t i = 0; body && i < body_length; i++) {
		lines += body[i] == '\n';
	}
	if (body && body_length && body[body_length - 1] != '\n') {
		lines++;
	}
	char title[640], note[64] = "", cwd[4096] = "";
	if (skin.tool_path == TOOL_PATH_RELATIVE && !getcwd(cwd, sizeof(cwd))) {
		cwd[0] = 0;
	}
	const char *detail = pending_is_shell() ? NULL : pending_detail();
	char shown[512] = "";
	if (detail) {
		snprintf(shown, sizeof(shown), "%.*s", (int)strcspn(detail, "\n"),
			md_tool_path(pending_name, detail, cwd));
	}
	tool_title_text(pending_name, shown, title, sizeof(title));
	int note_pair = 49;
	if (pending_approval) {
		snprintf(note, sizeof(note), "· APPROVAL REQUIRED");
		note_pair = 62;
	} else if (lines) {
		snprintf(note, sizeof(note), "· %zu %s%s%s", lines, lines == 1 ? "line" : "lines",
			pending_ready ? "" : "…", pending_truncated ? " · preview limit" : "");
	} else if (!pending_ready) {
		snprintf(note, sizeof(note), "…");
	}
	md_live_heading(c, full_width, title, note, note_pair);
	size_t command_size = 0;
	const char *command = pending_is_shell() ? pending_value("command", &command_size) : NULL;
	if (command && command_size) {
		char *copy = strndup(command, command_size);
		if (copy) {
			md_live_command(c, copy, PENDING_LINES);
			free(copy);
		}
	} else if (body) {
		const char *path = pending_value("path", NULL);
		const char *dot = path ? strrchr(path, '.') : NULL;
		int syntax = tool_syntax_for_path(path);
		int kind = syntax == TOOL_SYNTAX_PYTHON                     ? PENDING_PYTHON
			: syntax == TOOL_SYNTAX_C                               ? PENDING_C
			: dot && (!strcmp(dot, ".sh") || !strcmp(dot, ".bash")) ? PENDING_SHELL
																	: PENDING_PLAIN;
		if (tool_full) {
			/* Ctrl+O applies while arguments stream, not only after execution.
               Render all received lines; pending_draw paints only the viewport. */
			for (size_t at = 0, line = 1; at < body_length; line++) {
				size_t stop = at;
				while (stop < body_length && body[stop] != '\n') {
					stop++;
				}
				md_live_row(c, line == 1);
				char number[24];
				snprintf(number, sizeof(number), "%4zu  ", line);
				md_text(c, number, strlen(number), COLOR_PAIR(49), 0);
				pending_code_line(c, body + at, stop - at, kind);
				at = stop < body_length ? stop + 1 : stop;
			}
		} else {
			/* The last lines written so far, numbered as in the file. */
			size_t starts[PENDING_LINES] = {0}, count = 0, at = body_length;
			if (at && body[at - 1] == '\n') {
				at--; /* a finished line, not an empty new one */
			}
			size_t line_end = at;
			while (count < PENDING_LINES) {
				size_t begin = line_end;
				while (begin && body[begin - 1] != '\n') {
					begin--;
				}
				starts[count++] = begin;
				if (!begin) {
					break;
				}
				line_end = begin - 1;
			}
			size_t last = lines ? lines : 1;
			for (size_t i = count; i-- > 0;) {
				size_t begin = starts[i], stop = begin;
				while (stop < body_length && body[stop] != '\n') {
					stop++;
				}
				md_live_row(c, i == count - 1);
				char number[16];
				snprintf(number, sizeof(number), "%4zu  ", last - i);
				md_text(c, number, strlen(number), COLOR_PAIR(49), 0);
				pending_code_line(c, body + begin, stop - begin, kind);
			}
		}
	} else {
		/* Nothing being written: the other arguments, briefly. */
		int first = 1;
		for (int i = 0; i < pending_field_count; i++) {
			const char *value = pending_decoded + pending_fields[i].at;
			if (value == detail) {
				continue;
			}
			if (first) {
				md_live_row(c, 1);
				first = 0;
			} else if (c->x + 3 < c->width) {
				md_text(c, " · ", 5, COLOR_PAIR(49), 0);
			}
			if (c->x + 4 >= c->width) {
				break;
			}
			md_text(c, pending_fields[i].key, strlen(pending_fields[i].key), COLOR_PAIR(49), 0);
			md_char(c, L' ', COLOR_PAIR(49), 0);
			md_clip(c, value, strcspn(value, "\n"), COLOR_PAIR(24));
		}
	}
	if (menu) {
		/* Numbered permission actions; y/n remain available. */
		static const char *const choices[] = {"1. Yes", "2. No"};
		if (c->x) {
			c->y++;
			c->x = 0;
		}
		md_row(c, 0);
		c->x = c->indent;
		for (int i = 0; i < 2; i++) {
			int chosen = approval_choice == i;
			const char *mark = chosen ? (*skin.prompt ? skin.prompt : ">") : " ";
			md_text(c, mark, strlen(mark), COLOR_PAIR(62) | A_BOLD, 0);
			md_char(c, L' ', COLOR_PAIR(62), 0);
			md_text(c, choices[i], strlen(choices[i]),
				COLOR_PAIR(chosen ? 62 : 49) | (chosen ? A_BOLD : 0), 0);
			md_text(c, "    ", 4, COLOR_PAIR(49), 0);
		}
	}
	md_tool_close(c, full_width);
}

/* Rows the card takes, measured by rendering it without a pad. */
static int pending_height(int cols) {

	struct md_canvas probe = {.pad = NULL, .width = text_width(cols)};
	pending_render(&probe, text_width(cols));

	return probe.y + (probe.x ? 1 : 0);
}

static void pending_draw(int row, int cols, int height) {

	int width = text_width(cols), rows = pending_height(cols);
	int first = row < 0 ? -row : 0,
		last = row + rows - 1 < height - 1 ? rows - 1 : height - 1 - row;
	if (last < first) {
		return;
	}
	/* A large live write must not allocate a pad for thousands of offscreen
       rows. Parse for layout, but paint only the visible window. */
	WINDOW *box = newpad(last - first + 1, width);
	if (!box) {
		return;
	}
	wbkgd(box, COLOR_PAIR(27) | ' ');
	struct md_canvas canvas = {.pad = box, .width = width, .origin_y = -first};
	pending_render(&canvas, width);
	copywin(box, stdscr, 0, 0, row + first, skin.margin, row + last, skin.margin + width - 1, 0);
	delwin(box);
	attrset(A_NORMAL);
}
