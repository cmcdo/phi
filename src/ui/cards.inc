/* Tool cards: output, headings and titles, arguments and results,
   extension cards, read/write previews and diffs, folding, and the end of
   a tool. Included by ui.c after append_styled(). */

void ui_shell_output(const char *text, int ansi_color, int bold) {

	int style = ansi_color >= 0 && ansi_color < 8 ? 25 + ansi_color : 21;
	append_styled(text, style + (bold ? 8 : 0));
	if (*text) {
		tool_output_line = text[strlen(text) - 1] != '\n';
	}
}

void ui_append(const char *text) {

	append_styled(text, tool_active ? (tool_bash ? 21 : 5) : 0);
	if (tool_active && *text) {
		tool_output_line = text[strlen(text) - 1] != '\n';
	}
}

static int tool_syntax_for_path(const char *path) {

	if (!path) {
		return TOOL_SYNTAX_NONE;
	}
	const char *dot = strrchr(path, '.');
	if (!dot) {
		return TOOL_SYNTAX_NONE;
	}
	if (!strcmp(dot, ".py")) {
		return TOOL_SYNTAX_PYTHON;
	}
	if (!strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".cc") || !strcmp(dot, ".hh") ||
		!strcmp(dot, ".cpp") || !strcmp(dot, ".hpp") || !strcmp(dot, ".cxx") ||
		!strcmp(dot, ".hxx")) {
		return TOOL_SYNTAX_C;
	}

	return TOOL_SYNTAX_NONE;
}

static char tool_title_override[256];

void ui_tool_title(const char *title) {

	snprintf(tool_title_override, sizeof(tool_title_override), "%s", title ? title : "");
	for (char *c = tool_title_override; *c; c++) {
		if ((unsigned char)*c < 32) {
			*c = ' ';
		}
	}
}

void ui_tool_line(const char *text) {

	char line[512];
	snprintf(line, sizeof(line), "%s", text ? text : "");
	for (char *c = line; *c; c++) {
		if ((unsigned char)*c < 32) {
			*c = ' ';
		}
	}
	markup_append(line, 5);
	append_styled("\n", 5);
}

static void tool_title_detail(const char *detail) {

	if (!strncmp(detail, "[local]", 7)) {
		append_styled("[local]", 41);
		append_styled(detail + 7, 14);
	} else if (!strcmp(detail, "[extension]")) {
		append_styled("[extension]", 41);
	} else {
		append_styled(detail, 14);
	}
}

/* A skin's tool.title: {name}, {Name} (capitalised), {label} and {detail}
   in a template such as "{Name}({detail})". Other text is literal. */
static void tool_title_template(const char *name, const char *label, const char *detail) {

	const char *p = skin_title_for(&skin, name);
	char literal[64];
	size_t n = 0;
	while (*p) {
		const char *field = NULL, *end = *p == '{' ? strchr(p, '}') : NULL;
		size_t size = end ? (size_t)(end - p + 1) : 0;
		int capital = 0;
		if (size == 6 && !strncmp(p, "{name}", 6)) {
			field = name;
		} else if (size == 6 && !strncmp(p, "{Name}", 6)) {
			field = name;
			capital = 1;
		} else if (size == 7 && !strncmp(p, "{label}", 7)) {
			field = label;
		} else if (size == 8 && !strncmp(p, "{detail}", 8)) {
			field = detail ? detail : "";
		}
		if (!field) {
			if (n < sizeof(literal) - 1) {
				literal[n++] = *p;
			}
			p++;
			continue;
		}
		literal[n] = 0;
		if (n) {
			append_styled(literal, 14);
		}
		n = 0;
		if (size == 8) {
			if (*field) {
				tool_title_detail(field);
			}
		} else if (capital && *field) {
			char first[2] = {(char)toupper((unsigned char)*field), 0};
			append_styled(first, 4);
			append_styled(field + 1, 4);
		} else {
			append_styled(field, 4);
		}
		p += size;
	}
	literal[n] = 0;
	if (n) {
		append_styled(literal, 14);
	}
}

static int tool_activity_category(const char *name, long long job) {

	return !strcmp(name, "read")     ? ACT_READ
		: !strcmp(name, "edit")      ? ACT_EDIT
		: !strcmp(name, "write")     ? ACT_WRITE
		: !strcmp(name, "bash_poll") ? (job > 0 ? ACT_JOB : ACT_OTHER)
		: tool_shell(name)           ? ACT_SHELL
		: !strncmp(name, "web_", 4)  ? ACT_WEB
									 : ACT_OTHER;
}

