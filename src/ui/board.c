/* The chat board: live and saved chats (phi-ui only). Saved chats reopen
   with Enter; /resume focuses that section. Shown
   when NORMAL-mode Left on an empty prompt backgrounds a chat (twice only
   for its first foreground-to-background transition), or by
   `phi board`. Vim-style keys:

   normal   j/k ↑/↓ move · gg/G top/bottom · l/→/Enter open · h/←/Esc back
            to your chat · v visual · x or dd close · r rename ·
            Space reply · i/a/o new task · q leave · ? all keys
   visual   j/k extend · x/d close them all (asks first) · Esc/v cancel
   insert   type · Enter submit · Esc cancel · Ctrl+U clear · Ctrl+W word

   Colours and glyphs come from Phi's theme.
   It re-reads the chats' status files once a second while on screen;
   nothing runs when it is not. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#define _XOPEN_SOURCE_EXTENDED
#include "board.h"
#include "ui.h"
#include "../core/commands.h"
#include "../core/terminal.h"
#include "../tools.h"
#include "../core/chats.h"
#include "../core/fsutil.h"
#include "../core/settings.h"
#include "../core/sessions.h"
#include <curses.h>
#include <json-c/json.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include "terminal-colors.h"
#define THEME_LOADER_ONLY
#include "theme.inc" /* the same theme reading as Phi: extends, named themes, hex */
#include "surface-cells.inc"
#include "elements.inc"
#include "brand-header.inc"
#include "../version.h"
static int board_art_visible = 1, board_painting;

static double board_now(void) {

	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);

	return t.tv_sec + t.tv_nsec / 1e9;
}
static struct surface_client *board_program, *board_header_program, *board_footer_program;
static int board_program_y, board_program_height, board_header_y, board_header_height,
	board_footer_y, board_footer_height;

/* Shared UI palette roles; 54/55 are board-only roles below native cell pairs. */
enum {
	PAIR_ACCENT = 28,
	PAIR_MUTED = 3,
	PAIR_BORDER = 5,
	PAIR_SELECTED = 2,
	PAIR_SELECTED_MUTED = 4,
	PAIR_SELECTED_ACCENT = 63,
	PAIR_ERROR = 53,
	PAIR_WORKING = 54,
	PAIR_SELECTED_WORKING = 55,
	PAIR_TEXT = 27
};
static struct skin board_skin = SKIN_DEFAULTS;

enum mode {
	NORMAL,
	VISUAL,
	INSERT,
	REPLY,
	RENAME,
	CONFIRM,
	FILTER
};

static const char *const mode_names[] = {
	"NORMAL", "VISUAL", "INSERT", "REPLY", "RENAME", "CONFIRM", "FILTER"};
static const char *const groups[][2] = {{"working", "Working"}, {"approval", "Requires approval"},
	{"waiting", "Waiting"}, {"closed", "Saved"}};

/* ---- theme ------------------------------------------------------------------ */

#include "mascot-motion.inc"
/* The board has its own renderer and input loop, separate from welcome.inc. */
static double board_blink_deadline;
static int board_blink_closed;
static uint32_t board_blink_rng;
static int board_gesture, board_gesture_frame, board_intro_cursor;

static int board_blink_enabled(void) {

	if (!board_art_visible || !board_skin.board_header_visible || COLS < 75 || LINES < 12 ||
		!board_skin.art_count || board_skin.art_count != board_skin.art_blink_count) {
		return 0;
	}
	for (int i = 0; i < board_skin.art_count; i++) {
		if (skin_width(board_skin.art[i]) != skin_width(board_skin.art_blink[i])) {
			return 0;
		}
	}

	return 1;
}

static void board_blink_update(double stamp) {

	if (!board_blink_enabled()) {
		board_blink_deadline = 0;
		board_blink_closed = 0;
		board_intro_cursor = board_skin.art_intro_count;
		return;
	}
	int intro = mascot_intro_step(&board_skin, stamp, &board_blink_deadline, &board_blink_closed,
		&board_blink_rng, &board_gesture, &board_gesture_frame, &board_intro_cursor);
	if (intro >= 0) {
		return;
	}
	mascot_step(&board_skin, stamp, &board_blink_deadline, &board_blink_closed, &board_blink_rng,
		&board_gesture, &board_gesture_frame);
}

static int board_blink_wait(double stamp) {

	if (!board_blink_enabled() || !board_blink_deadline) {
		return -1;
	}
	double remaining = board_blink_deadline - stamp;

	return remaining > 0 ? (int)(remaining * 1000) + 1 : 0;
}

static struct theme_stamp board_theme;

/* Backgrounding keeps the originating chat's theme; a standalone board uses
   the global default. Never create or write a theme on the board's behalf. */
static int board_theme_snapshot(json_object *snapshot) {

	char next[sizeof(theme_transcript_path)] = "";
	json_object *file = json_object_object_get(snapshot, "session_file");
	if (json_object_is_type(file, json_type_string)) {
		const char *path = json_object_get_string(file);
		struct stat st;
		if (*path && strlen(path) == (size_t)json_object_get_string_len(file)) {
			int n = snprintf(next, sizeof(next), "%s.theme.json", path);
			if (n < 0 || (size_t)n >= sizeof(next) || stat(next, &st) || !S_ISREG(st.st_mode)) {
				*next = 0;
			}
		}
	}
	int changed = strcmp(next, theme_transcript_path) != 0;
	snprintf(theme_transcript_path, sizeof(theme_transcript_path), "%s", next);

	return changed;
}

static int board_theme_bind(const char *current) {

	char error[256];
	json_object *snapshot =
		current && *current ? chats_lookup(current, error, sizeof(error)) : NULL;
	int changed = board_theme_snapshot(snapshot);
	json_object_put(snapshot);

	return changed;
}

static void load_colors(void) {

	/* theme.json exactly as Phi reads it; a bad file means Phi's defaults. */
	board_theme = theme_stamp_now();
	short colors[THEME_COUNT];
	struct skin loaded;
	char problem[256];
	if (theme_read(colors, &loaded, problem, sizeof(problem)) < 0) {
		for (size_t i = 0; i < THEME_COUNT; i++) {
			colors[i] = theme_defaults[i].value;
		}
		loaded = skin_defaults;
	}
	skin_move(&board_skin, &loaded);
	if (!has_colors()) {
		return;
	}
	start_color();
	use_default_colors();
	short accent = theme_value(colors, "accent"), muted = theme_value(colors, "muted");
	short border = theme_value(colors, "border"), selected = theme_value(colors, "user_background");
	short error = theme_value(colors, "error"), working = theme_value(colors, "tool_success");
	short text = theme_value(colors, "text"), background = theme_value(colors, "background");
	init_pair(PAIR_TEXT, text, background);
	init_pair(PAIR_ACCENT, accent, background);
	init_pair(PAIR_MUTED, muted, background);
	init_pair(PAIR_BORDER, border, background);
	init_pair(PAIR_SELECTED, theme_value(colors, "user_text"), selected);
	init_pair(PAIR_SELECTED_MUTED, muted, selected);
	init_pair(PAIR_SELECTED_ACCENT, accent, selected);
	init_pair(PAIR_ERROR, error, background);
	init_pair(PAIR_WORKING, working, background);
	init_pair(PAIR_SELECTED_WORKING, working, selected);
	bkgd(COLOR_PAIR(PAIR_TEXT) | ' ');
}

/* ---- drawing ----------------------------------------------------------------- */

/* Status details contain arbitrary tool commands, including heredocs. Never
   pass controls (or a raw invalid-UTF-8 fallback) through to ncurses. */
static size_t board_line(wchar_t *wide, size_t capacity, const char **text) {

	mbstate_t state = {0};
	size_t remaining = strlen(*text), count = 0;
	while (remaining && count < capacity) {
		wchar_t value;
		size_t bytes = mbrtowc(&value, *text, remaining, &state);
		if (bytes == (size_t)-1 || bytes == (size_t)-2) {
			value = L'?';
			bytes = 1;
			memset(&state, 0, sizeof(state));
		}
		if (!bytes) {
			break;
		}
		*text += bytes;
		remaining -= bytes;
		if (iswcntrl(value)) {
			value = L' ';
		} else if (wcwidth(value) < 0 || (!count && wcwidth(value) == 0)) {
			value = L'?';
		}
		wide[count++] = value;
	}

	return count;
}

