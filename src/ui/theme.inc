/* Flat, bounded JSON theme. Direct-colour terminals render hex values exactly;
   indexed terminals can temporarily reuse extended palette slots.
   The board includes it with THEME_LOADER_ONLY: reading themes (extends,
   named themes, hex colours) without Phi's own colour pairs. */
#include <json-c/json.h>
#include "../core/ui-layout.h"
#include <limits.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct theme_entry {
	const char *name;
	short value;
} theme_defaults[] = {
	/* Keep the terminal's own canvas; colour is a small semantic accent,
       not a full-width surface. User compositions can opt into stronger bands. */
	{"text", -1}, {"background", -1}, {"user_text", -1}, {"user_background", -1}, {"muted", 246},
	{"border", 238}, {"spinner", 179}, {"tool_text", -1}, {"tool_heading", 179},
	{"tool_background", -1}, {"error", 174}, {"tool_rule", 240}, {"tool_success", 115},
	{"diff_removed", 174}, {"diff_added", 108}, {"diff_context", 245},
	{"diff_removed_background", 234}, {"diff_added_background", 234}, {"code_text", -1},
	{"code_background", -1},
	/* `inline code` in prose; -2 (unset) means the code block's colours. */
	{"inline_code", 223}, {"inline_code_background", -2}, {"syntax_keyword", 179},
	{"syntax_string", 115}, {"syntax_number", 180}, {"syntax_comment", 245},
	{"syntax_function", 223}, {"syntax_operator", 246}, {"accent", 179}, {"mascot_primary", 179},
	{"mascot_light", 223}, {"mascot_ink", 235}, {"mascot_casing", 244}};

#define THEME_COUNT (sizeof(theme_defaults) / sizeof(*theme_defaults))

static int theme_hex_digit(char ch) {

	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}

	return -1;
}

static void theme_rgb(int index, int *r, int *g, int *b) {

	static const int levels[] = {0, 95, 135, 175, 215, 255};
	if (index >= 232) {
		*r = *g = *b = 8 + (index - 232) * 10;
		return;
	}
	index -= 16;
	*r = levels[index / 36];
	*g = levels[(index / 6) % 6];
	*b = levels[index % 6];
}

/* The fixed xterm-256 colour nearest to r, g, b (the cube and greys only:
   ANSI 0-15 are user-defined). */
static short theme_nearest(int red, int green, int blue) {

	int best = 16, distance = INT_MAX;
	for (int i = 16; i < 256; i++) {
		int r, g, b;
		theme_rgb(i, &r, &g, &b);
		int delta = (r - red) * (r - red) + (g - green) * (g - green) + (b - blue) * (b - blue);
		if (delta < distance) {
			distance = delta;
			best = i;
		}
	}

	return (short)best;
}

/* Hex colours as written: a theme value of THEME_HEX + n is hex_colors[n].
   Direct terminals use literal RGB. Indexed terminals use the nearest colour,
   or, with palette.exact, a temporarily redefined palette slot. */
#define THEME_HEX 1000
#define THEME_HEX_MAX 64
static int hex_colors[THEME_HEX_MAX], hex_count;
static short hex_slot[THEME_HEX_MAX]; /* its exact palette slot, 0 if none */

static short theme_hex_value(int red, int green, int blue) {

	int rgb = red << 16 | green << 8 | blue;
	for (int i = 0; i < hex_count; i++) {
		if (hex_colors[i] == rgb) {
			return (short)(THEME_HEX + i);
		}
	}
	if (hex_count == THEME_HEX_MAX) {
		return theme_nearest(red, green, blue);
	}
	hex_colors[hex_count] = rgb;

	return (short)(THEME_HEX + hex_count++);
}

static int theme_color(json_object *value, short *result) {

	if (json_object_is_type(value, json_type_int)) {
		int64_t number = json_object_get_int64(value);
		if (number < -1 || number > 255) {
			return 0;
		}
		*result = (short)number;
		return 1;
	}
	if (!json_object_is_type(value, json_type_string)) {
		return 0;
	}
	const char *text = json_object_get_string(value);
	size_t size = (size_t)json_object_get_string_len(value);
	if (size == 7 && !memcmp(text, "default", 7)) {
		*result = -1;
		return 1;
	}
	if (size != 7 || text[0] != '#') {
		return 0;
	}
	int rgb[3] = {0};
	for (int i = 0; i < 6; i++) {
		int digit = theme_hex_digit(text[i + 1]);
		if (digit < 0) {
			return 0;
		}
		rgb[i / 2] = rgb[i / 2] * 16 + digit;
	}
	*result = theme_hex_value(rgb[0], rgb[1], rgb[2]);

	return 1;
}

/* Skin: presentation settings beside the colours (glyphs, spinner, prompt
   border, tool titles, approvals, welcome). Missing keys keep Phi's look. */
enum {
	SKIN_BORDER_LINE,
	SKIN_BORDER_ROUNDED,
	SKIN_BORDER_NONE,
	SKIN_BORDER_HEAVY
};

enum {
	SKIN_WELCOME_PHI,
	SKIN_WELCOME_NONE,
	SKIN_WELCOME_FETCH,
	SKIN_WELCOME_SCENE,
	SKIN_WELCOME_CINEMA
};

enum {
	TOOL_BORDER_NONE,
	TOOL_BORDER_LEFT,
	TOOL_BORDER_ROUNDED
};

enum {
	TOOL_PATH_FULL,
	TOOL_PATH_RELATIVE,
	TOOL_PATH_BASENAME
};

enum {
	DIFF_STYLE_BACKGROUND,
	DIFF_STYLE_TEXT,
	DIFF_STYLE_MARKERS
};

enum {
	DIFF_LABEL_FULL,
	DIFF_LABEL_COMPACT,
	DIFF_LABEL_NONE
};

enum {
	DIFF_SIGN_AFTER,
	DIFF_SIGN_BEFORE
};

enum {
	USER_GLYPH_ACCENT,
	USER_GLYPH_MUTED,
	USER_GLYPH_TEXT,
	USER_GLYPH_NONE
};

enum {
	TOOL_DURATION_OFF,
	TOOL_DURATION_HEADING,
	TOOL_DURATION_FOOTER
};

enum {
	TOOL_DETAIL_MUTED,
	TOOL_DETAIL_TITLE
};

enum {
	TABLE_RULES_HEADER,
	TABLE_RULES_ROWS,
	TABLE_RULES_NONE
};

#define SKIN_TOOLS 16

/* One tool's own settings (tool.title.NAME, tool.max_lines.NAME,
   tool.duration.NAME, tool.hide_output); unset ones follow the theme's. */
struct skin_tool {
	char name[32];
	char title[64], renderer[64], live_renderer[64];
	int renderer_set, live_renderer_set;
	int title_set, max_lines, max_lines_set, duration, duration_set, hidden;
};

#define SKIN_WORDS 16
#define SKIN_FRAMES 16

struct welcome_element {
	int x, y, anchor, color, bold, interval, frame_count;
	char **frames; /* Only the supplied frames and their text are allocated. */
};

/* Reusable normalized motion profile; keyframes can hold, overshoot or bounce. */
struct motion_profile {
	int enabled, duration, easing, count;

	struct {
		double at, value;
	} frames[12];
};

enum semantic_message_kind {
	SEM_USER,
	SEM_ASSISTANT,
	SEM_THINKING,
	SEM_MESSAGE_KINDS
};

struct surface_settings {
	char id[64];
	int rows, percent;
};

struct chrome_element {
	char id[64], text[256], glyph[16];
	int anchor, x, y, width, style;
};

struct chrome_elements {
	struct chrome_element items[16];
	int count;
};

/* Bounded idle gestures: six gestures, three poses each, no per-frame allocation. */
struct mascot_gesture {
	char frames[3][4][96];
	int rows[3], count, interval;
};

struct skin {
	char card[16], reply[16], output[16], prompt[16], fold[16];
	wchar_t frames[SKIN_FRAMES + 1];
	int frame_count, frame_ms;
	char words[SKIN_WORDS][40];
	int word_count;
	int border;
	char tool_title[64], tool_renderer[64], tool_live_renderer[64];
	char message_renderer[64], message_renderers[SEM_MESSAGE_KINDS][64];
	int message_renderer_set[SEM_MESSAGE_KINDS];
	int approval_menu;
	int welcome;
	char welcome_title[64], welcome_lines[3][128];
	int welcome_line_count;
	char welcome_fields[8][16];
	int welcome_field_count;
	struct welcome_element *scene;
	int scene_count;
	int welcome_life; /* enable the optional native header widget */
	char welcome_widget[64], hero_lines[3][128];
	int hero_line_count;
	int welcome_hero, welcome_persist;
	struct motion_profile welcome_motion;
	char welcome_transition[64];
	char footer_left[256], footer_right[256], composer_label[256], composer_hint[256];
	char header_left[256], header_right[256];
	int header_visible;
	int footer_left_set, footer_right_set, footer_visible, footer_dim;
	int composer_min_rows, composer_max_rows, composer_padding, composer_max_percent;
	int transcript_max_width, transcript_padding;
	struct phi_layout layout, empty_layout;
	struct surface_settings
		surfaces[6]; /* composer, transcript, footer, board, board header/footer */
	struct chrome_elements composer_elements, board_header_elements, board_footer_elements;
	int board_header_visible, board_header_border, board_footer_visible, board_footer_border,
		board_input_visible;
	int board_header_rows, board_footer_rows;
	char board_input_placeholder[256];
	int footer_rows;
	char footer_lines[8][256];
	int footer_line_count;
	int board_cards, board_gap, board_padding, board_margin;
	int board_summary, board_instructions, board_headings, board_preview, board_age, board_place;
	int board_card_border;
	char board_card_lines[8][256];
	int board_card_line_count;
	int welcome_message_padding;
	int welcome_message_row; /* 0: below artwork; otherwise transcript offset in rows */
	int mascot;
	char art[4][96]; /* welcome.art: lines drawn left of the title */
	int art_count, art_width;
	char art_blink[4][96]; /* Optional closed-eye pose; same bounds as welcome.art. */
	int art_blink_count;
	struct mascot_gesture art_gestures[6];
	int art_gesture_count;
	int art_intro[6], art_intro_count;
	int margin; /* layout.margin: blank columns left and right of the conversation */
	int shimmer, stats, rotate; /* spinner.shimmer, spinner.stats, spinner.rotate (s) */
	char tips[8][120];
	int tip_count;
	int user_bold, user_glyph; /* user.bold; user.glyph: USER_GLYPH_* */
	int thinking_visible; /* provider-visible summaries, never encrypted reasoning */
	int tool_padding, tool_spacing, tool_max_width, tool_border, tool_path;
	int tool_duration; /* tool.duration: TOOL_DURATION_* */
	int tool_change_counts, tool_heading_bold;
	int diff_style, diff_line_numbers, diff_gutter_width, diff_saved_label;
	int scroll_lines; /* scroll.lines: lines per wheel event (default 3); 0 is "auto" */
	char done[8][24]; /* turn.done: "Worked" in "Worked for 14s" after a turn; none: no line */
	int done_count;
	char done_glyph[16]; /* glyph.done: before it, e.g. "✻" */
	int done_clock; /* turn.clock: "· done 1:41 PM" after it */
	int tool_expanded; /* tool.expanded: cards open (as Ctrl+O), not folded */
	int tool_live; /* tool.live: 0 cards, 1 status, 2 grouped */
	int tool_progress; /* tool.progress: 0 below live preview, 1 replace */
	int tool_live_command; /* tool.live.command: show command under live status/group */
	int tool_padding_y; /* tool.padding_y: band rows above and below a card's text */
	int user_padding_y; /* user.padding_y: band rows above and below your messages */
	char bullet[16]; /* markdown.bullet: the mark of a "- " list item */
	int table_rules; /* markdown.table.rules: TABLE_RULES_* */
	int tool_indent; /* tool.indent: columns before a card's output (0-8) */
	int tool_captions; /* tool.captions: caption rows such as "2 lines · 36 B" */
	int tool_heading_gap; /* tool.heading_gap: blank rows between a card's heading and body */
	int tool_detail; /* tool.detail: TOOL_DETAIL_MUTED, or _TITLE (the heading's colour) */
	int tool_heading_wrap; /* tool.heading_wrap: a long heading wraps instead of ending in "…" */
	int tool_max_lines; /* tool.max_lines: open cards show this many output lines (0: all) */
	int palette_exact; /* palette.exact: hex colours exactly, in palette slots 232-251 */
	int diff_sign; /* diff.sign: DIFF_SIGN_AFTER the line number ("2 - x"), or _BEFORE ("-2 x") */
	int tool_line_numbers; /* tool.line_numbers: numbers beside write and read previews */
	struct skin_tool tools[SKIN_TOOLS]; /* per-tool settings */
	int tool_count;
	int compact_layout; /* layout.compact: no trailing prose gap, edge-to-edge input, spaced spinner */
};

