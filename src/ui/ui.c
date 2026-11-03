#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include <curses.h>
#include <locale.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "ui.h"
#include "../core/commands.h"
#include "../core/json.h"
#include "../core/fsutil.h"
#include "../core/host_attach.h"
#include "../core/tool-result.h"
#include "../core/models.h"
#include "../core/ui-layout.h"
#include "../core/ui-frame.h"
#include "../core/surfaces.h"
#include "terminal-colors.h"

/* Chat hosts paint in a local window; dialogs retain the physical root. */
static WINDOW *ui_canvas;

static WINDOW *ui_physical_screen(void) {

	return stdscr;
}

static int ui_physical_cols(void) {

	return COLS;
}

static int ui_physical_rows(void) {

	return LINES;
}

static int ui_physical_update(void) {

	return doupdate();
}

static WINDOW *ui_screen(void) {

	return ui_canvas ? ui_canvas : ui_physical_screen();
}

#undef stdscr
#undef COLS
#undef LINES
#define stdscr ui_screen()
#define COLS (ui_canvas ? getmaxx(ui_canvas) : ui_physical_cols())
#define LINES (ui_canvas ? getmaxy(ui_canvas) : ui_physical_rows())
#define doupdate() (ui_canvas ? OK : ui_physical_update())

/* A nonspacing mark belongs to the preceding cell, even at a wrap edge. */
static void ui_cell_mark(WINDOW *window, int row, int after, wchar_t mark) {

	if (after <= 0) {
		return;
	}
	cchar_t cell;
	wchar_t text[CCHARW_MAX + 1] = {0};
	attr_t attr;
	short pair;
	if (mvwin_wch(window, row, after - 1, &cell) == ERR ||
		getcchar(&cell, text, &attr, &pair, NULL) == ERR) {
		return;
	}
	int span = wcwidth(text[0]);
	size_t n = wcslen(text);
	if (n + 1 >= CCHARW_MAX) {
		return;
	}
	text[n] = mark;
	text[n + 1] = 0;
	if (setcchar(&cell, text, attr, pair, NULL) == OK) {
		(void)mvwadd_wch(window, row, after - (span > 0 ? span : 1), &cell);
	}
}

static int ui_chat_x, ui_chat_y, ui_chat_columns;
static WINDOW *ui_editor_canvas;
static void (*editor_host_redraw)(void *);
static void (*editor_host_feedback)(const char *, void *);
static void *editor_host_context;
static int editor_workspace;
static int ui_split_hosts, ui_editor_x, ui_editor_y;
/* Optional semantic hosts keep physical bounds for input, and temporary local
   windows only during a frame. They never own the data they display. */
static WINDOW *ui_host_canvas[PHI_HOST_COUNT];
static struct phi_rect ui_host_boxes[PHI_HOST_COUNT];

#define INPUT_SIZE 4096
#define LOG_SIZE 131072
#define KEY_NEWLINE (KEY_MAX + 1)
#define KEY_PASTE_BEGIN (KEY_MAX + 2)
#define KEY_PASTE_END (KEY_MAX + 3)
#define KEY_ESCAPE (KEY_MAX + 4)
#define KEY_EOF (KEY_MAX + 5)
#define KEY_FOCUS_IN (KEY_MAX + 6)
#define KEY_FOCUS_OUT (KEY_MAX + 7)
#define KEY_INTERRUPT (KEY_MAX + 8)
#define KEY_CLIPBOARD (KEY_MAX + 9)
#define KEY_COMMAND_MENU (KEY_MAX + 10)
#define KEY_CHAT_BACKGROUNDED (KEY_MAX + 11)
#define KEY_SHIFT_SPACE (KEY_MAX + 12)
#define KEY_CTRL_LETTER_BASE (KEY_MAX + 32)
static WINDOW *key_window;
static int report_state; /* unknown terminal report currently being drained */
static int report_filtered; /* ERR caused by a report, not terminal EOF */
static struct timespec report_started;
static char transcript_storage[LOG_SIZE];
static char *transcript = transcript_storage;
/* Styles are display semantics, not curses pair numbers. Tool presentation:
   67–69 number gutters, 70 hunk label, 71 hidden heading fields, 72 card end,
   73 configurable spacing, 74 saved-snippet label, 75–77 change markers.
   Semantic spans travel with the bytes when cards fold, expand or get trimmed. */
static unsigned char transcript_style_storage[LOG_SIZE];
static unsigned char *transcript_style = transcript_style_storage;
static size_t used;

/* A saved block uses the same styled-byte grammar without changing live UI
   state. No event loop or extension callback runs while these sinks are used. */
struct styled_capture {
	char *text;
	unsigned char *styles;
	size_t used, capacity, body_start;
	int failed, tool_body, edit;
};
static struct styled_capture *styled_capture;
static int archive_layout;
static size_t theme_generation;
/* INPUT_SIZE is the initial allocation, not a message-length limit. */
static wchar_t input_initial[INPUT_SIZE];
static wchar_t *input = input_initial;
static size_t input_capacity = INPUT_SIZE;
static size_t length, cursor;

static int wide_reserve(wchar_t **buffer, size_t *capacity, size_t needed, wchar_t *initial) {

	if (needed <= *capacity) {
		return 1;
	}
	if (needed > SIZE_MAX / sizeof(**buffer)) {
		return 0;
	}
	size_t next = *capacity ? *capacity : INPUT_SIZE;
	while (next < needed) {
		if (next > SIZE_MAX / sizeof(**buffer) / 2) {
			next = needed;
			break;
		}
		next *= 2;
	}
	wchar_t *grown = *buffer == initial ? malloc(next * sizeof(*grown))
										: realloc(*buffer, next * sizeof(*grown));
	if (!grown) {
		return 0;
	}
	if (*buffer == initial && initial) {
		memcpy(grown, initial, *capacity * sizeof(*grown));
	}
	*buffer = grown;
	*capacity = next;

	return 1;
}

static int input_reserve(size_t characters) {

	return characters < SIZE_MAX &&
		wide_reserve(&input, &input_capacity, characters + 1, input_initial);
}

static wchar_t *wide_copy(const wchar_t *text, size_t count) {

	if (count >= SIZE_MAX / sizeof(wchar_t)) {
		return NULL;
	}
	wchar_t *copy = malloc((count + 1) * sizeof(*copy));
	if (copy) {
		memcpy(copy, text, (count + 1) * sizeof(*copy));
	}

	return copy;
}

static wchar_t *wide_decode(const char *text, size_t *count) {

	*count = mbstowcs(NULL, text ? text : "", 0);
	if (*count == (size_t)-1 || *count >= SIZE_MAX / sizeof(wchar_t)) {
		return NULL;
	}
	wchar_t *wide = malloc((*count + 1) * sizeof(*wide));
	if (wide) {
		mbstowcs(wide, text ? text : "", *count + 1);
	}

	return wide;
}

static int active, working, scroll_offset, dirty, modal_active, presentation_batch;
static int working_view_valid, working_view_maximum;
/* Keep the expensive markdown transcript rendered between viewport-only redraws.
   Wheel/PgUp scrolling should copy rows, not parse and repaint the whole log. */
static WINDOW *transcript_pad_cache;
static size_t transcript_revision, transcript_cache_revision = (size_t)-1;
static int transcript_cache_width, transcript_cache_rows;
static int ctrl_c_armed;
/* A completely empty chat/composer closes with one Left in either mode.
   First foreground backgrounding requires NORMAL-mode Left twice; revisits
   need one. Both require an empty composer and never stop running work. */
static int chat_turns, left_armed, chat_backgrounded;
static double left_armed_at;
/* Esc while Phi works: the first press warns, a second within 2.5 s stops
   the turn (a running tool included), so one stray Esc cannot. */
static int esc_armed;
static double esc_armed_at;
static int sync_output;
static unsigned sync_depth;
static char footer_model[96], account_usage[128], extension_footer[256];
static void (*idle_callback)(void);
static int external_cancelled;

void ui_request_cancel(void) {

	external_cancelled = 1;
}

static int (*external_pending)(void);

void ui_set_wakeup(int (*pending)(void)) {

	external_pending = pending;
}

#define UI_COMMAND_LIMIT 512
static size_t (*extra_command_count)(void);
static const struct phi_command *(*extra_command_at)(size_t);
static size_t (*extra_model_count)(void);
static int (*extra_model_at)(size_t, const char **, const char **, const char **, int64_t *);

void ui_set_extension_commands(size_t (*count)(void), const struct phi_command *(*at)(size_t)) {

	extra_command_count = count;
	extra_command_at = at;
	dirty = 1;
}

void ui_set_extension_models(size_t (*count)(void),
	int (*at)(size_t, const char **, const char **, const char **, int64_t *)) {

	extra_model_count = count;
	extra_model_at = at;
	dirty = 1;
}

void ui_extension_status(const char *text) {

	if (!text) {
		text = "";
	}
	if (strcmp(extension_footer, text)) {
		snprintf(extension_footer, sizeof(extension_footer), "%s", text);
		dirty = 1;
	}
}

static size_t ui_command_count(void) {

	size_t extra = extra_command_count && extra_command_at ? extra_command_count() : 0;
	if (extra > UI_COMMAND_LIMIT - PHI_COMMAND_COUNT) {
		extra = UI_COMMAND_LIMIT - PHI_COMMAND_COUNT;
	}

	return PHI_COMMAND_COUNT + extra;
}

static const struct phi_command *ui_command_at(size_t index) {

	return index < PHI_COMMAND_COUNT ? &phi_commands[index]
		: extra_command_at           ? extra_command_at(index - PHI_COMMAND_COUNT)
									 : NULL;
}

void ui_set_idle(void (*callback)(void)) {

	idle_callback = callback;
}

void ui_account_usage(const char *text) {

	if (strcmp(account_usage, text)) {
		snprintf(account_usage, sizeof(account_usage), "%s", text);
		dirty = 1;
	}
}

/* The effort in use: always one the current model takes. */
static char current_effort[16] = "none";

const char *ui_effort(void) {

	return current_effort;
}

static void effort_set(const char *model, const char *effort) {

	if (!effort || !*effort) {
		effort = "none";
	}
	const char *usable = phi_effort_for(model, effort);
	if (usable != current_effort) {
		snprintf(current_effort, sizeof(current_effort), "%s", usable);
	}
}

int ui_columns(void) {

	int n = COLS > 8 ? COLS - 8 : 40;

	return n > 92 ? 92 : n;
}

/* Shift+Tab: the model's next level, wrapping to its lightest. A model the
   catalogue does not describe gets off, low, medium and high. */
/* The current model's effort levels in the server's order, with its own
   descriptions (off, low, medium and high for a model the catalogue does
   not describe). Returns how many there are; fills in entry i. */
static size_t model_level(size_t i, const char **name, const char **description, int *preferred) {

	static const char *const plain[][2] = {{"none", "No reasoning"}, {"low", "Light reasoning"},
		{"medium", "Balanced reasoning"}, {"high", "More reasoning"}};
	const struct phi_effort_level *levels;
	int fallback;
	size_t count = phi_catalog_levels(footer_model, &levels, &fallback);
	if (!count) {
		if (i < 4) {
			*name = plain[i][0];
			*description = plain[i][1];
			*preferred = 0;
		}
		return 4;
	}
	if (i < count) {
		*name = levels[i].name;
		*description = levels[i].description;
		*preferred = (int)i == fallback;
	}

	return count;
}

/* Shift+Tab: the model's next level, wrapping to its first. */
static void cycle_effort(void) {

	const char *name = NULL, *description;
	int preferred;
	size_t count = model_level(0, &name, &description, &preferred), at = count - 1;
	for (size_t i = 0; i < count; i++) {
		model_level(i, &name, &description, &preferred);
		if (!strcmp(name, current_effort)) {
			at = i;
		}
	}
	model_level((at + 1) % count, &name, &description, &preferred);
	snprintf(current_effort, sizeof(current_effort), "%s", name);
	dirty = 1;
}

#define FRAME_INTERVAL 0.033
/* Wheel scrolling paints at most every SCROLL_FRAME. 0: every wheel event
   paints at once (uncapped); 0.016 caps a fast spin at one frame per 16 ms. */
#define SCROLL_FRAME 0
static int wheel_pending; /* a wheel scroll waiting for its frame */
static double last_draw, work_started;
static time_t work_started_wall; /* the turn's start in whole seconds */
static long last_activity_frame = -1, last_shimmer_step = -1;
static char *queued[8];
static size_t queued_count;
static int normal_mode, delete_pending, pasting, paste_cr;
static int replace_pending, replace_changed;
static size_t vim_count, replace_count, last_replace_count, replace_start;
static void normal_cursor(void);
static wchar_t last_replace_character;

static void vim_pending_clear(void) {

	replace_pending = 0;
	vim_count = replace_count = 0;
}

static int history_view_paste(int state);
/* One snapshot per bracketed paste, rather than one per received character. */
static wchar_t *paste_before;
static size_t paste_before_length, paste_before_cursor;
static double paste_saved_at;
static int paste_recovery_pending;
static void paste_snapshot_finish(void);
static void paste_checkpoint(void);

static void paste_snapshot_clear(void) {

	free(paste_before);
	paste_before = NULL;
	paste_recovery_pending = 0;
}

static int input_view, conversation_height;

/* Transcript selection lives in rendered pad coordinates, not source bytes.
   This makes copied text match what the user actually sees after Markdown and
   tool-card rendering. */
struct transcript_selection_state {
	int visible, dragging;
	int anchor_row, anchor_col, row, col;
	int mouse_y, mouse_x;
	double last_scroll;
};
static struct transcript_selection_state transcript_selection;
static int conversation_source, conversation_destination, conversation_copy_rows;
static int conversation_width, conversation_pad_rows, conversation_maximum;
static int input_left = 2;
static int reading_width = 0; /* Full terminal width unless explicitly limited. */
static int tool_preview_ms = 750; /* pre-execution cancel window, PHI_TOOL_PREVIEW_MS */
static int cursor_shape = -1;
static int focused = 1;

enum tool_syntax_kind {
	TOOL_SYNTAX_NONE,
	TOOL_SYNTAX_PYTHON,
	TOOL_SYNTAX_C
};

static int tool_active, tool_output_line, tool_syntax, tool_heading_valid, tool_bash;
static int tool_poll, tool_edit, tool_historical;
static int tool_category, tool_details;
static char tool_running_label[640]; /* "ls" or "read notes.txt": the running card's one line */
static char tool_live_name[64]; /* the running tool's name, for its live card */

/* End of visible content, excluding trailing layout separators. */
static size_t content_end(size_t end) {

	while (end) {
		char ch = transcript[end - 1];
		if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') {
			break;
		}
		if ((transcript_style[end - 1] == 1 || transcript_style[end - 1] >= 4) && ch != '\n' &&
			ch != '\r') {
			break;
		}
		end--;
	}

	return end;
}

static char shell_activity[512];
static const char *shell_activity_state = "";

static void shell_activity_set(const char *command, const char *state) {

	size_t n = strcspn(command, "\r\n");
	if (n >= sizeof(shell_activity)) {
		n = sizeof(shell_activity) - 1;
	}
	while (n && ((unsigned char)command[n] & 0xc0) == 0x80) {
		n--;
	}
	memcpy(shell_activity, command, n);
	shell_activity[n] = 0;
	for (char *p = shell_activity; *p; p++) {
		if ((unsigned char)*p < 32 || *p == 127) {
			*p = ' ';
		}
	}
	shell_activity_state = state;
}

static long long tool_job_id;

enum {
	ACT_READ,
	ACT_EDIT,
	ACT_WRITE,
	ACT_SHELL,
	ACT_JOB,
	ACT_WEB,
	ACT_OTHER,
	ACT_COUNT
};