static int text_width(const char *text) {

	wchar_t wide[512];
	size_t n = board_line(wide, 511, &text);

	return wcswidth(wide, n);
}

/* Draw text in at most width cells, ending in … when cut. */
static void put(int row, int col, const char *text, int width) {

	if (width <= 0 || row < 0 || row >= LINES || col < 0 || col >= COLS) {
		return;
	}
	if (width > COLS - col - 1) {
		width = COLS - col - 1;
	}
	if (width <= 0) {
		return;
	}
	wchar_t wide[512];
	size_t n = board_line(wide, 511, &text);
	int used = 0;
	size_t i = 0;
	for (; i < n; i++) {
		int w = wcwidth(wide[i]);
		if (w < 0) {
			w = 1;
		}
		if (used + w > width) {
			break;
		}
		used += w;
	}
	if (i < n || *text) {
		while (i > 0 && used + 1 > width) {
			int w = wcwidth(wide[--i]);
			used -= w < 0 ? 1 : w;
		}
		mvaddnwstr(row, col, wide, (int)i);
		addstr("…");
	} else {
		mvaddnwstr(row, col, wide, (int)i);
	}
}

struct view {
	struct chat_row *rows;
	size_t count, cursor, anchor, top;
	enum mode mode;
	int help, pending_g, pending_d, ctrl_c, selecting, editor, quit, defaults_theme,
		command_running, paste_block;
	struct settings *settings;
	char *output;
	size_t output_used, output_capacity;
	int output_truncated;
	char workspace_place[256], label[160], notice[256], target[16], current[16],
		confirm_ids[64][16], follow[16];
	size_t confirm_count;
	unsigned collapsed, motion_count;
	int heading, heading_y[4];
	wchar_t query[256];
	int *scores;
	char (*recent)[4097];
	const char *model, *place, *effort;
};

static void move_cursor(struct view *v, long delta);
#include "board-filter.inc"

static int in_selection(const struct view *v, size_t i) {

	if (v->heading || !board_visible(v, i)) {
		return 0;
	}
	if (!v->selecting) {
		return i == v->cursor;
	}
	size_t low = v->anchor < v->cursor ? v->anchor : v->cursor,
		   high = v->anchor < v->cursor ? v->cursor : v->anchor;

	return i >= low && i <= high;
}

static int board_line_enabled(const char *line) {

	return (board_skin.board_preview || !strstr(line, "{preview}")) &&
		(board_skin.board_place || !strstr(line, "{path}"));
}

static int board_card_height(void) {

	int height = 1 + 2 * board_skin.board_padding + 2 * board_skin.board_card_border;
	for (int i = 0; i < board_skin.board_card_line_count; i++) {
		height += board_line_enabled(board_skin.board_card_lines[i]);
	}

	return height;
}

static void board_expand(const char *text, const struct chat_row *row, char *out, size_t capacity) {

	size_t used = 0;
	while (*text && used + 1 < capacity) {
		const char *value = NULL;
		size_t token = 0;
		const char *names[] = {
			"{id}", "{title}", "{preview}", "{path}", "{model}", "{effort}", "{state}"};
		const char *values[] = {
			row->id, row->title, row->detail, row->place, row->model, row->effort, row->state};
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			if (!strncmp(text, names[i], strlen(names[i]))) {
				value = values[i];
				token = strlen(names[i]);
				break;
			}
		}
		if (value) {
			size_t bytes = strlen(value);
			if (bytes > capacity - used - 1) {
				bytes = capacity - used - 1;
			}
			memcpy(out + used, value, bytes);
			used += bytes;
			text += token;
		} else {
			out[used++] = *text++;
		}
	}
	out[used] = 0;
}

static const char *board_hint(const struct view *v) {

	return *v->notice ? v->notice
		: v->mode == NORMAL
		? "j/k select rows/sections · Enter/Space fold · / filter · : commands · i new · ? keys"
		: v->mode == VISUAL ? "j/k extend · x close selected · esc cancel"
							: "enter submit · esc cancel";
}

static void board_chrome_expand(const char *source, char *out, size_t capacity, void *context) {

	const struct view *v = context;
	char total[32], working[32], waiting[32], approval[32], saved[32];
	int workers = 0, approvals = 0, saved_count = 0;
	for (size_t i = 0; i < v->count; i++) {
		workers += !strcmp(v->rows[i].group, "working");
		approvals += !strcmp(v->rows[i].group, "approval");
		saved_count += !v->rows[i].live;
	}
	snprintf(total, sizeof(total), "%zu", v->count);
	snprintf(working, sizeof(working), "%d", workers);
	snprintf(waiting, sizeof(waiting), "%d", (int)v->count - workers - approvals - saved_count);
	snprintf(approval, sizeof(approval), "%d", approvals);
	snprintf(saved, sizeof(saved), "%d", saved_count);
	const char *place = v->place ? v->place : "", *model = v->model ? v->model : "";
	const char *project = strrchr(place, '/');
	project = project && project[1] ? project + 1 : place;
	const char *keys[] = {"{model}", "{path}", "{mode}", "{hint}", "{count}", "{working}",
		"{waiting}", "{approval}", "{current}", "{effort}", "{version}", "{project}", "{saved}"};
	const char *values[] = {model, place, mode_names[v->mode], board_hint(v), total, working,
		waiting, approval, v->current, !v->effort || !strcmp(v->effort, "none") ? "off" : v->effort,
		PHI_VERSION, project, saved};
	size_t used = 0;
	while (*source && used + 1 < capacity) {
		int found = 0;
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			if (!strncmp(source, keys[i], strlen(keys[i]))) {
				size_t bytes = strlen(values[i]);
				if (bytes > capacity - used - 1) {
					bytes = capacity - used - 1;
				}
				memcpy(out + used, values[i], bytes);
				used += bytes;
				source += strlen(keys[i]);
				found = 1;
				break;
			}
		}
		if (!found) {
			out[used++] = *source++;
		}
	}
	out[used] = 0;
}

static json_object *board_surface_context(const struct view *v) {

	json_object *out = json_object_new_object(), *rows = json_object_new_array();
	json_object_object_add(out, "model", json_object_new_string(v->model));
	json_object_object_add(out, "path", json_object_new_string(v->place));
	json_object_object_add(out, "count", json_object_new_int64((int64_t)v->count));
	int working_count = 0, approval_count = 0, saved_count = 0;
	for (size_t i = 0; i < v->count; i++) {
		working_count += !strcmp(v->rows[i].group, "working");
		approval_count += !strcmp(v->rows[i].group, "approval");
		saved_count += !v->rows[i].live;
	}
	json_object_object_add(out, "working", json_object_new_int(working_count));
	json_object_object_add(out, "requires_approval", json_object_new_int(approval_count));
	json_object_object_add(out, "waiting",
		json_object_new_int((int)v->count - working_count - approval_count - saved_count));
	json_object_object_add(out, "saved", json_object_new_int(saved_count));
	json_object_object_add(
		out, "selected", json_object_new_string(v->cursor < v->count ? v->rows[v->cursor].id : ""));
	size_t first = v->cursor > 32 ? v->cursor - 32 : 0;
	for (size_t i = first; i < v->count && i < first + 64; i++) {
		const struct chat_row *r = &v->rows[i];
		json_object *row = json_object_new_object();
		json_object_object_add(row, "id", json_object_new_string(r->id));
		json_object_object_add(row, "title", json_object_new_string(r->title));
		json_object_object_add(row, "preview", json_object_new_string(r->detail));
		json_object_object_add(row, "model", json_object_new_string(r->model));
		json_object_object_add(row, "effort", json_object_new_string(r->effort));
		json_object_object_add(row, "path", json_object_new_string(r->place));
		json_object_object_add(row, "state", json_object_new_string(r->state));
		json_object_array_add(rows, row);
	}
	json_object_object_add(out, "rows", rows);

	return out;
}

