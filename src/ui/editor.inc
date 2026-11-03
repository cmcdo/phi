/* Editing the prompt: recalling earlier messages, Vim motions and
   operators, yank and paste, block and visual modes, path completion and
   the external editor. Included by ui.c after the theme commands. */

static int draft_has_text(void) {

	size_t start = input[0] == L'!' ? (length > 1 && input[1] == L'!' ? 2 : 1) : 0;
	for (size_t i = start; i < length; i++) {
		if (!iswspace(input[i])) {
			return 1;
		}
	}

	return 0;
}

/* Up on the prompt's top line recalls your earlier messages; Down on its
   bottom line goes forward again, ending at the draft you were writing. */
#define RECALL_LIMIT 200
static char *recall[RECALL_LIMIT];
static int recall_count, recall_at = -1; /* -1: the draft, not a recalled message */
static wchar_t *recall_draft;
static size_t recall_draft_length;

static void editor_storage_close(void) {

	free(recall_draft);
	recall_draft = NULL;
	recall_draft_length = 0;
	recall_at = -1;
}

static void recall_add(const char *text) {

	recall_at = -1;
	if (!text || !text[strspn(text, " \t\r\n")]) {
		return;
	}
	if (recall_count && !strcmp(recall[recall_count - 1], text)) {
		return;
	}
	char *copy = strdup(text);
	if (!copy) {
		return;
	}
	if (recall_count == RECALL_LIMIT) {
		free(recall[0]);
		memmove(recall, recall + 1, (RECALL_LIMIT - 1) * sizeof(*recall));
		recall_count--;
	}
	recall[recall_count++] = copy;
}

static void normal_cursor(void);

/* -1: an older message, +1: a newer one. Returns 0 when there is none. */
static int recall_move(int direction) {

	if (editor_workspace == 2) {
		return 0; /* A reply/rename must not recall another target's draft. */
	}
	int next;
	if (direction < 0) {
		if (!recall_count || recall_at == 0) {
			return recall_at == 0;
		}
		if (recall_at < 0) {
			wchar_t *saved = wide_copy(input, length);
			if (!saved) {
				return 1;
			}
			free(recall_draft);
			recall_draft = saved;
			recall_draft_length = length;
		}
		next = recall_at < 0 ? recall_count - 1 : recall_at - 1;
	} else {
		if (recall_at < 0) {
			return 0;
		}
		next = recall_at + 1;
	}
	if (next >= recall_count) {
		if (!input_reserve(recall_draft_length)) {
			return 1;
		}
		memcpy(input, recall_draft, (recall_draft_length + 1) * sizeof(*input));
		length = cursor = recall_draft_length;
		recall_at = -1;
	} else {
		size_t n;
		wchar_t *text = wide_decode(recall[next], &n);
		if (!text) {
			return 1;
		}
		if (!input_reserve(n)) {
			free(text);
			return 1;
		}
		memcpy(input, text, (n + 1) * sizeof(*input));
		free(text);
		length = n;
		/* Going back, stay on the top line so Up keeps going; forward, the bottom. */
		cursor = direction < 0 ? 0 : n;
		recall_at = next;
	}
	visual_mode = delete_pending = 0;
	input_view = 0;
	if (normal_mode) {
		normal_cursor();
	}

	return 1;
}

static char *take_draft(void) {

	if (!draft_has_text()) {
		return NULL;
	}
	size_t bytes = wcstombs(NULL, input, 0);
	if (bytes == (size_t)-1) {
		return NULL;
	}
	char *result = malloc(bytes + 1);
	if (!result) {
		return NULL;
	}
	wcstombs(result, input, bytes + 1);
	recall_add(result);
	size_t shell_prefix = input[0] == L'!' ? (length > 1 && input[1] == L'!' ? 2 : 1) : 0;
	length = cursor = shell_prefix;
	if (shell_prefix) {
		input[0] = L'!';
		if (shell_prefix == 2) {
			input[1] = L'!';
		}
	}
	input[length] = 0;
	recovery_save();
	undo_clear();
	normal_mode = visual_mode = delete_pending = input_view = 0;
	vim_pending_clear();
	last_replace_count = 0;
	scroll_offset = 0;
	command_files_reset();
	dirty = 1;

	return result;
}