static size_t tool_header_start;
static size_t tool_added, tool_removed;
static char tool_detail[512];
static size_t tool_heading_end;
static double tool_started;
/* Folded bodies live outside the bounded visible transcript. */
#define TOOL_CARD_LIMIT 32

struct semantic_frame_cache {
	struct phi_widget_cell *cells;
	int width, rows;
	unsigned generation;
	uint64_t version;
	char provider[64];
};

static void semantic_messages_clear(void);
static void semantic_live_clear(void);
static void semantic_live_changed(void);

struct tool_card {
	size_t start, end, full_len, folded_len;
	int failed; /* never merged with neighbours, so an error is never hidden in a count */
	char *full, *folded;
	unsigned char *styles, *folded_styles;
	int expanded, fixed; /* fixed review cards never fold into activity summaries */
	json_object *semantic; /* immutable invocation/result records, grouped only for display */
	struct semantic_frame_cache projection;
	unsigned counts[ACT_COUNT];
	long long jobs[64];
	size_t job_count;
};
static struct tool_card tool_cards[TOOL_CARD_LIMIT];
static size_t tool_card_count, tool_body_start;
static int tools_expanded_default; /* the skin's tool.expanded (skin_use) */
static int tool_full; /* Ctrl+O with tool.max_lines: open cards show all their output */
static int pending_approval;
static json_object *tool_pending_semantic;
static uint64_t semantic_block_id;

static void tool_card_release(struct tool_card *card) {

	free(card->full);
	free(card->styles);
	free(card->folded);
	free(card->folded_styles);
	if (card->semantic) {
		json_object_put(card->semantic);
	}
	free(card->projection.cells);
}

static void tool_cards_clear(void) {

	semantic_live_clear();
	for (size_t i = 0; i < tool_card_count; i++) {
		tool_card_release(&tool_cards[i]);
	}
	tool_card_count = 0;
	if (tool_pending_semantic) {
		json_object_put(tool_pending_semantic);
	}
	tool_pending_semantic = NULL;
	shell_activity[0] = 0;
	tool_details = tools_expanded_default; /* a fresh conversation starts as the theme says */
}

static void tool_cards_drop(size_t bytes) {

	size_t keep = 0;
	for (size_t i = 0; i < tool_card_count; i++) {
		struct tool_card card = tool_cards[i];
		if (card.start < bytes) {
			tool_card_release(&card);
		} else {
			card.start -= bytes;
			card.end -= bytes;
			tool_cards[keep++] = card;
		}
	}
	tool_card_count = keep;
	tool_body_start = tool_body_start < bytes ? 0 : tool_body_start - bytes;
	tool_header_start = tool_header_start < bytes ? 0 : tool_header_start - bytes;
}

static int visual_mode; /* 1 character, 2 rectangular block */
static size_t visual_anchor;
static wchar_t find_character;
static int find_direction, find_till, find_pending;
static wchar_t yank_initial[INPUT_SIZE + 1];
static wchar_t *yank = yank_initial;
static size_t yank_capacity = INPUT_SIZE + 1;
static size_t yank_length;
static int yank_linewise, yank_blockwise;

static void selection(size_t *begin, size_t *end) {

	*begin = cursor < visual_anchor ? cursor : visual_anchor;
	*end = (cursor > visual_anchor ? cursor : visual_anchor) + 1;
	if (*begin > length) {
		*begin = length;
	}
	if (*end > length) {
		*end = length;
	}
}

static double now(void) {

	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);

	return t.tv_sec + t.tv_nsec / 1e9;
}

#include "undo.inc"

/* DEC private mode 2026 lets supporting terminals reveal a complete repaint
   atomically.  A depth counter makes this safe for nested users such as the
   optional Sixel title. */
static void frame_begin(void) {

	if (sync_depth++ == 0 && sync_output) {
		static const char sequence[] = "\033[?2026h";
		fputs(sequence, stdout);
		fflush(stdout);
	}
}

static void frame_end(void) {

	if (!sync_depth) {
		return;
	}
	if (--sync_depth == 0 && sync_output) {
		static const char sequence[] = "\033[?2026l";
		fflush(stdout); /* title/Sixel writes must land before the frame closes. */
		fputs(sequence, stdout);
		fflush(stdout);
	}
}

/* wget_wch() may refresh a touched non-pad window before reading. SIGWINCH
   touches stdscr, which used to publish an old/intermediate layout before
   Phi's explicit repaint.  An undisplayed pad keeps ncurses key decoding
   without ever being presented. */
static int read_key_input(wint_t *key, int wait_ms) {

	report_filtered = 0;
	wtimeout(key_window, wait_ms);
	int kind = wget_wch(key_window, key);
	/* Ncurses returns an unknown escape sequence as individual characters.
       Consume its protocol envelope here, before any editor or modal sees it.
       Keep partial reports across reads; a lone Escape remains a real key. */
	/* States: 1 CSI/SS3, 2 control string, 3 string Escape. */
	struct timespec current;
	clock_gettime(CLOCK_MONOTONIC, &current);
	if (report_state &&
		(current.tv_sec - report_started.tv_sec + (current.tv_nsec - report_started.tv_nsec) / 1e9 >
			0.25)) {
		report_state = 0;
	}
	if (!pasting && !history_view_paste(-1) && !report_state && kind == OK && *key == 27) {
		wint_t next;
		wtimeout(key_window, 25);
		int next_kind = wget_wch(key_window, &next);
		if (next_kind == OK &&
			(next == '[' || next == 'O' || next == ']' || next == 'P' || next == '^' ||
				next == '_')) {
			report_state = (next == '[' || next == 'O') ? 1 : 2;
			report_started = current;
			wtimeout(key_window, 0);
			kind = wget_wch(key_window, key);
		} else if (next_kind == OK) {
			unget_wch(next);
		} else if (next_kind == KEY_CODE_YES) {
			ungetch((int)next);
		}
	}
	if (report_state) {
		wtimeout(key_window, 0);
		size_t budget = 4096;
		while (kind != ERR && budget--) {
			if (kind == KEY_CODE_YES) {
				/* An incomplete terminal report must not eat the next real
                   decoded key, paste boundary, or mouse event. Resize does
                   not finish the report; subsequent bytes may still arrive. */
				if (*key == KEY_RESIZE) {
					return kind;
				}
				report_state = 0;
				break;
			} else if (report_state == 1) {
				if (*key >= 0x40 && *key <= 0x7e) {
					report_state = 0;
				} else if (*key == 27) {
					report_state = 3;
				}
			} else if (*key == 7 || (report_state == 3 && *key == '\\')) {
				report_state = 0;
			} else {
				report_state = *key == 27 ? 3 : 2;
			}
			if (!report_state || !budget) {
				report_filtered = 1;
				return ERR;
			}
			kind = wget_wch(key_window, key);
		}
		if (kind == ERR) {
			report_filtered = 1;
			return ERR;
		}
	}
	if (kind == KEY_CODE_YES && *key == KEY_CHAT_BACKGROUNDED) {
		chat_backgrounded = 1;
		left_armed = 0;
		dirty = 1;
		/* A same-size reattach can coalesce both SIGWINCH notifications.
           Its explicit lifecycle notice must invalidate the screen too. */
		clearok(curscr, TRUE);
		clearok(ui_physical_screen(), TRUE);
		cursor_shape = -1;
		report_filtered = 1;
		return ERR; /* not a user/editor/approval keystroke */
	}
	/* Modified Space is still text, including in shared/modal editors. */
	if (kind == KEY_CODE_YES && *key == KEY_SHIFT_SPACE) {
		*key = L' ';
		return OK;
	}
	if (kind == KEY_CODE_YES && *key >= KEY_CTRL_LETTER_BASE && *key < KEY_CTRL_LETTER_BASE + 26) {
		*key = *key - KEY_CTRL_LETTER_BASE + 1;
		return OK;
	}

	return kind;
}

#include "clipboard.inc"
#include "theme.inc"

static int read_key(wint_t *key, int wait_ms) {

	int kind = read_key_input(key, wait_ms);
	if (kind == KEY_CODE_YES && *key == KEY_PASTE_BEGIN) {
		history_view_paste(1);
	}
	if (kind == KEY_CODE_YES && *key == KEY_PASTE_END) {
		history_view_paste(0);
	}
	/* Repaint at the shared input boundary: inspectors and extension hooks
       can consume resize before edit_key_raw ever sees it. SSH reattachment
       must invalidate the physical screen even in those readers. Ctrl+L is
       the same non-destructive recovery path, never model input. */
	int redraw_key = kind == OK && *key == 12 && !pasting && !history_view_paste(-1);
	if ((kind == KEY_CODE_YES && *key == KEY_RESIZE) || redraw_key) {
		clearok(curscr, TRUE);
		clearok(ui_physical_screen(), TRUE);
		dirty = 1;
		cursor_shape = -1;
		if (redraw_key) {
			kind = KEY_CODE_YES;
			*key = KEY_RESIZE;
		}
	}
	/* Reattach forces a resize. The board (or another chat) may have reset
       the terminal palette while this UI's ncurses state stayed alive.
       Handle this here so dialogs and other modal readers restore it too. */
	if (kind == KEY_CODE_YES && *key == KEY_RESIZE && theme_current_known) {
		theme_apply(theme_current);
	}

	return kind;
}

/* theme.json as last loaded here; see theme_follow. */
static struct theme_stamp theme_seen;
static double theme_checked;
static char theme_pending_name[64];
void ui_set_tools_expanded(int expanded);

/* Make next the skin. tool.expanded opens or folds the cards only when the
   theme changes it, so a reload never undoes your Ctrl+O. */
static void skin_use(struct skin *next, int first) {

	int was = skin.tool_expanded;
	skin_move(&skin, next);
	theme_generation++;
	tools_expanded_default = skin.tool_expanded;
	if (first || skin.tool_expanded != was) {
		ui_set_tools_expanded(skin.tool_expanded);
	}
}

#include "title.inc"

static size_t shell_projection_end(size_t end, int width);
static short surface_pair(struct phi_widget_cell cell, int base_pair);
#include "semantic-frame.inc"
#include "semantic-tools.inc"
#include "semantic-live.inc"
#include "semantic-messages.inc"
#include "markdown.inc"
#include "context.inc"
static void ui_native_transition_forget(const char *owner);
static void chrome_header_row(int cols);
static int scrollback_active(void);
static WINDOW *scrollback_page(void);
static void scrollback_close(void);
static int scrollback_open(int movement);
static int scrollback_move(int movement);
static int scrollback_paint(int width, int rows, int left);
static int scrollback_key(int kind, wint_t key);
static int history_view_active(void);
static void history_view_close(void);
static int history_view_paint(int force);
static int history_view_key(int kind, wint_t key);
#include "widgets.inc"
static int cinema_ready(void);
static int cinema_banner_rows(int height);
static const char *cinema_controls(void);
#include "transition-plugins.inc"
#include "transitions.inc"
#include "welcome.inc"
#include "pending.inc"
static void transcript_selection_tracking(int enabled);
static void draw(int force);
#include "cinema.inc"
#include "side.inc"
#include "commands.inc"
#include "search.inc"
#include "draft-recovery.inc"

void ui_cwd_changed(void) {

	welcome_init();
	command_files_reset();
	transcript_revision++; /* relative tool-heading paths use the new cwd */
	dirty = 1;
}

static int block_selected(size_t at);

/* Draft layout without allocating a pad, including soft wraps. */
/* The draft starts with a command Phi knows ("/theme dark"): the length
   of its name, drawn in the accent colour; 0 otherwise, so a typo or a path
   such as /tmp/x stays plain. */
static size_t command_word_end(void) {

	if (!length || input[0] != L'/') {
		return 0;
	}
	size_t end = 1;
	while (end < length && !iswspace(input[end])) {
		end++;
	}
	char name[96];
	if (end >= sizeof(name)) {
		return 0;
	}
	for (size_t i = 0; i < end; i++) {
		if (input[i] > 127) {
			return 0;
		}
		name[i] = (char)input[i];
	}
	name[end] = 0;
	for (size_t i = 0, count = ui_command_count(); i < count; i++) {
		const struct phi_command *command = ui_command_at(i);
		if (command && command->name && !strcmp(command->name, name)) {
			return end;
		}
	}

	return 0;
}

static int draft_navigation_width;

static int draft_scan(int width, int top, int visible, int *cy, int *cx, int target_row,
	int target_col, size_t *target) {

	int closest = INT_MAX;
	if (visible) {
		draft_navigation_width = width;
	}
	int y = 0, x = 0;
	size_t begin = 0, end = 0;
	if (visual_mode) {
		selection(&begin, &end);
	}
	size_t command_end = visible ? command_word_end() : 0;
	for (size_t i = 0; i <= length; i++) {
		int cells = i < length && input[i] != L'\n'
			? (input[i] == L'\t' ? 4 - x % 4 : wcwidth(input[i]))
			: 0;
		if (cells < 0) {
			cells = 1;
		}
		if (i < length && input[i] == L'\t' && cells > width) {
			cells = width;
		}
		/* Move a whole word when it fits on a fresh line. Long words still
           break by character, and both passes use the same cursor geometry. */
		if (i < length && !iswspace(input[i]) && (i == 0 || iswspace(input[i - 1])) && x > 0) {
			int word_cells = 0;
			for (size_t j = i; j < length && !iswspace(input[j]); j++) {
				int span = wcwidth(input[j]);
				word_cells += span < 0 ? 1 : span;
				if (word_cells > width) {
					break;
				}
			}
			if (word_cells <= width && x + word_cells > width) {
				y++;
				x = 0;
			}
		}
		/* A full row is pending wrap, not an extra empty row. Only the
		   next printable character wraps; an explicit newline advances once. */
		if (i < length && cells > 0 && input[i] != L'\n' && (x + cells > width || x == width)) {
			y++;
			x = 0;
			if (i < length && input[i] == L'\t') {
				cells = width < 4 ? width : 4;
			}
		}
		if (i == cursor) {
			*cy = y;
			*cx = x;
		}
		if (target && y == target_row && (cells > 0 || i == length || input[i] == L'\n')) {
			int distance = abs(x - target_col);
			if (distance < closest) {
				closest = distance;
				*target = i;
			}
		}
		if (i == length) {
			break;
		}
		if (visible && y >= input_view && y < input_view + visible &&
			(input[i] != L'\n' || x < width)) {
			int selected =
				visual_mode == 2 ? block_selected(i) : (visual_mode && i >= begin && i < end);
			wchar_t ch[2] = {input[i] == L'\n' ? L' ' : input[i], 0};
			if (selected) {
				attron(A_REVERSE);
			}
			if (i < command_end) {
				attron(COLOR_PAIR(28));
			}
			if (input[i] == L'\t') {
				mvhline(top + y - input_view, input_left + x, ' ', cells);
			} else if (!cells && input[i] != L'\n' && x > 0) {
				ui_cell_mark(stdscr, top + y - input_view, input_left + x, input[i]);
			} else {
				mvaddwstr(top + y - input_view, input_left + x, ch);
			}
			if (i < command_end) {
				attroff(COLOR_PAIR(28));
			}
			if (selected) {
				attroff(A_REVERSE);
			}
		}
		if (input[i] == L'\n') {
			y++;
			x = 0;
			continue;
		}
		x += cells;
	}

	return y + 1;
}

static int draft_layout(int width, int top, int visible, int *cy, int *cx) {

	return draft_scan(width, top, visible, cy, cx, -1, 0, NULL);
}

static void draft_cursor_move(int row, int column) {

	/* Compact composers can end at the terminal's last cell. The logical
	   caret stays after the text, but curses cannot address a cell past it. */
	int columns = getmaxx(stdscr);
	if (column >= columns) {
		column = columns - 1;
	}
	move(row, column);
}