static void board_panel(struct surface_client **client, int index, const char *slot, int y,
	int height, const struct view *v) {

	if (height <= 0) {
		surface_client_free(*client);
		*client = NULL;
		return;
	}
	if (v->mode == CONFIRM) {
		return;
	}
	if (!*client) {
		*client = surface_client_new();
	}
	json_object *context = board_surface_context(v);
	surface_client_poll(*client, board_skin.surfaces[index].id, slot, COLS - 4, height, context);
	json_object_put(context);
	surface_paint(*client, y, 2, COLS - 4, height, PAIR_MUTED);
}

static const char *board_brand_art(int row, void *context) {

	(void)context;

	return mascot_line(&board_skin, board_blink_closed, board_gesture, board_gesture_frame, row);
}

static void board_brand_paint(
	int y, int x, int width, const char *text, int pair, int bold, void *context) {

	(void)context;
	attrset(COLOR_PAIR(pair) | (bold ? A_BOLD : 0));
	put(y, x, text, width);
	attrset(A_NORMAL);
}

static void draw(struct view *v) {

	if (board_painting) {
		return;
	}
	board_painting = 1;
	erase();
	if (LINES < 12 || COLS < 45) {
		board_art_visible = 0;
		board_blink_update(board_now());
		put(0, 1, "Board needs at least 45 columns and 12 rows. Resize the terminal.", COLS - 2);
		refresh();
		board_painting = 0;
		return;
	}
	int counts[4] = {0, 0, 0, 0};
	for (size_t i = 0; i < v->count; i++) {
		for (int g = 0; g < 4; g++) {
			if (board_matches(v, i) && !strcmp(v->rows[i].group, groups[g][0])) {
				counts[g]++;
			}
		}
	}

	int show_help = v->help && LINES >= 18;
	int typing = v->mode == INSERT || v->mode == REPLY || v->mode == RENAME;
	int show_input = board_skin.board_input_visible || typing || v->mode == CONFIRM;
	int footer_rows = board_skin.board_footer_visible ? board_skin.board_footer_rows : 0;
	if ((v->mode == CONFIRM || *v->notice) && footer_rows < 1) {
		footer_rows = 1;
	}
	if (footer_rows > LINES / 4) {
		footer_rows = LINES / 4;
	}
	int input_rows = show_input ? (v->editor ? ui_editor_rows(COLS, LINES / 3) : 3) : 0;
	int custom = board_skin.board_header_elements.count > 0;
	int intrinsic = brand_header_rows(&board_skin);
	if (!intrinsic && board_skin.header_visible) {
		intrinsic = 1;
	}
	int header_row = board_skin.board_header_visible ? board_skin.board_header_rows : 0;
	int automatic = board_skin.board_header_visible && !header_row && !custom;
	if (automatic) {
		header_row = intrinsic;
	}
	int metadata = board_skin.board_header_visible
		? (!!board_skin.board_summary + !!board_skin.board_header_border +
			  !!board_skin.board_instructions)
		: 0;
	int budget = LINES - footer_rows - input_rows - (show_help ? 4 : 0) - metadata - 5;
	if (budget < 0) {
		budget = 0;
	}
	if (header_row > LINES / 3) {
		header_row = LINES / 3;
	}
	if (header_row > budget) {
		header_row = automatic ? 0 : budget;
	}
	int header_top =
		automatic && brand_header_rows(&board_skin) && header_row && header_row + 1 <= budget ? 1
																							  : 0;
	header_row += header_top;
	board_art_visible = header_row > 0 && board_skin.art_count &&
		(custom ? COLS >= 75 : board_skin.art_width + 12 < COLS);
	board_blink_update(board_now());
	char line[512];
	if (board_skin.board_header_visible) {
		int width = COLS - (board_skin.art_count && COLS >= 75 ? board_skin.art_width + 7 : 4);
		if (custom) {
			chrome_draw(&board_skin.board_header_elements, 2, 0, width, header_row, 0, PAIR_MUTED,
				PAIR_ACCENT, board_chrome_expand, v);
		} else if (brand_header_rows(&board_skin)) {
			brand_header_draw(&board_skin, header_top, board_skin.margin,
				COLS - board_skin.margin - 2, header_row - header_top, PAIR_ACCENT, PAIR_TEXT,
				PAIR_MUTED, board_brand_art, board_chrome_expand, board_brand_paint, v);
		} else if (board_skin.header_visible && header_row) {
			brand_compact_draw(&board_skin, 0, board_skin.margin, COLS - 2 * board_skin.margin,
				PAIR_ACCENT, PAIR_MUTED, board_chrome_expand, board_brand_paint, v);
		}
		if (custom && board_art_visible) {
			int x = COLS - board_skin.art_width - 3;
			attrset(COLOR_PAIR(PAIR_ACCENT));
			for (int i = 0; i < board_skin.art_count && i < header_row; i++) {
				put(i, x,
					mascot_line(
						&board_skin, board_blink_closed, board_gesture, board_gesture_frame, i),
					board_skin.art_width);
			}
		}
		if (counts[1]) {
			snprintf(line, sizeof(line),
				"%d working · %d requires approval · %d waiting · %d saved", counts[0], counts[1],
				counts[2], counts[3]);
		} else {
			snprintf(line, sizeof(line), "%d working · %d waiting · %d saved", counts[0], counts[2],
				counts[3]);
		}
		attrset(COLOR_PAIR(PAIR_MUTED));
		if (board_skin.board_summary) {
			put(header_row++, 2, line, COLS - 4);
		}
		if (board_skin.board_header_border) {
			attrset(COLOR_PAIR(PAIR_BORDER));
			mvhline(header_row++, 2, ACS_HLINE, COLS - 4);
		}
		attrset(COLOR_PAIR(PAIR_MUTED));
		if (board_skin.board_instructions) {
			put(header_row++, 2,
				*v->current ? "Your chat is in the background · l open · h return · q leave"
							: "l open a chat · i start a new one · q leave",
				COLS - 4);
		}
	}
	attrset(A_NORMAL);

	/* The list, scrolled to keep the cursor visible. Line codes: row index,
       -1 blank, -10-g heading of group g. */
	int list_top = header_row + 1,
		list_bottom = LINES - footer_rows - input_rows - 1 - (show_help ? 4 : 0);
	board_footer_height =
		surface_rows(&board_skin.surfaces[5], LINES, COLS - 4, list_bottom - list_top - 3);
	list_bottom -= board_footer_height;
	board_footer_y = list_bottom;
	board_panel(&board_footer_program, 5, "board.footer", board_footer_y, board_footer_height, v);
	board_header_y = list_top;
	board_header_height =
		surface_rows(&board_skin.surfaces[4], LINES, COLS - 4, list_bottom - list_top - 3);
	board_panel(&board_header_program, 4, "board.header", board_header_y, board_header_height, v);
	list_top += board_header_height;
	board_program_y = list_top;
	board_program_height =
		surface_rows(&board_skin.surfaces[3], LINES, COLS - 4, list_bottom - list_top - 3);
	board_panel(&board_program, 3, "board", board_program_y, board_program_height, v);
	list_top += board_program_height;
	int title_width = 8;
	for (size_t i = 0; i < v->count; i++) {
		int w = text_width(*v->rows[i].title ? v->rows[i].title : "new chat");
		if (w > title_width) {
			title_width = w;
		}
	}
	if (title_width > COLS / 3) {
		title_width = COLS / 3;
	}
	static int lines[8192], parts[8192];
	size_t line_count = 0, cursor_line = 0;
	for (int g = 0; g < 4; g++) {
		v->heading_y[g] = -1;
	}
	for (int g = 0; g < 4; g++) {
		if (line_count >= 8188) {
			break;
		}
		if (!counts[g]) {
			continue;
		}
		if (line_count) {
			lines[line_count++] = -1;
		}
		if (v->heading == g + 1) {
			cursor_line = line_count;
		}
		if (board_skin.board_headings || v->heading == g + 1 || (v->collapsed & (1u << g))) {
			lines[line_count++] = -10 - g;
		}
		if (v->collapsed & (1u << g)) {
			continue;
		}
		for (size_t i = 0; i < v->count && line_count < 8100; i++) {
			if (board_matches(v, i) && !strcmp(v->rows[i].group, groups[g][0])) {
				if (!v->heading && i == v->cursor) {
					cursor_line = line_count;
				}
				int height = board_skin.board_cards ? board_card_height() : 1;
				for (int part = 0; part < height && line_count < 8190; part++) {
					parts[line_count] = part;
					lines[line_count++] = (int)i;
				}
				if (board_skin.board_cards) {
					for (int gap = 0; gap < board_skin.board_gap && line_count < 8190; gap++) {
						lines[line_count++] = -1;
					}
				}
			}
		}
	}
	size_t visible = list_bottom > list_top ? (size_t)(list_bottom - list_top) : 1;
	if (cursor_line < v->top + 1) {
		v->top = cursor_line ? cursor_line - 1 : 0; /* keep its heading in view */
	}
	if (cursor_line >= v->top + visible) {
		v->top = cursor_line - visible + 1;
	}
	int screen_row = list_top;
	for (size_t l = v->top; l < line_count && screen_row < list_bottom; l++, screen_row++) {
		int code = lines[l];
		if (code == -1) {
			continue;
		}
		if (code <= -10) {
			int g = -10 - code;
			int pair = g == 1 ? PAIR_ACCENT : g == 0 ? PAIR_WORKING : PAIR_MUTED;
			v->heading_y[g] = screen_row;
			attrset(COLOR_PAIR(pair) | A_BOLD | (v->heading == g + 1 ? A_REVERSE : 0));
			put(screen_row, 2, (v->collapsed & (1u << g)) ? "▸" : "▾", 2);
			put(screen_row, 5, groups[g][1], text_width(groups[g][1]));
			snprintf(line, sizeof(line), "%d", counts[g]);
			attrset(COLOR_PAIR(PAIR_MUTED));
			put(screen_row, 5 + text_width(groups[g][1]) + 1, line, 5);
			attrset(COLOR_PAIR(PAIR_BORDER));
			int rule = 8 + text_width(groups[g][1]) + (int)strlen(line);
			if (rule < COLS - 3) {
				mvhline(screen_row, rule, ACS_HLINE, COLS - rule - 2);
			}
			attrset(A_NORMAL);
			continue;
		}
		const struct chat_row *r = &v->rows[code];
		int selected = in_selection(v, (size_t)code), current = !strcmp(r->id, v->current);
		int waiting = !strcmp(r->group, "approval"),
			ended = !strcmp(r->group, "waiting") || !r->live;
		int plain = selected ? PAIR_SELECTED : 0,
			muted = selected ? PAIR_SELECTED_MUTED : PAIR_MUTED;
		if (board_skin.board_cards) {
			int margin = board_skin.board_margin;
			if (margin > COLS / 8) {
				margin = COLS / 8;
			}
			int left = margin, right = COLS - margin - 1, part = parts[l],
				height = board_card_height();
			int edge = board_skin.board_card_border, inner = left + edge + board_skin.board_padding;
			int title_part = edge + board_skin.board_padding;
			attrset(COLOR_PAIR(selected ? PAIR_SELECTED : 0));
			mvhline(screen_row, left, ' ', right - left + 1);
			if (edge) {
				attrset(COLOR_PAIR(selected ? PAIR_SELECTED_ACCENT : PAIR_BORDER));
				if (!part || part == height - 1) {
					mvhline(screen_row, left + 1, ACS_HLINE, right - left - 1);
					mvaddch(screen_row, left, !part ? ACS_ULCORNER : ACS_LLCORNER);
					mvaddch(screen_row, right, !part ? ACS_URCORNER : ACS_LRCORNER);
				} else {
					mvaddch(screen_row, left, ACS_VLINE);
					mvaddch(screen_row, right, ACS_VLINE);
				}
			}
			if (part == title_part) {
				char age[32] = "";
				if (board_skin.board_age) {
					chats_age(r->updated, age, sizeof(age));
				}
				int age_width = (int)strlen(age),
					available = right - inner - board_skin.board_padding - age_width - 4;
				attrset(COLOR_PAIR(selected ? PAIR_SELECTED_ACCENT
						: waiting           ? PAIR_ACCENT
						: ended             ? PAIR_MUTED
											: PAIR_WORKING));
				put(screen_row, inner,
					selected      ? board_skin.prompt
						: waiting ? "●"
						: ended   ? "○"
								  : "✳",
					2);
				attrset(COLOR_PAIR(plain) | A_BOLD);
				put(screen_row, inner + 3, *r->title ? r->title : "new chat", available);
				attrset(COLOR_PAIR(muted));
				if (*age) {
					put(screen_row, right - edge - board_skin.board_padding - age_width, age,
						age_width);
				}
			} else if (part > title_part && part <= title_part + board_skin.board_card_line_count) {
				int wanted = part - title_part - 1, found = 0;
				for (int i = 0; i < board_skin.board_card_line_count; i++) {
					if (board_line_enabled(board_skin.board_card_lines[i])) {
						if (found++ != wanted) {
							continue;
						}
						board_expand(board_skin.board_card_lines[i], r, line, sizeof(line));
						attrset(COLOR_PAIR(muted));
						put(screen_row, inner + 3, line,
							right - inner - edge - board_skin.board_padding - 3);
						break;
					}
				}
			}
			attrset(A_NORMAL);
			continue;
		}
		if (selected) {
			attron(COLOR_PAIR(PAIR_SELECTED));
			mvhline(screen_row, 0, ' ', COLS - 1);
		}
		attrset(COLOR_PAIR(selected ? PAIR_SELECTED_ACCENT : PAIR_ACCENT));
		if (selected && !v->selecting) {
			put(screen_row, 1, board_skin.prompt, 1);
		}
		attrset(COLOR_PAIR(waiting ? (selected ? PAIR_SELECTED_ACCENT : PAIR_ACCENT)
				: ended            ? muted
								   : (selected ? PAIR_SELECTED_WORKING : PAIR_WORKING)));
		put(screen_row, 3, waiting ? "●" : ended ? "○" : "✳", 2);
		attrset(COLOR_PAIR(plain));
		if (current || (selected && !v->selecting)) {
			attron(A_BOLD);
		}
		put(screen_row, 6, *r->title ? r->title : "new chat", title_width);
		attrset(COLOR_PAIR(plain));
		/* Detail: the pending question or last reply; the place for a new chat. */
		const char *detail = *r->detail ? r->detail : r->place;
		if (current || ended || !*r->detail) {
			attrset(COLOR_PAIR(muted));
		}
		if (board_skin.board_preview) {
			put(screen_row, 8 + title_width, detail, COLS - title_width - 19);
		}
		char age[32];
		chats_age(r->updated, age, sizeof(age));
		attrset(COLOR_PAIR(muted));
		if (board_skin.board_age) {
			put(screen_row, COLS - 2 - (int)strlen(age), age, (int)strlen(age));
		}
		attrset(A_NORMAL);
	}
	if (!v->count) {
		attron(COLOR_PAIR(PAIR_MUTED));
		put(list_top + 1, 2, "No chats yet. Press i and describe a task to start one.", COLS - 4);
		attroff(COLOR_PAIR(PAIR_MUTED));
	}

	if (v->command_running && v->output) {
		attrset(COLOR_PAIR(PAIR_TEXT));
		for (int y = list_top; y < list_bottom; y++) {
			mvhline(y, 0, ' ', COLS);
		}
		int row = list_top;
		const char *at = v->output;
		while (*at && row < list_bottom) {
			char output_line[1024];
			size_t bytes = strcspn(at, "\n");
			if (bytes >= sizeof(output_line)) {
				bytes = sizeof(output_line) - 1;
			}
			memcpy(output_line, at, bytes);
			output_line[bytes] = 0;
			put(row++, 2, output_line, COLS - 4);
			at += strcspn(at, "\n");
			if (*at) {
				at++;
			}
		}
	}
	if (show_help) {
		static const char *const help[] = {
			"normal  j/k move · gg/G top/bottom · l or enter open · h or esc back · v visual · x or dd close",
			"        Tab sections · Enter fold · / fuzzy filter · : commands · i new · r rename · q leave",
			"visual  j/k extend · x or d close all selected · esc or v cancel",
			"insert  enter submit · esc cancel · ctrl+u clear · ctrl+w delete word"};
		attron(COLOR_PAIR(PAIR_MUTED));
		for (int i = 0; i < 4; i++) {
			put(LINES - footer_rows - input_rows - 4 + i, 1, help[i], COLS - 2);
		}
		attroff(COLOR_PAIR(PAIR_MUTED));
	}

	/* Input is core-owned, chrome is theme-declared. Hidden idle input always
       returns for actual editing and confirmations. */
	int input_row = LINES - footer_rows - 2;
	int input_col = 3 + text_width(v->label);
	if (show_input && !v->editor) {
		if (board_skin.board_footer_border) {
			attron(COLOR_PAIR(PAIR_BORDER));
			mvhline(input_row - 1, 0, ACS_HLINE, COLS - 1);
			mvhline(input_row + 1, 0, ACS_HLINE, COLS - 1);
			attroff(COLOR_PAIR(PAIR_BORDER));
		}
		attron(COLOR_PAIR(typing ? PAIR_ACCENT : PAIR_MUTED));
		put(input_row, 1, board_skin.prompt, 1);
		attrset(COLOR_PAIR(PAIR_MUTED));
		if (*v->label) {
			put(input_row, 3, v->label, COLS - 4);
		}
		if (!typing) {
			put(input_row, input_col, board_skin.board_input_placeholder, COLS - input_col - 1);
		}
	}
	if (footer_rows && board_skin.board_footer_visible) {
		chrome_draw(&board_skin.board_footer_elements, 1, LINES - footer_rows, COLS - 2,
			footer_rows, 0, PAIR_MUTED, PAIR_ACCENT, board_chrome_expand, v);
	}
	if (v->mode == CONFIRM || *v->notice) {
		attrset(COLOR_PAIR(v->mode == CONFIRM ? PAIR_ERROR : PAIR_ACCENT) | A_BOLD);
		put(LINES - 1, 1, board_hint(v), COLS - 2);
	}
	attrset(A_NORMAL);
	if (show_input && v->editor) {
		ui_editor_paint(LINES - footer_rows - input_rows, input_rows, COLS,
			*v->label                ? v->label
				: v->command_running ? "Board command · Ctrl+C cancels"
				: typing             ? "New chat · / commands · !! local shell"
									 : "i edit · / filter · : commands",
			typing);
	}
	if (!v->editor) {
		if (typing) {
			curs_set(1);
			move(input_row, input_col);
		} else {
			curs_set(0);
		}
	}
	refresh();
	board_painting = 0;
}