static size_t line_start(size_t at) {

	while (at && input[at - 1] != L'\n') {
		at--;
	}

	return at;
}

static size_t line_end(size_t at) {

	while (at < length && input[at] != L'\n') {
		at++;
	}

	return at;
}

static int block_selected(size_t at) {

	size_t as = line_start(visual_anchor), cs = line_start(cursor);
	size_t top = as < cs ? as : cs, bottom = as > cs ? as : cs;
	size_t ac = visual_anchor - as, cc = cursor - cs, left = ac < cc ? ac : cc,
		   right = ac > cc ? ac : cc;
	size_t row = line_start(at), column = at - row;

	return row >= top && row <= bottom && column >= left && column <= right && at < line_end(row);
}

static void normal_cursor(void) {

	if (cursor > length) {
		cursor = length;
	}
	if (cursor == line_end(cursor) && cursor > line_start(cursor)) {
		cursor--;
	}
}

static void insert_char(wchar_t ch) {

	if (length == SIZE_MAX || !input_reserve(length + 1)) {
		return;
	}
	memmove(input + cursor + 1, input + cursor, (length - cursor + 1) * sizeof(*input));
	input[cursor++] = ch;
	length++;
}

static void vertical(int direction) {

	size_t start = line_start(cursor), column = cursor - start, target;
	if (direction < 0) {
		if (!start) {
			return;
		}
		target = line_start(start - 1);
	} else {
		target = line_end(cursor);
		if (target == length) {
			return;
		}
		target++;
	}
	size_t end = line_end(target);
	cursor = target + column < end ? target + column : end;
	if (normal_mode) {
		normal_cursor();
	}
}

/* Arrow keys follow painted soft rows; Vim j/k remain logical-line motions. */
static int visual_vertical(int direction) {

	size_t target;
	if (!draft_vertical_target(direction, &target)) {
		return 0;
	}
	cursor = target;
	if (normal_mode) {
		normal_cursor();
	}

	return 1;
}

static int word_class(wchar_t ch) {

	return iswspace(ch) ? 0 : (iswalnum(ch) || ch == L'_') ? 1 : 2;
}

static size_t next_word(size_t at) {

	if (at < length) {
		int group = word_class(input[at]);
		while (at < length && word_class(input[at]) == group) {
			at++;
		}
		while (at < length && iswspace(input[at])) {
			at++;
		}
	}

	return at;
}

static size_t word_end(size_t at) {

	if (at < length && iswspace(input[at])) {
		while (at < length && iswspace(input[at])) {
			at++;
		}
	}
	if (at >= length) {
		return length;
	}
	int group = word_class(input[at]);
	while (at + 1 < length && word_class(input[at + 1]) == group) {
		at++;
	}

	return at;
}

static void find_motion(wchar_t wanted, int direction, int till) {

	size_t start = line_start(cursor), end = line_end(cursor), at = cursor;
	if (direction > 0) {
		while (++at < end && input[at] != wanted) {
			/* The condition advances to the target character or the line end. */
			;
		}
		if (at < end) {
			cursor = till && at ? at - 1 : at;
		}
	} else {
		while (at > start) {
			at--;
			if (input[at] == wanted) {
				cursor = till && at < end ? at + 1 : at;
				break;
			}
		}
	}
}

static size_t previous_word(size_t at) {

	if (at) {
		at--;
	}
	while (at && iswspace(input[at])) {
		at--;
	}
	int group = word_class(input[at]);
	while (at && word_class(input[at - 1]) == group) {
		at--;
	}

	return at;
}

static void delete_range(size_t begin, size_t end) {

	memmove(input + begin, input + end, (length - end + 1) * sizeof(*input));
	length -= end - begin;
	cursor = begin;
	if (normal_mode) {
		normal_cursor();
	}
}

static int copy_range(size_t begin, size_t end) {

	/* Leave both the draft and previous register intact on allocation failure. */
	if (!wide_reserve(&yank, &yank_capacity, end - begin + 2, yank_initial)) {
		return 0;
	}
	yank_linewise = yank_blockwise = 0;
	yank_length = end - begin;
	memcpy(yank, input + begin, yank_length * sizeof(*yank));
	yank[yank_length] = 0;

	return 1;
}