void ui_tool_begin(const char *name, const char *detail) {

	semantic_live_clear();
	if (tool_pending_semantic) {
		json_object_put(tool_pending_semantic);
	}
	tool_pending_semantic = NULL;
	tool_syntax = tool_syntax_for_path(detail);
	tool_heading_valid = 0;
	tool_started = now();
	tool_historical = 0;
	tool_added = tool_removed = 0;
	snprintf(tool_detail, sizeof(tool_detail), "%.500s", detail ? detail : "");
	for (char *p = tool_detail; *p; p++) {
		if ((unsigned char)*p < 32) {
			*p = ' ';
		}
	}
	presentation_batch++;
	tool_bash = !strcmp(name, "bash");
	tool_poll = !strcmp(name, "bash_poll");
	tool_edit = !strcmp(name, "edit");
	tool_job_id = tool_poll && detail ? strtoll(detail, NULL, 10) : 0;
	tool_category = tool_activity_category(name, tool_job_id);
	/* Give tool sections a little breathing room without boxed cards. */
	while (used && (transcript_style[used - 1] == 0 || transcript_style[used - 1] == 3) &&
		(transcript[used - 1] == '\n' || transcript[used - 1] == '\r')) {
		used--;
	}
	semantic_messages_truncate(used);
	transcript[used] = 0;
	/* A separator whose displayed height comes from tool.spacing. */
	if (used) {
		append_styled(transcript[used - 1] == '\n' ? "\n" : "\n\n", 73);
	}
	tool_header_start = used;
	const char *label = tool_poll ? "bash output" : !strcmp(name, "bash_start") ? "bash" : name;
	int shell = tool_shell(name);
	if (shell) {
		shell_activity_set(tool_detail, "running");
	} else {
		shell_activity[0] = 0;
	}
	snprintf(tool_live_name, sizeof(tool_live_name), "%s", name);
	snprintf(tool_running_label, sizeof(tool_running_label), "%s%s%s",
		shell && *tool_detail ? "" : label, *tool_detail && !shell ? " " : "", tool_detail);
	/* Display-only semantic heading. It travels with folded/expanded spans,
       so layout can restyle old cards without replaying tools or the model.
       Tabs delimit sanitized fields; the ordinary heading remains a fallback. */
	char metadata[1024];
	snprintf(metadata, sizeof(metadata), "%s\t%s\t%s\t%d", name, label, tool_detail,
		!!*tool_title_override);
	append_styled(metadata, 71);
	if (!tool_edit) {
		append_styled(GLYPH_CARD, 14);
	}
	if (*tool_title_override) {
		markup_append(tool_title_override, 4);
		tool_title_override[0] = 0;
	} else if (*skin_title_for(&skin, name)) {
		tool_title_template(name, label, tool_detail);
	} else {
		append_styled(label, 4);
		if (*tool_detail) {
			append_styled(" ", 14);
			tool_title_detail(tool_detail);
		}
	}
	tool_heading_end = used;
	tool_heading_valid = 1;
	append_styled("\n", 4);
	tool_body_start = used;
	tool_active = 1;
	tool_output_line = 0;
	dirty = 1;
	presentation_batch--;
	draw(0);
}

static void ui_tool_value(json_object *value) {

	if (!value) {
		append_styled("null", 5);
		return;
	}
	const char *text = json_object_is_type(value, json_type_string)
		? json_object_get_string(value)
		: json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
	if (!text) {
		text = "";
	}
	char preview[321];
	size_t filled = 0;
	const char *p = text;
	mbstate_t state = {0};
	while (*p && filled < 300) {
		wchar_t wc;
		size_t step = mbrtowc(&wc, p, strlen(p), &state);
		if (!step) {
			break;
		}
		if (step == (size_t)-1 || step == (size_t)-2) {
			state = (mbstate_t){0};
			p++;
			continue;
		}
		if (wc == L'\n' || wc == L'\r' || wc == L'\t') {
			preview[filled++] = ' ';
			p += step;
			continue;
		}
		if (iswcntrl(wc)) {
			p += step;
			continue;
		}
		if (filled + step > 300) {
			break;
		}
		memcpy(preview + filled, p, step);
		filled += step;
		p += step;
	}
	if (*p && filled + 3 < sizeof(preview)) {
		memcpy(preview + filled, "…", 3);
		filled += 3;
	}
	preview[filled] = 0;
	append_styled(preview, 5);
}