/* ---- actions ----------------------------------------------------------------- */

static void reload(struct view *v) {

	char keep[16] = "", anchor[16] = "";
	if (v->count) {
		snprintf(keep, sizeof(keep), "%s", v->rows[v->cursor].id);
		snprintf(anchor, sizeof(anchor), "%s", v->rows[v->anchor].id);
	}
	struct chat_row *old_rows = v->rows;
	size_t old_count = v->count;
	char(*old_recent)[4097] = v->recent;
	free(v->scores);
	v->scores = NULL;
	v->recent = NULL;
	v->count = chats_rows(&v->rows);
	if (v->query[0] && v->count) {
		v->recent = calloc(v->count, sizeof(*v->recent));
		if (v->recent && old_recent) {
			for (size_t i = 0; i < v->count; i++) {
				for (size_t j = 0; j < old_count; j++) {
					if (!strcmp(v->rows[i].id, old_rows[j].id) &&
						v->rows[i].session_bytes == old_rows[j].session_bytes &&
						v->rows[i].session_stamp == old_rows[j].session_stamp &&
						!strcmp(v->rows[i].session_file, old_rows[j].session_file)) {
						memcpy(v->recent[i], old_recent[j], sizeof(*v->recent));
						break;
					}
				}
			}
		}
	}
	free(old_rows);
	free(old_recent);
	v->cursor = v->anchor = 0;
	for (size_t i = 0; i < v->count; i++) {
		if (!strcmp(v->rows[i].id, keep)) {
			v->cursor = i;
		}
		if (!strcmp(v->rows[i].id, anchor)) {
			v->anchor = i;
		}
	}
	/* A chat just started appears once it has written its status. */
	for (size_t i = 0; *v->follow && i < v->count; i++) {
		if (!strcmp(v->rows[i].id, v->follow)) {
			v->cursor = v->anchor = i;
			v->follow[0] = 0;
		}
	}
	board_filter_update(v, 0);
}