/* Fields left out are 0 (off, empty or the first choice). */
#define SKIN_DEFAULTS \
	{.card = "◆", \
		.prompt = "❯", \
		.frames = L"⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏", \
		.frame_count = 10, \
		.frame_ms = 80, \
		.words = {"Working"}, \
		.word_count = 1, \
		.border = SKIN_BORDER_LINE, \
		.welcome = SKIN_WELCOME_CINEMA, \
		.welcome_widget = "orbit", \
		.welcome_hero = 1, \
		.welcome_title = "P H I", \
		.hero_lines = {"{model}", "reasoning / {effort}", "{project}"}, \
		.hero_line_count = 3, \
		.welcome_life = 1, \
		.welcome_persist = 1, \
		.welcome_motion = {.enabled = 4, .duration = 250}, \
		.welcome_message_padding = 2, \
		.board_card_lines = {"{preview}", "{path} · {model} · {effort}"}, \
		.board_card_line_count = 2, \
		.board_gap = 1, \
		.board_padding = 1, \
		.board_margin = 2, \
		.board_summary = 0, \
		.board_instructions = 1, \
		.board_headings = 1, \
		.board_preview = 1, \
		.board_age = 1, \
		.board_place = 1, \
		.board_header_visible = 1, \
		.board_header_border = 0, \
		.board_footer_visible = 0, \
		.board_footer_border = 1, \
		.board_input_visible = 1, \
		.board_header_rows = 0, \
		.board_footer_rows = 1, \
		.board_footer_elements = \
			{.items = {{.id = "keys", .text = "{mode} · {hint}", .anchor = 2, .style = 1}}, \
				.count = 1}, \
		.board_input_placeholder = "press i to describe a task for a new chat", \
		.surfaces = {{.rows = 4}, {.rows = 4}, {.rows = 4}, {.rows = 4}, {.rows = 4}, \
			{.rows = 4}}, \
		.header_visible = 1, \
		.header_left = "PHI / {project}", \
		.header_right = "{model}", \
		.footer_rows = 1, \
		.composer_max_percent = 90, \
		.footer_visible = 1, \
		.footer_dim = 0, \
		.composer_min_rows = 1, \
		.composer_max_rows = 6, \
		.welcome_fields = {"model", "effort", "path", "theme", "memory"}, \
		.welcome_field_count = 5, \
		.margin = 1, \
		.user_bold = 1, \
		.user_glyph = USER_GLYPH_ACCENT, \
		.tool_spacing = 1, \
		.tool_border = TOOL_BORDER_NONE, \
		.tool_path = TOOL_PATH_FULL, \
		.tool_duration = TOOL_DURATION_HEADING, \
		.tool_change_counts = 1, \
		.tool_heading_bold = 1, \
		.diff_style = DIFF_STYLE_TEXT, \
		.diff_line_numbers = 1, \
		.diff_gutter_width = 3, \
		.diff_saved_label = DIFF_LABEL_FULL, \
		.scroll_lines = 3, \
		.done = {"Worked"}, \
		.done_count = 1, \
		.done_clock = 1, \
		.bullet = "•", \
		.tool_indent = 3, \
		.tool_captions = 1, \
		.tool_detail = TOOL_DETAIL_MUTED, \
		.diff_sign = DIFF_SIGN_AFTER, \
		.tool_line_numbers = 1, \
		.tool_live_command = 1}
static const struct skin skin_defaults = SKIN_DEFAULTS;

/* Skins own optional scenes and layouts. Assignment is safe only for empty
   defaults; use clone for a candidate and move when committing it. */
static inline void welcome_elements_free(struct welcome_element *scene, int count) {

	if (!scene) {
		return;
	}
	for (int i = 0; i < count; i++) {
		if (scene[i].frames) {
			for (int j = 0; j < scene[i].frame_count; j++) {
				free(scene[i].frames[j]);
			}
		}
		free(scene[i].frames);
	}
	free(scene);
}

static inline void skin_scene_release(struct skin *s) {

	welcome_elements_free(s->scene, s->scene_count);
	s->scene = NULL;
	s->scene_count = 0;
}

static inline void skin_release(struct skin *s) {

	skin_scene_release(s);
	phi_layout_free(&s->layout);
	phi_layout_free(&s->empty_layout);
}

/* dst is fresh/unowned; failed clones leave it safe to release. */
static inline int skin_clone(struct skin *dst, const struct skin *src) {

	*dst = *src;
	dst->scene = NULL;
	dst->layout = (struct phi_layout){0};
	dst->empty_layout = (struct phi_layout){0};
	if (!phi_layout_clone(&dst->layout, &src->layout) ||
		!phi_layout_clone(&dst->empty_layout, &src->empty_layout)) {
		skin_release(dst);
		return 0;
	}
	if (!src->scene_count) {
		return 1;
	}
	dst->scene = calloc((size_t)src->scene_count, sizeof(*dst->scene));
	if (!dst->scene) {
		skin_release(dst);
		return 0;
	}
	for (int i = 0; i < src->scene_count; i++) {
		dst->scene[i] = src->scene[i];
		dst->scene[i].frames = NULL;
		int count = src->scene[i].frame_count;
		dst->scene[i].frames = calloc((size_t)count, sizeof(*dst->scene[i].frames));
		if (!dst->scene[i].frames) {
			skin_release(dst);
			return 0;
		}
		for (int j = 0; j < count; j++) {
			dst->scene[i].frames[j] = strdup(src->scene[i].frames[j]);
			if (!dst->scene[i].frames[j]) {
				skin_release(dst);
				return 0;
			}
		}
	}

	return 1;
}

static inline void skin_move(struct skin *dst, struct skin *src) {

	if (dst == src) {
		return;
	}
	skin_release(dst);
	*dst = *src;
	src->scene = NULL;
	src->scene_count = 0;
	src->layout = (struct phi_layout){0};
	src->empty_layout = (struct phi_layout){0};
}

/* The entry for a tool named in a key, created if new; NULL for a bad name
   or a full table. */
static struct skin_tool *skin_tool_slot(struct skin *s, const char *name) {

	size_t n = strlen(name);
	if (!n || n >= sizeof(s->tools[0].name)) {
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') {
			return NULL;
		}
	}
	for (int i = 0; i < s->tool_count; i++) {
		if (!strcmp(s->tools[i].name, name)) {
			return &s->tools[i];
		}
	}
	if (s->tool_count == SKIN_TOOLS) {
		return NULL;
	}
	struct skin_tool *tool = &s->tools[s->tool_count++];
	memset(tool, 0, sizeof(*tool));
	snprintf(tool->name, sizeof(tool->name), "%s", name);

	return tool;
}

#ifndef THEME_LOADER_ONLY
/* Shell tools: their cards show a command, and their output keeps its last
   lines under tool.max_lines (the newest output matters most). */
static int tool_shell(const char *name) {

	return name && (!strcmp(name, "bash") || !strcmp(name, "bash_start"));
}

/* A tool's own settings, or NULL. bash_start (a background command) uses
   bash's unless it has its own. */
static const struct skin_tool *skin_tool_find(const struct skin *s, const char *name) {

	for (int pass = 0; name && pass < 2; pass++) {
		for (int i = 0; i < s->tool_count; i++) {
			if (!strcmp(s->tools[i].name, name)) {
				return &s->tools[i];
			}
		}
		if (strcmp(name, "bash_start")) {
			break;
		}
		name = "bash";
	}

	return NULL;
}

/* A tool's output cap: its own tool.max_lines.NAME, else tool.max_lines. */
static int skin_cap_for(const struct skin *s, const char *name) {

	const struct skin_tool *tool = skin_tool_find(s, name);

	return tool && tool->max_lines_set ? tool->max_lines : s->tool_max_lines;
}

/* Whether any card can be capped. */
static int skin_caps_any(const struct skin *s) {

	for (int i = 0; i < s->tool_count; i++) {
		if (s->tools[i].max_lines_set && s->tools[i].max_lines) {
			return 1;
		}
	}

	return s->tool_max_lines > 0;
}

/* The heading template for a tool: its own tool.title.NAME, else tool.title. */
static const char *skin_title_for(const struct skin *s, const char *name) {

	const struct skin_tool *tool = skin_tool_find(s, name);

	return tool && tool->title_set ? tool->title : s->tool_title;
}

static const char *skin_renderer_for(const struct skin *s, const char *name) {

	for (int pass = 0; name && pass < 2; pass++) {
		for (int i = 0; i < s->tool_count; i++) {
			if (!strcmp(s->tools[i].name, name) && s->tools[i].renderer_set) {
				return s->tools[i].renderer;
			}
		}
		if (strcmp(name, "bash_start")) {
			break;
		}
		name = "bash";
	}

	return s->tool_renderer;
}

static const char *skin_live_renderer_for(const struct skin *s, const char *name) {

	for (int pass = 0; name && pass < 2; pass++) {
		for (int i = 0; i < s->tool_count; i++) {
			if (!strcmp(s->tools[i].name, name) && s->tools[i].live_renderer_set) {
				return s->tools[i].live_renderer;
			}
		}
		if (strcmp(name, "bash_start")) {
			break;
		}
		name = "bash";
	}

	return s->tool_live_renderer;
}

static const char *skin_message_renderer(const struct skin *s, int kind) {

	return s->message_renderer_set[kind] ? s->message_renderers[kind] : s->message_renderer;
}

static int skin_messages_any(const struct skin *s) {

	if (*s->message_renderer) {
		return 1;
	}
	for (int i = 0; i < SEM_MESSAGE_KINDS; i++) {
		if (*s->message_renderers[i]) {
			return 1;
		}
	}

	return 0;
}

static int skin_renderers_any(const struct skin *s) {

	if (*s->tool_renderer) {
		return 1;
	}
	for (int i = 0; i < s->tool_count; i++) {
		if (*s->tools[i].renderer) {
			return 1;
		}
	}

	return 0;
}

/* Where a tool's time goes (TOOL_DURATION_*): tool.duration.NAME, else tool.duration. */
static int skin_duration_for(const struct skin *s, const char *name) {

	const struct skin_tool *tool = skin_tool_find(s, name);

	return tool && tool->duration_set ? tool->duration : s->tool_duration;
}

/* tool.hide_output: the tool's cards show only their heading. */
static int skin_hidden_for(const struct skin *s, const char *name) {

	const struct skin_tool *tool = skin_tool_find(s, name);

	return tool && tool->hidden;
}
#endif

/* Glyph slots. The transcript stores these private-use characters, and
   layout draws the current skin's glyph (with its space) in their place, so
   switching themes or resuming a session restyles every message alike. A
   slot for an empty glyph draws nothing. GLYPH_HANG is the indent under an
   output glyph. */