void ui_tool_arguments(json_object *arguments) {

	if (!json_object_is_type(arguments, json_type_object)) {
		return;
	}
	size_t shown = 0;
	json_object_object_foreach(arguments, key, value) {
		if (shown++ == 4) {
			append_styled("… more arguments\n", 14);
			break;
		}
		append_styled(key, 14);
		append_styled("  ", 14);
		ui_tool_value(value);
		append_styled("\n", 5);
	}
}

static void ui_tool_output_preview(const char *text) {

	if (!text || !*text) {
		return;
	}
	size_t bytes = 0, lines = 0;
	while (text[bytes] && bytes < 4096 && lines < 12) {
		if (text[bytes++] == '\n') {
			lines++;
		}
	}
	while (bytes && ((unsigned char)text[bytes] & 0xc0) == 0x80) {
		bytes--;
	}
	char preview[4097];
	memcpy(preview, text, bytes);
	preview[bytes] = 0;
	append_styled(preview, 5);
	if (bytes && preview[bytes - 1] != '\n') {
		append_styled("\n", 5);
	}
	if (text[bytes]) {
		append_styled("… result preview truncated\n", 14);
	}
}

void ui_tool_result(json_object *answer) {

	if (!json_object_is_type(answer, json_type_object)) {
		return;
	}
	json_object *value = NULL;
	const char *label = NULL;
	/* A structured extension result may supply its own card text. */
	if (json_object_object_get_ex(answer, "_phi_display", &value) &&
		json_object_is_type(value, json_type_string)) {
		ui_tool_output_preview(json_object_get_string(value));
		return;
	}
	if (json_object_object_get_ex(answer, "output", &value) &&
		json_object_is_type(value, json_type_string)) {
		label = "result";
	} else if (json_object_object_get_ex(answer, "status", &value) &&
		json_object_is_type(value, json_type_string)) {
		label = "status";
	} else if (json_object_object_get_ex(answer, "disabled", &value) &&
		json_object_is_type(value, json_type_string)) {
		label = "disabled";
	}
	if (!label) {
		return;
	}
	append_styled(label, 14);
	append_styled("\n", 14);
	ui_tool_output_preview(json_object_get_string(value));
}

/* Extension notices use the same card shape as tool calls: a "◆ title
   detail" heading on the card background, then body lines in card text. */
static void extension_card_begin(const char *title, const char *detail, size_t detail_length) {

	presentation_batch++;
	while (used && (transcript[used - 1] == '\n' || transcript[used - 1] == '\r')) {
		used--;
	}
	transcript[used] = 0;
	if (used) {
		append_styled("\n\n", 0);
	}
	append_styled(GLYPH_CARD, 14);
	append_styled(title, 4);
	if (detail_length) {
		char heading[160];
		snprintf(heading, sizeof(heading), " %.*s", (int)detail_length, detail);
		append_styled(heading, 14);
	}
	append_styled("\n", 4);
}

static void extension_card_end(void) {

	presentation_batch--;
	draw(0);
}

/* Append text as card body lines; style 5 is card text, 7 an error. */
static void extension_card_body(const char *text, size_t size, int style) {

	while (size && (text[size - 1] == '\n' || text[size - 1] == '\r')) {
		size--;
	}
	if (!size) {
		return;
	}
	char *copy = malloc(size + 2);
	if (!copy) {
		return;
	}
	memcpy(copy, text, size);
	copy[size] = '\n';
	copy[size + 1] = 0;
	markup_append(copy, style);
	free(copy);
}

void ui_extension_message(const char *text) {

	if (!text || !*text) {
		return;
	}
	/* A short first line reads as the heading; anything longer is body text. */
	size_t first = strcspn(text, "\n");
	int heading = first && first <= 100;
	extension_card_begin("extension", text, heading ? first : 0);
	const char *rest = heading ? text + first + (text[first] == '\n') : text;
	extension_card_body(rest, strlen(rest), 5);
	extension_card_end();
}

static void ui_extension_status_line(const char *name, const char *status, int failed) {

	append_styled(name && *name ? name : "extension", 5);
	append_styled("  ", 5);
	append_styled(status && *status ? status : "unknown", failed ? 7 : 14);
	append_styled("\n", failed ? 7 : 14);
}