/* Move in display order (the rows are already grouped in that order). */
static void move_cursor(struct view *v, long delta) {

	if (!v->count) {
		return;
	}
	long current = 0, total = 0;
	for (int g = 0; g < 4; g++) {
		int present = 0;
		for (size_t i = 0; i < v->count; i++) {
			if (board_group(&v->rows[i]) == g && board_matches(v, i)) {
				present = 1;
				break;
			}
		}
		if (!present) {
			continue;
		}
		if (v->mode == NORMAL && !v->selecting) {
			if (v->heading == g + 1) {
				current = total;
			}
			total++;
		}
		for (size_t i = 0; i < v->count; i++) {
			if (board_group(&v->rows[i]) == g && board_visible(v, i)) {
				if (!v->heading && v->cursor == i) {
					current = total;
				}
				total++;
			}
		}
	}
	if (!total) {
		return;
	}
	long target = current + delta;
	if (target < 0) {
		target = 0;
	}
	if (target >= total) {
		target = total - 1;
	}
	long at = 0;
	for (int g = 0; g < 4; g++) {
		int present = 0;
		for (size_t i = 0; i < v->count; i++) {
			if (board_group(&v->rows[i]) == g && board_matches(v, i)) {
				present = 1;
				break;
			}
		}
		if (!present) {
			continue;
		}
		if (v->mode == NORMAL && !v->selecting && at++ == target) {
			v->heading = g + 1;
			return;
		}
		for (size_t i = 0; i < v->count; i++) {
			if (board_group(&v->rows[i]) == g && board_visible(v, i)) {
				if (at++ == target) {
					v->heading = 0;
					v->cursor = i;
					if (!v->selecting) {
						v->anchor = i;
					}
					return;
				}
			}
		}
	}
}

#include "board-motion.inc"

static int current_live(const struct view *v) {

	for (size_t i = 0; i < v->count; i++) {
		if (!strcmp(v->rows[i].id, v->current)) {
			return v->rows[i].live;
		}
	}
	/* The current chat may be empty and therefore absent from the board. */

	return chats_live(v->current);
}

static void begin_input(struct view *v, enum mode mode, const char *label, const char *text) {

	if (v->editor) {
		if (mode != INSERT) {
			char *draft = ui_editor_text();
			if (draft && *draft) {
				free(draft);
				snprintf(v->notice, sizeof(v->notice),
					"Finish or clear the new-chat draft before replying or renaming.");
				return;
			}
			free(draft);
		}
		if (mode != INSERT || *text) {
			if (!ui_editor_reset(text)) {
				snprintf(v->notice, sizeof(v->notice), "Cannot allocate composer draft.");
				return;
			}
		}
		ui_editor_scope(mode == INSERT ? 1 : 2);
		ui_editor_focus();
	}
	v->motion_count = 0;
	v->mode = mode;
	v->selecting = v->pending_g = v->pending_d = 0;
	v->anchor = v->cursor;
	snprintf(v->label, sizeof(v->label), "%s", label);
	v->notice[0] = 0;
}