#define GLYPH_CARD "\xee\x80\x80"
#define GLYPH_REPLY "\xee\x80\x81"
#define GLYPH_OUTPUT "\xee\x80\x82"
#define GLYPH_FOLD "\xee\x80\x83"
#define GLYPH_PROMPT "\xee\x80\x84"
#define GLYPH_HANG "\xee\x80\x85"
#define GLYPH_FIRST 0xE000
#define GLYPH_LAST 0xE005
#ifndef THEME_LOADER_ONLY
static struct skin skin = SKIN_DEFAULTS;
#endif

/* Display width of printable UTF-8 text, or -1 (invalid or control). */
static int skin_width(const char *text) {

	mbstate_t state = {0};
	int cells = 0;
	while (*text) {
		wchar_t ch;
		size_t n = mbrtowc(&ch, text, strlen(text), &state);
		if (n == (size_t)-1 || n == (size_t)-2 || !n) {
			return -1;
		}
		int width = wcwidth(ch);
		if (width < 0) {
			return -1;
		}
		cells += width;
		text += n;
	}

	return cells;
}

/* A short string with a display width in [least, most]. */
static int skin_string(json_object *value, char *out, size_t size, int least, int most) {

	if (!json_object_is_type(value, json_type_string)) {
		return 0;
	}
	const char *text = json_object_get_string(value);
	if (strlen(text) >= size) {
		return 0;
	}
	int width = skin_width(text);
	if (width < least || width > most) {
		return 0;
	}
	memcpy(out, text, strlen(text) + 1);

	return 1;
}