static int draft_vertical_target(int direction, size_t *target) {

	int width = draft_navigation_width, cy = 0, cx = 0;
	if (width <= 0) {
		struct phi_rect box = phi_frame_input_columns(COLS, skin.border == SKIN_BORDER_ROUNDED,
			*skin.prompt != 0, skin.composer_padding, skin.compact_layout);
		width = box.width;
	}
	draft_layout(width, 0, 0, &cy, &cx);
	if (cy + direction < 0) {
		return 0;
	}
	*target = SIZE_MAX;
	draft_scan(width, 0, 0, &cy, &cx, cy + direction, cx, target);

	return *target != SIZE_MAX;
}

/* Ignore trailing transcript separators when positioning the indicator,
   but preserve the shaded bottom padding of a user message. */
static int activity_line(int width) {

	size_t saved = used;
	used = semantic_messages_floor(content_end(shell_projection_end(used, width)), used);
	int line = layout(NULL, width) + 1 + skin.compact_layout;
	used = saved;

	return line;
}

static void border_overflow(int row, int cols, int count, wchar_t arrow) {

	if (count <= 0) {
		return;
	}
	wchar_t label[48];
	swprintf(label, sizeof(label) / sizeof(*label), L" %lc %d more ", arrow, count);
	int width = wcswidth(label, wcslen(label));
	if (width < 0 || width > cols - 2) {
		return;
	}
	attron(COLOR_PAIR(3));
	mvaddwstr(row, (cols - width) / 2, label);
	attroff(COLOR_PAIR(3));
}

static void draw(int force);
static int skin_word, skin_tip;
#define SHIMMER_STEP 0.07 /* seconds per letter */
static size_t turn_output_bytes; /* reply text this turn, for the token estimate */

/* The working line's tip: an extension's, else the skin's for this turn. */
static const char *activity_tip(void) {

	if (*working_tip) {
		return working_tip;
	}

	return skin.tip_count ? skin.tips[skin_tip % skin.tip_count] : "";
}

/* Collapsed shell work occupies the activity subline, not a transient card.
   Keep the last command there between calls so quick tools do not flash. */
static int activity_shell(void) {

	return skin.tool_live == 1 && !tool_details && *shell_activity &&
		(!pending_active || pending_is_shell());
}

/* A grouped live preview replaces only an adjacent, successful shell group.
   Mixed groups, failures, prose and expanded cards remain separate. */
static int shell_group_live(void) {

	return !semantic_live_frame(conversation_width) && working && skin.tool_live == 2 &&
		!tool_details && !pending_approval && *shell_activity &&
		(!strcmp(shell_activity_state, "queued") || !strcmp(shell_activity_state, "running")) &&
		(pending_active ? pending_is_shell() : (tool_active && tool_shell(tool_live_name)));
}

static struct tool_card *shell_previous_group(size_t end) {

	if (!tool_card_count) {
		return NULL;
	}
	struct tool_card *card = &tool_cards[tool_card_count - 1];
	if (card->failed || card->expanded || card->end > end || !card->counts[ACT_SHELL]) {
		return NULL;
	}
	for (int i = 0; i < ACT_COUNT; i++) {
		if (i != ACT_SHELL && card->counts[i]) {
			return NULL;
		}
	}
	for (size_t i = card->end; i < end; i++) {
		if (transcript[i] != '\n' && transcript[i] != '\r') {
			return NULL;
		}
	}

	return card;
}

static size_t shell_projection_end(size_t end, int width) {

	if (semantic_live_frame(width)) {
		return end;
	}
	int live =
		working && skin.tool_live && tool_active && !tool_details && tool_shell(tool_live_name);
	size_t boundary = live && tool_header_start <= end ? tool_header_start : end;
	if (shell_group_live()) {
		struct tool_card *card = shell_previous_group(boundary);
		return content_end(card ? card->start : boundary);
	}

	return live ? content_end(boundary) : end;
}

static int pending_display_rows(int cols) {

	return pending_active && cols >= 40 &&
			(pending_approval || !pending_is_shell() || tool_details || !skin.tool_live)
		? pending_height(cols)
		: 0;
}

static int activity_rows(int cols) {

	int pending = pending_display_rows(cols);
	if (pending && (pending_approval || skin.tool_progress)) {
		return pending;
	}
	int group = shell_group_live() ? 1 + skin.tool_live_command : 0;
	if (group && skin.tool_progress) {
		return group;
	}

	return pending + group + 1 +
		(!working_hidden && ((activity_shell() && skin.tool_live_command) || *activity_tip()));
}

/* "(14s · ↓ 1.2k tokens)": elapsed time and an estimate of reply tokens. */
static void activity_stats(double elapsed, char *out, size_t size) {

	int seconds = (int)elapsed;
	char time_text[32], tokens[32];
	if (seconds < 60) {
		snprintf(time_text, sizeof(time_text), "%ds", seconds);
	} else {
		snprintf(time_text, sizeof(time_text), "%dm %ds", seconds / 60, seconds % 60);
	}
	size_t estimate = turn_output_bytes / 4;
	if (estimate >= 1000) {
		snprintf(tokens, sizeof(tokens), " · ↓ %.1fk tokens", estimate / 1000.0);
	} else if (estimate) {
		snprintf(tokens, sizeof(tokens), " · ↓ %zu tokens", estimate);
	} else {
		tokens[0] = 0;
	}
	snprintf(out, size, "%s%s", time_text, tokens);
}

/* The working line: spinner, word (rotating, with a shimmer sweep), detail
   and tip. tick counts spinner frames since the turn began. */
static void activity_draw(int row, int cols, int height, long tick) {

	int x = skin.margin;
	wchar_t frame[2] = {skin.frames[tick % skin.frame_count], 0};
	attrset(COLOR_PAIR(7));
	mvaddwstr(row, x, frame);
	attrset(A_NORMAL);
	x += 2;
	double elapsed = now() - work_started;
	const char *word = "Working";
	if (skin.word_count) {
		int turn = skin.rotate ? (int)(elapsed / skin.rotate) : 0;
		word = skin.words[(skin_word + turn) % skin.word_count];
	}
	if (*working_message) {
		x = markup_text(row, x, cols - x - skin.margin, working_message, 3, 0);
	} else if (skin.shimmer) {
		/* A soft highlight glides across the word in lighter tints of its
           own colour: brightest in the middle, half as bright either side. */
		wchar_t wide[48];
		size_t count = mbstowcs(wide, word, 47);
		if (count == (size_t)-1) {
			count = 0;
		}
		long sweep = (long)count + 8, at = (long)(elapsed / SHIMMER_STEP) % sweep - 4;
		for (size_t i = 0; i < count && x < cols - skin.margin; i++) {
			long distance = labs((long)i - at);
			wchar_t cell[2] = {wide[i], 0};
			attrset(COLOR_PAIR(distance == 0 ? 64 : distance == 1 ? 65 : 7));
			mvaddwstr(row, x, cell);
			x += wcwidth(wide[i]) > 0 ? wcwidth(wide[i]) : 1;
		}
		attrset(A_NORMAL);
	} else {
		welcome_text(row, x, cols - x - skin.margin, word, 3, 0);
		x += skin_width(word) > 0 ? skin_width(word) : 0;
	}
	char detail[192] = "";
	if (*working_detail) {
		snprintf(detail, sizeof(detail), " (%s)", working_detail);
	} else if (skin.stats) {
		char stats[96];
		activity_stats(elapsed, stats, sizeof(stats));
		snprintf(detail, sizeof(detail), " (%s)", stats);
	}
	if (*detail) {
		markup_text(row, x, cols - x - skin.margin, detail, 3, 0);
	}
	const char *tip = activity_tip();
	if (activity_shell() && skin.tool_live_command && row + 1 < height) {
		char line[640];
		snprintf(line, sizeof(line), "$ %s  [%s]", shell_activity, shell_activity_state);
		welcome_text(row + 1, skin.margin + 2, cols - skin.margin * 2 - 2, line, 3, 0);
	} else if (*tip && row + 1 < height) {
		char line[256];
		snprintf(line, sizeof(line), "%s  %s", *skin.output ? skin.output : "└", tip);
		markup_text(row + 1, skin.margin + 2, cols - skin.margin * 2 - 2, line, 3, 0);
	}
}

/* Transcript width: the terminal less the skin's margin on each side. */
static int transcript_left(int columns) {

	int room = (columns - 2 * skin.margin - 4) / 2;
	int padding = skin.transcript_padding < room ? skin.transcript_padding : room;

	return skin.margin + (padding > 0 ? padding : 0);
}

static int text_width(int columns) {

	int width = columns - 2 * transcript_left(columns);
	if (skin.transcript_max_width && width > skin.transcript_max_width) {
		width = skin.transcript_max_width;
	}

	return width;
}

/* The rules around the prompt: lines (default), a rounded box, or none. */
static void prompt_border(int top, int bottom, int cols, short pair) {

	if (skin.border == SKIN_BORDER_NONE) {
		return;
	}
	cchar_t line;
	setcchar(&line, skin.border == SKIN_BORDER_HEAVY ? L"━" : L"─", A_NORMAL, pair, NULL);
	mvhline_set(top - 1, 0, &line, cols);
	mvhline_set(bottom, 0, &line, cols);
	if (skin.border != SKIN_BORDER_ROUNDED || cols < 2) {
		return;
	}
	static const wchar_t *corners[] = {L"╭", L"╮", L"╰", L"╯"};
	cchar_t cell;
	for (int i = 0; i < 4; i++) {
		setcchar(&cell, corners[i], A_NORMAL, pair, NULL);
		mvadd_wch(i < 2 ? top - 1 : bottom, i % 2 ? cols - 1 : 0, &cell);
	}
	setcchar(&cell, L"│", A_NORMAL, pair, NULL);
	for (int y = top; y < bottom; y++) {
		mvadd_wch(y, 0, &cell);
		mvadd_wch(y, cols - 1, &cell);
	}
}

#include "selection.inc"
static int ui_frame_suspended;

static int ui_frame_surfaces_visible(void) {

	return active && !ui_frame_suspended && !modal_active && !pending_approval &&
		!history_view_active() && getmaxy(ui_physical_screen()) >= 6 &&
		getmaxx(ui_physical_screen()) >= 20;
}

#include "ui-surfaces.inc"

/* A long chat's rows near the screen (see transcript_window_pad). */
static WINDOW *window_pad;
static int window_first, window_width;
static size_t window_revision = (size_t)-1;

static void transcript_cache_drop(void) {

	if (transcript_pad_cache) {
		delwin(transcript_pad_cache);
		transcript_pad_cache = NULL;
	}
	if (window_pad) {
		delwin(window_pad);
		window_pad = NULL;
	}
	window_revision = (size_t)-1;
	transcript_cache_revision = (size_t)-1;
	transcript_cache_width = transcript_cache_rows = 0;
}

/* A full-transcript pad costs rows x width x sizeof(cchar_t) (28 bytes per
   cell), which can exceed the whole rest of Phi for a long session. Cache it
   only while it is small; otherwise draw() lays out a bounded viewport cache. */
#define TRANSCRIPT_CACHE_BYTES (256u * 1024u)

static WINDOW *transcript_cached_pad(int width, int minimum_rows, int *text_rows) {

	if (transcript_cache_revision != transcript_revision || transcript_cache_width != width) {
		transcript_cache_drop();
		int rows = layout(NULL, width);
		transcript_cache_revision = transcript_revision;
		transcript_cache_width = width;
		transcript_cache_rows = rows;
		int pad_rows = rows > minimum_rows ? rows : minimum_rows;
		if (pad_rows < 1) {
			pad_rows = 1;
		}
		if ((size_t)pad_rows * (size_t)width * sizeof(cchar_t) <= TRANSCRIPT_CACHE_BYTES) {
			transcript_pad_cache = newpad(pad_rows, width);
			if (transcript_pad_cache) {
				wbkgd(transcript_pad_cache, COLOR_PAIR(27) | ' ');
				layout(transcript_pad_cache, width);
			}
		}
	}
	if (text_rows) {
		*text_rows = transcript_cache_rows;
	}

	return transcript_pad_cache;
}

/* At least one screen of look-ahead on either side, allocated only for the
   actual viewport. A fixed 256 KiB ceiling cannot even hold one large screen. */
static size_t transcript_viewport_budget(int width, int rows) {

	size_t bytes =
		(size_t)(width > 0 ? width : 1) * (size_t)(rows > 0 ? rows : 1) * sizeof(cchar_t);
	size_t budget = bytes > SIZE_MAX / 3 ? SIZE_MAX : bytes * 3;

	return budget > TRANSCRIPT_CACHE_BYTES ? budget : TRANSCRIPT_CACHE_BYTES;
}

/* When the whole chat is too big to keep laid out, keep the rows around the
   screen instead: two screens either side of it, laid out once and reused
   while scrolling stays inside them. Returns the pad, with *first the
   transcript row of its first row; NULL when it cannot be made. */
static WINDOW *transcript_window_pad(int width, int source, int rows, int *first) {

	if (rows < 1) {
		rows = 1;
	}
	if (window_pad && window_revision == transcript_revision && window_width == width &&
		source >= window_first && source + rows <= window_first + getmaxy(window_pad)) {
		*first = window_first;
		return window_pad;
	}
	if (window_pad) {
		delwin(window_pad);
		window_pad = NULL;
	}
	int margin = rows * 2;
	size_t row_bytes = width > 0 ? (size_t)width * sizeof(cchar_t) : 0;
	size_t budget = transcript_viewport_budget(width, rows);
	size_t budget_rows = row_bytes ? budget / row_bytes : 0;
	size_t spare = budget_rows > (size_t)rows ? (budget_rows - (size_t)rows) / 2 : 0;
	if (spare < (size_t)margin) {
		margin = (int)spare;
	}
	int start = source - margin > 0 ? source - margin : 0;
	int count = source - start + rows + margin;
	if ((size_t)count * (size_t)width * sizeof(cchar_t) > budget) {
		start = source;
		count = rows;
	}
	if (transcript_cache_width == width && transcript_cache_revision == transcript_revision &&
		count > transcript_cache_rows - start) {
		count = transcript_cache_rows - start;
		if (count < rows) {
			count = rows;
		}
	}
	window_pad = newpad(count, width);
	if (!window_pad) {
		return NULL;
	}
	wbkgd(window_pad, COLOR_PAIR(27) | ' ');
	layout_rows(window_pad, width, start);
	window_first = start;
	window_width = width;
	window_revision = transcript_revision;
	*first = start;

	return window_pad;
}

/* Messages sent while a turn runs wait here until the agent takes them at
   its next step. They show above the prompt, so nothing typed disappears,
   and Up on an empty prompt takes the newest back to edit. */
#define QUEUE_SHOWN 3

static int queue_rows(int cols) {

	if (!working || !queued_count || cols < 30) {
		return 0;
	}

	return (queued_count < QUEUE_SHOWN ? (int)queued_count : QUEUE_SHOWN) +
		(queued_count > QUEUE_SHOWN);
}