static void to_normal(struct view *v) {

	v->mode = NORMAL;
	v->motion_count = 0;
	v->selecting = 0;
	v->label[0] = v->target[0] = 0;
	if (v->editor) {
		ui_editor_scope(1);
	}
	v->anchor = v->cursor;
}

/* x/dd: ask first, then end running chats and forget ended ones. */
static void end_or_forget(struct view *v) {

	if (!v->count) {
		return;
	}
	size_t running = 0;
	v->confirm_count = 0;
	for (size_t i = 0; i < v->count && v->confirm_count < 64; i++) {
		if (!in_selection(v, i)) {
			continue;
		}
		memcpy(v->confirm_ids[v->confirm_count++], v->rows[i].id, sizeof(v->rows[i].id));
		running += v->rows[i].live;
	}
	size_t total = v->confirm_count;
	if (!total) {
		return;
	}
	v->mode = CONFIRM;
	if (total == 1) {
		const struct chat_row *r = &v->rows[v->cursor];
		snprintf(v->notice, sizeof(v->notice),
			"close \"%.60s\"? its conversation stays saved · y/n",
			*r->title ? r->title : "new chat");
	} else {
		snprintf(
			v->notice, sizeof(v->notice), "close %zu chats? conversations stay saved · y/n", total);
	}
	(void)running;
}

/* After y: end the running ones, forget the ended ones. */
static void confirm_delete(struct view *v) {

	char error[256] = "";
	size_t ended = 0, forgotten = 0;
	for (size_t k = 0; k < v->confirm_count; k++) {
		int live = 0;
		for (size_t i = 0; i < v->count; i++) {
			if (!strcmp(v->rows[i].id, v->confirm_ids[k])) {
				live = v->rows[i].live;
			}
		}
		if (live) {
			ended += chats_close(v->confirm_ids[k], error, sizeof(error));
		} else {
			forgotten += chats_forget(v->confirm_ids[k], error, sizeof(error));
		}
	}
	to_normal(v);
	reload(v);
	if (ended && forgotten) {
		snprintf(v->notice, sizeof(v->notice), "closing %zu, deleted %zu · sessions kept", ended,
			forgotten);
	} else if (ended) {
		snprintf(v->notice, sizeof(v->notice), "closing %zu chat%s · sessions kept", ended,
			ended == 1 ? "" : "s");
	} else if (forgotten) {
		snprintf(v->notice, sizeof(v->notice), "deleted %zu chat%s · sessions kept", forgotten,
			forgotten == 1 ? "" : "s");
	} else {
		snprintf(v->notice, sizeof(v->notice), "%s", *error ? error : "nothing closed");
	}
}

static struct view *board_editor_view;
static unsigned board_saved_collapsed;
static char
	*board_saved_draft; /* New-task draft belongs to this viewer, never a chat's recovery file. */

static void board_editor_redraw(void *context) {

	draw(context);
}

static void board_output(const char *text, FILE *destination) {

	(void)destination;
	struct view *v = board_editor_view;
	if (!v || !text) {
		return;
	}
	size_t n = strlen(text), limit = 65536;
	if (n > limit - v->output_used) {
		n = limit - v->output_used;
		v->output_truncated = 1;
	}
	if (v->output_used + n + 1 > v->output_capacity) {
		size_t cap = v->output_used + n + 1;
		char *next = realloc(v->output, cap);
		if (!next) {
			v->output_truncated = 1;
			return;
		}
		v->output = next;
		v->output_capacity = cap;
	}
	memcpy(v->output + v->output_used, text, n);
	v->output_used += n;
	v->output[v->output_used] = 0;
	if (v->command_running) {
		ui_editor_refresh();
	}
}

static void board_editor_feedback(const char *text, void *context) {

	struct view *v = context;
	if (v->command_running) {
		board_output(text, stdout);
		board_output("\n", stdout);
	} else {
		snprintf(v->notice, sizeof(v->notice), "%s", text);
	}
}

static void board_styled_output(const char *text, int color, int bold) {

	(void)color;
	(void)bold;
	board_output(text, stdout);
}

static void board_show_output(struct view *v) {

	if (!v->output_used) {
		return;
	}
	char *copy = strdup(v->output);
	if (!copy) {
		return;
	}
	const char *lines[513];
	size_t count = 0;
	char *at = copy;
	while (*at && count < 512) {
		char *end = strchr(at, '\n');
		if (end) {
			*end = 0;
		}
		if (*at) {
			lines[count++] = at;
		}
		if (!end) {
			at += strlen(at);
			break;
		}
		at = end + 1;
	}
	if (v->output_truncated || *at) {
		lines[count++] = "[Output limited to 64 KiB / 512 lines]";
	}
	if (count) {
		(void)ui_select("Board command · Enter/Esc returns", lines, count, 0);
	}
	free(copy);
}

static void board_local_command(struct view *v, const char *message) {

	v->output_used = 0;
	v->output_truncated = 0;
	if (v->output) {
		v->output[0] = 0;
	}
	v->command_running = 1;
	v->mode = INSERT;
	struct settings latest;
	char settings_error[256];
	if (settings_load(&latest, settings_error, sizeof(settings_error)) >= 0) {
		*v->settings = latest;
		terminal_configure(v->settings->model, v->settings->effort);
	}
	terminal_configure_safety(v->settings->approval, v->settings->sandbox);
	ui_busy(1);
	if (!strncmp(message, "/resume", 7) && !message[7 + strspn(message + 7, " \t\r\n")]) {
		reload(v);
		snprintf(v->notice, sizeof(v->notice), "No saved chats yet.");
		for (size_t i = 0; i < v->count; i++) {
			if (!v->rows[i].live) {
				v->cursor = v->anchor = i;
				v->heading = 0;
				v->collapsed &= ~(1u << 3);
				v->query[0] = 0;
				snprintf(v->notice, sizeof(v->notice),
					"Saved chats · Enter reopens the selected conversation");
				break;
			}
		}
	} else if (message[0] == '!' && message[1] == '!') {
		const char *shell = message + 2;
		shell += strspn(shell, " \t");
		if (!*shell) {
			board_output("Usage: !!command (runs in the board working directory)\n", stdout);
		} else {
			json_object *args = json_object_new_object(), *call = json_object_new_object();
			json_object_object_add(args, "command", json_object_new_string(shell));
			json_object_object_add(call, "name", json_object_new_string("bash"));
			json_object_object_add(call, "arguments",
				json_object_new_string(
					json_object_to_json_string_ext(args, JSON_C_TO_STRING_PLAIN)));
			tools_user_pty(-ui_columns()); /* Same persistent-cwd user-shell mode as chat !!. */
			json_object *answer = tools_execute(call);
			tools_user_pty(0);
			json_object *failure = json_object_object_get(answer, "error");
			if (failure) {
				board_output(json_object_get_string(failure), stderr);
			}
			json_object_put(answer);
			json_object_put(call);
			json_object_put(args);
		}
	} else {
		terminal_configure_safety(v->settings->approval, v->settings->sandbox);
		int result = command_run(NULL, v->settings, message);
		if (result == 2) {
			v->quit = 1;
		}
	}
	ui_busy(0);
	v->command_running = 0;
	char *draft = ui_editor_text();
	v->mode = draft && *draft ? INSERT : NORMAL;
	free(draft);
	char theme_path[4096];
	ui_editor_theme_source(theme_path, sizeof(theme_path));
	if (strcmp(theme_path, theme_transcript_path)) {
		snprintf(theme_transcript_path, sizeof(theme_transcript_path), "%s", theme_path);
		v->defaults_theme = 1;
	}
	char cwd[4096];
	if (getcwd(cwd, sizeof(cwd))) {
		const char *home = getenv("HOME");
		size_t n = home ? strlen(home) : 0;
		if (n > 1 && !strncmp(cwd, home, n) && (!cwd[n] || cwd[n] == '/')) {
			snprintf(v->workspace_place, sizeof(v->workspace_place), "~%.250s", cwd + n);
		} else {
			snprintf(v->workspace_place, sizeof(v->workspace_place), "%.250s", cwd);
		}
		v->place = v->workspace_place;
		ui_cwd_changed();
	}
	load_colors();
	ui_configure(v->settings->model, v->settings->effort);
	board_show_output(v);
}