static int skin_parse(const char *key, json_object *value, struct skin *s) {

	if (!strcmp(key, "ui.layout") || !strcmp(key, "ui.layout.empty")) {
		struct phi_layout *layout = !strcmp(key, "ui.layout") ? &s->layout : &s->empty_layout;
		if (json_object_is_type(value, json_type_null)) {
			phi_layout_free(layout);
			return 1;
		}
		return phi_layout_parse(value, layout);
	}
	if (!strcmp(key, "welcome.transition")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *type = json_object_get_string(value);
		if (strcmp(type, "dither") && strcmp(type, "none") && strcmp(type, "slide") &&
			strcmp(type, "dissolve") && strcmp(type, "instant")) {
			if (!*type || strlen(type) >= sizeof(s->welcome_transition)) {
				return 0;
			}
			for (const char *p = type; *p; p++) {
				if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
					return 0;
				}
			}
			snprintf(s->welcome_transition, sizeof(s->welcome_transition), "%s", type);
			s->welcome_motion.enabled = 5;
			return 1;
		}
		s->welcome_transition[0] = 0;
		s->welcome_motion.enabled = !strcmp(type, "dither") ? 4
			: !strcmp(type, "none")                         ? 3
			: !strcmp(type, "dissolve")                     ? 2
															: !strcmp(type, "slide");
		return 1;
	}
	if (!strcmp(key, "welcome.transition.duration")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int ms = json_object_get_int(value);
		if (ms < 0 || ms > 2000) {
			return 0;
		}
		s->welcome_motion.duration = ms;
		return 1;
	}
	if (!strcmp(key, "welcome.transition.easing")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *easing = json_object_get_string(value);
		const char *const names[] = {"smoothstep", "linear", "ease-out", "ease-in"};
		for (int i = 0; i < 4; i++) {
			if (!strcmp(easing, names[i])) {
				s->welcome_motion.easing = i;
				return 1;
			}
		}
		return 0;
	}
	if (!strcmp(key, "welcome.transition.curve")) {
		if (!json_object_is_type(value, json_type_array)) {
			return 0;
		}
		size_t count = json_object_array_length(value);
		if (count && (count < 2 || count > 12)) {
			return 0;
		}
		struct motion_profile profile = s->welcome_motion;
		profile.count = (int)count;
		for (size_t i = 0; i < count; i++) {
			json_object *point = json_object_array_get_idx(value, i);
			if (!json_object_is_type(point, json_type_array) ||
				json_object_array_length(point) != 2) {
				return 0;
			}
			json_object *a = json_object_array_get_idx(point, 0),
						*v = json_object_array_get_idx(point, 1);
			if ((!json_object_is_type(a, json_type_double) &&
					!json_object_is_type(a, json_type_int)) ||
				(!json_object_is_type(v, json_type_double) &&
					!json_object_is_type(v, json_type_int))) {
				return 0;
			}
			double at = json_object_get_double(a), amount = json_object_get_double(v);
			if (!(at >= 0 && at <= 1 && amount >= -.25 && amount <= 1.25) ||
				(i && at <= profile.frames[i - 1].at)) {
				return 0;
			}
			profile.frames[i].at = at;
			profile.frames[i].value = amount;
		}
		if (count &&
			(profile.frames[0].at != 0 || profile.frames[0].value != 0 ||
				profile.frames[count - 1].at != 1 || profile.frames[count - 1].value != 1)) {
			return 0;
		}
		s->welcome_motion = profile;
		return 1;
	}
	if (!strcmp(key, "welcome.hero.lines")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 3) {
			return 0;
		}
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			if (!skin_string(json_object_array_get_idx(value, i), s->hero_lines[i],
					sizeof(s->hero_lines[i]), 0, 120)) {
				return 0;
			}
		}
		s->hero_line_count = (int)json_object_array_length(value);
		return 1;
	}
	if (!strcmp(key, "welcome.widget")) {
		return skin_string(value, s->welcome_widget, sizeof(s->welcome_widget), 1, 63);
	}
	if (!strcmp(key, "welcome.layout")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *layout = json_object_get_string(value);
		if (strcmp(layout, "hero") && strcmp(layout, "canvas")) {
			return 0;
		}
		s->welcome_hero = !strcmp(layout, "hero");
		return 1;
	}
	if (!strcmp(key, "header.visible")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		s->header_visible = json_object_get_boolean(value);
		return 1;
	}
	if (!strcmp(key, "header.left") || !strcmp(key, "header.right")) {
		return skin_string(value, !strcmp(key, "header.left") ? s->header_left : s->header_right,
			sizeof(s->header_left), 0, 240);
	}
	if (!strcmp(key, "welcome.persist") || !strcmp(key, "footer.visible") ||
		!strcmp(key, "footer.dim")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		if (!strcmp(key, "welcome.persist")) {
			s->welcome_persist = json_object_get_boolean(value);
		} else if (!strcmp(key, "footer.visible")) {
			s->footer_visible = json_object_get_boolean(value);
		} else {
			s->footer_dim = json_object_get_boolean(value);
		}
		return 1;
	}
	if (!strcmp(key, "footer.left") || !strcmp(key, "footer.right") ||
		!strcmp(key, "composer.label") || !strcmp(key, "composer.hint")) {
		char *dest = !strcmp(key, "footer.left") ? s->footer_left
			: !strcmp(key, "footer.right")       ? s->footer_right
			: !strcmp(key, "composer.label")     ? s->composer_label
												 : s->composer_hint;
		if (!skin_string(value, dest, 256, 0, 120)) {
			return 0;
		}
		if (!strcmp(key, "footer.left")) {
			s->footer_left_set = 1;
		}
		if (!strcmp(key, "footer.right")) {
			s->footer_right_set = 1;
		}
		return 1;
	}
	const char *surface_names[] = {"composer.surface", "transcript.surface", "footer.surface",
		"board.surface", "board.header.surface", "board.footer.surface"};
	for (int i = 0; i < 6; i++) {
		size_t n = strlen(surface_names[i]);
		if (strncmp(key, surface_names[i], n)) {
			continue;
		}
		if (!key[n]) {
			if (!json_object_is_type(value, json_type_string)) {
				return 0;
			}
			const char *id = json_object_get_string(value);
			if (strlen(id) >= sizeof(s->surfaces[i].id)) {
				return 0;
			}
			for (const char *p = id; *p; p++) {
				if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
					return 0;
				}
			}
			snprintf(s->surfaces[i].id, sizeof(s->surfaces[i].id), "%s", id);
			return 1;
		}
		if (!strcmp(key + n, ".rows") || !strcmp(key + n, ".percent")) {
			if (!json_object_is_type(value, json_type_int)) {
				return 0;
			}
			int count = json_object_get_int(value), percent = !strcmp(key + n, ".percent");
			if (count < 0 || count > (percent ? 95 : 512)) {
				return 0;
			}
			if (percent) {
				s->surfaces[i].percent = count;
			} else {
				s->surfaces[i].rows = count;
			}
			return 1;
		}
	}
	if (!strcmp(key, "board.header.visible") || !strcmp(key, "board.header.border") ||
		!strcmp(key, "board.footer.visible") || !strcmp(key, "board.footer.border") ||
		!strcmp(key, "board.input.visible")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		int n = json_object_get_boolean(value);
		if (!strcmp(key, "board.header.visible")) {
			s->board_header_visible = n;
		} else if (!strcmp(key, "board.header.border")) {
			s->board_header_border = n;
		} else if (!strcmp(key, "board.footer.visible")) {
			s->board_footer_visible = n;
		} else if (!strcmp(key, "board.footer.border")) {
			s->board_footer_border = n;
		} else {
			s->board_input_visible = n;
		}
		return 1;
	}
	if (!strcmp(key, "board.input.placeholder")) {
		return skin_string(
			value, s->board_input_placeholder, sizeof(s->board_input_placeholder), 0, 240);
	}
	if (!strcmp(key, "board.header.rows") || !strcmp(key, "board.footer.rows")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int rows = json_object_get_int(value);
		if (rows < 0 || rows > 64) {
			return 0;
		}
		if (!strcmp(key, "board.header.rows")) {
			s->board_header_rows = rows;
		} else {
			s->board_footer_rows = rows;
		}
		return 1;
	}
	if (!strcmp(key, "composer.elements") || !strcmp(key, "board.header.elements") ||
		!strcmp(key, "board.footer.elements")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 16) {
			return 0;
		}
		struct chrome_elements parsed = {0};
		parsed.count = (int)json_object_array_length(value);
		for (int i = 0; i < parsed.count; i++) {
			json_object *item = json_object_array_get_idx(value, (size_t)i);
			struct chrome_element *e = &parsed.items[i];
			if (!json_object_is_type(item, json_type_object)) {
				return 0;
			}
			json_object_object_foreach(item, k, v) {
				if (!strcmp(k, "id")) {
					if (!skin_string(v, e->id, sizeof(e->id), 0, 60)) {
						return 0;
					}
				} else if (!strcmp(k, "text")) {
					if (!skin_string(v, e->text, sizeof(e->text), 0, 240)) {
						return 0;
					}
				} else if (!strcmp(k, "glyph")) {
					if (!skin_string(v, e->glyph, sizeof(e->glyph), 0, 1)) {
						return 0;
					}
				} else if (!strcmp(k, "anchor")) {
					if (!json_object_is_type(v, json_type_string)) {
						return 0;
					}
					const char *names[] = {
						"top-left", "top-right", "bottom-left", "bottom-right", "center"};
					int found = 0;
					for (int a = 0; a < 5; a++) {
						if (!strcmp(json_object_get_string(v), names[a])) {
							e->anchor = a;
							found = 1;
						}
					}
					if (!found) {
						return 0;
					}
				} else if (!strcmp(k, "x") || !strcmp(k, "y") || !strcmp(k, "width")) {
					if (!json_object_is_type(v, json_type_int)) {
						return 0;
					}
					int n = json_object_get_int(v);
					if (n < (!strcmp(k, "width") ? 0 : -4096) || n > 4096) {
						return 0;
					}
					if (!strcmp(k, "x")) {
						e->x = n;
					} else if (!strcmp(k, "y")) {
						e->y = n;
					} else {
						e->width = n;
					}
				} else if (!strcmp(k, "style")) {
					if (!json_object_is_type(v, json_type_string)) {
						return 0;
					}
					const char *names[] = {"normal", "muted", "accent", "bold"};
					int found = 0;
					for (int a = 0; a < 4; a++) {
						if (!strcmp(json_object_get_string(v), names[a])) {
							e->style = a;
							found = 1;
						}
					}
					if (!found) {
						return 0;
					}
				} else {
					return 0;
				}
			}
		}
		if (!strcmp(key, "composer.elements")) {
			s->composer_elements = parsed;
		} else if (!strcmp(key, "board.header.elements")) {
			s->board_header_elements = parsed;
		} else {
			s->board_footer_elements = parsed;
		}
		return 1;
	}
	if (!strcmp(key, "board.layout")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *layout = json_object_get_string(value);
		if (strcmp(layout, "rows") && strcmp(layout, "cards")) {
			return 0;
		}
		s->board_cards = !strcmp(layout, "cards");
		return 1;
	}
	if (!strcmp(key, "board.card.border")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *border = json_object_get_string(value);
		if (strcmp(border, "none") && strcmp(border, "line")) {
			return 0;
		}
		s->board_card_border = !strcmp(border, "line");
		return 1;
	}
	if (!strcmp(key, "board.summary") || !strcmp(key, "board.instructions") ||
		!strcmp(key, "board.headings") || !strcmp(key, "board.preview") ||
		!strcmp(key, "board.age") || !strcmp(key, "board.place")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		int enabled = json_object_get_boolean(value);
		if (!strcmp(key, "board.summary")) {
			s->board_summary = enabled;
		} else if (!strcmp(key, "board.instructions")) {
			s->board_instructions = enabled;
		} else if (!strcmp(key, "board.headings")) {
			s->board_headings = enabled;
		} else if (!strcmp(key, "board.preview")) {
			s->board_preview = enabled;
		} else if (!strcmp(key, "board.age")) {
			s->board_age = enabled;
		} else {
			s->board_place = enabled;
		}
		return 1;
	}
	if (!strcmp(key, "board.spacing") || !strcmp(key, "board.padding") ||
		!strcmp(key, "board.margin") || !strcmp(key, "transcript.max_width") ||
		!strcmp(key, "transcript.padding") || !strcmp(key, "composer.max_percent") ||
		!strcmp(key, "footer.rows")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int n = json_object_get_int(value);
		if (!strcmp(key, "composer.max_percent")) {
			if (n < 10 || n > 95) {
				return 0;
			}
			s->composer_max_percent = n;
		} else if (!strcmp(key, "transcript.max_width")) {
			if (n && (n < 20 || n > 4096)) {
				return 0;
			}
			s->transcript_max_width = n;
		} else if (!strcmp(key, "transcript.padding")) {
			if (n < 0 || n > 32) {
				return 0;
			}
			s->transcript_padding = n;
		} else if (!strcmp(key, "footer.rows")) {
			if (n < 1 || n > 8) {
				return 0;
			}
			s->footer_rows = n;
		} else {
			if (n < 0 || n > 16) {
				return 0;
			}
			if (!strcmp(key, "board.spacing")) {
				s->board_gap = n;
			} else if (!strcmp(key, "board.padding")) {
				s->board_padding = n;
			} else {
				s->board_margin = n;
			}
		}
		return 1;
	}
	if (!strcmp(key, "footer.lines") || !strcmp(key, "board.card.lines")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 8) {
			return 0;
		}
		int board = !strcmp(key, "board.card.lines");
		char(*lines)[256] = board ? s->board_card_lines : s->footer_lines;
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			if (!skin_string(
					json_object_array_get_idx(value, i), lines[i], sizeof(lines[i]), 0, 240)) {
				return 0;
			}
		}
		if (board) {
			s->board_card_line_count = (int)json_object_array_length(value);
		} else {
			s->footer_line_count = (int)json_object_array_length(value);
		}
		return 1;
	}
	if (!strcmp(key, "composer.min_rows") || !strcmp(key, "composer.max_rows") ||
		!strcmp(key, "composer.padding")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int n = json_object_get_int(value), padding = !strcmp(key, "composer.padding");
		if (n < (padding ? 0 : 1) || n > (padding ? 8 : 4096)) {
			return 0;
		}
		if (padding) {
			s->composer_padding = n;
		} else if (!strcmp(key, "composer.min_rows")) {
			s->composer_min_rows = n;
		} else {
			s->composer_max_rows = n;
		}
		return 1;
	}
	if (!strcmp(key, "composer.border")) {
		key = "prompt.border";
	}
	if (!strcmp(key, "composer.prompt")) {
		key = "glyph.prompt";
	}
	if (!strcmp(key, "glyph.card")) {
		return skin_string(value, s->card, sizeof(s->card), 0, 2); /* "": no glyph */
	}
	if (!strcmp(key, "glyph.reply")) {
		return skin_string(value, s->reply, sizeof(s->reply), 0, 2);
	}
	if (!strcmp(key, "glyph.output")) {
		return skin_string(value, s->output, sizeof(s->output), 0, 3);
	}
	if (!strcmp(key, "glyph.fold")) {
		return skin_string(value, s->fold, sizeof(s->fold), 0, 2);
	}
	if (!strcmp(key, "glyph.prompt")) {
		return skin_string(value, s->prompt, sizeof(s->prompt), 0, 1); /* "": no marker */
	}
	if (!strcmp(key, "glyph.done")) {
		return skin_string(value, s->done_glyph, sizeof(s->done_glyph), 0, 2);
	}
	if (!strcmp(key, "markdown.bullet")) {
		return skin_string(value, s->bullet, sizeof(s->bullet), 1, 2);
	}
	if (!strcmp(key, "markdown.table.rules")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *mode = json_object_get_string(value);
		if (strlen(mode) != (size_t)json_object_get_string_len(value)) {
			return 0;
		}
		if (!strcmp(mode, "header")) {
			s->table_rules = TABLE_RULES_HEADER;
		} else if (!strcmp(mode, "rows")) {
			s->table_rules = TABLE_RULES_ROWS;
		} else if (!strcmp(mode, "none")) {
			s->table_rules = TABLE_RULES_NONE;
		} else {
			return 0;
		}
		return 1;
	}
	if (!strcmp(key, "turn.clock")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		s->done_clock = json_object_get_boolean(value);
		return 1;
	}
	if (!strcmp(key, "turn.done")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 8) {
			return 0;
		}
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			if (!skin_string(
					json_object_array_get_idx(value, i), s->done[i], sizeof(s->done[i]), 1, 20)) {
				return 0;
			}
		}
		s->done_count = (int)json_object_array_length(value);
		return 1;
	}
	if (!strcmp(key, "notice.style")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *mode = json_object_get_string(value);
		/* Retired key: accept old files without retaining presentation modes. */
		return !strcmp(mode, "plain") || !strcmp(mode, "compact") || !strcmp(mode, "card") ||
			!strcmp(mode, "toast");
	}
	if (!strcmp(key, "tool.live")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *mode = json_object_get_string(value);
		if (!strcmp(mode, "cards")) {
			s->tool_live = 0;
		} else if (!strcmp(mode, "status")) {
			s->tool_live = 1;
		} else if (!strcmp(mode, "grouped")) {
			s->tool_live = 2;
		} else {
			return 0;
		}
		return 1;
	}
	if (!strcmp(key, "tool.progress")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *mode = json_object_get_string(value);
		if (!strcmp(mode, "below")) {
			s->tool_progress = 0;
		} else if (!strcmp(mode, "replace")) {
			s->tool_progress = 1;
		} else {
			return 0;
		}
		return 1;
	}
	if (!strcmp(key, "tool.live.command")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		s->tool_live_command = json_object_get_boolean(value);
		return 1;
	}
	if (!strcmp(key, "message.renderer") || !strncmp(key, "message.renderer.", 17)) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *id = json_object_get_string(value);
		if ((size_t)json_object_get_string_len(value) != strlen(id) || strlen(id) >= 64 ||
			(*id && !phi_layout_identifier(id))) {
			return 0;
		}
		if (!strcmp(key, "message.renderer")) {
			snprintf(s->message_renderer, sizeof(s->message_renderer), "%s", id);
		} else {
			const char *names[] = {"user", "assistant", "thinking"};
			int kind = 0;
			while (kind < SEM_MESSAGE_KINDS && strcmp(key + 17, names[kind])) {
				kind++;
			}
			if (kind == SEM_MESSAGE_KINDS) {
				return 0;
			}
			snprintf(s->message_renderers[kind], sizeof(s->message_renderers[kind]), "%s", id);
			s->message_renderer_set[kind] = 1;
		}
		return 1;
	}
	if (!strcmp(key, "tool.live.renderer") || !strncmp(key, "tool.live.renderer.", 19)) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *id = json_object_get_string(value);
		if ((size_t)json_object_get_string_len(value) != strlen(id) || strlen(id) >= 64 ||
			(*id && !phi_layout_identifier(id))) {
			return 0;
		}
		if (!strcmp(key, "tool.live.renderer")) {
			snprintf(s->tool_live_renderer, sizeof(s->tool_live_renderer), "%s", id);
		} else {
			struct skin_tool *tool = skin_tool_slot(s, key + 19);
			if (!tool) {
				return 0;
			}
			snprintf(tool->live_renderer, sizeof(tool->live_renderer), "%s", id);
			tool->live_renderer_set = 1;
		}
		return 1;
	}
	if (!strcmp(key, "tool.renderer") || !strncmp(key, "tool.renderer.", 14)) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *id = json_object_get_string(value);
		if ((size_t)json_object_get_string_len(value) != strlen(id) || strlen(id) >= 64 ||
			(*id && !phi_layout_identifier(id))) {
			return 0;
		}
		if (!strcmp(key, "tool.renderer")) {
			snprintf(s->tool_renderer, sizeof(s->tool_renderer), "%s", id);
		} else {
			struct skin_tool *tool = skin_tool_slot(s, key + 14);
			if (!tool) {
				return 0;
			}
			snprintf(tool->renderer, sizeof(tool->renderer), "%s", id);
			tool->renderer_set = 1;
		}
		return 1;
	}
	if (!strcmp(key, "tool.title")) {
		return skin_string(value, s->tool_title, sizeof(s->tool_title), 0, 60);
	}
	if (!strncmp(key, "tool.title.", 11)) {
		/* One tool's heading, e.g. "tool.title.bash": "$ {detail}". */
		struct skin_tool *tool = skin_tool_slot(s, key + 11);
		if (!tool || !skin_string(value, tool->title, sizeof(tool->title), 0, 60)) {
			return 0;
		}
		tool->title_set = 1;
		return 1;
	}
	if (!strcmp(key, "tool.hide_output")) {
		/* ["read"]: those tools' cards show only their heading. */
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 8) {
			return 0;
		}
		for (int i = 0; i < s->tool_count; i++) {
			s->tools[i].hidden = 0;
		}
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			json_object *name = json_object_array_get_idx(value, i);
			struct skin_tool *tool = json_object_is_type(name, json_type_string)
				? skin_tool_slot(s, json_object_get_string(name))
				: NULL;
			if (!tool) {
				return 0;
			}
			tool->hidden = 1;
		}
		return 1;
	}
	if (!strncmp(key, "tool.max_lines.", 15)) {
		/* One tool's cap, e.g. "tool.max_lines.bash": 5. */
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int64_t lines = json_object_get_int64(value);
		struct skin_tool *tool = lines >= 0 && lines <= 500 ? skin_tool_slot(s, key + 15) : NULL;
		if (!tool) {
			return 0;
		}
		tool->max_lines = (int)lines;
		tool->max_lines_set = 1;
		return 1;
	}
	if (!strcmp(key, "tool.duration") || !strncmp(key, "tool.duration.", 14)) {
		/* true: after the heading; "footer": "Took 0.4s" as the card's
           last row, as pi does; false: none. ".NAME": one tool's. */
		int duration;
		if (json_object_is_type(value, json_type_boolean)) {
			duration = json_object_get_boolean(value) ? TOOL_DURATION_HEADING : TOOL_DURATION_OFF;
		} else if (json_object_is_type(value, json_type_string) &&
			!strcmp(json_object_get_string(value), "footer")) {
			duration = TOOL_DURATION_FOOTER;
		} else {
			return 0;
		}
		if (key[13] == 0) {
			s->tool_duration = duration;
			return 1;
		}
		struct skin_tool *tool = skin_tool_slot(s, key + 14);
		if (!tool) {
			return 0;
		}
		tool->duration = duration;
		tool->duration_set = 1;
		return 1;
	}
	if (!strcmp(key, "tool.expanded") || !strcmp(key, "layout.compact")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		if (!strcmp(key, "layout.compact")) {
			s->compact_layout = json_object_get_boolean(value);
		} else {
			s->tool_expanded = json_object_get_boolean(value);
		}
		return 1;
	}

	struct {
		const char *key;
		int *field, min, max;
	} numbers[] = {{"tool.padding", &s->tool_padding, 0, 4},
		{"tool.padding_y", &s->tool_padding_y, 0, 2}, {"user.padding_y", &s->user_padding_y, 0, 2},
		{"tool.indent", &s->tool_indent, 0, 8}, {"tool.heading_gap", &s->tool_heading_gap, 0, 2},
		{"tool.max_lines", &s->tool_max_lines, 0, 500}, {"tool.spacing", &s->tool_spacing, 0, 3},
		{"tool.max_width", &s->tool_max_width, 0, 400},
		{"diff.gutter_width", &s->diff_gutter_width, 1, 8}};

	for (size_t i = 0; i < sizeof(numbers) / sizeof(*numbers); i++) {
		if (!strcmp(key, numbers[i].key)) {
			if (!json_object_is_type(value, json_type_int)) {
				return 0;
			}
			int64_t n = json_object_get_int64(value);
			if (n < numbers[i].min || n > numbers[i].max ||
				(!strcmp(key, "tool.max_width") && n && n < 20)) {
				return 0;
			}
			*numbers[i].field = (int)n;
			return 1;
		}
	}

	struct {
		const char *key;
		int *field;
	} booleans[] = {{"thinking.visible", &s->thinking_visible},
		{"tool.change_counts", &s->tool_change_counts},
		{"tool.heading_bold", &s->tool_heading_bold}, {"diff.line_numbers", &s->diff_line_numbers},
		{"tool.line_numbers", &s->tool_line_numbers}, {"tool.captions", &s->tool_captions},
		{"tool.heading_wrap", &s->tool_heading_wrap}, {"palette.exact", &s->palette_exact}};

	for (size_t i = 0; i < sizeof(booleans) / sizeof(*booleans); i++) {
		if (!strcmp(key, booleans[i].key)) {
			if (!json_object_is_type(value, json_type_boolean)) {
				return 0;
			}
			*booleans[i].field = json_object_get_boolean(value);
			return 1;
		}
	}

	struct {
		const char *key;
		int *field;
		const char *values[3];
	} choices[] = {{"tool.border", &s->tool_border, {"none", "left", "rounded"}},
		{"tool.path", &s->tool_path, {"full", "relative", "basename"}},
		{"diff.style", &s->diff_style, {"background", "text", "markers"}},
		{"diff.saved_label", &s->diff_saved_label, {"full", "compact", "none"}},
		{"diff.sign", &s->diff_sign, {"after", "before", NULL}},
		{"tool.detail", &s->tool_detail, {"muted", "title", NULL}}};

	for (size_t i = 0; i < sizeof(choices) / sizeof(*choices); i++) {
		if (!strcmp(key, choices[i].key)) {
			if (!json_object_is_type(value, json_type_string)) {
				return 0;
			}
			const char *text = json_object_get_string(value);
			if ((size_t)json_object_get_string_len(value) != strlen(text)) {
				return 0;
			}
			for (int j = 0; j < 3; j++) {
				if (choices[i].values[j] && !strcmp(text, choices[i].values[j])) {
					*choices[i].field = j;
					return 1;
				}
			}
			return 0;
		}
	}
	if (!strcmp(key, "welcome.scene")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 16) {
			return 0;
		}
		int count = (int)json_object_array_length(value);
		struct welcome_element *scene = count ? calloc((size_t)count, sizeof(*scene)) : NULL;
		if (count && !scene) {
			return 0;
		}
		for (size_t i = 0; i < (size_t)count; i++) {
			json_object *obj = json_object_array_get_idx(value, i);
			if (!json_object_is_type(obj, json_type_object)) {
				goto scene_invalid;
			}
			struct welcome_element *e = &scene[i];
			e->interval = 150;
			e->color = 27;
			json_object_object_foreach(obj, name, v) {
				if (!strcmp(name, "x") || !strcmp(name, "y") || !strcmp(name, "interval")) {
					if (!json_object_is_type(v, json_type_int)) {
						goto scene_invalid;
					}
					int n = json_object_get_int(v);
					if (!strcmp(name, "interval")) {
						if (n < 50 || n > 10000) {
							goto scene_invalid;
						}
						e->interval = n;
					} else {
						if (n < -500 || n > 500) {
							goto scene_invalid;
						}
						if (name[0] == 'x') {
							e->x = n;
						} else {
							e->y = n;
						}
					}
				} else if (!strcmp(name, "bold")) {
					if (!json_object_is_type(v, json_type_boolean)) {
						goto scene_invalid;
					}
					e->bold = json_object_get_boolean(v);
				} else if (!strcmp(name, "anchor") || !strcmp(name, "color")) {
					if (!json_object_is_type(v, json_type_string)) {
						goto scene_invalid;
					}
					const char *n = json_object_get_string(v);
					if (!strcmp(name, "anchor")) {
						if (!strcmp(n, "top-left")) {
							e->anchor = 0;
						} else if (!strcmp(n, "center")) {
							e->anchor = 1;
						} else if (!strcmp(n, "bottom-right")) {
							e->anchor = 2;
						} else {
							goto scene_invalid;
						}
					} else {
						if (!strcmp(n, "text")) {
							e->color = 27;
						} else if (!strcmp(n, "accent")) {
							e->color = 28;
						} else if (!strcmp(n, "muted")) {
							e->color = 3;
						} else if (!strcmp(n, "error")) {
							e->color = 10;
						} else {
							goto scene_invalid;
						}
					}
				} else if (!strcmp(name, "text") || !strcmp(name, "frames")) {
					if (e->frame_count) {
						goto scene_invalid;
					}
					int array = !strcmp(name, "frames");
					if (array &&
						(!json_object_is_type(v, json_type_array) || !json_object_array_length(v) ||
							json_object_array_length(v) > 8)) {
						goto scene_invalid;
					}
					e->frame_count = array ? (int)json_object_array_length(v) : 1;
					e->frames = calloc((size_t)e->frame_count, sizeof(*e->frames));
					if (!e->frames) {
						goto scene_invalid;
					}
					for (int j = 0; j < e->frame_count; j++) {
						json_object *f = array ? json_object_array_get_idx(v, j) : v;
						if (!json_object_is_type(f, json_type_string) ||
							json_object_get_string_len(f) >= 1024) {
							goto scene_invalid;
						}
						const char *t = json_object_get_string(f);
						for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
							if (*p < 32 && *p != '\n') {
								goto scene_invalid;
							}
						}
						if ((size_t)json_object_get_string_len(f) != strlen(t)) {
							goto scene_invalid;
						}
						e->frames[j] = strdup(t);
						if (!e->frames[j]) {
							goto scene_invalid;
						}
					}
				} else {
					goto scene_invalid;
				}
			}
			if (!e->frame_count) {
				goto scene_invalid;
			}
		}
		skin_scene_release(s);
		s->scene = scene;
		s->scene_count = count;
		return 1;
	scene_invalid:
		welcome_elements_free(scene, count);
		return 0;
	}
	if (!strcmp(key, "welcome.life")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		s->welcome_life = json_object_get_boolean(value);
		return 1;
	}
	if (!strcmp(key, "welcome.message.padding")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int padding = json_object_get_int(value);
		if (padding < 0 || padding > 100) {
			return 0;
		}
		s->welcome_message_padding = padding;
		return 1;
	}
	if (!strcmp(key, "welcome.message_row")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int row = json_object_get_int(value);
		if (row < 0 || row > 500) {
			return 0;
		}
		s->welcome_message_row = row;
		return 1;
	}
	if (!strcmp(key, "welcome.fields")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 8) {
			return 0;
		}
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			json_object *v = json_object_array_get_idx(value, i);
			if (!skin_string(v, s->welcome_fields[i], sizeof(s->welcome_fields[i]), 1, 15)) {
				return 0;
			}
			const char *field = s->welcome_fields[i];
			if (strcmp(field, "model") && strcmp(field, "effort") && strcmp(field, "path") &&
				strcmp(field, "theme") && strcmp(field, "memory") && strcmp(field, "version")) {
				return 0;
			}
		}
		s->welcome_field_count = (int)json_object_array_length(value);
		return 1;
	}
	if (!strcmp(key, "welcome.title")) {
		return skin_string(value, s->welcome_title, sizeof(s->welcome_title), 0, 60);
	}
	if (!strcmp(key, "spinner.frames")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		wchar_t frames[SKIN_FRAMES + 2];
		size_t count = mbstowcs(frames, json_object_get_string(value), SKIN_FRAMES + 1);
		if (count == (size_t)-1 || count < 1 || count > SKIN_FRAMES) {
			return 0;
		}
		for (size_t i = 0; i < count; i++) {
			if (wcwidth(frames[i]) != 1) {
				return 0;
			}
		}
		wmemcpy(s->frames, frames, count);
		s->frames[count] = 0;
		s->frame_count = (int)count;
		return 1;
	}
	if (!strcmp(key, "spinner.interval")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int64_t ms = json_object_get_int64(value);
		if (ms < 40 || ms > 1000) {
			return 0;
		}
		s->frame_ms = (int)ms;
		return 1;
	}
	if (!strcmp(key, "spinner.words") || !strcmp(key, "welcome.lines")) {
		int words = key[0] == 's';
		size_t limit = words ? SKIN_WORDS : 3,
			   size = words ? sizeof(s->words[0]) : sizeof(s->welcome_lines[0]);
		if (!json_object_is_type(value, json_type_array)) {
			return 0;
		}
		size_t count = json_object_array_length(value);
		if (count > limit || (words && !count)) {
			return 0;
		}
		for (size_t i = 0; i < count; i++) {
			if (!skin_string(json_object_array_get_idx(value, i),
					words ? s->words[i] : s->welcome_lines[i], size, words, 120)) {
				return 0;
			}
		}
		if (words) {
			s->word_count = (int)count;
		} else {
			s->welcome_line_count = (int)count;
		}
		return 1;
	}
	if (!strcmp(key, "prompt.border") || !strcmp(key, "approval.style") ||
		!strcmp(key, "welcome")) {
		if (!json_object_is_type(value, json_type_string)) {
			return 0;
		}
		const char *text = json_object_get_string(value);
		if (key[0] == 'p') {
			s->border = !strcmp(text, "line") ? SKIN_BORDER_LINE
				: !strcmp(text, "rounded")    ? SKIN_BORDER_ROUNDED
				: !strcmp(text, "none")       ? SKIN_BORDER_NONE
				: !strcmp(text, "heavy")      ? SKIN_BORDER_HEAVY
											  : -1;
			return s->border >= 0;
		}
		if (key[0] == 'a') {
			s->approval_menu = !strcmp(text, "menu") ? 1 : !strcmp(text, "line") ? 0 : -1;
			return s->approval_menu >= 0;
		}
		s->welcome = !strcmp(text, "phi")                          ? SKIN_WELCOME_PHI
			: !strcmp(text, "none")                                ? SKIN_WELCOME_NONE
			: !strcmp(text, "fetch")                               ? SKIN_WELCOME_FETCH
			: !strcmp(text, "scene")                               ? SKIN_WELCOME_SCENE
			: (!strcmp(text, "cinema") || !strcmp(text, "widget")) ? SKIN_WELCOME_CINEMA
																   : -1;
		return s->welcome >= 0;
	}
	if (!strcmp(key, "welcome.art.intro")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 6) {
			return 0;
		}
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			json_object *item = json_object_array_get_idx(value, i);
			if (!json_object_is_type(item, json_type_int)) {
				return 0;
			}
			int64_t index = json_object_get_int64(item);
			if (index < 1 || index > 6) {
				return 0;
			}
			s->art_intro[i] = (int)index;
		}
		s->art_intro_count = (int)json_object_array_length(value);
		return 1;
	}
	if (!strcmp(key, "welcome.art.gestures")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 6) {
			return 0;
		}
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			json_object *entry = json_object_array_get_idx(value, i);
			json_object *frames = json_object_object_get(entry, "frames");
			json_object *interval = json_object_object_get(entry, "interval");
			if (!json_object_is_type(entry, json_type_object) ||
				!json_object_is_type(frames, json_type_array) ||
				!json_object_array_length(frames) || json_object_array_length(frames) > 3) {
				return 0;
			}
			struct mascot_gesture *g = &s->art_gestures[i];
			memset(g, 0, sizeof(*g));
			g->interval = 220;
			if (interval) {
				if (!json_object_is_type(interval, json_type_int)) {
					return 0;
				}
				int64_t ms = json_object_get_int64(interval);
				if (ms < 80 || ms > 1000) {
					return 0;
				}
				g->interval = (int)ms;
			}
			g->count = (int)json_object_array_length(frames);
			for (int f = 0; f < g->count; f++) {
				json_object *rows = json_object_array_get_idx(frames, f);
				if (!json_object_is_type(rows, json_type_array) ||
					!json_object_array_length(rows) || json_object_array_length(rows) > 4) {
					return 0;
				}
				g->rows[f] = (int)json_object_array_length(rows);
				for (int r = 0; r < g->rows[f]; r++) {
					if (!skin_string(json_object_array_get_idx(rows, r), g->frames[f][r],
							sizeof(g->frames[f][r]), 0, 24)) {
						return 0;
					}
				}
			}
		}
		s->art_gesture_count = (int)json_object_array_length(value);
		return 1;
	}
	if (!strcmp(key, "welcome.art") || !strcmp(key, "welcome.art.blink")) {
		int blink = !strcmp(key, "welcome.art.blink");
		char(*art)[96] = blink ? s->art_blink : s->art;
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 4) {
			return 0;
		}
		int widest = 0;
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			if (!skin_string(json_object_array_get_idx(value, i), art[i], sizeof(art[i]), 0, 24)) {
				return 0;
			}
			int width = skin_width(art[i]);
			if (width > widest) {
				widest = width;
			}
		}
		if (blink) {
			s->art_blink_count = (int)json_object_array_length(value);
		} else {
			s->art_count = (int)json_object_array_length(value);
			s->art_width = widest;
		}
		return 1;
	}
	if (!strcmp(key, "spinner.shimmer") || !strcmp(key, "spinner.stats")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		*(key[8] == 's' && key[9] == 'h' ? &s->shimmer : &s->stats) =
			json_object_get_boolean(value);
		return 1;
	}
	if (!strcmp(key, "spinner.rotate")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int64_t seconds = json_object_get_int64(value);
		if (seconds < 0 || seconds > 600) {
			return 0;
		}
		s->rotate = (int)seconds;
		return 1;
	}
	if (!strcmp(key, "spinner.tips")) {
		if (!json_object_is_type(value, json_type_array) || json_object_array_length(value) > 8) {
			return 0;
		}
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			if (!skin_string(
					json_object_array_get_idx(value, i), s->tips[i], sizeof(s->tips[i]), 1, 110)) {
				return 0;
			}
		}
		s->tip_count = (int)json_object_array_length(value);
		return 1;
	}
	if (!strcmp(key, "user.bold")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		s->user_bold = json_object_get_boolean(value);
		return 1;
	}
	if (!strcmp(key, "user.glyph")) {
		const char *text =
			json_object_is_type(value, json_type_string) ? json_object_get_string(value) : "";
		s->user_glyph = !strcmp(text, "accent") ? USER_GLYPH_ACCENT
			: !strcmp(text, "muted")            ? USER_GLYPH_MUTED
			: !strcmp(text, "text")             ? USER_GLYPH_TEXT
			: !strcmp(text, "none")             ? USER_GLYPH_NONE
												: -1;
		return s->user_glyph >= 0;
	}
	if (!strcmp(key, "layout.margin")) {
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int64_t columns = json_object_get_int64(value);
		if (columns < 0 || columns > 4) {
			return 0;
		}
		s->margin = (int)columns;
		return 1;
	}
	if (!strcmp(key, "scroll.lines")) {
		if (json_object_is_type(value, json_type_string)) {
			return !strcmp(json_object_get_string(value), "auto") && !(s->scroll_lines = 0);
		}
		if (!json_object_is_type(value, json_type_int)) {
			return 0;
		}
		int64_t lines = json_object_get_int64(value);
		if (lines < 1 || lines > 10) {
			return 0;
		}
		s->scroll_lines = (int)lines;
		return 1;
	}
	if (!strcmp(key, "mascot")) {
		if (!json_object_is_type(value, json_type_boolean)) {
			return 0;
		}
		s->mascot = json_object_get_boolean(value);
		return 1;
	}

	return 0;
}