static void queue_draw(int row, int rows, int cols) {

	int width = cols - 2 * skin.margin, shown = rows;
	if ((size_t)shown > queued_count) {
		shown = (int)queued_count;
	}
	if (queued_count > (size_t)shown) {
		shown = rows - 1;
	}
	size_t first = queued_count - (size_t)shown;
	for (int i = 0; i < shown; i++) {
		const char *message = queued[first + (size_t)i];
		char line[512];
		snprintf(line, sizeof(line), "%s%s%.*s", skin.prompt, *skin.prompt ? " " : "",
			(int)strcspn(message, "\n"), message);
		const char *hint = i == shown - 1
			? (message[0] == '/' ? "  · runs after this turn · ↑ edits" : "  · queued · ↑ edits")
			: "";
		int hint_cells = *hint ? skin_width(hint) : 0;
		int room = width - hint_cells;
		if (room < 10) {
			room = width;
			hint = "";
		}
		welcome_text(row + i, skin.margin, room, line, 3, 0);
		int used_cells = skin_width(line) < room ? skin_width(line) : room;
		if (*hint) {
			welcome_text(row + i, skin.margin + used_cells, hint_cells, hint, 3, 0);
		}
	}
	if (queued_count > (size_t)shown) {
		char more[64];
		snprintf(more, sizeof(more), "  +%zu more queued", queued_count - (size_t)shown);
		welcome_text(row + shown, skin.margin, width, more, 3, 0);
	}
}

static void ui_frame_measure(struct phi_frame_measure *measure, int *cy, int *cx);

/* The physical screen outlives the temporary composed role windows. Cache
   against it, not against a per-draw transcript parent. */
static struct {
	WINDOW *window, *screen;
	struct phi_rect box;
	int rows, cols;
} ui_viewport;

static void ui_viewport_drop(void) {

	if (ui_viewport.window) {
		delwin(ui_viewport.window);
	}
	memset(&ui_viewport, 0, sizeof(ui_viewport));
}

static WINDOW *ui_viewport_get(struct phi_rect box) {

	WINDOW *screen = ui_physical_screen();
	int rows = getmaxy(screen), cols = getmaxx(screen);
	if (ui_viewport.screen != screen || ui_viewport.rows != rows || ui_viewport.cols != cols ||
		memcmp(&ui_viewport.box, &box, sizeof(box))) {
		ui_viewport_drop();
	}
	if (!ui_viewport.window) {
		ui_viewport.window = derwin(screen, box.height, box.width, box.y, box.x);
		if (ui_viewport.window) {
			ui_viewport.screen = screen;
			ui_viewport.rows = rows;
			ui_viewport.cols = cols;
			ui_viewport.box = box;
		}
	}
	if (ui_viewport.window) {
		wbkgdset(ui_viewport.window, getbkgd(stdscr));
		wattrset(ui_viewport.window, getattrs(stdscr));
	}

	return ui_viewport.window;
}

/* A failed/undersized frame is itself a settled presentation, not a reason
   to retry on provider/animation timers. Keep a stable warning until an
   actual UI event changes the geometry/state. */
static int ui_frame_fallback(int kind, int rows, int cols) {

	static int previous_kind, previous_rows, previous_cols, previous_x, previous_y;
	int repaint = !ui_frame_suspended || previous_kind != kind || previous_rows != rows ||
		previous_cols != cols || previous_x != ui_chat_x || previous_y != ui_chat_y ||
		is_cleared(ui_physical_screen());
	ui_frame_suspended = 1;
	ui_viewport_drop();
	dirty = wheel_pending = 0;
	conversation_height = conversation_width = conversation_copy_rows = conversation_pad_rows = 0;
	if (!repaint) {
		return 0;
	}
	previous_kind = kind;
	previous_rows = rows;
	previous_cols = cols;
	previous_x = ui_chat_x;
	previous_y = ui_chat_y;
	erase();
	curs_set(0);
	cursor_shape = -1;
	mvaddnstr(0, 0, kind == 1 ? "Enlarge terminal" : "Layout unavailable", cols);
	frame_begin();
	wnoutrefresh(stdscr);
	title_update_begin();
	doupdate();
	title_update_end();
	frame_end();
	last_draw = now();

	return 1;
}

static int draw_chat(int force) {

	if (!active || presentation_batch) {
		return 0;
	}
	int rows = LINES, cols = COLS;
	static int screen_rows, screen_cols;
	int physical_rows = getmaxy(ui_physical_screen()),
		physical_cols = getmaxx(ui_physical_screen());
	if (screen_rows != physical_rows || screen_cols != physical_cols) {
		screen_rows = physical_rows;
		screen_cols = physical_cols;
		dirty = 1;
	}
	int visible = ui_frame_surfaces_visible();
	if (ui_surfaces_service(visible)) {
		dirty = 1;
	}
	/* Only clean, unforced frames still have current provider geometry.
       Otherwise requests wait for measure/arrange below. */
	if (visible && !dirty && !force && ui_surfaces_poll()) {
		dirty = 1;
	}
	if (rows < (ui_split_hosts ? 4 : 6) || cols < 20) {
		return ui_frame_fallback(1, rows, cols);
	}
	if (ui_frame_suspended && !dirty && !force) {
		return 0;
	}
	if (welcome_blink_update(now())) {
		dirty = 1;
	}
	long activity_frame = working ? (long)((now() - work_started) * 1000 / skin.frame_ms) : -1;
	/* The shimmer moves on its own clock, smoother than the spinner's frames. */
	long shimmer_step =
		working && skin.shimmer ? (long)((now() - work_started) / SHIMMER_STEP) : -1;
	if (activity_frame != last_activity_frame || shimmer_step != last_shimmer_step ||
		title_redraw_due()) {
		dirty = 1;
	}
	if (wheel_pending && force && dirty) {
		force = now() - last_draw >= SCROLL_FRAME;
	}
	if (welcome_scene_animated()) {
		long tick = (long)(now() * 20);
		if (tick != scene_tick) {
			scene_tick = tick;
			dirty = 1;
		}
	}
	static unsigned renderer_generation;
	if (renderer_generation != surfaces_generation()) {
		renderer_generation = surfaces_generation();
		transcript_revision++;
		dirty = 1;
	}
	if (header_motion_due()) {
		dirty = 1;
	}
	if (cinema_due()) {
		if (!ui_canvas && !dirty && !force && !presentation_batch && cinema_refresh()) {
			return 1;
		}
		dirty = 1;
	}
	if (!active || presentation_batch ||
		(!force &&
			(!dirty || now() - last_draw < (wheel_pending ? SCROLL_FRAME : FRAME_INTERVAL)))) {
		return 0;
	}
	wheel_pending = 0;
	size_t painted_revision = transcript_revision;
	int welcome = !used && !working;
	struct phi_frame_measure measured;
	struct phi_frame_plan frame;
	int cy = 0, cx = 0;
	ui_frame_measure(&measured, &cy, &cx);
	if (!phi_frame_arrange(&measured, &frame)) {
		return ui_frame_fallback(2, rows, cols);
	}
	WINDOW *transcript_canvas = ui_canvas, *viewport_canvas = NULL;
	if (frame.view.y) {
		viewport_canvas =
			ui_viewport_get(phi_frame_screen_rect(frame.view, frame.owners[PHI_FRAME_TRANSCRIPT]));
		if (!viewport_canvas) {
			/* Optional attached chrome yields on allocation failure; the
               existing editor and its draft remain available. */
			measured.header_rows = measured.panels[PHI_FRAME_TRANSCRIPT] = 0;
			if (!phi_frame_arrange(&measured, &frame)) {
				return ui_frame_fallback(2, rows, cols);
			}
		}
	}
	ui_frame_suspended = 0;
	title_begin_frame();
	cinema_begin();
	if (ui_editor_canvas) {
		werase(ui_editor_canvas);
	}
	for (int host = 0; host < PHI_HOST_COUNT; host++) {
		if (ui_host_canvas[host]) {
			wbkgd(ui_host_canvas[host], COLOR_PAIR(27) | ' ');
			werase(ui_host_canvas[host]);
		}
	}
	erase();
	input_left = frame.input.x;
	input_view = frame.input_view;
	int menu_rows = command_rows(frame.input.y);
	int activity_cols = ui_host_canvas[PHI_HOST_ACTIVITY]
		? getmaxx(ui_host_canvas[PHI_HOST_ACTIVITY])
		: frame.owners[PHI_FRAME_TRANSCRIPT].width;
	int pending_rows = working ? pending_display_rows(activity_cols) : 0;
	int working_rows = working ? activity_rows(activity_cols) : 1;
	conversation_height = frame.view.height;
	cols = frame.owners[PHI_FRAME_TRANSCRIPT].width;
	rows = frame.owners[PHI_FRAME_TRANSCRIPT].height;
	int width = text_width(cols);
	int banner_lines = welcome_banner_height(frame.view.height);
	int text_lines = -1;
	int saved_view = scrollback_active() && !pending_approval;
	int cache_ok = !welcome && (saved_view || (!modal_active && !transcript_selection.visible));
	WINDOW *cached_pad =
		cache_ok ? transcript_cached_pad(width, frame.view.height, &text_lines) : NULL;
	if (text_lines < 0) {
		text_lines = layout(NULL, width);
	}
	/* Ignore trailing assistant separators in the compact viewport, not in
       stored history or the pad. Preserve intentional user/tool padding. */
	if (skin.compact_layout && used && !working) {
		static size_t compact_revision = (size_t)-1;
		static int compact_width, compact_lines;
		if (compact_revision != transcript_revision || compact_width != width) {
			size_t saved = used;
			while (used && (transcript_style[used - 1] == 0 || transcript_style[used - 1] == 3) &&
				(transcript[used - 1] == '\n' || transcript[used - 1] == '\r')) {
				used--;
			}
			used = semantic_messages_floor(used, saved);
			compact_lines = used != saved ? layout(NULL, width) : text_lines;
			used = saved;
			compact_revision = transcript_revision;
			compact_width = width;
		}
		text_lines = compact_lines;
	}
	int inline_activity = working && !ui_host_canvas[PHI_HOST_ACTIVITY];
	int indicator_line = inline_activity ? banner_lines + activity_line(width) : 0;
	int lines = welcome   ? 0
		: inline_activity ? indicator_line + working_rows
						  : banner_lines + text_lines;
	int maximum =
		header_motion_body_visible() && lines > frame.view.height ? lines - frame.view.height : 0;
	/* Anchor to the rendered top row, not the transcript's row count:
       pending cards and the activity line also change the scroll range.
       Do this once per frame, including frames without a transcript cache. */
	if (working && working_view_valid && scroll_offset > 0) {
		scroll_offset += maximum - working_view_maximum;
		if (scroll_offset < 0) {
			scroll_offset = 0;
		}
	}
	working_view_valid = working;
	working_view_maximum = maximum;
	if (scrollback_active()) {
		scroll_offset = pending_approval ? 0 : maximum;
	}
	if (scroll_offset > maximum) {
		scroll_offset = maximum;
	}
	int view_top = maximum - scroll_offset;
	cinema_view(view_top);
	int pad_lines = lines - banner_lines > text_lines ? lines - banner_lines : text_lines;
	if (pad_lines < frame.view.height) {
		pad_lines = frame.view.height;
	}
	conversation_maximum = maximum;
	conversation_width = width;
	conversation_pad_rows = welcome ? 0 : pad_lines;
	int destination = banner_lines > view_top ? banner_lines - view_top : 0;
	int source = view_top > banner_lines ? view_top - banner_lines : 0;
	for (int i = 0; i < PHI_FRAME_PANELS; i++) {
		ui_surface_configure(i, frame.panels[i], frame.owners[i]);
	}
	WINDOW *pad = welcome || saved_view ? NULL : cached_pad;
	/* Without the cached full pad, lay out only the rows on screen: pad row 0
       is transcript row `first`. */
	int temporary_pad = 0, first = 0;
	if (!welcome && !saved_view && !pad && cache_ok) {
		pad = transcript_window_pad(width, source, frame.view.height - destination, &first);
	}
	if (!welcome && !saved_view && !pad) {
		int visible_rows = frame.view.height - destination;
		pad = newpad(visible_rows > 0 ? visible_rows : 1, width);
		temporary_pad = pad != NULL;
		first = source;
	}
	if (viewport_canvas) {
		ui_canvas = viewport_canvas;
	}
	conversation_source = conversation_destination = conversation_copy_rows = 0;
	if (welcome) {
		welcome_draw(frame.view.height, cols);
	} else {
		welcome_header(frame.view.height, cols, view_top);
	}
	if (pad) {
		if (temporary_pad) {
			wbkgd(pad, COLOR_PAIR(27) | ' ');
			layout_rows(pad, width, first);
			conversation_highlight(pad, first);
		}
		int copy_rows = header_motion_body_visible() ? frame.view.height - destination : 0;
		if (copy_rows > getmaxy(pad) - (source - first)) {
			copy_rows = getmaxy(pad) - (source - first);
		}
		if (source + copy_rows > pad_lines) {
			copy_rows = pad_lines - source;
		}
		conversation_source = source;
		conversation_destination = destination;
		conversation_copy_rows = copy_rows > 0 ? copy_rows : 0;
		if (temporary_pad) {
			transcript_selection_highlight(pad, first);
		}
		/* Compose one screen: a later stdscr refresh must not erase pad text. */
		if (copy_rows > 0) {
			copywin(pad, stdscr, source - first, 0, destination, transcript_left(cols),
				destination + copy_rows - 1, transcript_left(cols) + width - 1, 0);
		}
		if (temporary_pad) {
			delwin(pad);
		}
	}
	scrollback_paint(width, frame.view.height, transcript_left(cols));
	int activity_row = scrollback_active() && !pending_approval ? -1000
		: header_motion_body_visible() ? indicator_line - (maximum - scroll_offset)
									   : -1000;
	WINDOW *activity_parent = ui_canvas;
	int activity_height = frame.view.height;
	if (ui_host_canvas[PHI_HOST_ACTIVITY]) {
		ui_canvas = ui_host_canvas[PHI_HOST_ACTIVITY];
		activity_row = 0;
		activity_height = getmaxy(ui_canvas);
		cols = getmaxx(ui_canvas);
	}
	if (pending_rows) {
		pending_draw(activity_row, cols, activity_height);
	}
	int group_rows = shell_group_live() ? 1 + skin.tool_live_command : 0;
	if (group_rows) {
		size_t end = tool_active ? tool_header_start : used;
		struct tool_card *previous = shell_previous_group(end);
		unsigned count = 1 + (previous ? previous->counts[ACT_SHELL] : 0);
		char line[640];
		snprintf(line, sizeof(line), "Running %u shell command%s…", count, count == 1 ? "" : "s");
		if (activity_row >= 0 && activity_row < activity_height) {
			welcome_text(activity_row, skin.margin, cols - skin.margin * 2, line, 27, 0);
		}
		if (skin.tool_live_command && activity_row + 1 >= 0 && activity_row + 1 < activity_height) {
			snprintf(line, sizeof(line), "%s%s$ %s", skin.output, *skin.output ? "  " : "",
				shell_activity);
			welcome_text(activity_row + 1, skin.margin, cols - skin.margin * 2, line, 3, 0);
		}
	}
	int spinner_row = activity_row + pending_rows + group_rows;
	int replace =
		(pending_rows && pending_approval) || ((pending_rows || group_rows) && skin.tool_progress);
	if (working && !working_hidden && !replace && spinner_row >= 0 &&
		spinner_row < activity_height) {
		activity_draw(spinner_row, cols, activity_height, activity_frame);
	}
	ui_canvas = activity_parent;
	cols = frame.owners[PHI_FRAME_TRANSCRIPT].width;
	/* Suggestions cover the viewport; they never change its scroll geometry. */
	if (!ui_split_hosts) {
		command_draw(frame.view.height - menu_rows, menu_rows, cols);
	}
	ui_canvas = transcript_canvas;
	conversation_destination += frame.view.y;
	if (frame.header.height && !ui_host_canvas[PHI_HOST_HEADER]) {
		header_draw(cols);
	}
	ui_surface_paint(PHI_FRAME_TRANSCRIPT);
	cinema_origin(frame.header.height);
	cinema_draw(frame.view.height, cols, view_top);
	if (ui_editor_canvas) {
		ui_canvas = ui_editor_canvas;
	}
	cols = frame.owners[PHI_FRAME_COMPOSER].width;
	rows = frame.owners[PHI_FRAME_COMPOSER].height;
	if (ui_split_hosts) {
		command_draw(frame.input.y - 2 - menu_rows, menu_rows, cols);
	}
	if (frame.above.height) {
		widget_draw(frame.above.y, 0, cols);
	}
	ui_surface_paint(PHI_FRAME_COMPOSER);
	if (frame.side.height) {
		side_draw(frame.side.y, frame.side.height, cols);
	}
	if (frame.queue.height) {
		queue_draw(frame.queue.y, frame.queue.height, cols);
	}
	if (frame.below.height) {
		widget_draw(frame.below.y, 1, cols);
	}
	prompt_border(
		frame.input.y, frame.border_bottom, cols, pending_active && pending_approval ? 28 : 5);
	int shell_mode = length && input[0] == L'!';
	char effort_status[64];
	int approval_status = pending_active && pending_approval;
	if (approval_status) {
		snprintf(effort_status, sizeof(effort_status),
			skin.approval_menu ? "APPROVAL REQUIRED · 1 yes · 2 no"
							   : "APPROVAL REQUIRED · y approve · n deny");
	} else if (ctrl_c_armed) {
		snprintf(effort_status, sizeof(effort_status), "Press Ctrl+C again to quit");
	} else if (left_armed && now() - left_armed_at < 2.5) {
		snprintf(effort_status, sizeof(effort_status), "Press ← again to background this chat");
	} else if (normal_mode && esc_armed && working && now() - esc_armed_at < 2.5) {
		snprintf(effort_status, sizeof(effort_status), "Press Esc again to stop");
	} else if (recovery_failed) {
		snprintf(effort_status, sizeof(effort_status), "Draft backup unavailable");
	} else if (shell_mode) {
		snprintf(effort_status, sizeof(effort_status), "bash · %s",
			length > 1 && input[1] == L'!' ? "local" : "send to model");
	} else {
		effort_status[0] = 0;
	}
	chrome_draw(&skin.composer_elements, frame.chrome.x, frame.chrome.y, frame.chrome.width,
		frame.chrome.height, 27, 3, 28, ui_chrome_expand, NULL);
	int effort_width = skin_width(effort_status); /* cells, not bytes: · and ← are multi-byte */
	if (effort_width < 0) {
		effort_width = (int)strlen(effort_status);
	}
	if (*effort_status && cols >= effort_width + 8) {
		int x = cols - effort_width - 5;
		const char *mark = skin_width(skin.card) == 1 ? skin.card : "◆";
		welcome_text(frame.status_row, x, 1, shell_mode && !approval_status ? "$" : mark, 28,
			approval_status);
		welcome_text(frame.status_row, x + 2, effort_width, effort_status, approval_status ? 28 : 3,
			approval_status);
	} else if (approval_status && cols >= 24) {
		const char *compact = "APPROVAL · y/n";
		int compact_width = (int)strlen(compact) - 1;
		int x = cols - compact_width - 5;
		welcome_text(frame.status_row, x, 1, skin_width(skin.card) == 1 ? skin.card : "◆", 28, 1);
		welcome_text(frame.status_row, x + 2, compact_width, compact, 28, 1);
	}
	/* Labels/hints occupy their own rows; status warnings must not suppress
       unrelated chrome (for example the INSERT label above the editor). */
	if (!pending_active) {
		char label[512];
		const char *label_template = *composer_owner ? composer_label : skin.composer_label;
		const char *hint_template = *composer_owner ? composer_hint : skin.composer_hint;
		int available = cols - skin.margin * 2 - effort_width - 8;
		if (*label_template && available > 0) {
			welcome_expand(label_template, label, sizeof(label));
			welcome_text(frame.input.y - 1, skin.margin + 2, available, label, 3, 0);
		}
		if (*hint_template) {
			welcome_expand(hint_template, label, sizeof(label));
			welcome_text(
				frame.border_bottom, skin.margin + 2, cols - skin.margin * 2 - 6, label, 3, 0);
		}
	}
	if (*skin.prompt) {
		welcome_text(frame.input.y, frame.input.x - 2, 1, skin.prompt, 28, 0);
	}
	border_overflow(frame.input.y - 1, cols, frame.input_view, L'↑');
	border_overflow(frame.border_bottom, cols,
		measured.draft_rows - frame.input_view - frame.input.height, L'↓');
	draft_layout(frame.input.width, frame.input.y, frame.input.height, &cy, &cx);
	if (normal_mode && focused) {
		/* Software block stays solid when unfocused and cannot invert the
           Visual selection twice. Underline distinguishes its endpoint. */
		wchar_t ch[2] = {cursor < length && input[cursor] != L'\n' ? input[cursor] : L' ', 0};
		attr_t attributes = A_REVERSE | (visual_mode ? A_BOLD | A_UNDERLINE : 0);
		attron(attributes);
		mvaddwstr(frame.input.y + cy - frame.input_view, frame.input.x + cx, ch);
		attroff(attributes);
	}
	WINDOW *editor_parent = ui_canvas;
	if (measured.footer_detached) {
		ui_canvas = ui_host_canvas[PHI_HOST_FOOTER];
		rows = frame.owners[PHI_FRAME_FOOTER].height;
		cols = frame.owners[PHI_FRAME_FOOTER].width;
	}
	if (skin.footer_dim) {
		attron(A_DIM);
	}
	ui_surface_paint(PHI_FRAME_FOOTER);
	for (int i = 0; i < frame.footer_text.height - 1 && i < skin.footer_line_count; i++) {
		char footer_line[1024];
		welcome_expand(skin.footer_lines[i], footer_line, sizeof(footer_line));
		welcome_text(
			frame.footer_text.y + i, skin.margin, cols - skin.margin * 2, footer_line, 3, 0);
	}
	welcome_footer(rows - 1, cols);
	if (skin.footer_dim) {
		attroff(A_DIM);
	}
	ui_canvas = editor_parent;
	rows = frame.owners[PHI_FRAME_COMPOSER].height;
	cols = frame.owners[PHI_FRAME_COMPOSER].width;
	for (int host = 0; host < PHI_HOST_COUNT; host++) {
		WINDOW *host_window = ui_host_canvas[host];
		if (!host_window || host == PHI_HOST_ACTIVITY || host == PHI_HOST_FOOTER) {
			continue;
		}
		ui_canvas = host_window;
		int host_rows = getmaxy(host_window), host_cols = getmaxx(host_window);
		if (host == PHI_HOST_HEADER) {
			header_draw(host_cols);
		}
		if (host == PHI_HOST_QUEUE) {
			int shown = queue_rows(host_cols);
			if (shown > host_rows) {
				shown = host_rows;
			}
			if (shown) {
				queue_draw(0, shown, host_cols);
			}
		}
	}
	ui_canvas = editor_parent;
	if (!modal_active) {
		int input_locked = pending_active && pending_approval;
		curs_set(input_locked || normal_mode || !focused ? 0 : 1);
		if (!input_locked) {
			draft_cursor_move(frame.input.y + cy - frame.input_view, frame.input.x + cx);
		}
		frame_begin();
		if (ui_editor_canvas) {
			wnoutrefresh(transcript_canvas);
		}
		wnoutrefresh(stdscr);
		title_update_begin();
		doupdate();
		title_update_end();
		/* DECSCUSR: blinking bar in INSERT, steady block in NORMAL/VISUAL. */
		int shape = normal_mode ? 2 : 5;
		if (shape != cursor_shape) {
			fprintf(stdout, "\033[%d q", shape);
			fflush(stdout);
			cursor_shape = shape;
		}
		frame_end();
	}
	/* Error handlers may append feedback while composing this frame.
       Leave it dirty for the next normal paint, rather than drawing reentrantly. */
	dirty = painted_revision != transcript_revision;
	last_draw = now();
	last_activity_frame = activity_frame;
	last_shimmer_step = shimmer_step;
	ui_canvas = transcript_canvas;

	return 1;
}