static void ui_extension_diagnostics(json_object *item) {

	const char *diagnostics = field(item, "compiler_output");
	if (!*diagnostics) {
		return;
	}
	size_t bytes = strlen(diagnostics);
	if (bytes > 4096) {
		bytes = 4096;
	}
	while (bytes && ((unsigned char)diagnostics[bytes] & 0xc0) == 0x80) {
		bytes--;
	}
	extension_card_body(diagnostics, bytes, 5);
	if (diagnostics[bytes]) {
		append_styled("… compiler output truncated\n", 14);
	}
}

void ui_extension_result(json_object *answer) {

	if (!json_object_is_type(answer, json_type_object)) {
		return;
	}
	extension_card_begin("extensions", NULL, 0);

	json_object *results = json_object_object_get(answer, "results");
	if (json_object_is_type(results, json_type_array)) {
		for (size_t i = 0; i < json_object_array_length(results); i++) {
			json_object *item = json_object_array_get_idx(results, i);
			const char *name = field(item, "name");
			const char *error = field(item, "error");
			const char *status = field(item, "status");
			const char *disabled = field(item, "disabled");
			if (!*name && *disabled) {
				name = disabled;
			}
			ui_extension_status_line(name,
				*error          ? error
					: *status   ? status
					: *disabled ? "disabled"
								: "done",
				*error != 0);
			ui_extension_diagnostics(item);
		}
	} else {
		json_object *sources = json_object_object_get(answer, "sources");
		if (json_object_is_type(sources, json_type_array)) {
			for (size_t i = 0; i < json_object_array_length(sources); i++) {
				json_object *item = json_object_array_get_idx(sources, i);
				const char *error = field(item, "error");
				ui_extension_status_line(
					field(item, "name"), *error ? error : field(item, "status"), *error != 0);
			}
		} else if (*field(answer, "name")) {
			const char *error = field(answer, "error");
			const char *status = field(answer, "status");
			ui_extension_status_line(field(answer, "name"),
				*error        ? error
					: *status ? status
							  : "done",
				*error != 0);
			ui_extension_diagnostics(answer);
		} else if (*field(answer, "disabled")) {
			ui_extension_status_line(field(answer, "disabled"), "disabled", 0);
		} else if (json_object_object_get(answer, "guide")) {
			ui_extension_status_line("api", "SDK and authoring guide loaded", 0);
		} else if (*field(answer, "error")) {
			ui_extension_status_line("error", field(answer, "error"), 1);
			ui_extension_diagnostics(answer);
		}
	}
	const char *notice = field(answer, "notice");
	if (*notice) {
		ui_extension_status_line("notice", notice, 0);
	}
	extension_card_end();
}

void ui_read_preview(const char *text) {

	if (!text || !*text) {
		return;
	}
	/* Keep the result: the shared layout applies the theme's line cap and
       Ctrl+O can reveal the rest. A fixed preview would discard it forever. */
	size_t at = 0, limit = strlen(text);
	while (text[at] && at < limit) {
		size_t n = strcspn(text + at, "\n"), cap = limit - at;
		if (n > cap) {
			n = cap;
		}
		while (n && ((unsigned char)text[at + n] & 0xc0) == 0x80) {
			n--;
		}
		if (!n && text[at] != '\n') {
			break;
		}
		char *line = strndup(text + at, n);
		if (!line) {
			return;
		}
		size_t digits = strspn(line, "0123456789");
		if (digits && digits <= 20 && !strncmp(line + digits, " | ", 3)) {
			char prefix[32];
			snprintf(prefix, sizeof(prefix), "%5.*s  ", (int)digits, line);
			append_styled(prefix, 10);
			append_styled(line + digits + 3, 5);
		} else {
			append_styled(line, 5);
		}
		free(line);
		append_styled("\n", 5);
		at += n;
		if (text[at] == '\n') {
			at++;
		} else if (text[at]) {
			break;
		}
	}
	tool_output_line = 0;
}