/* Apply a theme object's settings over colors and skin. 0 and an error on a bad key. */
static int theme_parse(
	json_object *object, short *colors, struct skin *s, char *error, size_t error_size) {

	int saved_hex_count = hex_count;
	json_object_object_foreach(object, key, value) {
		if (strchr(key, '.') || !strcmp(key, "welcome") || !strcmp(key, "mascot")) {
			if (!skin_parse(key, value, s)) {
				snprintf(error, error_size, "invalid theme setting: %.80s", key);
				hex_count = saved_hex_count;
				return 0;
			}
			continue;
		}
		size_t i = 0;
		while (i < THEME_COUNT && strcmp(key, theme_defaults[i].name)) {
			i++;
		}
		if (i == THEME_COUNT || !theme_color(value, &colors[i])) {
			snprintf(error, error_size, "invalid theme setting: %.80s", key);
			hex_count = saved_hex_count;
			return 0;
		}
	}

	return 1;
}

static int theme_extending, theme_selecting, theme_named_depth;
static int theme_read_path(
	const char *path, short *colors, struct skin *s, char *error, size_t error_size);

/* Where named themes live: the user's ~/.config/phi/themes first, then the
   themes/ shipped beside the Phi binary. Returns how many were found. */
static int theme_directories(char directories[2][4096]) {

	int count =
		phi_xdg_path(directories[0], 4096, "XDG_CONFIG_HOME", ".config", "phi/themes") ? 1 : 0;
	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n > 0) {
		self[n] = 0;
		char *slash = strrchr(self, '/');
		size_t folder = slash ? (size_t)(slash - self) : 0;
		if (slash && folder + sizeof("/themes") <= 4096) {
			memcpy(directories[count], self, folder);
			memcpy(directories[count] + folder, "/themes", sizeof("/themes"));
			if (!count || strcmp(directories[0], directories[count])) {
				count++;
			}
		}
	}

	return count;
}