/* OSC 52 sets the terminal's clipboard without a platform subprocess.
   Terminals may disable this; the internal yank register still works. */
static void clipboard_yank(void) {

	size_t bytes = wcstombs(NULL, yank, 0);
	if (bytes == (size_t)-1) {
		return;
	}
	unsigned char *text = malloc(bytes + 1);
	if (!text) {
		return;
	}
	wcstombs((char *)text, yank, bytes + 1);
	static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	fputs("\033]52;c;", stdout);
	for (size_t i = 0; i < bytes; i += 3) {
		unsigned a = text[i], b = i + 1 < bytes ? text[i + 1] : 0;
		unsigned c = i + 2 < bytes ? text[i + 2] : 0;
		fputc(base64[a >> 2], stdout);
		fputc(base64[((a & 3) << 4) | (b >> 4)], stdout);
		fputc(i + 1 < bytes ? base64[((b & 15) << 2) | (c >> 6)] : '=', stdout);
		fputc(i + 2 < bytes ? base64[c & 63] : '=', stdout);
	}
	fputs("\033\\", stdout);
	fflush(stdout);
	free(text);
}

static int copy_line(void) {

	size_t begin = line_start(cursor), end = line_end(cursor);
	if (!copy_range(begin, end)) {
		return 0;
	}
	yank[yank_length++] = L'\n';
	yank[yank_length] = 0;
	yank_linewise = 1;
	yank_blockwise = 0;
	clipboard_yank();

	return 1;
}

static void cut_range(size_t begin, size_t end) {

	if (begin == end || !copy_range(begin, end)) {
		return;
	}
	clipboard_yank();
	delete_range(begin, end);
}

static void paste_yank(void) {

	if (!yank_length || yank_length > SIZE_MAX - length - 1 ||
		!input_reserve(length + yank_length + 1)) {
		return;
	}
	if (yank_blockwise) {
		size_t row = line_start(cursor), column = cursor - row;
		const wchar_t *part = yank;
		while (*part) {
			const wchar_t *nl = wcschr(part, L'\n');
			size_t n = nl ? (size_t)(nl - part) : wcslen(part), end = line_end(row),
				   at = row + column < end ? row + column : end;
			if (!input_reserve(length + n)) {
				return;
			}
			memmove(input + at + n, input + at, (length - at + 1) * sizeof(*input));
			memcpy(input + at, part, n * sizeof(*input));
			length += n;
			if (!nl) {
				break;
			}
			part = nl + 1;
			end = line_end(row);
			if (end == length) {
				if (!input_reserve(length + 1)) {
					return;
				}
				input[length++] = L'\n';
				input[length] = 0;
			}
			row = end + 1;
		}
		cursor = line_start(cursor) + column;
		normal_cursor();
		return;
	}
	size_t at = cursor;
	if (yank_linewise) {
		at = line_end(cursor);
		if (at < length) {
			at++;
		} else if (length) {
			/* At EOF, put the separator before the line, not after it. */
			input[length++] = L'\n';
			memcpy(input + length, yank, (yank_length - 1) * sizeof(*input));
			cursor = length;
			length += yank_length - 1;
			input[length] = 0;
			normal_cursor();
			return;
		}
	} else if (at < line_end(at)) {
		at++;
	}
	memmove(input + at + yank_length, input + at, (length - at + 1) * sizeof(*input));
	memcpy(input + at, yank, yank_length * sizeof(*input));
	length += yank_length;
	cursor = yank_linewise ? at : at + yank_length - 1;
	normal_cursor();
}