void ui_write_preview(const char *text) {

	if (!text || !*text) {
		return;
	}
	size_t bytes = strlen(text), total_lines = 1;
	for (const char *p = text; *p; p++) {
		if (*p == '\n' && p[1]) {
			total_lines++;
		}
	}

	char summary[96];
	if (bytes < 1024) {
		snprintf(summary, sizeof(summary), "%zu %s · %zu B\n", total_lines,
			total_lines == 1 ? "line" : "lines", bytes);
	} else {
		snprintf(summary, sizeof(summary), "%zu %s · %.1f KiB\n", total_lines,
			total_lines == 1 ? "line" : "lines", bytes / 1024.0);
	}
	append_styled(summary, 14);

	/* Keep the contents for Ctrl+O; folding/layout controls the short view.
       Only the shared transcript capacity limits retention, not a separate
       28-line/8 KiB preview. Reserve room for heading/trailer and count the
       line-number/output glyph overhead too, so a write keeps its heading. */
	size_t room = styled_capture ? SIZE_MAX : LOG_SIZE - 4096;
	size_t at = 0, line = 1;
	while (text[at] && room > 32) {
		size_t len = strcspn(text + at, "\n");
		if (len > room - 32) {
			len = room - 32;
		}
		while (len && ((unsigned char)text[at + len] & 0xc0) == 0x80) {
			len--;
		}
		char *row = strndup(text + at, len);
		if (!row) {
			append_styled("… cannot allocate write display\n", 14);
			return;
		}

		char prefix[24];
		int prefix_size = snprintf(prefix, sizeof(prefix), "%4zu  ", line);
		append_styled(prefix, 13);
		append_styled(row,
			tool_syntax == TOOL_SYNTAX_PYTHON  ? 17
				: tool_syntax == TOOL_SYNTAX_C ? 42
											   : 5);
		free(row);
		append_styled("\n", tool_syntax == TOOL_SYNTAX_C ? 42 : 5);
		room -= len + (size_t)prefix_size + 4;
		at += len;
		if (text[at] == '\n') {
			at++;
		} else if (text[at]) {
			break;
		}
		line++;
	}
	if (text[at]) {
		char omitted[96];
		size_t remaining = total_lines >= line ? total_lines - line + 1 : 0;
		snprintf(omitted, sizeof(omitted), "      … %zu more %s (transcript display limit)\n",
			remaining, remaining == 1 ? "line" : "lines");
		append_styled(omitted, 14);
	}
}

void ui_diff_highlight(
	char kind, size_t line, const char *text, size_t size, size_t first, size_t end) {

	int style = kind == '-' ? 8 : kind == '+' ? 9 : kind == '@' ? 70 : 10;
	if (kind == '+') {
		tool_added++;
	}
	if (kind == '-') {
		tool_removed++;
	}
	char prefix[48], content[2049];
	if (size > 2048) {
		size = 2048;
	}
	memcpy(content, text, size);
	content[size] = 0;
	if (kind != '@') {
		/* A zero line means a saved snippet without original file positions.
           Keep the gutter blank rather than inventing file line numbers. */
		if (line) {
			snprintf(prefix, sizeof(prefix), "%3zu ", line);
		} else {
			snprintf(prefix, sizeof(prefix), "    ");
		}
		append_styled(prefix, kind == '-' ? 67 : kind == '+' ? 68 : 69);
		char marker[] = {kind, ' ', 0};
		append_styled(marker, kind == '-' ? 75 : kind == '+' ? 76 : 77);
	}
	if ((kind == '-' || kind == '+') && first < end && end <= size) {
		char saved = content[first];
		content[first] = 0;
		append_styled(content, style);
		content[first] = saved;
		saved = content[end];
		content[end] = 0;
		append_styled(content + first, kind == '-' ? 15 : 16);
		content[end] = saved;
		append_styled(content + end, style);
	} else {
		append_styled(content, style);
	}
	if (!size || text[size - 1] != '\n') {
		append_styled("\n", style);
		append_styled("\\ No newline at end of file\n", 5);
	}
	tool_output_line = 0;
}

void ui_diff_line(char kind, size_t line, const char *text, size_t size) {

	ui_diff_highlight(kind, line, text, size, 0, 0);
}