static int theme_name_valid(const char *name) {

	return name && *name && name[0] != '.' && !strchr(name, '/') && strlen(name) <= 63;
}

/* Resolve a named definition, with user files taking precedence. Missing
   default is the built-in baseline, but its future file is watched too. */
static inline int theme_named_path(const char *name, char *path, size_t size) {

	if (!theme_name_valid(name)) {
		return 0;
	}
	char directories[2][4096];
	int count = theme_directories(directories);
	for (int i = 0; i < count; i++) {
		int n = snprintf(path, size, "%s/%s.json", directories[i], name);
		if (n < 0 || (size_t)n >= size) {
			return 0;
		}
		struct stat st;
		if (!stat(path, &st) || errno != ENOENT) {
			return 1;
		}
	}

	return 0;
}

/* Read a named theme over the defaults: 1 loaded, 0 no such theme, -1 invalid. */
static int theme_read_named(
	const char *name, short *colors, struct skin *s, char *error, size_t error_size) {

	if (!*name || strchr(name, '/') || name[0] == '.' || strlen(name) > 63) {
		return 0;
	}
	char directories[2][4096], path[4096 + 72];
	int count = theme_directories(directories);
	for (int i = 0; i < count; i++) {
		size_t folder = strnlen(directories[i], 4095);
		memcpy(path, directories[i], folder);
		snprintf(path + folder, sizeof(path) - folder, "/%.63s.json", name);
		theme_named_depth++;
		int status = theme_read_path(path, colors, s, error, error_size);
		theme_named_depth--;
		if (status) {
			return status; /* Named compositions inherit the neutral baseline too. */
		}
	}

	return 0;
}

enum {
	THEME_FILE_BYTES = 32 * 1024
};

/* Syntax/shape only; resolving extends and applying settings are separate.
   text is NUL-terminated; len excludes that terminator. *out is owned. */
static int theme_decode_config(
	const char *text, size_t len, json_object **out, char *error, size_t error_size) {

	*out = NULL;
	if (len > THEME_FILE_BYTES) {
		snprintf(error, error_size, "theme must fit within 32 KiB");
		return -1;
	}
	json_tokener *parser = json_tokener_new();
	if (!parser) {
		snprintf(error, error_size, "cannot allocate JSON parser");
		return -1;
	}
	json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
	json_object *object = json_tokener_parse_ex(parser, text, (int)len + 1);
	int valid = json_tokener_get_error(parser) == json_tokener_success &&
		json_object_is_type(object, json_type_object);
	json_tokener_free(parser);
	if (!valid) {
		if (object) {
			json_object_put(object);
		}
		snprintf(error, error_size, "theme must be a valid JSON object");
		return -1;
	}
	*out = object;

	return 1;
}

/* 1: owned object in *out, 0: absent, -1: error. All callers share the
   regular-file policy, byte limit and strict decoder. */
static int theme_read_config(const char *path, json_object **out, char *error, size_t error_size) {

	*out = NULL;
	size_t len = 0;
	char *text = phi_read_file(path, THEME_FILE_BYTES, PHI_READ_FOLLOW, &len);
	if (!text) {
		if (errno == ENOENT) {
			return 0;
		}
		if (errno == EFBIG) {
			snprintf(error, error_size, "theme must be a regular JSON file up to 32 KiB");
		} else if (errno == EINVAL) {
			snprintf(error, error_size, "theme must be a regular JSON file without NUL bytes");
		} else {
			snprintf(error, error_size, "cannot read theme: %s", strerror(errno));
		}
		return -1;
	}
	int status = theme_decode_config(text, len, out, error, error_size);
	free(text);

	return status;
}

/* Read a theme file over the defaults. 1: loaded, 0: absent, -1: invalid. */
static int theme_read_path(
	const char *path, short *colors, struct skin *s, char *error, size_t error_size) {

	for (size_t i = 0; i < THEME_COUNT; i++) {
		colors[i] = theme_defaults[i].value;
	}
	*s = skin_defaults;
	json_object *object = NULL;
	int config_status = theme_read_config(path, &object, error, error_size);
	if (config_status <= 0) {
		return config_status;
	}
	int saved_hex_count = hex_count;
	/* Selection files contain a name only, never copied presentation data. */
	json_object *selection = NULL;
	if (json_object_object_get_ex(object, "theme", &selection)) {
		const char *name = json_object_is_type(selection, json_type_string)
			? json_object_get_string(selection)
			: "";
		int valid = json_object_object_length(object) == 1 && theme_name_valid(name) &&
			strlen(name) == (size_t)json_object_get_string_len(selection) && !theme_selecting &&
			!theme_extending && !theme_named_depth;
		int status = -1;
		if (valid) {
			theme_selecting = 1;
			status = theme_read_named(name, colors, s, error, error_size);
			theme_selecting = 0;
			if (!status && !strcmp(name, "default")) {
				status = 1;
			}
		}
		if (status <= 0) {
			if (!valid || !status) {
				snprintf(error, error_size, "invalid or missing theme selection: %.63s", name);
			}
			hex_count = saved_hex_count;
			skin_release(s);
		}
		json_object_put(object);
		return status > 0 ? 1 : -1;
	}
	/* "extends": a named theme (themes/NAME.json) read first, one level. */
	json_object *base = NULL;
	if (json_object_object_get_ex(object, "extends", &base)) {
		const char *name =
			json_object_is_type(base, json_type_string) ? json_object_get_string(base) : "";
		int status = 0;
		if (!theme_extending) {
			theme_extending = 1;
			status = theme_read_named(name, colors, s, error, error_size);
			theme_extending = 0;
		}
		if (status <= 0) {
			if (status == 0) {
				snprintf(error, error_size, "invalid theme setting: extends");
			}
			json_object_put(object);
			hex_count = saved_hex_count;
			return -1;
		}
		json_object_object_del(object, "extends");
	}
	int parsed = theme_parse(object, colors, s, error, error_size);
	json_object_put(object);
	if (!parsed) {
		hex_count = saved_hex_count;
		skin_release(s);
	}

	return parsed ? 1 : -1;
}