static void block_action(wint_t key) {

	size_t anchor_row = line_start(visual_anchor);
	size_t cursor_row = line_start(cursor);
	size_t top = anchor_row < cursor_row ? anchor_row : cursor_row;
	size_t bottom = anchor_row > cursor_row ? anchor_row : cursor_row;
	size_t anchor_column = visual_anchor - anchor_row;
	size_t cursor_column = cursor - cursor_row;
	size_t left = anchor_column < cursor_column ? anchor_column : cursor_column;
	size_t right = anchor_column > cursor_column ? anchor_column : cursor_column;

	if (!wide_reserve(&yank, &yank_capacity, length + 2, yank_initial)) {
		return;
	}
	yank_length = 0;
	yank_linewise = 0;
	yank_blockwise = 1;
	for (size_t row = top;;) {
		size_t end = line_end(row);
		size_t begin = row + left < end ? row + left : end;
		size_t finish = row + right + 1 < end ? row + right + 1 : end;
		if (row != top) {
			yank[yank_length++] = L'\n';
		}
		size_t count = finish - begin;
		memcpy(yank + yank_length, input + begin, count * sizeof(*yank));
		yank_length += count;
		if (row == bottom || end == length) {
			break;
		}
		row = end + 1;
	}
	yank[yank_length] = 0;
	clipboard_yank();
	visual_mode = 0;
	if (key == L'y') {
		cursor = top + left < line_end(top) ? top + left : line_end(top);
		return;
	}

	/* Every character may be a newline. Delete bottom-up so offsets stay valid. */
	for (size_t row = bottom;;) {
		size_t previous = row ? line_start(row - 1) : 0;
		size_t end = line_end(row);
		size_t begin = row + left < end ? row + left : end;
		size_t finish = row + right + 1 < end ? row + right + 1 : end;
		delete_range(begin, finish);
		if (row == top) {
			break;
		}
		row = previous;
	}
	cursor = top + left < line_end(top) ? top + left : line_end(top);
	if (key == L'c') {
		normal_mode = 0;
	}
}

static void visual_action(wint_t key) {

	if (visual_mode == 2) {
		block_action(key);
		return;
	}
	size_t begin, end;
	selection(&begin, &end);
	if (!copy_range(begin, end)) {
		return;
	}
	clipboard_yank();
	visual_mode = 0;
	if (key == L'y') {
		cursor = begin;
		normal_cursor();
		return;
	}
	if (key == L'c') {
		normal_mode = 0;
	}
	delete_range(begin, end);
}

static int complete_path(void) {

	size_t begin = cursor;
	while (begin && !iswspace(input[begin - 1])) {
		begin--;
	}
	if (begin == cursor) {
		return 0;
	}

	wchar_t wide[INPUT_SIZE];
	size_t characters = cursor - begin;
	if (characters >= INPUT_SIZE) {
		return 0;
	}
	memcpy(wide, input + begin, characters * sizeof(*wide));
	wide[characters] = 0;
	char token[4096];
	size_t bytes = wcstombs(NULL, wide, 0);
	if (bytes == (size_t)-1 || bytes >= sizeof(token)) {
		return 0;
	}
	if (wcstombs(token, wide, sizeof(token)) != bytes) {
		return 0;
	}
	if (token[0] == '@') {
		return 0;
	}

	char expanded[4096];
	const char *home = getenv("HOME");
	int size = token[0] == '~' && token[1] == '/' && home
		? snprintf(expanded, sizeof(expanded), "%s/%s", home, token + 2)
		: snprintf(expanded, sizeof(expanded), "%s", token);
	if (size < 0 || (size_t)size >= sizeof(expanded)) {
		return 0;
	}
	char *slash = strrchr(expanded, '/');
	const char *directory = ".", *prefix = expanded;
	if (slash) {
		*slash = 0;
		directory = *expanded ? expanded : "/";
		prefix = slash + 1;
	}
	size_t prefix_size = strlen(prefix);
	DIR *stream = opendir(directory);
	if (!stream) {
		return 0;
	}
	int fd = dirfd(stream);
	if (fd < 0) {
		closedir(stream);
		return 0;
	}

	char common[4096] = "";
	size_t matches = 0;
	int is_directory = 0;
	struct dirent *entry;
	while ((entry = readdir(stream))) {
		if (strncmp(entry->d_name, prefix, prefix_size) ||
			(!prefix_size && entry->d_name[0] == '.')) {
			continue;
		}
		if (!matches) {
			snprintf(common, sizeof(common), "%s", entry->d_name);
			struct stat status;
			is_directory = !fstatat(fd, entry->d_name, &status, 0) && S_ISDIR(status.st_mode);
		} else {
			size_t i = 0;
			while (common[i] && entry->d_name[i] && common[i] == entry->d_name[i]) {
				i++;
			}
			common[i] = 0;
			is_directory = 0;
		}
		matches++;
	}
	closedir(stream);
	if (!matches || strlen(common) < prefix_size) {
		return 0;
	}

	wchar_t suffix[INPUT_SIZE];
	size_t added = mbstowcs(suffix, common + prefix_size, INPUT_SIZE);
	if (added == (size_t)-1 || added >= INPUT_SIZE ||
		!input_reserve(length + added + is_directory)) {
		return 0;
	}
	for (size_t i = 0; i < added; i++) {
		insert_char(suffix[i]);
	}
	if (is_directory) {
		insert_char(L'/');
	}

	return 1;
}