#include "ui-layout.inc"

void ui_pending_tool(json_object *call) {

	int was_grouped = shell_group_live();
	json_object *name = json_object_object_get(call, "name");
	json_object *arguments = json_object_object_get(call, "arguments");
	if (!json_object_is_type(name, json_type_string)) {
		return;
	}
	snprintf(pending_name, sizeof(pending_name), "%s", json_object_get_string(name));
	const char *text =
		json_object_is_type(arguments, json_type_string) ? json_object_get_string(arguments) : "";
	snprintf(pending_arguments, sizeof(pending_arguments), "%s", text);
	char *full = strndup(text, LOG_SIZE);
	free(pending_full);
	pending_full = full;
	pending_truncated = strlen(text) > LOG_SIZE ||
		(!full && strlen(text) >= sizeof(pending_arguments)) ||
		json_object_get_boolean(json_object_object_get(call, "arguments_truncated"));
	pending_decode();
	if (pending_is_shell()) {
		/* Argument streaming starts with an empty object. Keep the previous
           completed command until the next command text actually arrives. */
		const char *detail = pending_detail();
		if (detail && *detail) {
			shell_activity_set(detail, "queued");
		}
	} else {
		shell_activity[0] = 0;
	}
	pending_active = 1;
	pending_ready = 0;
	dirty = 1;
	if (was_grouped != shell_group_live()) {
		transcript_revision++;
	}
	draw(0);
}

static int approval_finished(int approved) {

	if (skin.tool_live == 2) {
		transcript_revision++;
	}
	if (pending_is_shell()) {
		shell_activity_state = approved ? "approved" : "denied";
	}
	pending_active = pending_ready = pending_approval = 0;
	dirty = 1;

	return approved;
}

int ui_tool_guard(json_object *call, int approval) {

	ui_pending_tool(call);
	pending_ready = 1;
	pending_approval = approval;
	if (skin.tool_live == 2) {
		transcript_revision++;
	}
	approval_choice = 0;
	dirty = 1;
	draw(1);
	if (approval) {
		int ignore_paste = pasting || history_view_paste(-1);
		for (;;) {
			if (external_cancelled) {
				return approval_finished(0);
			}
			paste_checkpoint();
			wint_t key = 0;
			int kind = read_key(&key, 50);
			if (kind == KEY_CODE_YES && key == KEY_RESIZE) {
				title_resized();
				dirty = 1;
				draw(1);
				continue;
			}
			if (kind == KEY_CODE_YES && key == KEY_PASTE_BEGIN) {
				ignore_paste = 1;
				continue;
			}
			if (kind == KEY_CODE_YES && key == KEY_PASTE_END) {
				ignore_paste = pasting = paste_cr = 0;
				if (paste_before) {
					paste_snapshot_finish();
				}
				continue;
			}
			if (kind == ERR) {
				struct pollfd terminal = {STDIN_FILENO, POLLIN, 0};
				if (poll(&terminal, 1, 0) > 0 &&
					(terminal.revents & (POLLHUP | POLLERR | POLLNVAL))) {
					return approval_finished(0);
				}
			}
			/* Pasting source containing 'y' must never authorize a tool. */
			if (ignore_paste) {
				continue;
			}
			if (kind == OK && (key == 'y' || key == 'Y')) {
				return approval_finished(1);
			}
			if (skin.approval_menu) {
				if (kind == OK && (key == '1' || key == '2')) {
					return approval_finished(key == '1');
				}
				if ((kind == OK && (key == '\r' || key == '\n')) ||
					(kind == KEY_CODE_YES && key == KEY_ENTER)) {
					return approval_finished(approval_choice == 0);
				}
				if ((kind == OK && (key == 'j' || key == 'k' || key == '\t')) ||
					(kind == KEY_CODE_YES &&
						(key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT ||
							key == KEY_RIGHT))) {
					approval_choice = !approval_choice;
					dirty = 1;
				}
			}
			if ((kind == OK && (key == 'n' || key == 'N' || key == 27 || key == 3 || key == 4)) ||
				(kind == KEY_CODE_YES &&
					(key == KEY_ESCAPE || key == KEY_INTERRUPT || key == KEY_EOF))) {
				return approval_finished(0);
			}
			if (idle_callback) {
				idle_callback();
			}
			draw(0);
		}
	}
	/* Short cancellation window when approvals are disabled. It delays every
       tool call, so PHI_TOOL_PREVIEW_MS can shorten it (0 disables it). */
	double deadline = now() + tool_preview_ms / 1000.0;
	for (;;) {
		if (ui_cancel()) {
			if (pending_is_shell()) {
				shell_activity_state = "cancelled";
			}
			pending_approval = 0;
			dirty = 1;
			return 0;
		}
		if (now() >= deadline) {
			break;
		}
		struct timespec pause = {0, 10000000};
		nanosleep(&pause, NULL);
	}
	pending_active = pending_ready = pending_approval = 0;
	if (skin.tool_live == 2) {
		transcript_revision++;
	}
	dirty = 1;

	return 1;
}

static int ui_open_mode(
	const char *model, const char *effort, const char *theme_path, int recover) {

	external_cancelled = 0;
	setlocale(LC_ALL, "");
	if (!phi_curses_open()) {
		return 0;
	}
	report_state = 0; /* Never carry an incomplete report into a new UI. */
	key_window = newpad(1, 1);
	if (!key_window) {
		endwin();
		return 0;
	}
	keypad(key_window, TRUE);
	active = 1;
	reading_width = 0;
	const char *text_width = getenv("PHI_TEXT_WIDTH");
	if (text_width && *text_width) {
		char *end;
		long value = strtol(text_width, &end, 10);
		if (!*end && (value == 0 || (value >= 40 && value <= 1000))) {
			reading_width = (int)value;
		}
	}
	const char *preview_setting = getenv("PHI_TOOL_PREVIEW_MS");
	if (preview_setting && *preview_setting) {
		char *end;
		long value = strtol(preview_setting, &end, 10);
		if (!*end && value >= 0 && value <= 10000) {
			tool_preview_ms = (int)value;
		}
	}
	const char *sync_setting = getenv("PHI_SYNC_OUTPUT");
	sync_output = isatty(STDOUT_FILENO) && (!sync_setting || strcmp(sync_setting, "0"));
	raw();
	noecho();
	nonl();
	keypad(stdscr, TRUE);
	/* Accept both normal and application cursor sequences. Some terminals
       keep emitting CSI arrows while curses requests application mode. */
	/* Normalize extended Ctrl-letter reports to their traditional control
       bytes before dispatch, including inside tree/palette modal readers. */
	for (int i = 0; i < 26; i++) {
		char sequence[32];
		snprintf(sequence, sizeof(sequence), "\033[%d;5u", 'a' + i);
		define_key(sequence, KEY_CTRL_LETTER_BASE + i);
	}
	define_key("\033[A", KEY_UP);
	define_key("\033OA", KEY_UP);
	define_key("\033[B", KEY_DOWN);
	define_key("\033OB", KEY_DOWN);
	define_key("\033[C", KEY_RIGHT);
	define_key("\033OC", KEY_RIGHT);
	define_key("\033[D", KEY_LEFT);
	define_key("\033OD", KEY_LEFT);
	define_key("\033[H", KEY_HOME);
	define_key("\033OH", KEY_HOME);
	define_key("\033[F", KEY_END);
	define_key("\033OF", KEY_END);
	define_key("\033[Z", KEY_BTAB);
	define_key("\033[9;2u", KEY_BTAB);
	define_key("\033[32;2u", KEY_SHIFT_SPACE);
	define_key("\033[27;2;32~", KEY_SHIFT_SPACE);
	define_key("\033[13;2u", KEY_NEWLINE);
	define_key("\033[27;2;13~", KEY_NEWLINE);
	define_key(BACKGROUNDED_NOTICE, KEY_CHAT_BACKGROUNDED);
	define_key("\033[I", KEY_FOCUS_IN);
	define_key("\033[O", KEY_FOCUS_OUT);
	define_key("\033[27u", KEY_ESCAPE);
	define_key("\033[106;5u", KEY_NEWLINE);
	define_key("\033[100;5u", KEY_EOF);
	define_key("\033[99;5u", KEY_INTERRUPT);
	define_key("\033[118;5u", KEY_CLIPBOARD);
	define_key("\033m", KEY_COMMAND_MENU);
	define_key("\033[109;3u", KEY_COMMAND_MENU);
	define_key("\033[200~", KEY_PASTE_BEGIN);
	define_key("\033[201~", KEY_PASTE_END);
	/* Ask supporting terminals to distinguish Shift+Enter from Enter. */
	fputs("\033[?2004h\033[?1004h\033[>1u", stdout);
	fflush(stdout);
	mousemask(ALL_MOUSE_EVENTS, NULL);
	mouseinterval(0);
	set_escdelay(25);
	if (has_colors()) {
		start_color();
		use_default_colors();
	}
	short colors[THEME_COUNT];
	char theme_error[256] = {0};
	struct skin next_skin;
	snprintf(
		theme_transcript_path, sizeof(theme_transcript_path), "%s", theme_path ? theme_path : "");
	*theme_pending_name = 0;
	theme_seen = theme_stamp_now();
	int theme_status = theme_read(colors, &next_skin, theme_error, sizeof(theme_error));
	if (theme_status < 0) {
		for (size_t i = 0; i < THEME_COUNT; i++) {
			colors[i] = theme_defaults[i].value;
		}
		next_skin = skin_defaults;
	}
	skin_use(&next_skin, 1);
	theme_apply(colors);
	theme_selection_label(theme_current_name);
	title_init();
	welcome_init();
	snprintf(footer_model, sizeof(footer_model), "%s", model);
	context_model(model);
	effort_set(model, effort);
	if (recover) {
		(void)recovery_open(); /* Embedded editors never claim a chat's recovery file. */
	}
	recovery_failed = recover && recovery_dir < 0 &&
		(!getenv("PHI_DRAFT_RECOVERY") || strcmp(getenv("PHI_DRAFT_RECOVERY"), "0"));
	if (theme_status < 0) {
		if (editor_host_feedback) {
			editor_host_feedback(theme_error, editor_host_context);
		} else {
			ui_append("[Theme: ");
			ui_append(theme_error);
			ui_append("; using defaults.]\n");
		}
	}
	dirty = 1;
	if (!editor_host_redraw) {
		draw(1);
	}

	return 1;
}