/* Unbound views use the global default; chats and their background board can
   bind the originating transcript's sidecar without changing that default. */
static char theme_transcript_path[4096];

static int theme_config_path(char *path, size_t size) {

	if (*theme_transcript_path) {
		int n = snprintf(path, size, "%s", theme_transcript_path);
		return n >= 0 && (size_t)n < size;
	}

	return phi_xdg_path(path, size, "XDG_CONFIG_HOME", ".config", "phi/theme.json");
}

/* 1: loaded, 0: absent (defaults), -1: invalid. No colours applied here. */
static int theme_read(short *colors, struct skin *s, char *error, size_t error_size) {

	char path[4096];
	if (!theme_config_path(path, sizeof(path))) {
		for (size_t i = 0; i < THEME_COUNT; i++) {
			colors[i] = theme_defaults[i].value;
		}
		*s = skin_defaults;
		snprintf(error, error_size, getenv("HOME") ? "theme path is too long" : "HOME is not set");
		return -1;
	}
	int status = theme_read_path(path, colors, s, error, error_size);
	/* Failed selector creation must not change the fallback look. */
	if (!status && *theme_transcript_path &&
		phi_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config", "phi/theme.json")) {
		status = theme_read_path(path, colors, s, error, error_size);
	}
	if (!status) {
		status = theme_read_named("default", colors, s, error, error_size);
	}

	return status;
}

/* Watch the selector and its shared definition/inheritance dependencies. */
struct theme_stamp {
	long long device, inode, size, modified_ns;
	int exists;
	unsigned long long dependencies;
};

static unsigned long long theme_hash_bytes(unsigned long long hash, const void *data, size_t size) {

	const unsigned char *bytes = data;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}

	return hash;
}