/* Ctrl+G: the prompt in $VISUAL (else $EDITOR, else nano); what is saved
   there becomes the prompt. */
static void external_editor(void) {

	char path[] = "/tmp/phi-edit-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		return;
	}
	size_t bytes = wcstombs(NULL, input, 0);
	char *utf = bytes == (size_t)-1 ? NULL : malloc(bytes + 1);
	if (bytes == (size_t)-1 || !utf) {
		free(utf);
		close(fd);
		unlink(path);
		return;
	}
	wcstombs(utf, input, bytes + 1);
	utf[bytes] = 0;
	size_t off = 0;
	while (off < bytes) {
		ssize_t n = write(fd, utf + off, bytes - off);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		off += (size_t)n;
	}
	free(utf);
	close(fd);

	def_prog_mode();
	endwin();
	pid_t pid = fork();
	if (!pid) {
		setenv("PHI_EDIT_FILE", path, 1);
		execl("/bin/sh", "sh", "-c", "exec ${VISUAL:-${EDITOR:-nano}} \"$PHI_EDIT_FILE\"",
			(char *)NULL);
		_exit(127);
	}
	int status;
	while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	reset_prog_mode();
	/* The editor may have changed the terminal's colours. */
	short restored_colors[THEME_COUNT];
	struct skin restored_skin;
	char restored_error[256];
	if (theme_current_known) {
		theme_apply(theme_current);
	} else if (theme_read(
				   restored_colors, &restored_skin, restored_error, sizeof(restored_error)) >= 0) {
		skin_move(&skin, &restored_skin);
		theme_apply(restored_colors);
	}
	clearok(stdscr, TRUE);

	fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 &&
		(uintmax_t)st.st_size < SIZE_MAX) {
		char *text = malloc((size_t)st.st_size + 1);
		if (text) {
			size_t got = 0, size = (size_t)st.st_size;
			while (got < size) {
				ssize_t n = read(fd, text + got, size - got);
				if (n < 0 && errno == EINTR) {
					continue;
				}
				if (n <= 0) {
					break;
				}
				got += (size_t)n;
			}
			if (got == size) {
				text[got] = 0;
				size_t n;
				wchar_t *wide = memchr(text, 0, got) ? NULL : wide_decode(text, &n);
				if (wide && input_reserve(n)) {
					memcpy(input, wide, (n + 1) * sizeof(*input));
					length = cursor = n;
					normal_mode = visual_mode = delete_pending = 0;
				}
				free(wide);
			}
			free(text);
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	unlink(path);
	dirty = 1;
}

static int clipboard_insert(const char *text) {

	size_t count;
	wchar_t *pasted = wide_decode(text, &count);
	if (!pasted) {
		return 0;
	}
	/* Normalize in place, then move the suffix once, not once per character. */
	size_t added = 0;
	for (size_t i = 0; i < count; i++) {
		wchar_t ch = pasted[i];
		if (ch == L'\r') {
			if (i + 1 < count && pasted[i + 1] == L'\n') {
				i++;
			}
			ch = L'\n';
		}
		if (ch == L'\t') {
			ch = L' ';
		}
		if (ch == L'\n' || (ch >= 32 && wcwidth(ch) >= 0)) {
			pasted[added++] = ch;
		}
	}
	size_t begin = cursor, end = cursor;
	if (visual_mode) {
		selection(&begin, &end);
	}
	size_t retained = length - (end - begin);
	if (added > SIZE_MAX - retained || !input_reserve(retained + added)) {
		free(pasted);
		return 0;
	}
	memmove(input + begin + added, input + end, (length - end + 1) * sizeof(*input));
	memcpy(input + begin, pasted, added * sizeof(*input));
	length = retained + added;
	cursor = begin + added;
	visual_mode = delete_pending = normal_mode = 0;
	free(pasted);

	return 1;
}

static void paste_clipboard(void) {

	char *text = clipboard_paste();
	if (!text) {
		ui_append("\n[Clipboard unavailable: requires wl-paste on Wayland or xclip on X11.]\n");
		return;
	}
	if (!clipboard_insert(text)) {
		ui_append("\n[Clipboard text is invalid UTF-8 or could not be allocated.]\n");
	}
	free(text);
}

/* r is a single change, not Insert mode. A count may not cross a real
   newline. Consume composing marks with the character being replaced. */
static void replace_character(wchar_t ch, size_t count) {

	if (visual_mode) {
		size_t begin, end;
		selection(&begin, &end);
		if (visual_mode == 2) {
			size_t a = line_start(visual_anchor), b = line_start(cursor);
			size_t ac = visual_anchor - a, bc = cursor - b;
			size_t top = a < b ? a : b, bottom = a > b ? a : b, left = ac < bc ? ac : bc,
				   right = ac > bc ? ac : bc;
			begin = top + (left < line_end(top) - top ? left : line_end(top) - top);
			for (size_t row = top;;) {
				size_t eol = line_end(row), width = eol - row;
				size_t first = left < width ? row + left : eol,
					   last = right < width ? row + right + 1 : eol;
				for (size_t i = first; i < last; i++) {
					input[i] = ch;
				}
				if (row == bottom) {
					break;
				}
				row = eol + 1;
			}
		} else {
			while (end < length && input[end] != L'\n' && wcwidth(input[end]) == 0) {
				end++;
			}
			size_t out = begin;
			for (size_t i = begin; i < end;) {
				if (input[i] == L'\n') {
					input[out++] = input[i++];
					continue;
				}
				input[out++] = ch;
				i++;
				while (i < end && input[i] != L'\n' && wcwidth(input[i]) == 0) {
					i++;
				}
			}
			memmove(input + out, input + end, (length - end + 1) * sizeof(*input));
			length -= end - out;
		}
		replace_start = begin;
		cursor = begin;
		visual_mode = 0;
		normal_cursor();
		last_replace_count = 0;
		replace_changed = 1;
		return;
	}
	size_t end = cursor, eol = line_end(cursor);
	if (!count || count > eol - cursor) {
		return;
	}
	for (size_t i = 0; i < count; i++) {
		if (end == eol) {
			return;
		}
		end++;
		while (end < eol && wcwidth(input[end]) == 0) {
			end++;
		}
	}
	replace_start = cursor;
	int newline = ch == L'\r' || ch == L'\n';
	size_t added = newline ? 1 : count;
	memmove(input + cursor + added, input + end, (length - end + 1) * sizeof(*input));
	for (size_t i = 0; i < added; i++) {
		input[cursor + i] = newline ? L'\n' : ch;
	}
	length = length - (end - cursor) + added;
	cursor += newline ? 1 : added - 1;
	normal_cursor();
	last_replace_character = newline ? L'\n' : ch;
	last_replace_count = count;
	replace_changed = 1;
}

static int g_pending; /* g seen: g- / g+ travel the undo tree in time, gg goes to the start */

static void normal_key(wint_t key) {

	if (g_pending) {
		g_pending = 0;
		if (key == L'-') {
			undo_time(-1);
		} else if (key == L'+') {
			undo_time(1);
		} else if (key == L'g') {
			cursor = 0;
		}
		return;
	}
	if (find_pending) { /* f/F/t/T then a character */
		find_character = (wchar_t)key;
		find_direction = find_pending == L'f' || find_pending == L't' ? 1 : -1;
		find_till = find_pending == L't' || find_pending == L'T';
		find_pending = 0;
		find_motion(find_character, find_direction, find_till);
		return;
	}
	if (!visual_mode && !delete_pending && key >= L'0' && key <= L'9' &&
		(vim_count || key != L'0')) {
		size_t digit = (size_t)(key - L'0');
		vim_count = vim_count > (SIZE_MAX - digit) / 10 ? SIZE_MAX : vim_count * 10 + digit;
		return;
	}
	size_t count = vim_count;
	vim_count = 0;
	if (key == L'r' && !delete_pending) {
		replace_pending = 1;
		replace_count = count ? count : 1;
		return;
	}
	if (key == L'f' || key == L'F' || key == L't' || key == L'T') {
		find_pending = (int)key;
		return;
	}
	if (key == L';' && find_character) {
		find_motion(find_character, find_direction, find_till);
		return;
	}
	if (key == L',' && find_character) {
		find_motion(find_character, -find_direction, find_till);
		return;
	}
	if (key == L'v') {
		visual_mode = !visual_mode;
		visual_anchor = cursor;
		delete_pending = 0;
		return;
	}
	if (visual_mode) {
		if (key == L'd' || key == L'x' || key == L'c' || key == L'y') {
			visual_action(key);
			return;
		}
		/* Only motions act in Visual mode; editing requires c/d/x. */
		if (key != L'h' && key != L'l' && key != L'j' && key != L'k' && key != L'w' &&
			key != L'e' && key != L'b' && key != L'0' && key != L'$' && key != L'f' &&
			key != L'F' && key != L't' && key != L'T' && key != L';' && key != L',') {
			return;
		}
	}
	if (delete_pending) {
		int op = delete_pending;
		delete_pending = 0;
		if (key == L'h' || key == L'l') {
			size_t begin = cursor, end = cursor;
			if (key == L'h' && cursor > line_start(cursor)) {
				begin--;
			}
			if (key == L'l' && cursor < line_end(cursor)) {
				end++;
			}
			if (begin < end) {
				if (op == 'd') {
					cut_range(begin, end);
				} else if (copy_range(begin, end)) {
					clipboard_yank();
				}
			}
		} else if (key == L'w' || key == L'$') {
			size_t end = key == L'w' ? next_word(cursor) : line_end(cursor);
			if (op == 'd') {
				cut_range(cursor, end);
			} else if (copy_range(cursor, end)) {
				clipboard_yank();
			}
		} else if (key == (wint_t)op) {
			if (!copy_line()) {
				return;
			}
			if (op == 'd') {
				size_t begin = line_start(cursor), end = line_end(cursor);
				if (end < length) {
					end++;
				} else if (begin) {
					begin--;
				}
				delete_range(begin, end);
				cursor = line_start(cursor);
			}
		}
		return;
	}
	switch (key) {
	case L'.':
		if (last_replace_count) {
			replace_character(last_replace_character, count ? count : last_replace_count);
		}
		break;
	case L'u':
		undo_step(-1);
		break;
	case 18:
		undo_step(1);
		break;
	case L'U':
		undo_tree_view();
		break;
	case L'g':
		g_pending = 1;
		break;
	case L'h':
	case 8:
	case 127:
		if (cursor > line_start(cursor)) {
			cursor--;
		}
		break;
	case L'l':
		if (cursor + 1 < line_end(cursor)) {
			cursor++;
		}
		break;
	case L'j':
		vertical(1);
		break;
	case L'k':
		vertical(-1);
		break;
	case L'w':
		cursor = next_word(cursor);
		normal_cursor();
		break;
	case L'e':
		cursor = word_end(cursor);
		normal_cursor();
		break;
	case L'b':
		cursor = previous_word(cursor);
		break;
	case L'0':
		cursor = line_start(cursor);
		break;
	case L'$':
		cursor = line_end(cursor);
		normal_cursor();
		break;
	case L'G':
		cursor = line_start(length);
		normal_cursor();
		break;
	case L'i':
		normal_mode = 0;
		break;
	case L'a':
		if (cursor < line_end(cursor)) {
			cursor++;
		}
		normal_mode = 0;
		break;
	case L'o':
		if (!input_reserve(length + 1)) {
			break;
		}
		cursor = line_end(cursor);
		normal_mode = 0;
		insert_char(L'\n');
		break;
	case L'O':
		if (!input_reserve(length + 1)) {
			break;
		}
		cursor = line_start(cursor);
		normal_mode = 0;
		insert_char(L'\n');
		cursor--;
		break;
	case L'x':
		if (cursor < line_end(cursor)) {
			cut_range(cursor, cursor + 1);
		}
		break;
	case L'd':
	case L'y':
		delete_pending = (int)key;
		break;
	case L'p':
		paste_yank();
		break;
	default:
		break;
	}
}