int ui_open(const char *model, const char *effort) {

	return ui_open_mode(model, effort, NULL, 1);
}

int ui_editor_attach(const char *model, const char *effort, const char *path,
	void (*redraw)(void *), void (*feedback)(const char *, void *), void *context) {

	if (active || !redraw) {
		return 0;
	}
	editor_host_redraw = redraw;
	editor_host_feedback = feedback;
	editor_host_context = context;
	editor_workspace = 1;
	if (!ui_open_mode(model, effort, path, 0)) {
		editor_host_redraw = NULL;
		editor_host_feedback = NULL;
		editor_host_context = NULL;
		editor_workspace = 0;
		return 0;
	}

	return 1;
}

int ui_editor_attached(void) {

	return editor_host_redraw != NULL;
}

void ui_editor_refresh(void) {

	dirty = 1;
	draw(0);
}

static void editor_storage_close(void);

void ui_close(void) {

	editor_host_redraw = NULL;
	editor_host_feedback = NULL;
	editor_host_context = NULL;
	editor_workspace = 0;
	scrollback_close();
	history_view_close();
	history_view_paste(0);
	semantic_messages_clear();
	tool_cards_clear();
	cinema_close();
	ui_viewport_drop();
	ui_frame_suspended = 0;
	ui_layout_close();
	ui_surfaces_close();
	ui_native_widget_forget(NULL);
	ui_native_transition_forget(NULL);
	ui_composer(NULL, NULL, NULL);
	report_state = 0;
	draft_navigation_width = 0;
	recovery_close();
	transcript_selection_clear();
	command_files_reset();
	idle_callback = NULL;
	external_pending = NULL;
	account_usage[0] = 0;
	extension_footer[0] = 0;
	title_close();
	while (sync_depth) {
		frame_end();
	}
	transcript_cache_drop();
	if (key_window) {
		delwin(key_window);
		key_window = NULL;
	}
	if (active) {
		fputs("\033[<u\033[?2004l\033[?1004l\033[?1003l", stdout);
		fflush(stdout);
		endwin();
		fputs("\033[0 q", stdout);
		fflush(stdout);
		cursor_shape = -1;
		active = 0;
	}
	for (size_t i = 0; i < queued_count; i++) {
		free(queued[i]);
	}
	queued_count = 0;
	undo_clear();
	vim_pending_clear();
	last_replace_count = 0;
	replace_changed = 0;
	paste_snapshot_clear();
	pasting = paste_cr = 0;
	editor_storage_close();
	skin_release(&skin);
	if (input != input_initial) {
		free(input);
	}
	input = input_initial;
	input_capacity = INPUT_SIZE;
	input[0] = 0;
	length = cursor = 0;
	if (yank != yank_initial) {
		free(yank);
	}
	yank = yank_initial;
	yank_capacity = INPUT_SIZE + 1;
	yank_length = 0;
	yank[0] = 0;
	if (command_query != command_query_initial) {
		free(command_query);
	}
	command_query = command_query_initial;
	command_query_capacity = INPUT_SIZE;
	command_query[0] = 0;
}

/* The reply being streamed, for render_markdown: where it starts in the
   transcript and its text as the model wrote it. */
static char *(*markdown_hook)(const char *text);
static size_t assistant_start;
static int assistant_open, in_assistant;
static char *assistant_raw;
static size_t assistant_raw_used, assistant_raw_capacity;

void ui_set_markdown_hook(char *(*hook)(const char *text)) {

	markdown_hook = hook;
}

static void capture_bytes(struct styled_capture *out, const char *text, size_t n, int style) {

	if (out->failed || !n) {
		return;
	}
	if (n > SIZE_MAX - out->used - 1) {
		out->failed = 1;
		return;
	}
	size_t needed = out->used + n + 1;
	if (needed > out->capacity) {
		size_t capacity = out->capacity ? out->capacity : 512;
		while (capacity < needed) {
			if (capacity > SIZE_MAX / 2) {
				capacity = needed;
				break;
			}
			capacity *= 2;
		}
		char *bytes = realloc(out->text, capacity);
		if (!bytes) {
			out->failed = 1;
			return;
		}
		out->text = bytes;
		unsigned char *styles = realloc(out->styles, capacity);
		if (!styles) {
			out->failed = 1;
			return;
		}
		out->styles = styles;
		out->capacity = capacity;
	}
	memcpy(out->text + out->used, text, n);
	memset(out->styles + out->used, style, n);
	out->used += n;
	out->text[out->used] = 0;
}

static void capture_styled(struct styled_capture *out, const char *text, int style) {

	if (out->failed) {
		return;
	}
	size_t n = strlen(text);
	if (!out->tool_body || out->edit || style >= 71) {
		capture_bytes(out, text, n, style);
		return;
	}
	int line_start =
		out->used == out->body_start ? 2 : out->used && out->text[out->used - 1] == '\n';
	size_t start = 0;
	for (size_t i = 0; i < n; i++) {
		if (line_start) {
			capture_bytes(out, line_start == 2 ? GLYPH_OUTPUT : GLYPH_HANG, 3, style);
			line_start = 0;
		}
		if (text[i] == '\n' || i + 1 == n) {
			capture_bytes(out, text + start, i - start + 1, style);
			start = i + 1;
			if (start < n) {
				line_start = 1;
			}
		}
	}
}

static void append_styled(const char *text, int user) {

	if (styled_capture) {
		capture_styled(styled_capture, text, user);
		return;
	}
	static int in_glyph;
	/* A tool card's body opens with its configured output glyph slot
       and later body lines hang under it; both draw nothing unless the skin
       sets glyph.output. Edit diffs keep their own layout. */
	if (tool_active && !tool_edit && user < 71 && *text && !in_glyph && used >= tool_body_start) {
		size_t lines = 1;
		for (const char *p = text; *p; p++) {
			lines += *p == '\n';
		}
		size_t n = strlen(text);
		char *expanded = malloc(n + 3 * lines + 4);
		if (expanded) {
			size_t out = 0;
			int line_start = used == tool_body_start ? 2 : transcript[used - 1] == '\n';
			for (const char *p = text; *p; p++) {
				if (line_start) {
					memcpy(expanded + out, line_start == 2 ? GLYPH_OUTPUT : GLYPH_HANG, 3);
					out += 3;
					line_start = 0;
				}
				expanded[out++] = *p;
				if (*p == '\n' && p[1]) {
					line_start = 1;
				}
			}
			expanded[out] = 0;
			in_glyph = 1;
			append_styled(expanded, user);
			in_glyph = 0;
			free(expanded);
			return;
		}
	}
	if (!in_assistant) {
		assistant_open = 0;
		semantic_message_finish("completed");
	}
	transcript_selection_clear();
	/* Do not reflow the complete transcript around every streaming chunk.
       draw() is already frame-rate limited and anchors a scrolled viewport
       using the complete rendered scroll range once per frame. */
	int old_lines = (!working && active && scroll_offset && text_width(COLS) > 0)
		? layout(NULL, text_width(COLS))
		: 0;
	size_t n = strlen(text);
	if (n >= LOG_SIZE) {
		text += n - (LOG_SIZE - 1);
		n = LOG_SIZE - 1;
		used = 0;
		tool_heading_valid = 0;
		tool_cards_clear();
		semantic_messages_clear();
		tool_body_start = 0;
		assistant_open = 0;
	}
	if (used + n >= LOG_SIZE) {
		size_t drop = used + n - (LOG_SIZE - 1);
		/* Prefer trimming an entire old line. */
		while (drop < used && transcript[drop - 1] != '\n') {
			drop++;
		}
		tool_cards_drop(drop);
		semantic_messages_drop(drop);
		if (assistant_open) {
			if (drop > assistant_start) {
				assistant_open = 0;
			} else {
				assistant_start -= drop;
			}
		}
		memmove(transcript, transcript + drop, used - drop);
		memmove(transcript_style, transcript_style + drop, used - drop);
		used -= drop;
		if (tool_heading_valid) {
			if (drop >= tool_heading_end) {
				tool_heading_valid = 0;
			} else {
				tool_heading_end -= drop;
			}
		}
	}
	memcpy(transcript + used, text, n);
	memset(transcript_style + used, user, n);
	used += n;
	transcript[used] = 0;
	transcript_revision++;
	if (old_lines) {
		scroll_offset += layout(NULL, text_width(COLS)) - old_lines;
		if (scroll_offset < 0) {
			scroll_offset = 0;
		}
	}
	dirty = 1;
	draw(0);
}

#include "cards.inc"

static void reply_glyph(void) {

	if (assistant_open) {
		return;
	}
	append_styled(GLYPH_REPLY, 0);
}

static void ui_assistant_compat(const char *text) {

	if (!*text) {
		return;
	}
	turn_output_bytes += strlen(text);
	if (!markdown_hook) {
		in_assistant = 1;
		append_styled(text, 3);
		in_assistant = 0;
		assistant_open = 1;
		return;
	}
	size_t n = strlen(text);
	if (!assistant_open) {
		assistant_start = used;
		assistant_raw_used = 0;
	}
	if (assistant_raw_used + n + 1 > assistant_raw_capacity) {
		size_t capacity = (assistant_raw_used + n + 1) * 2;
		char *grown = realloc(assistant_raw, capacity);
		if (!grown) {
			append_styled(text, 3);
			return;
		}
		assistant_raw = grown;
		assistant_raw_capacity = capacity;
	}
	memcpy(assistant_raw + assistant_raw_used, text, n + 1);
	assistant_raw_used += n;
	/* A transformer redraws the whole reply so far; otherwise append. */
	char *shown = markdown_hook(assistant_raw);
	in_assistant = 1;
	if (shown) {
		used = assistant_start;
		transcript[used] = 0;
		append_styled(shown, 3);
		free(shown);
	} else {
		append_styled(text, 3);
	}
	in_assistant = 0;
	assistant_open = 1;
}

void ui_assistant(const char *text) {

	if (!text || !*text) {
		return;
	}
	presentation_batch++;
	if (!assistant_open) {
		size_t start = used;
		reply_glyph();
		semantic_message_begin(SEM_ASSISTANT, start);
	} else if (!semantic_message_is_active(SEM_ASSISTANT)) {
		semantic_message_begin(SEM_ASSISTANT, used);
	}
	uint64_t id = semantic_message_active;
	in_assistant = 1;
	ui_assistant_compat(text);
	in_assistant = 0;
	semantic_message_append(id, text, used);
	presentation_batch--;
	draw(0);
}

void ui_thinking(const char *text, int complete) {

	presentation_batch++;
	if (text && *text) {
		if (!semantic_message_is_active(SEM_THINKING)) {
			semantic_message_finish("completed");
			assistant_open = 0;
			if (skin.thinking_visible && used && transcript[used - 1] != '\n') {
				append_styled("\n\n", 81);
			}
			size_t start = used;
			append_styled(
				"\xee\x80\x87", 80); /* invisible compatibility anchor, not reasoning text */
			semantic_message_begin(SEM_THINKING, start);
		}
		uint64_t id = semantic_message_active;
		if (skin.thinking_visible || *skin_message_renderer(&skin, SEM_THINKING)) {
			in_assistant = 1;
			append_styled(text, skin.thinking_visible ? 81 : 2);
			in_assistant = 0;
		}
		semantic_message_append(id, text, used);
	}
	if (complete && semantic_message_is_active(SEM_THINKING)) {
		if (skin.thinking_visible) {
			in_assistant = 1;
			append_styled("\n\n", 81);
			in_assistant = 0;
			struct semantic_message *message = semantic_message_find(semantic_message_active);
			if (message) {
				message->end = used;
			}
		}
		semantic_message_finish("completed");
	}
	presentation_batch--;
	draw(0);
}

static void recall_add(const char *text);

void ui_user(const char *text) {

	chat_turns++;
	recall_add(text); /* also restores recall for a resumed session */
	context_stale = 1;
	presentation_batch++;
	/* One blank line between turns, consistent with tool/prose boundaries. */
	while (used && transcript[used - 1] == '\n') {
		used--;
	}
	semantic_messages_truncate(used);
	transcript[used] = 0;
	if (used) {
		append_styled("\n\n", 0);
	}
	size_t start = used;
	append_styled(GLYPH_PROMPT, 19);
	append_styled(text, 1);
	semantic_message_begin(SEM_USER, start);
	semantic_message_append(semantic_message_active, text, used);
	semantic_message_finish("completed");
	append_styled("\n\n", 0);
	presentation_batch--;
	draw(0);
}

/* Scroll anchoring must use the resolved transcript host, not the physical
   terminal or a detached activity panel's width. */
static int ui_conversation_lines(void) {

	int cols = ui_chat_columns ? ui_chat_columns : COLS, width = text_width(cols);
	if (width < 1) {
		return 0;
	}

	return working && !ui_host_boxes[PHI_HOST_ACTIVITY].width
		? activity_line(width) + activity_rows(cols)
		: layout(NULL, width);
}