static void submit(struct view *v) {

	char error[256] = "";
	enum mode mode = v->mode;
	if (!v->editor) {
		return;
	}
	if (LINES < 12 || COLS < 45) {
		snprintf(v->notice, sizeof(v->notice), "Resize before submitting; draft kept.");
		return;
	}
	char *input = ui_editor_text();
	char target[16];
	if (!input) {
		snprintf(v->notice, sizeof(v->notice), "Cannot allocate submitted draft; kept.");
		return;
	}
	snprintf(target, sizeof(target), "%s", v->target);
	if (!*input) {
		free(input);
		return;
	}
	if (mode == INSERT && (input[0] == '/' || (input[0] == '!' && input[1] == '!'))) {
		/* Paths beginning with / are ordinary task text, as in a chat. */
		if (!command_input_path(input)) {
			ui_editor_remember(input);
			if (!ui_editor_reset("")) {
				free(input);
				return;
			}
			to_normal(v);
			board_local_command(v, input);
			free(input);
			return;
		}
	}
	int accepted = 0;
	if (mode == INSERT) {
		char id[16];
		if (!chats_start(input, id, sizeof(id))) {
			snprintf(v->notice, sizeof(v->notice), "could not start a chat");
			free(input);
			return;
		}
		accepted = 1;
		snprintf(v->follow, sizeof(v->follow), "%s", id);
		reload(v);
		snprintf(v->notice, sizeof(v->notice), "started a new chat in the background");
	} else if (mode == REPLY) {
		accepted = chats_send(target, input, error, sizeof(error));
		snprintf(v->notice, sizeof(v->notice), "%s", accepted ? "sent" : error);
	} else if (mode == RENAME) {
		accepted = chats_rename(target, input, error, sizeof(error));
		snprintf(v->notice, sizeof(v->notice), "%s", accepted ? "renamed" : error);
		reload(v);
	}
	if (accepted) {
		if (mode == INSERT) {
			ui_editor_remember(input);
		}
		if (ui_editor_reset("")) {
			to_normal(v);
		} else {
			snprintf(v->notice, sizeof(v->notice),
				"Accepted, but draft could not be cleared; avoid resending.");
		}
	}
	free(input);
}

static void go_back(struct view *v, int *result) {

	if (current_live(v)) {
		*result = CHATS_BOARD_RETURN;
	} else {
		snprintf(v->notice, sizeof(v->notice),
			*v->current ? "your chat has ended" : "no chat to return to");
	}
}

/* ---- main loop --------------------------------------------------------------- */