/* Replace only the visible span; the model result and saved transcript are untouched. */
static int tool_card_replace(size_t index, int expand) {

	struct tool_card *card = &tool_cards[index];
	size_t old_end = card->end;
	size_t old = old_end - card->start;
	size_t size = expand ? card->full_len : card->folded_len;
	if (used - old + size >= LOG_SIZE) {
		return 0;
	}
	int columns = ui_chat_columns ? ui_chat_columns : COLS, width = text_width(columns);
	int old_rows = !working && active && scroll_offset && width > 0 ? layout(NULL, width) : 0;
	memmove(transcript + card->start + size, transcript + card->end, used - card->end + 1);
	memmove(transcript_style + card->start + size, transcript_style + card->end, used - card->end);
	memcpy(transcript + card->start, expand ? card->full : card->folded, size);
	memcpy(transcript_style + card->start, expand ? card->styles : card->folded_styles, size);
	used = used - old + size;
	ptrdiff_t delta = (ptrdiff_t)size - (ptrdiff_t)old;
	semantic_messages_shift(card->start, old_end, delta);
	card->end = card->start + size;
	card->expanded = expand;
	for (size_t i = index + 1; i < tool_card_count; i++) {
		tool_cards[i].start = (size_t)((ptrdiff_t)tool_cards[i].start + delta);
		tool_cards[i].end = (size_t)((ptrdiff_t)tool_cards[i].end + delta);
	}
	/* All live offsets refer to the pre-replacement transcript. Comparing
       against the new end loses anchors when an earlier card expands over
       them; stale headings can then fold away intervening conversation. */
	if (tool_active && tool_header_start >= old_end) {
		tool_header_start = (size_t)((ptrdiff_t)tool_header_start + delta);
		tool_heading_end = (size_t)((ptrdiff_t)tool_heading_end + delta);
		tool_body_start = (size_t)((ptrdiff_t)tool_body_start + delta);
	}
	if (assistant_open && assistant_start >= old_end) {
		assistant_start = (size_t)((ptrdiff_t)assistant_start + delta);
	}
	transcript_revision++;
	if (old_rows) {
		scroll_offset += layout(NULL, width) - old_rows;
		if (scroll_offset < 0) {
			scroll_offset = 0;
		}
	}
	dirty = 1;
	draw(0);

	return 1;
}

/* The same folded sentence is used by live, resumed and disk-backed cards. */
static size_t tool_activity_summary(char *summary, size_t size, const unsigned counts[ACT_COUNT],
	const char *write_path, const char *failure, size_t *failure_at) {

	const char *verbs[] = {"Read", "Made", "Wrote", "Ran", "Checked", "Made", "Completed"};
	const char *nouns[] = {
		"file", "edit", "file", "shell command", "background job", "web request", "tool call"};
	size_t len = (size_t)snprintf(summary, size, "%s", GLYPH_FOLD), lead = len;
	if (len >= size) {
		return 0;
	}
	for (int i = 0; i < ACT_COUNT; i++) {
		if (counts[i]) {
			int n = snprintf(summary + len, size - len, "%s%s %u %s%s", len > lead ? ", " : "",
				verbs[i], counts[i], nouns[i], counts[i] == 1 ? "" : "s");
			if (n < 0 || (size_t)n >= size - len - 1) {
				return 0;
			}
			if (len > lead) {
				summary[len + 2] = (char)tolower((unsigned char)summary[len + 2]);
			}
			len += (size_t)n;
		}
	}
	if (write_path && *write_path) {
		int n = snprintf(summary + lead, size - lead, "Wrote %s", write_path);
		if (n > 0 && (size_t)n < size - lead - 1) {
			len = lead + (size_t)n;
		}
	}
	*failure_at = len;
	if (failure) {
		int n = snprintf(summary + len, size - len - 1, " · ✗ %.200s", failure);
		if (n > 0 && (size_t)n < size - len - 1) {
			len += (size_t)n;
		}
		char *newline = memchr(summary + *failure_at, '\n', len - *failure_at);
		if (newline) {
			len = (size_t)(newline - summary);
		}
	}
	if (len + 1 >= size) {
		return 0;
	}
	summary[len++] = '\n';
	summary[len] = 0;

	return len;
}