void ui_stopped(int tools_started) {

	semantic_live_stop();
	semantic_message_finish("cancelled");
	working_view_valid = 0;
	int held_scroll = scroll_offset;
	int old_lines = active && held_scroll ? ui_conversation_lines() : 0;
	presentation_batch++;
	working = 0;
	int frozen_tool = pending_active;
	while (used && (transcript[used - 1] == '\n' || transcript[used - 1] == '\r')) {
		used--;
	}
	semantic_messages_truncate(used);
	transcript[used] = 0;
	tool_heading_valid = 0;
	if (used) {
		append_styled("\n\n", 0);
	}
	if (frozen_tool) {
		char heading[160], arguments[1024];
		snprintf(heading, sizeof(heading), "%s · stopped before execution\n", pending_name);
		/* The decoded detail (a path, a command), else the raw prefix. */
		const char *detail = pending_detail();
		if (detail) {
			snprintf(arguments, sizeof(arguments), "%.*s", (int)strcspn(detail, "\n"), detail);
		} else {
			pending_snapshot(arguments, sizeof(arguments));
		}
		append_styled(heading, 23);
		if (*arguments) {
			append_styled(arguments, 24);
			append_styled("\n", 24);
		}
		append_styled("\n", 0);
	} else {
		append_styled("Stopped", 22);
		append_styled("\n\n", 0);
	}
	if (tools_started) {
		append_styled("Completed tool actions aren't undone\n\n", 2);
	}
	pending_active = pending_ready = 0;
	if (old_lines) {
		scroll_offset = held_scroll + ui_conversation_lines() - old_lines;
		if (scroll_offset < 0) {
			scroll_offset = 0;
		}
	}
	presentation_batch--;
	dirty = 1;
	draw(1);
}

void ui_busy(int busy) {

	if (!busy) {
		semantic_message_finish("completed");
	}
	if (busy) {
		external_cancelled = 0;
	}
	if (!!busy != !!working && tool_active) {
		transcript_revision++;
	}
	working_view_valid = 0;
	int hold = active && scroll_offset;
	int old_lines = hold ? ui_conversation_lines() : 0;
	if (!busy) {
		pending_active = pending_ready = 0;
	}
	if (!busy && working) {
		working = 0;
		side_flush();
	}
	if (busy && !working) {
		shell_activity[0] = 0;
		work_started = now();
		work_started_wall = time(NULL);
		/* A skin's spinner words rotate per turn. */
		skin_word = skin.word_count > 1
			? (int)((unsigned)(work_started * 1000) % (unsigned)skin.word_count)
			: 0;
		skin_tip = (int)((unsigned)(work_started * 7919) % 8u);
		turn_output_bytes = 0;
	}
	working = busy;
	if (hold) {
		int new_lines = ui_conversation_lines();
		scroll_offset += new_lines - old_lines;
		if (scroll_offset < 0) {
			scroll_offset = 0;
		}
	}
	dirty = 1;
	draw(1);
}

/* After a turn, how long it took ("Worked for 14s"; turn.done words).
   Turns under a second leave no line. */
static void turn_done_line(long seconds, time_t finished) {

	if (seconds < 1) {
		return;
	}
	while (used && transcript[used - 1] == '\n') {
		used--;
	}
	semantic_messages_truncate(used);
	transcript[used] = 0;
	/* "SECONDS@WHEN": how long, and when it finished (for "done 1:41 PM"). */
	char digits[48];
	snprintf(digits, sizeof(digits), "%ld@%lld", seconds, (long long)finished);
	if (used) {
		append_styled("\n\n", 0);
	}
	append_styled(digits, 78);
	append_styled("\n", 0);
}

/* Whole seconds (work_started_wall), the same clock as saved items, so a
   resumed session shows the same lengths as it did live. */
void ui_turn_done(void) {

	time_t finished = time(NULL);
	if (working && work_started_wall) {
		turn_done_line((long)(finished - work_started_wall), finished);
	}
}

void ui_clear(void) {

	scrollback_close();
	semantic_messages_clear();
	header_motion_cancel();
	working_view_valid = 0;
	assistant_open = 0;
	transcript_selection_clear();
	tool_cards_clear();
	tool_body_start = 0;
	used = 0;
	transcript[0] = 0;
	transcript_revision++;
	scroll_offset = 0;
	tool_heading_valid = tool_active = 0;
	context_used = -1;
	context_stale = 0;
	dirty = 1;
}

void ui_configure(const char *model, const char *effort) {

	if (strcmp(model, footer_model)) {
		context_model(model);
	}
	snprintf(footer_model, sizeof(footer_model), "%s", model);
	effort_set(model, effort);
	dirty = 1;
	draw(1);
}

static void theme_reload(int announce);

void ui_reload_theme(void) {

	theme_reload(1);
}

/* Follow this chat's selection and the shared definitions it references. */
static void theme_follow(void) {

	double at = now();
	if (at - theme_checked < 1) {
		return;
	}
	theme_checked = at;
	struct theme_stamp stamp = !*theme_transcript_path && *theme_pending_name
		? theme_stamp_named(theme_pending_name)
		: theme_stamp_now();
	if (theme_stamp_same(stamp, theme_seen)) {
		return;
	}
	theme_reload(0);
}

/* Before the first turn the transcript has no path: keep its choice until
   creation. Never use the global default as a /theme write destination. */
int ui_theme_save_defaults(const char *name, char *error, size_t size) {

	if (!name || !*name || strchr(name, '/') || name[0] == '.' || strlen(name) > 63) {
		snprintf(error, size, "invalid theme name");
		return 0;
	}
	char path[4096];
	if (!phi_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config", "phi/theme.json")) {
		snprintf(error, size, "theme path unavailable");
		return 0;
	}
	char directory[4096];
	snprintf(directory, sizeof(directory), "%s", path);
	*strrchr(directory, '/') = 0;
	if (!phi_make_directories(directory)) {
		snprintf(error, size, "cannot create theme directory: %s", strerror(errno));
		return 0;
	}
	json_object *object = theme_named_object(name);
	if (!object) {
		snprintf(error, size, "cannot allocate theme selection");
		return 0;
	}
	int ok = theme_write_object(path, object, error, size);
	json_object_put(object);
	if (ok) {
		snprintf(theme_transcript_path, sizeof(theme_transcript_path), "%s", path);
		*theme_pending_name = 0;
		theme_seen = theme_stamp_now();
	}

	return ok;
}

void ui_editor_theme_source(char *path, size_t size) {

	snprintf(path, size, "%s", theme_transcript_path);
}

void ui_editor_theme(const char *path) {

	if (!editor_host_redraw) {
		return;
	}
	snprintf(theme_transcript_path, sizeof(theme_transcript_path), "%s", path ? path : "");
	*theme_pending_name = 0;
	theme_reload(0);
}

int ui_theme_save(const char *name, char *error, size_t size) {

	if (!name || !*name || strchr(name, '/') || name[0] == '.' || strlen(name) > 63) {
		snprintf(error, size, "invalid theme name");
		return 0;
	}
	if (!*theme_transcript_path) {
		snprintf(theme_pending_name, sizeof(theme_pending_name), "%s", name);
		theme_seen = theme_stamp_named(name);
		return 1;
	}
	json_object *object = theme_named_object(name);
	int ok = theme_write_object(theme_transcript_path, object, error, size);
	json_object_put(object);
	if (ok) {
		theme_seen = theme_stamp_now();
	}

	return ok;
}

void ui_theme_transcript(const char *path, int inherit) {

	if (!path) {
		path = "";
	}
	if (!*path && inherit) {
		return; /* --no-session: remain window-local */
	}
	char next[4096] = "", source[4096] = "", global[4096] = "", error[256], name[64];
	if (*path) {
		int n = snprintf(next, sizeof(next), "%s.theme.json", path);
		if (n < 0 || (size_t)n >= sizeof(next)) {
			ui_notice("Transcript theme path is too long.", 1);
			return;
		}
		if (inherit && !strcmp(next, theme_transcript_path)) {
			return;
		}
	}
	int have_global =
		phi_xdg_path(global, sizeof(global), "XDG_CONFIG_HOME", ".config", "phi/theme.json");
	if (inherit) {
		theme_config_path(source, sizeof(source));
	} else if (have_global) {
		snprintf(source, sizeof(source), "%s", global);
	}
	if (*next) {
		struct stat st;
		int missing = stat(next, &st) && errno == ENOENT;
		/* Migrate the old default first so identical snapshots can share it. */
		char ignored[64], warning[256];
		if (have_global) {
			(void)theme_migrate_reference(global, 1, ignored, warning, sizeof(warning));
		}
		int ok;
		if (missing) {
			if (inherit && *theme_pending_name) {
				snprintf(name, sizeof(name), "%s", theme_pending_name);
				ok = 1;
			} else {
				ok = *source &&
					theme_migrate_reference(
						source, have_global && !strcmp(source, global), name, error, sizeof(error));
			}
			if (!*source && !(inherit && *theme_pending_name)) {
				snprintf(error, sizeof(error), "theme source is unavailable");
			}
			if (ok) {
				json_object *reference = theme_named_object(name);
				ok = theme_write_object(next, reference, error, sizeof(error));
				json_object_put(reference);
			}
		} else {
			ok = theme_migrate_reference(next, 0, name, error, sizeof(error));
		}
		if (!ok) {
			ui_notice(error, 1);
		}
	}
	snprintf(theme_transcript_path, sizeof(theme_transcript_path), "%s", next);
	*theme_pending_name = 0;
	/* Forks keep transient window overrides until the next explicit reload. */
	if (inherit) {
		theme_seen = theme_stamp_now();
		theme_selection_label(theme_current_name);
	} else {
		theme_reload(0);
	}
}

static void theme_reload(int announce) {

	if (!*theme_transcript_path && *theme_pending_name) {
		char error[256];
		if (!ui_theme_set(theme_pending_name, NULL, error, sizeof(error)) && announce) {
			ui_notice(error, 1);
		}
		theme_seen = theme_stamp_named(theme_pending_name);
		return;
	}
	theme_seen = theme_stamp_now();
	short colors[THEME_COUNT];
	char error[256];
	struct skin next_skin;
	int status = theme_read(colors, &next_skin, error, sizeof(error));
	if (status < 0) {
		if (announce) {
			ui_append("\n[Theme unchanged: ");
			ui_append(error);
			ui_append("]\n");
		}
		return;
	}
	transcript_selection_clear();
	skin_use(&next_skin, 0);
	theme_apply(colors);
	theme_selection_label(theme_current_name);
	transcript_revision++;
	clearok(stdscr, TRUE);
	if (announce && (used || working)) {
		ui_append(status ? "\n[Theme reloaded.]\n" : "\n[No theme file; restored defaults.]\n");
	}
	dirty = 1;
	draw(1);
}

/* Extension themes (ui_get_theme / ui_set_theme / ui_list_themes). Named
   themes are $XDG_CONFIG_HOME/phi/themes/NAME.json, or themes/ beside the
   binary; changes last for this
   session (/reload restores this transcript's selected shared theme). */
json_object *ui_theme_get(void) {

	json_object *answer = json_object_new_object(), *colors = json_object_new_object();
	for (size_t i = 0; i < THEME_COUNT; i++) {
		short value = theme_current_known ? theme_current[i] : theme_defaults[i].value;
		if (value >= THEME_HEX && value < THEME_HEX + hex_count) { /* a hex colour, as written */
			char hex[8];
			int rgb = hex_colors[value - THEME_HEX];
			snprintf(hex, sizeof(hex), "#%06x", rgb);
			json_object_object_add(colors, theme_defaults[i].name, json_object_new_string(hex));
			continue;
		}
		json_object_object_add(colors, theme_defaults[i].name,
			value < 0 ? json_object_new_string("default") : json_object_new_int(value));
	}
	json_object_object_add(answer, "name", json_object_new_string(theme_current_name));
	json_object_object_add(answer, "colors", colors);

	return answer;
}

json_object *ui_theme_list(void) {

	json_object *names = json_object_new_array();
	json_object_array_add(names, json_object_new_string("default")); /* Phi's own look */
	char directories[2][4096];
	int count = theme_directories(directories);
	for (int d = 0; d < count; d++) {
		DIR *dir = opendir(directories[d]);
		struct dirent *entry;
		while (dir && (entry = readdir(dir))) {
			size_t n = strlen(entry->d_name);
			if (n <= 5 || n >= 69 || strcmp(entry->d_name + n - 5, ".json") ||
				entry->d_name[0] == '.') {
				continue;
			}
			char name[64];
			snprintf(name, sizeof(name), "%.*s", (int)(n - 5), entry->d_name);
			int seen = 0; /* a user's theme hides a shipped one of the same name */
			for (size_t i = 0; i < json_object_array_length(names); i++) {
				if (!strcmp(json_object_get_string(json_object_array_get_idx(names, i)), name)) {
					seen = 1;
				}
			}
			if (!seen) {
				json_object_array_add(names, json_object_new_string(name));
			}
		}
		if (dir) {
			closedir(dir);
		}
	}

	return names;
}

/* name: a theme file; colors: settings over the current theme. */
int ui_theme_set(const char *name, json_object *colors, char *error, size_t size) {

	short next[THEME_COUNT];
	int saved_hex_count = hex_count;
	struct skin next_skin = skin_defaults;
	if (name && *name) {
		if (strchr(name, '/') || name[0] == '.' || strlen(name) > 63) {
			snprintf(error, size, "invalid theme name");
			return 0;
		}
		int status = theme_read_named(name, next, &next_skin, error, size);
		if (status == 0 && !strcmp(name, "default")) {
			/* Phi's built-in look, unless the user has a theme of that name. */
			for (size_t i = 0; i < THEME_COUNT; i++) {
				next[i] = theme_defaults[i].value;
			}
			next_skin = skin_defaults;
			status = 1;
		}
		if (status == 0) {
			snprintf(error, size, "no theme named %s", name);
			return 0;
		}
		if (status < 0) {
			return 0;
		}
	} else {
		if (!skin_clone(&next_skin, &skin)) {
			snprintf(error, size, "cannot allocate theme");
			return 0;
		}
		for (size_t i = 0; i < THEME_COUNT; i++) {
			next[i] = theme_current_known ? theme_current[i] : theme_defaults[i].value;
		}
	}
	if (json_object_is_type(colors, json_type_object) &&
		!theme_parse(colors, next, &next_skin, error, size)) {
		hex_count = saved_hex_count;
		skin_release(&next_skin);
		return 0;
	}
	if (name && *name) {
		snprintf(theme_current_name, sizeof(theme_current_name), "%s", name);
	}
	if (skin.welcome != next_skin.welcome ||
		strcmp(skin.welcome_widget, next_skin.welcome_widget) ||
		skin.welcome_motion.enabled != next_skin.welcome_motion.enabled ||
		strcmp(skin.welcome_transition, next_skin.welcome_transition) ||
		(skin.welcome_motion.enabled != 3 && skin.welcome_motion.enabled != 4 &&
			skin.welcome_motion.enabled != 5)) {
		header_motion_cancel();
	}
	transcript_selection_clear();
	skin_use(&next_skin, 0);
	theme_apply(next);
	cinema_palette_reset();
	transcript_revision++;
	clearok(stdscr, TRUE);
	dirty = 1;
	draw(1);

	return 1;
}

#include "editor.inc"