int board_run(const char *current, char *chosen, size_t size) {

	board_theme_bind(current);
	struct settings settings = {0};
	char error[256], place[256] = "", cwd[4096];
	if (settings_load(&settings, error, sizeof(error)) < 0) {
		snprintf(settings.model, sizeof(settings.model), "phi");
	}
	const char *home = getenv("HOME");
	if (getcwd(cwd, sizeof(cwd))) {
		size_t home_length = home ? strlen(home) : 0;
		if (home_length && !strncmp(cwd, home, home_length) &&
			(cwd[home_length] == '/' || !cwd[home_length])) {
			snprintf(place, sizeof(place), "~%.250s", cwd + home_length);
		} else {
			snprintf(place, sizeof(place), "%.250s", cwd);
		}
	}
	struct view v;
	memset(&v, 0, sizeof(v));
	v.model = settings.model;
	v.effort = settings.effort;
	v.place = place;
	v.settings = &settings;
	v.collapsed = board_saved_collapsed;
	snprintf(v.current, sizeof(v.current), "%s", current);
	v.count = chats_rows(&v.rows);
	for (size_t i = 0; i < v.count; i++) {
		if (!strcmp(v.rows[i].id, current)) {
			v.cursor = v.anchor = i;
		}
	}

	board_editor_view = &v;
	if (!ui_editor_attach(v.model, v.effort, theme_transcript_path, board_editor_redraw,
			board_editor_feedback, &v)) {
		board_editor_view = NULL;
		free(v.rows);
		return CHATS_BOARD_RETURN;
	}
	v.editor = 1;
	if (board_saved_draft && !ui_editor_reset(board_saved_draft)) {
		ui_close();
		board_editor_view = NULL;
		free(v.rows);
		return CHATS_BOARD_RETURN;
	}
	terminal_editor_host(1);
	terminal_configure_safety(settings.approval, settings.sandbox);
	struct tool_host previous_host;
	tools_get_host(&previous_host);
	struct tool_host output_host = {
		.display = board_output, .styled = board_styled_output, .cancel = ui_cancel, .banners = 1};
	tools_set_host(&output_host);
	load_colors();
	clearok(curscr, TRUE);
	flushinp();
	mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
	set_escdelay(25);
	fputs("\033[?1003h", stdout);
	fflush(stdout);
	int result = -1;
	double last_reload = board_now();
	while (result < 0 && !v.quit) {
		draw(&v);
		int program_wait = -1;
		struct surface_client *programs[] = {
			board_program, board_header_program, board_footer_program};
		if (v.mode != CONFIRM) {
			for (int i = 0; i < 3; i++) {
				int wait = surface_client_wait(programs[i]);
				if (wait >= 0 && (program_wait < 0 || wait < program_wait)) {
					program_wait = wait;
				}
			}
		}
		int blink_wait = board_blink_wait(board_now());
		if (blink_wait >= 0 && (program_wait < 0 || blink_wait < program_wait)) {
			program_wait = blink_wait;
		}
		int wait = program_wait < 0 || program_wait > 1000 ? 1000
			: program_wait < 10                            ? 10
														   : program_wait;
		wint_t key;
		int kind = ui_editor_read(&key, wait);
		if (kind == ERR) {
			/* A closed terminal also reads as ERR, at once and forever: leave
               instead of spinning (chats keep running). */
			struct pollfd terminal = {STDIN_FILENO, POLLIN, 0};
			if (poll(&terminal, 1, 0) > 0 && (terminal.revents & (POLLHUP | POLLERR | POLLNVAL))) {
				result = CHATS_BOARD_QUIT;
				continue;
			}
			if (board_now() - last_reload >= 1) {
				last_reload = board_now();
				reload(&v);
				/* Native panel polling must not turn status scans into a busy loop. */
				int theme_changed = v.defaults_theme ? 0 : board_theme_bind(v.current);
				if (theme_changed || !theme_stamp_same(theme_stamp_now(), board_theme)) {
					ui_editor_theme(theme_transcript_path);
					load_colors();
					surface_pair_count = 0;
					clearok(curscr, TRUE);
				}
			}
			continue;
		}
		int special = kind == KEY_CODE_YES;
		int paste_event = ui_editor_paste_event(kind, key);
		if (v.mode == FILTER) {
			if (special && key == KEY_MOUSE) {
				MEVENT ignored;
				(void)getmouse(&ignored);
				continue;
			}
			if (paste_event) {
				v.paste_block = paste_event == 1;
				continue;
			}
			board_filter_key(&v, special, key);
			continue;
		}
		if (v.mode == NORMAL && !special && key == '/') {
			v.mode = FILTER;
			v.heading = 0;
			v.motion_count = 0;
			v.pending_g = v.pending_d = v.ctrl_c = v.paste_block = 0;
			snprintf(v.notice, sizeof(v.notice),
				"Filter / · titles and recent messages · Enter keeps · Esc clears");
			board_filter_update(&v, 0);
			continue;
		}
		if (v.mode == NORMAL && !special && key == ':') {
			begin_input(&v, INSERT, "", "");
			(void)ui_editor_key(OK, '/');
			continue;
		}
		if (v.mode == NORMAL && ((!special && key == '\t') || (special && key == KEY_BTAB))) {
			v.pending_g = v.pending_d = v.ctrl_c = 0;
			v.motion_count = 0;
			board_next_heading(&v, special ? -1 : 1);
			continue;
		}
		if (v.mode == CONFIRM) {
			if (paste_event == 1) {
				v.paste_block = 1;
			}
			if (paste_event == -1) {
				v.paste_block = 0;
				continue;
			}
			if (v.paste_block) {
				continue;
			}
		}
		if (v.mode != CONFIRM && v.mode != INSERT && v.mode != REPLY && v.mode != RENAME &&
			ui_editor_activation(kind, key)) {
			begin_input(&v, INSERT, "", "");
			int action = ui_editor_key(kind, key);
			if (action == 1) {
				submit(&v);
			}
			continue;
		}
		if (special && key == KEY_MOUSE && v.mode != CONFIRM) {
			MEVENT mouse;
			if (getmouse(&mouse) == OK && mouse.x >= 2 && mouse.x < COLS - 2) {
				if (board_heading_mouse(&v, &mouse)) {
					continue;
				}
				int positions[] = {board_program_y, board_header_y, board_footer_y};
				int heights[] = {board_program_height, board_header_height, board_footer_height};
				int event_kind = (mouse.bstate & BUTTON1_PRESSED) ? PHI_WIDGET_PRESS
					: (mouse.bstate & BUTTON1_RELEASED)           ? PHI_WIDGET_RELEASE
					: (mouse.bstate & BUTTON1_CLICKED)            ? PHI_WIDGET_CLICK
																  : -1;
				for (int i = 0; event_kind >= 0 && i < 3; i++) {
					if (mouse.y >= positions[i] && mouse.y < positions[i] + heights[i]) {
						struct phi_surface_event event = {.kind = event_kind,
							.x = mouse.x - 2,
							.y = mouse.y - positions[i],
							.button = 1};
						surface_client_event(programs[i], &event);
						break;
					}
				}
			}
			continue;
		}
		int enter = (special && key == KEY_ENTER) || (!special && (key == '\r' || key == '\n'));
		if (!special && key == 3 && v.mode != INSERT && v.mode != REPLY &&
			v.mode != RENAME) { /* Ctrl+C twice leaves the board, never agents */
			if (v.ctrl_c) {
				result = CHATS_BOARD_QUIT;
				continue;
			}
			v.ctrl_c = 1;
			to_normal(&v);
			snprintf(v.notice, sizeof(v.notice), "ctrl+c again to leave · chats keep running");
			continue;
		}
		v.ctrl_c = 0;

		if (v.mode == CONFIRM) {
			if (LINES < 12 || COLS < 45) {
				if (!special && key == 27) {
					to_normal(&v);
				}
				continue;
			}
			if (!special && (key == 'y' || key == 'Y')) {
				confirm_delete(&v);
			} else {
				to_normal(&v);
				snprintf(v.notice, sizeof(v.notice), "kept");
			}
			continue;
		}

		if (v.mode == INSERT || v.mode == REPLY || v.mode == RENAME) {
			if (!ui_editor_pasting() && !special && key == 27 && ui_editor_normal()) {
				if (v.mode != INSERT) {
					ui_editor_reset("");
				}
				to_normal(&v);
			} else {
				int action = ui_editor_key(kind, key);
				if (action == 1) {
					submit(&v);
				} else if (action == 2) {
					result = CHATS_BOARD_QUIT;
				}
				if (special && key == KEY_BTAB && v.mode == INSERT &&
					strcmp(settings.effort, ui_effort())) {
					char effort_command[64];
					snprintf(effort_command, sizeof(effort_command), "/effort %s", ui_effort());
					(void)command_run(NULL, &settings, effort_command);
					if (strcmp(settings.effort, ui_effort())) {
						ui_configure(settings.model, settings.effort);
						snprintf(v.notice, sizeof(v.notice),
							"Could not save default effort; unchanged.");
					}
				}
			}
			continue;
		}

		/* Normal and visual. */
		if (board_motion_key(&v, special, key)) {
			continue;
		}
		if (v.mode == NORMAL && v.heading && (enter || (!special && key == ' '))) {
			v.pending_g = v.pending_d = 0;
			v.collapsed ^= 1u << (v.heading - 1);
			continue;
		}
		if (v.mode == NORMAL && v.heading &&
			((special && (key == KEY_LEFT || key == KEY_RIGHT)) ||
				(!special && (key == 'h' || key == 'l')))) {
			v.pending_g = v.pending_d = 0;
			if ((special && key == KEY_LEFT) || (!special && key == 'h')) {
				v.collapsed |= 1u << (v.heading - 1);
			} else {
				v.collapsed &= ~(1u << (v.heading - 1));
			}
			continue;
		}
		if (v.mode == NORMAL && !special && key == 27 && v.query[0]) {
			v.query[0] = 0;
			v.notice[0] = 0;
			v.pending_g = v.pending_d = 0;
			continue;
		}
		if (v.mode == NORMAL && !special && key == 27 && v.heading) {
			v.heading = 0;
			continue;
		}
		if (!v.query[0]) {
			v.notice[0] = 0;
		}
		int d = v.pending_d;
		v.pending_d = 0;
		if (!special && key == 27) {
			if (v.mode == VISUAL) {
				to_normal(&v);
			} else {
				go_back(&v, &result);
			}
		} else if (!special && key == 'v' && !v.heading) {
			if (v.mode == VISUAL) {
				to_normal(&v);
			} else if (v.count) {
				v.mode = VISUAL;
				v.selecting = 1;
				v.anchor = v.cursor;
			}
		} else if (!special && (key == 'x' || (key == 'd' && (d || v.mode == VISUAL)))) {
			end_or_forget(&v);
		} else if (!special && key == 'd') {
			v.pending_d = 1;
		} else if (v.mode == VISUAL) {
			continue; /* nothing else while selecting */
		} else if ((special && key == KEY_LEFT) || (!special && key == 'h')) {
			go_back(&v, &result);
		} else if (((special && key == KEY_RIGHT) || (!special && key == 'l') || enter) &&
			v.count && !v.heading && board_visible(&v, v.cursor)) {
			const struct chat_row *r = &v.rows[v.cursor];
			snprintf(chosen, size, "%s", r->id);
			result = !strcmp(r->id, v.current) && r->live ? CHATS_BOARD_RETURN : CHATS_BOARD_OPEN;
		} else if (!special && (key == 'i' || key == 'a' || key == 'o' || key == 'O')) {
			begin_input(&v, INSERT, "", "");
		} else if (!special && key == ' ' && v.count && !v.heading && board_visible(&v, v.cursor)) {
			char label[160];
			snprintf(label, sizeof(label),
				"reply to %.60s: ", *v.rows[v.cursor].title ? v.rows[v.cursor].title : "new chat");
			begin_input(&v, REPLY, label, "");
			snprintf(v.target, sizeof(v.target), "%s", v.rows[v.cursor].id);
		} else if (!special && key == 'r' && v.count && !v.heading && board_visible(&v, v.cursor)) {
			begin_input(&v, RENAME, "rename: ", v.rows[v.cursor].title);
			snprintf(v.target, sizeof(v.target), "%s", v.rows[v.cursor].id);
		} else if (!special && key == 'q') {
			result = CHATS_BOARD_QUIT;
		} else if (!special && key == '?') {
			v.help = !v.help;
		}
	}
	surface_client_free(board_program);
	surface_client_free(board_header_program);
	surface_client_free(board_footer_program);
	board_program = board_header_program = board_footer_program = NULL;
	board_program_height = board_header_height = board_footer_height = 0;
	if (v.quit) {
		result = CHATS_BOARD_QUIT;
	}
	if (v.mode == INSERT || v.mode == NORMAL || v.mode == VISUAL) {
		char *draft = ui_editor_text();
		if (draft) {
			free(board_saved_draft);
			board_saved_draft = draft;
		}
	}
	tools_set_host(&previous_host);
	terminal_editor_host(0);
	ui_close();
	board_editor_view = NULL;
	board_saved_collapsed = v.collapsed;
	free(v.output);
	free(v.rows);
	free(v.recent);
	free(v.scores);
	skin_release(&board_skin);

	return result;
}