static void tool_card_fold(size_t start, size_t end, const char *failure, int fixed) {

	if (start >= end || end > used) {
		return;
	}
	/* Store the detailed calls separately; the default projection is a single
       activity sentence. Only adjacent successes merge, never across prose or
       errors. Allocate the replacement completely before changing anything. */
	struct tool_card *previous = tool_card_count ? &tool_cards[tool_card_count - 1] : NULL;
	int merge = !fixed && !failure && previous && !previous->fixed && !previous->failed &&
		previous->end <= start && previous->job_count < 64 &&
		semantic_tools_mergeable(previous->semantic, tool_pending_semantic) &&
		previous->full_len + 1 + end - start < LOG_SIZE;
	if (merge) {
		for (size_t i = previous->end; i < start; i++) {
			if (transcript[i] != '\n' && transcript[i] != '\r') {
				merge = 0;
				break;
			}
		}
	}
	struct tool_card card = {0};
	if (merge) {
		memcpy(card.counts, previous->counts, sizeof(card.counts));
		memcpy(card.jobs, previous->jobs, sizeof(card.jobs));
		card.job_count = previous->job_count;
	}
	int count = 1;
	if (tool_category == ACT_JOB) {
		for (size_t i = 0; i < card.job_count; i++) {
			if (card.jobs[i] == tool_job_id) {
				count = 0;
			}
		}
		if (count) {
			card.jobs[card.job_count++] = tool_job_id;
		}
	}
	card.counts[tool_category] += count;
	char summary[512];
	size_t failure_at;
	size_t len = tool_activity_summary(summary, sizeof(summary), card.counts,
		!merge && tool_category == ACT_WRITE ? tool_detail : NULL, failure, &failure_at);
	if (!len) {
		return;
	}
	size_t prefix = merge ? previous->full_len + 1 : 0;
	card.start = merge ? previous->start : start;
	card.end = end;
	card.full_len = prefix + end - start;
	card.fixed = fixed;
	card.folded_len = fixed ? card.full_len : len;
	if (used - (end - card.start) + len >= LOG_SIZE) {
		return;
	}
	card.full = malloc(card.full_len);
	card.styles = malloc(card.full_len);
	card.folded = malloc(card.folded_len);
	card.folded_styles = malloc(card.folded_len);
	if (!card.full || !card.styles || !card.folded || !card.folded_styles) {
		free(card.full);
		free(card.styles);
		free(card.folded);
		free(card.folded_styles);
		return;
	}
	if (merge) {
		memcpy(card.full, previous->full, previous->full_len);
		memcpy(card.styles, previous->styles, previous->full_len);
		card.full[prefix - 1] = '\n';
		card.styles[prefix - 1] = 73;
	}
	memcpy(card.full + prefix, transcript + start, end - start);
	memcpy(card.styles + prefix, transcript_style + start, end - start);
	if (fixed) {
		memcpy(card.folded, card.full, card.full_len);
		memcpy(card.folded_styles, card.styles, card.full_len);
		card.expanded = 1;
	} else {
		memcpy(card.folded, summary, len);
		memset(card.folded_styles, 45, len);
		if (failure) {
			memset(card.folded_styles + failure_at, 7, len - failure_at - 1);
		}
	}
	card.failed = failure != NULL;
	/* Keep complete, bounded renderer groups only. The plain activity count
       can keep growing even when its optional renderer payload cannot. */
	if (tool_pending_semantic &&
		(!merge || semantic_tools_payload_mergeable(previous->semantic, tool_pending_semantic))) {
		card.semantic = json_object_new_array();
		for (size_t i = 0; merge && i < json_object_array_length(previous->semantic); i++) {
			json_object_array_add(
				card.semantic, json_object_get(json_object_array_get_idx(previous->semantic, i)));
		}
		json_object_array_add(card.semantic, json_object_get(tool_pending_semantic));
	}
	if (merge) {
		tool_card_release(previous);
		*previous = card;
	} else {
		if (tool_card_count == TOOL_CARD_LIMIT) {
			struct tool_card *old = &tool_cards[0];
			tool_card_release(old);
			memmove(tool_cards, tool_cards + 1, (TOOL_CARD_LIMIT - 1) * sizeof(*tool_cards));
			tool_card_count--;
		}
		tool_cards[tool_card_count++] = card;
	}
	if (fixed) {
		transcript_revision++;
		return;
	}
	tool_card_replace(tool_card_count - 1, 0);
	if (tool_details) {
		tool_card_replace(tool_card_count - 1, 1);
	}
}

static void tool_card_toggle(void) {

	int foldable = 0;
	for (size_t i = 0; i < tool_card_count; i++) {
		if (!tool_cards[i].fixed) {
			foldable = 1;
			break;
		}
	}
	if (!foldable) {
		tool_details = !tool_details;
		return;
	}
	/* Ctrl+O is a transcript-wide toggle, not a last-visible-card toggle.
       If the display buffer only allowed a partial expansion, the next press
       must still collapse it rather than getting stuck retrying expansion. */
	int expanding = 1;
	for (size_t i = 0; i < tool_card_count; i++) {
		if (!tool_cards[i].fixed && tool_cards[i].expanded) {
			expanding = 0;
			break;
		}
	}
	tool_details = expanding;
	/* Preserve distance from the bottom and paint only the completed toggle. */
	int bottom_offset = scroll_offset;
	scroll_offset = 0;
	presentation_batch++;
	for (size_t i = tool_card_count; i-- > 0;) {
		if (!tool_cards[i].fixed && tool_cards[i].expanded != expanding) {
			tool_card_replace(i, expanding);
		}
	}
	presentation_batch--;
	scroll_offset = bottom_offset;
	draw(1);
}