static void conversation_search(void) {

	size_t query_length = wcslen(conversation_query);
	int saved_scroll = scroll_offset;
	conversation_search_active = modal_active = 1;
	conversation_selected = 0;
	for (;;) {
		if (idle_callback) {
			idle_callback();
		}
		int width = text_width(COLS), rows = width > 0 ? layout(NULL, width) : 0;
		/* Scan in screen-sized windows rather than one full-transcript pad. */
		conversation_collect(NULL, 0);
		for (int first = 0; width > 0 && first < rows; first += 256) {
			WINDOW *pad = newpad(256, width);
			if (!pad) {
				break;
			}
			layout_rows(pad, width, first);
			conversation_collect(pad, first);
			delwin(pad);
		}
		if (conversation_selected >= conversation_count) {
			conversation_selected = 0;
		}
		if (conversation_count) {
			int banner = welcome_banner_height(conversation_height);
			int lines = working
				? banner + activity_line(width) + (pending_active && COLS >= 40 ? 3 : 1)
				: banner + rows;
			int maximum = lines > conversation_height ? lines - conversation_height : 0;
			int target =
				banner + conversation_hits[conversation_selected].row - conversation_height / 2;
			if (target < 0) {
				target = 0;
			}
			scroll_offset = maximum > target ? maximum - target : 0;
		}
		draw(1);
		char query[512] = "", bar[640];
		wcstombs(query, conversation_query, sizeof(query) - 1);
		snprintf(bar, sizeof(bar), "Find: %s  [%zu/%zu]  Enter/↓ next · ↑ previous · Esc close",
			query, conversation_count ? conversation_selected + 1 : 0, conversation_count);
		attrset(A_NORMAL);
		mvhline(LINES - 1, 0, ' ', COLS);
		welcome_text(LINES - 1, 1, COLS - 2, bar, 28, 1);
		curs_set(0);
		frame_begin();
		wnoutrefresh(stdscr);
		doupdate();
		frame_end();
		wint_t key;
		int kind = read_key(&key, 250);
		if (kind == ERR) {
			continue;
		}
		if (kind == KEY_CODE_YES && key == KEY_RESIZE) {
			title_resized();
			continue;
		}
		if ((kind == OK && (key == 27 || key == 3 || key == 6)) ||
			(kind == KEY_CODE_YES && (key == KEY_ESCAPE || key == KEY_INTERRUPT))) {
			break;
		}
		if ((kind == OK && (key == '\r' || key == 14)) ||
			(kind == KEY_CODE_YES && (key == KEY_ENTER || key == KEY_DOWN))) {
			if (conversation_count) {
				conversation_selected = (conversation_selected + 1) % conversation_count;
			}
			continue;
		}
		if ((kind == OK && key == 16) || (kind == KEY_CODE_YES && key == KEY_UP)) {
			if (conversation_count) {
				conversation_selected =
					(conversation_selected + conversation_count - 1) % conversation_count;
			}
			continue;
		}
		if ((kind == OK && (key == 127 || key == 8)) ||
			(kind == KEY_CODE_YES && key == KEY_BACKSPACE)) {
			if (query_length) {
				conversation_query[--query_length] = 0;
			}
			conversation_selected = 0;
			continue;
		}
		if (kind == OK && key == 21) {
			conversation_query[query_length = 0] = 0;
			conversation_selected = 0;
			continue;
		}
		if (kind == OK && key >= 32 && wcwidth(key) >= 0 &&
			query_length + 1 < sizeof(conversation_query) / sizeof(*conversation_query)) {
			conversation_query[query_length++] = (wchar_t)key;
			conversation_query[query_length] = 0;
			conversation_selected = 0;
		}
	}
	if (!conversation_count) {
		scroll_offset = saved_scroll;
	}
	conversation_collect(NULL, 0);
	conversation_search_active = modal_active = 0;
	dirty = 1;
	cursor_shape = -1;
	draw(1);
}

#include "keys.inc"

int ui_board(void) {

	if (editor_host_redraw || !getenv("PHI_CHAT_ID")) {
		return 0;
	}
	chat_backgrounded = 1;
	normal_mode = 0;
	esc_armed = left_armed = 0;
	fputs("\033]777;phi-detach\007", stdout);
	fflush(stdout);

	return 1;
}

int ui_cancel(void) {

	if (idle_callback) {
		idle_callback();
	}
	theme_follow();
	if (external_cancelled) {
		return 1;
	}
	int cancel = 0;
	wint_t key;
	int kind;
	while ((kind = read_key(&key, 0)) != ERR) {
		int action = edit_key(kind, key);
		if (action == 3) {
			cancel = 1;
			break;
		}
		if (action == 1 && editor_host_redraw) {
			ui_notice("Finish or cancel the local command first; draft kept.", 0);
			continue;
		}
		if (action == 1 && queued_count < 8) {
			char *message = take_draft();
			/* A command that runs during turns (such as /btw) runs now. */
			if (message && message[0] == '/' && turn_command && turn_command(message)) {
				free(message);
				continue;
			}
			if (message) {
				queued[queued_count++] = message;
				dirty = 1;
			}
		}
	}
	transcript_selection_autoscroll();
	paste_checkpoint();
	draw(0);

	return cancel;
}

const char *ui_peek_queued(void) {

	return queued_count ? queued[0] : NULL;
}

char *ui_take_queued(void) {

	if (!queued_count) {
		return NULL;
	}
	char *message = queued[0];
	queued_count--;
	memmove(queued, queued + 1, queued_count * sizeof(*queued));
	dirty = 1; /* it leaves the list above the prompt */
	/* A queued steering message makes the streamed tool preview stale. */
	if (working) {
		pending_active = pending_ready = 0;
		dirty = 1;
	}

	return message;
}

int ui_set_draft(const char *text) {

	if (!text) {
		text = "";
	}
	size_t count;
	wchar_t *next = wide_decode(text, &count);
	if (!next) {
		return 0;
	}
	if (!input_reserve(count)) {
		free(next);
		return 0;
	}
	int changed = count != length || memcmp(input, next, (count + 1) * sizeof(*input));
	wchar_t *before = wide_copy(input, length);
	if (!before) {
		free(next);
		return 0;
	}
	size_t before_length = length, before_cursor = cursor;
	if (count) {
		memcpy(input, next, (count + 1) * sizeof(*input));
	} else {
		input[0] = 0;
	}
	length = cursor = count;
	replace_changed = 0;
	if (changed) {
		undo_record(before, before_length, before_cursor);
	}
	free(before);
	free(next);
	recovery_save();
	paste_snapshot_clear();
	normal_mode = visual_mode = delete_pending = pasting = paste_cr = 0;
	esc_armed = 0;
	esc_armed_at = 0;
	vim_pending_clear();
	last_replace_count = 0;
	input_view = 0;
	dirty = 1;

	return 1;
}

int ui_editor_read(wint_t *key, int wait_ms) {

	int kind = read_key(key, wait_ms);
	if (kind == KEY_CODE_YES && (*key == KEY_FOCUS_IN || *key == KEY_FOCUS_OUT)) {
		focused = *key == KEY_FOCUS_IN;
		cursor_shape = -1;
	}
	if (kind == KEY_CODE_YES && (*key == KEY_ESCAPE || *key == KEY_INTERRUPT || *key == KEY_EOF)) {
		*key = *key == KEY_ESCAPE ? 27 : *key == KEY_INTERRUPT ? 3 : 4;
		kind = OK;
	}

	return kind;
}

int ui_editor_normal(void) {

	return normal_mode && !visual_mode && !replace_pending && !delete_pending && !g_pending &&
		!find_pending && !vim_count;
}

int ui_editor_pasting(void) {

	return pasting;
}

void ui_editor_focus(void) {

	esc_armed = 0;
	esc_armed_at = 0;
	normal_mode = visual_mode = delete_pending = g_pending = find_pending = 0;
	vim_pending_clear();
	ctrl_c_armed = 0;
}

void ui_editor_scope(int workspace) {

	editor_workspace = workspace;
}

void ui_editor_remember(const char *text) {

	recall_add(text);
}

int ui_editor_reset(const char *text) {

	if (!ui_set_draft(text)) {
		return 0;
	}
	undo_clear();

	return 1;
}

int ui_editor_paste_event(int kind, wint_t key) {

	return kind != KEY_CODE_YES ? 0 : key == KEY_PASTE_BEGIN ? 1 : key == KEY_PASTE_END ? -1 : 0;
}

int ui_editor_activation(int kind, wint_t key) {

	return (kind == OK && (key == '/' || key == '!' || key == '@' || key == 22)) ||
		(kind == KEY_CODE_YES &&
			(key == KEY_PASTE_BEGIN || key == KEY_CLIPBOARD || key == KEY_COMMAND_MENU));
}

int ui_editor_key(int kind, wint_t key) {

	if (!editor_host_redraw) {
		return 0;
	}
	if (kind == KEY_CODE_YES && (key == KEY_MOUSE || (key == KEY_BTAB && editor_workspace == 2))) {
		return 0;
	}
	if (!pasting && kind == OK && (key == 6 || key == 15)) {
		ui_notice("Open a chat to search its transcript or inspect tools.", 0);
		return 0;
	}

	return edit_key(kind, key);
}

int ui_editor_rows(int cols, int maximum) {

	int cy = 0, cx = 0;
	struct phi_rect box = phi_frame_input_columns(cols, skin.border == SKIN_BORDER_ROUNDED,
		*skin.prompt != 0, skin.composer_padding, skin.compact_layout);
	int rows = draft_layout(box.width, 0, 0, &cy, &cx);

	return phi_frame_editor_rows(LINES, maximum - 2, skin.composer_min_rows, skin.composer_max_rows,
			   skin.composer_max_percent, rows) +
		2;
}

void ui_editor_paint(int top, int rows, int cols, const char *label, int focus) {

	if (!editor_host_redraw || rows < 3 || cols < 4) {
		return;
	}
	struct phi_rect box = phi_frame_input_columns(cols, skin.border == SKIN_BORDER_ROUNDED,
		*skin.prompt != 0, skin.composer_padding, skin.compact_layout);
	int visible = rows - 2, cy = 0, cx = 0, total = draft_layout(box.width, 0, 0, &cy, &cx);
	if (cy < input_view) {
		input_view = cy;
	}
	if (cy >= input_view + visible) {
		input_view = cy - visible + 1;
	}
	if (input_view > total - visible) {
		input_view = total - visible;
	}
	if (input_view < 0) {
		input_view = 0;
	}
	input_left = box.x;
	int suggestions = focus ? command_rows(top) : 0;
	if (suggestions) {
		command_draw(top - suggestions, suggestions, cols);
	}
	prompt_border(top + 1, top + rows - 1, cols, 5);
	chrome_draw(&skin.composer_elements, 0, top, cols, rows, 27, 3, 28, ui_chrome_expand, NULL);
	if (label && *label) {
		welcome_text(top, skin.margin + 2, cols - 2 * skin.margin - 4, label, 3, 0);
	}
	if (*skin.prompt) {
		welcome_text(top + 1, input_left - 2, 1, skin.prompt, 28, 0);
	}
	draft_layout(box.width, top + 1, visible, &cy, &cx);
	border_overflow(top, cols, input_view, L'↑');
	border_overflow(top + rows - 1, cols, total - input_view - visible, L'↓');
	curs_set(focus && !normal_mode && focused ? 1 : 0);
	if (focus) {
		if (normal_mode && focused) {
			wchar_t ch[2] = {cursor < length && input[cursor] != L'\n' ? input[cursor] : L' ', 0};
			attron(A_REVERSE);
			mvaddwstr(top + 1 + cy - input_view, input_left + cx, ch);
			attroff(A_REVERSE);
		}
		draft_cursor_move(top + 1 + cy - input_view, input_left + cx);
	}
}

/* A throttled final frame must get a deadline even without an idle callback.
   Otherwise the last key/tool update can remain invisible until the next key.
   Usage polling wakes the loop, but does not force the transcript to redraw. */
/* Side requests are streaming: wake often so their text keeps flowing. */
static int quick_ticks;

void ui_quick_ticks(int on) {

	quick_ticks = on;
}

static int input_wait_timeout(void) {

	paste_checkpoint();
	if (history_view_active() || ui_frame_suspended || modal_active) {
		int wait = title_wait_timeout(), next = ui_surfaces_wait(), layout_next = ui_layout_wait();
		if (next >= 0 && (wait < 0 || next < wait)) {
			wait = next;
		}
		if (layout_next >= 0 && (wait < 0 || layout_next < wait)) {
			wait = layout_next;
		}
		if (idle_callback && (wait < 0 || wait > 250)) {
			wait = 250;
		}
		if (idle_callback && quick_ticks && wait > 30) {
			wait = 30;
		}
		if (dirty && !modal_active) {
			double remaining = FRAME_INTERVAL - (now() - last_draw);
			int frame_wait = remaining > 0 ? (int)(remaining * 1000) + 1 : 0;
			if (wait < 0 || frame_wait < wait) {
				wait = frame_wait;
			}
		}
		return wait;
	}
	int wait = title_wait_timeout();
	int blink_wait = welcome_blink_wait();
	if (blink_wait >= 0 && (wait < 0 || blink_wait < wait)) {
		wait = blink_wait;
	}
	int surface_timeout = ui_surfaces_wait();
	int layout_timeout = ui_layout_wait();
	if (layout_timeout >= 0 && (surface_timeout < 0 || layout_timeout < surface_timeout)) {
		surface_timeout = layout_timeout;
	}
	if (surface_timeout >= 0 && (wait < 0 || surface_timeout < wait)) {
		wait = surface_timeout;
	}
	int motion_timeout = header_motion_wait();
	if (motion_timeout >= 0 && (wait < 0 || motion_timeout < wait)) {
		wait = motion_timeout;
	}
	int cinema_timeout = cinema_wait();
	if (cinema_timeout >= 0 && (wait < 0 || cinema_timeout < wait)) {
		wait = cinema_timeout;
	}
	if (welcome_scene_animated() && (wait < 0 || wait > 50)) {
		wait = 50;
	}
	if (pasting && paste_recovery_pending && recovery_dir >= 0) {
		double remaining = 0.25 - (now() - paste_saved_at);
		int checkpoint_wait = remaining > 0 ? (int)(remaining * 1000) + 1 : 0;
		if (wait < 0 || wait > checkpoint_wait) {
			wait = checkpoint_wait;
		}
	}
	if (idle_callback && (wait < 0 || wait > 250)) {
		wait = 250;
	}
	if (idle_callback && quick_ticks && wait > 30) {
		wait = 30;
	}
	if (transcript_selection.dragging && (wait < 0 || wait > 50)) {
		wait = 50;
	}
	if (dirty) {
		double remaining = (wheel_pending ? SCROLL_FRAME : FRAME_INTERVAL) - (now() - last_draw);
		int frame_wait = remaining > 0 ? (int)(remaining * 1000) + 1 : 0;
		if (wait < 0 || frame_wait < wait) {
			wait = frame_wait;
		}
	}

	return wait;
}

char *ui_input(void) {

	if (working) {
		working = 0;
		dirty = 1;
	}
	command_files_reset();
	char *queued_message = ui_take_queued();
	if (queued_message) {
		return queued_message;
	}
	for (;;) {
		if (idle_callback) {
			idle_callback();
		}
		if (external_pending && external_pending()) {
			return strdup(""); /* yield to the host queue, not the editor */
		}
		theme_follow();
		/* Keep keystrokes/status changes immediate. A timer waking an otherwise
           clean UI is not a reason to allocate and repaint the transcript. */
		draw(dirty);
		int wait = input_wait_timeout();
		wint_t key;
		int kind = read_key(&key, wait);
		if (kind == ERR) {
			if (report_filtered) {
				continue; /* Unknown CSI/OSC is not EOF. */
			}
			transcript_selection_autoscroll();
			struct pollfd terminal = {STDIN_FILENO, POLLIN, 0};
			if (poll(&terminal, 1, 0) > 0 && (terminal.revents & (POLLHUP | POLLERR | POLLNVAL))) {
				return NULL;
			}
			if (wait >= 0) {
				continue; /* deadlines only paint when something is dirty */
			}
			return NULL;
		}
		int action = edit_key(kind, key);
		if (action == 2) {
			return NULL;
		}
		if (action == 1) {
			char *message = take_draft();
			if (message) {
				return message;
			}
		}
	}
}

#include "resume.inc"
#include "dialog.inc"
#include "undotree.inc"
#include "tree.inc"
#include "history-view.inc"
#include "scrollback.inc"