static unsigned long long theme_dependency_stamp(
	const char *path, int depth, unsigned long long hash) {

	hash = theme_hash_bytes(hash, path, strlen(path));
	struct stat st;
	if (!stat(path, &st)) {
		long long values[] = {(long long)st.st_dev, (long long)st.st_ino, (long long)st.st_size,
			(long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec};
		hash = theme_hash_bytes(hash, values, sizeof(values));
	}
	if (depth >= 4) {
		return hash;
	}
	json_object *object = NULL;
	char error[256];
	if (theme_read_config(path, &object, error, sizeof(error)) != 1) {
		return hash;
	}
	json_object *value = json_object_object_get(object, "theme");
	if (!value) {
		value = json_object_object_get(object, "extends");
	}
	const char *name =
		json_object_is_type(value, json_type_string) ? json_object_get_string(value) : "";
	if (theme_name_valid(name)) {
		char directories[2][4096], candidate[4200];
		int count = theme_directories(directories), found = 0;
		for (int i = 0; i < count; i++) {
			snprintf(candidate, sizeof(candidate), "%.4095s/%.63s.json", directories[i], name);
			/* Include absent higher-priority paths so new user overrides reload. */
			hash = theme_hash_bytes(hash, candidate, strlen(candidate));
			if (!stat(candidate, &st)) {
				if (!found) {
					hash = theme_dependency_stamp(candidate, depth + 1, hash);
					found = 1;
				}
			}
		}
	}
	json_object_put(object);

	return hash;
}

static inline struct theme_stamp theme_stamp_named(const char *name) {

	struct theme_stamp stamp = {0};
	char directories[2][4096], path[4200];
	int count = theme_directories(directories);
	stamp.dependencies = 14695981039346656037ULL;
	for (int i = 0; i < count; i++) {
		snprintf(path, sizeof(path), "%.4095s/%.63s.json", directories[i], name);
		stamp.dependencies = theme_dependency_stamp(path, 0, stamp.dependencies);
	}

	return stamp;
}

static struct theme_stamp theme_stamp_now(void) {

	struct theme_stamp stamp = {0};
	char path[4096];
	struct stat st;
	if (theme_config_path(path, sizeof(path)) && !stat(path, &st)) {
		stamp.exists = 1;
		stamp.device = (long long)st.st_dev;
		stamp.inode = (long long)st.st_ino;
		stamp.size = (long long)st.st_size;
		stamp.modified_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
	}
	if (theme_config_path(path, sizeof(path))) {
		stamp.dependencies = theme_dependency_stamp(path, 0, 14695981039346656037ULL);
		if (!stamp.exists) {
			int have_default = 0;
			if (*theme_transcript_path &&
				phi_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config", "phi/theme.json")) {
				stamp.dependencies = theme_dependency_stamp(path, 0, stamp.dependencies);
				have_default = !stat(path, &st);
			}
			if (!have_default) {
				struct theme_stamp implicit = theme_stamp_named("default");
				stamp.dependencies = theme_hash_bytes(
					stamp.dependencies, &implicit.dependencies, sizeof(implicit.dependencies));
			}
		}
	}

	return stamp;
}

static int theme_stamp_same(struct theme_stamp a, struct theme_stamp b) {

	return a.exists == b.exists && a.device == b.device && a.inode == b.inode && a.size == b.size &&
		a.modified_ns == b.modified_ns && a.dependencies == b.dependencies;
}

#ifndef THEME_LOADER_ONLY
static json_object *theme_named_object(const char *name) {

	if (!theme_name_valid(name)) {
		return NULL;
	}
	json_object *object = json_object_new_object(), *value = json_object_new_string(name);
	if (!object || !value) {
		json_object_put(object);
		json_object_put(value);
		return NULL;
	}
	if (json_object_object_add(object, "theme", value) < 0) {
		json_object_put(value);
		json_object_put(object);
		return NULL;
	}

	return object;
}

static int theme_write_object(const char *path, json_object *object, char *error, size_t size) {

	if (!json_object_is_type(object, json_type_object)) {
		snprintf(error, size, "theme must be a JSON object");
		return 0;
	}
	/* Compact output cannot acquire unreadable indentation overhead. Never
       replace an existing file with output exceeding the reader's limit. */
	const char *text = json_object_to_json_string_ext(
		object, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
	if (!text) {
		snprintf(error, size, "cannot serialize theme");
		return 0;
	}
	size_t bytes = strlen(text);
	json_object *checked = NULL;
	if (theme_decode_config(text, bytes, &checked, error, size) < 0) {
		return 0;
	}
	json_object_put(checked);
	if (!phi_write_file_atomic(path, text, bytes)) {
		snprintf(error, size, "cannot save theme: %s", strerror(errno));
		return 0;
	}

	return 1;
}

/* Install a definition without replacing an existing user theme, including
   when another process creates the same name during migration. */
static int theme_install_definition(
	const char *folder, const char *path, json_object *object, char *error, size_t size) {

	char temporary[4200];
	int n = snprintf(temporary, sizeof(temporary), "%s/.migrate-XXXXXX", folder);
	if (n < 0 || (size_t)n >= sizeof(temporary)) {
		snprintf(error, size, "theme path is too long");
		return -1;
	}
	int fd = mkstemp(temporary);
	if (fd < 0) {
		snprintf(error, size, "cannot create shared theme: %s", strerror(errno));
		return -1;
	}
	if (close(fd)) {
		unlink(temporary);
		snprintf(error, size, "cannot close shared theme: %s", strerror(errno));
		return -1;
	}
	if (!theme_write_object(temporary, object, error, size)) {
		unlink(temporary);
		return -1;
	}
	int result = link(temporary, path), saved = errno;
	unlink(temporary);
	if (result && saved == EEXIST) {
		return 0;
	}
	if (result) {
		snprintf(error, size, "cannot install shared theme: %s", strerror(saved));
		return -1;
	}
	fd = open(folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}

	return 1;
}

static int theme_definition_equal(const char *path, json_object *object) {

	json_object *existing = NULL;
	char error[256];
	int status = theme_read_config(path, &existing, error, sizeof(error));
	int equal = status == 1 && json_object_equal(existing, object);
	json_object_put(existing);

	return equal;
}

static int theme_export_legacy(
	json_object *object, int global, char name[64], char *error, size_t size) {

	char folder[4096], path[4200];
	if (!phi_xdg_path(folder, sizeof(folder), "XDG_CONFIG_HOME", ".config", "phi/themes") ||
		!phi_make_directories(folder)) {
		snprintf(error, size, "cannot create shared theme directory: %s", strerror(errno));
		return 0;
	}
	/* Reuse a definition already migrated by another chat. */
	DIR *directory = opendir(folder);
	struct dirent *entry;
	int scanned = 0;
	while (directory && (entry = readdir(directory)) && scanned++ < 4096) {
		size_t bytes = strlen(entry->d_name);
		if (bytes <= 5 || bytes > 68 || strcmp(entry->d_name + bytes - 5, ".json")) {
			continue;
		}
		char candidate[64];
		snprintf(candidate, sizeof(candidate), "%.*s", (int)(bytes - 5), entry->d_name);
		if (!theme_name_valid(candidate)) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%.68s", folder, entry->d_name);
		if (theme_definition_equal(path, object)) {
			snprintf(name, 64, "%s", candidate);
			closedir(directory);
			return 1;
		}
	}
	if (directory) {
		closedir(directory);
	}
	const char *text = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
	if (!text) {
		snprintf(error, size, "cannot serialize legacy theme");
		return 0;
	}
	unsigned long long hash = theme_hash_bytes(14695981039346656037ULL, text, strlen(text));
	for (int attempt = 0; attempt < 32; attempt++) {
		if (global && !attempt) {
			snprintf(name, 64, "default");
		} else {
			snprintf(name, 64, "migrated-%016llx-%d", hash, attempt);
		}
		snprintf(path, sizeof(path), "%s/%s.json", folder, name);
		if (theme_definition_equal(path, object)) {
			return 1;
		}
		int installed = theme_install_definition(folder, path, object, error, size);
		if (installed < 0) {
			return 0;
		}
		if (installed || theme_definition_equal(path, object)) {
			return 1;
		}
	}
	snprintf(error, size, "cannot find an unused name for the legacy theme");

	return 0;
}

/* Preserve legacy overrides as shared definitions, then persist only a name.
   Invalid files stay untouched. An absent config means the shared default. */
static int theme_migrate_reference(
	const char *path, int global, char name[64], char *error, size_t size) {

	struct stat before, after;
	int existed = !stat(path, &before);
	json_object *object = NULL;
	int status = theme_read_config(path, &object, error, size);
	if (!status) {
		snprintf(name, 64, "default");
		return 1;
	}
	if (status < 0) {
		return 0;
	}
	short colors[THEME_COUNT];
	struct skin parsed;
	int saved_hex = hex_count;
	status = theme_read_path(path, colors, &parsed, error, size);
	skin_release(&parsed);
	hex_count = saved_hex;
	if (status != 1) {
		if (!status) {
			snprintf(error, size, "theme changed during migration; retry");
		}
		json_object_put(object);
		return 0;
	}
	json_object *value = json_object_object_get(object, "theme");
	if (value) {
		snprintf(name, 64, "%s", json_object_get_string(value));
		json_object_put(object);
		return 1;
	}
	value = json_object_object_get(object, "extends");
	if (!json_object_object_length(object)) {
		char definition[4200];
		json_object *baseline = NULL;
		int found = theme_named_path("default", definition, sizeof(definition));
		int same = !found ||
			(theme_read_config(definition, &baseline, error, size) == 1 &&
				json_object_equal(object, baseline));
		json_object_put(baseline);
		if (same) {
			snprintf(name, 64, "default");
		} else if (!theme_export_legacy(object, global, name, error, size)) {
			json_object_put(object);
			return 0;
		}
	} else if (json_object_object_length(object) == 1 &&
		json_object_is_type(value, json_type_string)) {
		snprintf(name, 64, "%s", json_object_get_string(value));
	} else if (!theme_export_legacy(object, global, name, error, size)) {
		json_object_put(object);
		return 0;
	}
	json_object_put(object);
	if (!existed || stat(path, &after) || before.st_dev != after.st_dev ||
		before.st_ino != after.st_ino || before.st_size != after.st_size ||
		before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
		before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
		snprintf(error, size, "theme changed during migration; retry");
		return 0;
	}
	object = theme_named_object(name);
	int ok = theme_write_object(path, object, error, size);
	json_object_put(object);

	return ok;
}

static void theme_selection_label(char name[64]) {

	char path[4096], error[256];
	json_object *object = NULL;
	if (!theme_config_path(path, sizeof(path))) {
		snprintf(name, 64, "default");
		return;
	}
	int status = theme_read_config(path, &object, error, sizeof(error));
	if (!status && *theme_transcript_path &&
		phi_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config", "phi/theme.json")) {
		status = theme_read_config(path, &object, error, sizeof(error));
	}
	json_object *value = json_object_object_get(object, "theme");
	if (!value) {
		value = json_object_object_get(object, "extends");
	}
	snprintf(name, 64, "%s",
		json_object_is_type(value, json_type_string) ? json_object_get_string(value)
			: status == 1                            ? "theme.json"
													 : "default");
	json_object_put(object);
}

#endif

static short theme_value(const short *colors, const char *name) {

	short value = -1;
	for (size_t i = 0; i < THEME_COUNT; i++) {
		if (!strcmp(name, theme_defaults[i].name)) {
			value = colors[i];
			break;
		}
	}
	if (value >= THEME_HEX && value < THEME_HEX + hex_count) {
		int index = value - THEME_HEX, rgb = hex_colors[index];
		if (terminal_direct()) {
			return terminal_rgb(rgb);
		}
		if (hex_slot[index]) {
			return hex_slot[index];
		}
		value = theme_nearest(rgb >> 16, rgb >> 8 & 255, rgb & 255);
	}
	if (value >= COLORS) {
		if (COLORS >= 8) {
			int r, g, b;
			if (value < 16) {
				return value % 8;
			}
			theme_rgb(value, &r, &g, &b);
			int low = r < g ? r : g;
			if (b < low) {
				low = b;
			}
			int high = r > g ? r : g;
			if (b > high) {
				high = b;
			}
			/* Pastel accents have every channel above 128. Preserve their
               hue on basic terminals instead of turning every accent white;
               keep mid-grey rules visible rather than mapping them to black. */
			int cutoff = high - low >= 24 ? low + (high - low) / 2 : 63;
			return (short)((r > cutoff ? 1 : 0) | (g > cutoff ? 2 : 0) | (b > cutoff ? 4 : 0));
		}
		return -1;
	}

	return value;
}

#ifndef THEME_LOADER_ONLY
/* A lighter tint of RGB or a 256-colour index: percent of the way to white.
   Default and ANSI 0-15 have no known RGB and stay as they are. */
static short theme_tint(short index, int percent) {

	int r, g, b;
	if (terminal_direct() && index >= PHI_RGB_FIRST && index - PHI_RGB_FIRST < terminal_rgb_count) {
		int rgb = terminal_rgb_values[index - PHI_RGB_FIRST];
		r = rgb >> 16;
		g = rgb >> 8 & 255;
		b = rgb & 255;
	} else {
		if (index < 16 || index > 255) {
			return index;
		}
		theme_rgb(index, &r, &g, &b);
	}
	if (terminal_direct()) {
		return terminal_rgb((r + (255 - r) * percent / 100) << 16 |
			(g + (255 - g) * percent / 100) << 8 | (b + (255 - b) * percent / 100));
	}

	return theme_nearest(r + (255 - r) * percent / 100, g + (255 - g) * percent / 100,
		b + (255 - b) * percent / 100);
}

static short theme_current[THEME_COUNT];
static int theme_current_known;
static char theme_current_name[64] = "theme.json";

/* color_content() is ncurses' bookkeeping, not a terminal palette query.
   Uninitialised extended entries may contain repeated ANSI colours. Restore
   the fixed xterm-256 palette used by theme_nearest(), never that fake snapshot. */
static void theme_restore_slot(short slot) {

	int r, g, b;
	theme_rgb(slot, &r, &g, &b);
	init_color(slot, (short)(r * 1000 / 255), (short)(g * 1000 / 255), (short)(b * 1000 / 255));
}

/* palette.exact: each hex colour the theme uses gets a palette slot from
   232-251 set to exactly that colour (the terminal puts them back when Phi
   exits, and so does Phi when a theme no longer wants them). Colours past
   the twenty slots, and terminals that cannot, keep the nearest colour. */
static void theme_exact_slots(const short *colors) {

	static unsigned int changed_slots;
	for (int i = 0; i < THEME_HEX_MAX; i++) {
		hex_slot[i] = 0;
	}
	for (int i = 0; i < 20; i++) {
		if (changed_slots & (1u << i)) {
			theme_restore_slot((short)(232 + i));
		}
	}
	changed_slots = 0;
	if (terminal_direct() || !skin.palette_exact || COLORS < 256 || !can_change_color()) {
		return;
	}
	/* Slots the theme names by number stay as they are. */
	int taken[20] = {0};
	for (size_t k = 0; k < THEME_COUNT; k++) {
		if (colors[k] >= 232 && colors[k] <= 251) {
			taken[colors[k] - 232] = 1;
		}
	}
	int next = 0;
	for (size_t k = 0; k < THEME_COUNT; k++) {
		int index = colors[k] - THEME_HEX;
		if (colors[k] < THEME_HEX || index >= hex_count || hex_slot[index]) {
			continue;
		}
		while (next < 20 && taken[next]) {
			next++;
		}
		if (next == 20) {
			break;
		}
		int rgb = hex_colors[index];
		short slot = (short)(232 + next++);
		init_color(slot, (short)((rgb >> 16) * 1000 / 255), (short)((rgb >> 8 & 255) * 1000 / 255),
			(short)((rgb & 255) * 1000 / 255));
		hex_slot[index] = slot;
		changed_slots |= 1u << (slot - 232);
	}
}

static void theme_apply(const short *colors) {

	theme_exact_slots(colors);
	memcpy(theme_current, colors, sizeof(theme_current));
	theme_current_known = 1;
	if (!has_colors()) {
		return;
	}
#define THEME_PAIR(id, fg, bg) init_pair(id, theme_value(colors, fg), theme_value(colors, bg))
	THEME_PAIR(1, "spinner", "background");
	THEME_PAIR(2, "user_text", "user_background");
	THEME_PAIR(3, "muted", "background");
	THEME_PAIR(4, "muted", "user_background");
	THEME_PAIR(5, "border", "background");
	THEME_PAIR(6, "code_text", "code_background");
	{
		short inline_text = theme_value(colors, "inline_code"),
			  inline_background = theme_value(colors, "inline_code_background");
		if (inline_text == -2) {
			inline_text = theme_value(colors, "code_text");
		}
		if (inline_background == -2) {
			inline_background = theme_value(colors, "code_background");
		}
		init_pair(66, inline_text, inline_background);
	}
	THEME_PAIR(7, "spinner", "background");
	THEME_PAIR(8, "tool_text", "tool_background");
	THEME_PAIR(9, "tool_heading", "tool_background");
	THEME_PAIR(10, "error", "tool_background");
	/* Foot and other mutable 256-colour terminals can render the intended
       low-saturation diff bands exactly, by redefining palette slots 252-255
       (restored by the terminal's orig-colours capability when curses exits).
       Only when no theme colour uses those slots: a theme colour there (a hex
       grey often maps onto them) would otherwise be recoloured too. */
	int slots_free = 1;
	for (size_t i = 0; i < THEME_COUNT; i++) {
		short resolved = theme_value(colors, theme_defaults[i].name);
		if (resolved >= 252 && resolved <= 255) {
			slots_free = 0;
		}
	}
	int soft_diff = slots_free && COLORS >= 256 && can_change_color() &&
		theme_value(colors, "diff_removed") == 174 && theme_value(colors, "diff_added") == 108 &&
		theme_value(colors, "diff_removed_background") == 234 &&
		theme_value(colors, "diff_added_background") == 234;
	static int repainted;
	if (soft_diff) {
		repainted = 1;
		init_color(252, 863, 545, 545);
		init_color(253, 525, 780, 584);
		init_color(254, 227, 90, 102);
		init_color(255, 43, 208, 153);
		init_pair(11, 252, 254);
		init_pair(12, 253, 255);
	} else {
		if (repainted) {
			for (short i = 0; i < 4; i++) {
				theme_restore_slot((short)(252 + i));
			}
		}
		repainted = 0;
		THEME_PAIR(11, "diff_removed", "diff_removed_background");
		THEME_PAIR(12, "diff_added", "diff_added_background");
	}
	THEME_PAIR(13, "diff_context", "tool_background");
	const char *syntax[] = {
		"syntax_keyword", "syntax_string", "syntax_number", "syntax_comment", "syntax_function"};
	for (short i = 0; i < 5; i++) {
		THEME_PAIR((short)(14 + i), syntax[i], "code_background");
		THEME_PAIR((short)(19 + i), syntax[i], "tool_background");
	}
	THEME_PAIR(24, "tool_text", "tool_background");
	THEME_PAIR(25, "syntax_operator", "code_background");
	THEME_PAIR(26, "syntax_operator", "tool_background");
	THEME_PAIR(27, "text", "background");
	THEME_PAIR(28, "accent", "background");
	THEME_PAIR(31, "diff_removed", "tool_background");
	THEME_PAIR(48, "diff_added", "tool_background");
	THEME_PAIR(49, "muted", "tool_background");
	THEME_PAIR(50, "tool_rule", "background");
	THEME_PAIR(51, "tool_success", "tool_background");
	THEME_PAIR(52, "muted", "tool_background");
	THEME_PAIR(53, "error", "background");
	for (short i = 0; i < 8; i++) {
		init_pair((short)(54 + i), i, theme_value(colors, "tool_background"));
	}
	THEME_PAIR(62, "accent", "tool_background");
	THEME_PAIR(63, "accent", "user_background");
	/* spinner.shimmer: the peak and the edges of the highlight, lighter
       tints of the spinner colour. */
	short spinner = theme_value(colors, "spinner"),
		  shimmer_background = theme_value(colors, "background");
	init_pair(64, COLORS >= 256 ? theme_tint(spinner, 50) : spinner, shimmer_background);
	init_pair(65, COLORS >= 256 ? theme_tint(spinner, 25) : spinner, shimmer_background);
	THEME_PAIR(29, "mascot_casing", "background");
	THEME_PAIR(30, "mascot_primary", "background");
	const char *mascot[] = {"background", "mascot_primary", "mascot_light", "mascot_ink"};
	for (short a = 0; a < 4; a++) {
		for (short b = 0; b < 4; b++) {
			THEME_PAIR((short)(32 + a * 4 + b), mascot[a], mascot[b]);
		}
	}
#undef THEME_PAIR
	bkgd(COLOR_PAIR(27) | ' ');
}
#endif /* THEME_LOADER_ONLY */