void ui_tool_end(const char *summary, int failed) {

	semantic_tool_finish(failed);
	if (tool_shell(tool_live_name)) {
		shell_activity_state = failed ? "failed" : "done";
	}
	command_files_reset();
	presentation_batch++;
	context_stale = 1; /* tool output enters the next request, not the last usage report */
	if (tool_output_line) {
		append_styled("\n", 5);
	}
	size_t output_end = used;
	int has_heading = tool_heading_valid;
	/* Empty successful polls are scheduler noise, not useful transcript cards. */
	if (tool_poll && !failed && output_end == tool_body_start && tool_header_start <= used) {
		used = tool_header_start;
		if (tool_pending_semantic) {
			json_object_put(tool_pending_semantic);
		}
		tool_pending_semantic = NULL;
		semantic_live_clear();
		transcript[used] = 0;
		transcript_revision++;
		tool_heading_valid = tool_active = tool_output_line = tool_bash = tool_poll = tool_edit = 0;
		dirty = 1;
		presentation_batch--;
		draw(0);
		return;
	}
	if (failed) {
		append_styled("✗ ", 7);
		append_styled(summary, 7);
		append_styled("\n", 7);
	}
	if (tool_heading_valid && tool_heading_end < used && transcript[tool_heading_end] == '\n') {
		char duration[128] = "";
		unsigned char styles[128];
		size_t n = 0;
		if (tool_edit && (tool_added || tool_removed)) {
			n = (size_t)snprintf(duration, sizeof(duration), "  +%zu", tool_added);
			memset(styles, 47, n);
			size_t removed =
				(size_t)snprintf(duration + n, sizeof(duration) - n, " −%zu", tool_removed);
			memset(styles + n, 46, removed);
			n += removed;
		}
		if (!tool_historical) {
			double elapsed = now() - tool_started;
			size_t timing = elapsed < 1
				? (size_t)snprintf(duration + n, sizeof(duration) - n, " · %.0fms", elapsed * 1000)
				: (size_t)snprintf(duration + n, sizeof(duration) - n, " · %.1fs", elapsed);
			memset(styles + n, 18, timing);
			n += timing;
		}
		if (used + n < LOG_SIZE) {
			memmove(transcript + tool_heading_end + n, transcript + tool_heading_end,
				used - tool_heading_end + 1);
			memmove(transcript_style + tool_heading_end + n, transcript_style + tool_heading_end,
				used - tool_heading_end);
			memcpy(transcript + tool_heading_end, duration, n);
			memcpy(transcript_style + tool_heading_end, styles, n);
			used += n;
			tool_body_start += n;
			output_end += n;
		}
	}
	tool_heading_valid = 0;
	if (!tool_edit && has_heading) {
		size_t glyph = tool_header_start;
		while (glyph < used && transcript_style[glyph] == 71) {
			glyph++;
		}
		if (glyph + 3 <= used && !memcmp(transcript + glyph, GLYPH_CARD, 3)) {
			memset(transcript_style + glyph, failed ? 44 : 43, 3);
		}
	}
	if (has_heading) {
		append_styled("\xee\x80\x86", 72); /* end of this tool card */
	}
	/* File edits are reviewable changes, not routine activity to hide. A
       failure folds to a line that names it (Ctrl+O shows the output). */
	if (has_heading && (!tool_edit || tool_pending_semantic)) {
		tool_card_fold(tool_header_start, used, failed ? summary : NULL, tool_edit);
	}
	if (tool_pending_semantic) {
		json_object_put(tool_pending_semantic);
	}
	tool_pending_semantic = NULL;
	semantic_live_clear();
	tool_active = tool_output_line = tool_bash = tool_poll = tool_edit = 0;
	append_styled(
		used && transcript[used - 1] == '\n' ? "\n" : "\n\n", 73); /* theme-controlled spacing */
	presentation_batch--;
	draw(0);
}
