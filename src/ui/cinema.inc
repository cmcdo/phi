/* Native cell-based header host. No GIF decoder or terminal graphics protocol.
   Modules own state/cells; Phi owns clipping, timers and ncurses composition. */
#include <dlfcn.h>
#include "../../include/phi_widget.h"

static struct {
	void *module, *instance;
	const struct phi_widget_v1 *widget;
	const struct phi_widget_cell *cells;
	int loaded, loaded_mode, width, height, visible, painting;
	char loaded_id[64];
	int scroll, viewport_rows, viewport_cols, terminal_rows, draw_x, draw_y, compact, origin_y;
	unsigned revision, rendered_revision, render_count;
	double next;

	struct {
		short foreground, background;
	} pairs[256];

	int pair_count;
} cinema;

static void cinema_release_pointer(void) {

	if (cinema.instance) {
		struct phi_widget_input release = {.kind = PHI_WIDGET_RELEASE};
		cinema.widget->input(cinema.instance, &release);
	}
	cinema.painting = 0;
	transcript_selection_tracking(0);
}

static void cinema_close(void) {

	header_motion_cancel();
	if (cinema.painting) {
		cinema_release_pointer();
	}
	if (cinema.instance) {
		cinema.widget->destroy(cinema.instance);
	}
	if (cinema.module) {
		dlclose(cinema.module);
	}
	memset(&cinema, 0, sizeof(cinema));
}

/* Registration is metadata only. Only already-approved native extensions reach
   this API; theme JSON selects IDs, never shared-library filenames. */
static struct native_widget_registration {
	char id[64], owner[128], path[4096];
	struct native_widget_registration *next;
} *native_widgets;

int ui_native_widget_register(
	const char *owner, const char *id, const char *path, char *problem, size_t capacity) {

	if (!owner || !*owner || strlen(owner) >= 128 || !id || !*id || strlen(id) >= 64 ||
		!strcmp(id, "life") || !strcmp(id, "orbit")) {
		snprintf(problem, capacity, "Invalid or reserved widget ID");
		return 0;
	}
	for (const char *p = id; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
			snprintf(problem, capacity, "Widget IDs use letters, digits, dot, dash or underscore");
			return 0;
		}
	}
	struct native_widget_registration **slot = &native_widgets;
	int count = 0;
	while (*slot && strcmp((*slot)->id, id)) {
		slot = &(*slot)->next;
	}
	if (*slot && strcmp((*slot)->owner, owner)) {
		snprintf(problem, capacity, "Widget ID belongs to another extension");
		return 0;
	}
	char resolved[4096];
	if (path && (path[0] != '/' || !realpath(path, resolved))) {
		snprintf(problem, capacity, "Widget needs an existing absolute library path");
		return 0;
	}
	if (path && !*slot) {
		for (struct native_widget_registration *p = native_widgets; p; p = p->next) {
			count++;
		}
		if (count >= 16) {
			snprintf(problem, capacity, "Too many registered native widgets");
			return 0;
		}
		*slot = calloc(1, sizeof(**slot));
		if (!*slot) {
			snprintf(problem, capacity, "Out of memory");
			return 0;
		}
		snprintf((*slot)->id, sizeof((*slot)->id), "%s", id);
		snprintf((*slot)->owner, sizeof((*slot)->owner), "%s", owner);
	}
	if (!strcmp(cinema.loaded_id, id)) {
		cinema_close();
	}
	if (path) {
		snprintf((*slot)->path, sizeof((*slot)->path), "%s", resolved);
	} else if (*slot) {
		struct native_widget_registration *old = *slot;
		*slot = old->next;
		free(old);
	}
	dirty = 1;

	return 1;
}

static void ui_native_widget_forget(const char *owner) {

	struct native_widget_registration **slot = &native_widgets;
	while (*slot) {
		struct native_widget_registration *p = *slot;
		if (!owner || !strcmp(p->owner, owner)) {
			if (!strcmp(cinema.loaded_id, p->id)) {
				cinema_close();
			}
			*slot = p->next;
			free(p);
			dirty = 1;
		} else {
			slot = &p->next;
		}
	}
}

static void cinema_load(void) {

	if (skin.welcome != SKIN_WELCOME_CINEMA || ((used || working) && !skin.welcome_persist)) {
		return;
	}
	if (cinema.loaded &&
		(cinema.loaded_mode != skin.welcome_life ||
			strcmp(cinema.loaded_id, skin.welcome_widget))) {
		cinema_close();
	}
	if (cinema.loaded) {
		return;
	}
	cinema.loaded = 1;
	cinema.loaded_mode = skin.welcome_life;
	snprintf(cinema.loaded_id, sizeof(cinema.loaded_id), "%s", skin.welcome_widget);
	if (!skin.welcome_life) {
		return;
	}
	char path[4096] = "";
	for (struct native_widget_registration *p = native_widgets; p; p = p->next) {
		if (!strcmp(p->id, skin.welcome_widget)) {
			snprintf(path, sizeof(path), "%s", p->path);
		}
	}
	if (!*path && (!strcmp(skin.welcome_widget, "life") || !strcmp(skin.welcome_widget, "orbit"))) {
		ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
		if (n <= 0) {
			return;
		}
		path[n] = 0;
		char *slash = strrchr(path, '/');
		if (!slash || sizeof(path) - (size_t)(slash - path) < 64) {
			return;
		}
		snprintf(
			slash, sizeof(path) - (size_t)(slash - path), "/build/widgets/%s.so", skin.welcome_widget);
	}
	if (!*path) {
		return;
	}
	cinema.module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!cinema.module) {
		return;
	}
	const struct phi_widget_v1 *(*entry)(void) =
		(const struct phi_widget_v1 *(*)(void))dlsym(cinema.module, "phi_widget_entry");
	const struct phi_widget_v1 *w = entry ? entry() : NULL;
	if (w && w->abi == PHI_WIDGET_ABI && w->size >= sizeof(*w) && w->width > 0 && w->width <= 200 &&
		w->height > 0 && w->height <= 100 && w->create && w->destroy && w->tick && w->delay_ms &&
		w->render && w->input) {
		cinema.widget = w;
		cinema.instance = w->create();
		if (cinema.instance) {
			cinema.width = w->width;
			cinema.height = w->height;
			cinema.next = now();
			return;
		}
	}
	dlclose(cinema.module);
	cinema.module = NULL;
	cinema.widget = NULL;
}

static int cinema_ready(void) {

	if ((used || working) && !skin.welcome_persist) {
		if (cinema.loaded) {
			cinema_close();
		}
		return 0;
	}
	cinema_load();

	return cinema.instance != NULL;
}

static int cinema_live(void) {

	return cinema.instance != NULL;
}

static const char *cinema_controls(void) {

	return cinema.widget && cinema.widget->controls ? cinema.widget->controls : "";
}

static int cinema_banner_rows(int height) {

	if ((used || working) && !skin.welcome_persist) {
		return 2;
	}

	return cinema_ready() ? cinema.height + (skin.welcome_hero ? 4 : 1) : height / 2 + 1;
}

static int cinema_active(void) {

	return active && skin.welcome == SKIN_WELCOME_CINEMA &&
		(!(used || working) || skin.welcome_persist) && !modal_active && !pending_approval &&
		cinema.scroll < welcome_banner_height(conversation_height);
}

static int cinema_delay(void) {

	int ms = cinema.instance ? cinema.widget->delay_ms(cinema.instance) : 0;

	return ms <= 0 ? 0 : ms < 50 ? 50 : ms > 2000 ? 2000 : ms;
}

static int cinema_due(void) {

	if (!cinema.visible || header_motion_gone() || !cinema_active() || !cinema_ready() ||
		!cinema_delay()) {
		return 0;
	}
	if (now() < cinema.next) {
		return 0;
	}
	int changed = cinema.widget->tick(cinema.instance);
	if (changed) {
		cinema.revision++;
	}
	cinema.next = now() + cinema_delay() / 1000.0;

	return changed;
}

static int cinema_wait(void) {

	if (!cinema.visible || header_motion_gone() || !cinema_active() || !cinema_ready() ||
		!cinema_delay()) {
		return -1;
	}
	double remaining = cinema.next - now();

	return remaining > 0 ? (int)(remaining * 1000) + 1 : 0;
}

static void cinema_begin(void) {

	if ((skin.welcome != SKIN_WELCOME_CINEMA || ((used || working) && !skin.welcome_persist)) &&
		cinema.loaded) {
		cinema_close();
		return;
	}
	if ((modal_active || pending_approval) && header_motion.running) {
		header_motion_interrupt();
	}
	if (!modal_active && !pending_approval && cinema.instance && cinema.visible &&
		!cinema.compact && (used || working) && skin.welcome_persist) {
		header_motion_start(cinema.draw_y + cinema.scroll - cinema.origin_y);
	}
	if (cinema.painting && (!cinema_active() || header_motion_gone())) {
		cinema_release_pointer();
	}
}

static void cinema_view(int scroll) {

	if (header_motion.running && (cinema.terminal_rows != LINES || cinema.viewport_cols != COLS)) {
		header_motion_interrupt();
	}
	cinema.scroll = scroll;
	cinema.viewport_rows = conversation_height;
	cinema.viewport_cols = COLS;
	cinema.terminal_rows = LINES;
	if (cinema.painting && (!cinema_active() || header_motion_gone())) {
		cinema_release_pointer();
	}
}

static short cinema_color(unsigned color, short background, int indexed) {

	if (!color) {
		return background;
	}
	if (indexed && COLORS >= 256) {
		return (short)(color & 255);
	}
	if (COLORS < 256) {
		return color == 61 ? COLOR_YELLOW : COLOR_CYAN;
	}
	static const int levels[] = {0, 1, 3, 5};
	color &= 63;

	return (short)(16 + 36 * levels[color / 16] + 6 * levels[(color / 4) % 4] + levels[color % 4]);
}

static int cinema_pair(unsigned foreground, unsigned background, int indexed) {

	short text = -1, bg = -1;
	pair_content(27, &text, &bg);
	short fg = cinema_color(foreground, bg, indexed), back = cinema_color(background, bg, indexed);
	for (int i = 0; i < cinema.pair_count; i++) {
		if (cinema.pairs[i].foreground == fg && cinema.pairs[i].background == back) {
			return 384 + i;
		}
	}
	if (cinema.pair_count >= 256 || COLOR_PAIRS <= 384 + cinema.pair_count) {
		return 27;
	}
	int i = cinema.pair_count++;
	cinema.pairs[i].foreground = fg;
	cinema.pairs[i].background = back;
	init_pair((short)(384 + i), fg, back);

	return 384 + i;
}

static void cinema_draw(int rows, int cols, int scroll) {

	cinema.scroll = scroll;
	cinema.visible = 0;
	if (header_motion_gone() || !cinema_active() || !cinema_ready()) {
		return;
	}
	if (!cinema.cells || cinema.rendered_revision != cinema.revision) {
		cinema.cells = cinema.widget->render(cinema.instance);
		cinema.rendered_revision = cinema.revision;
		cinema.render_count++;
	}
	if (!cinema.cells) {
		return;
	}
	cinema.compact = !!(used || working);
	int top = cinema.compact ? header_motion_offset() : (rows - cinema.height) / 2;
	if (!cinema.compact && top < 0) {
		top = 0;
	}
	int limit = cinema.compact ? welcome_banner_height(rows) - 1 - scroll : rows;
	if (limit > rows) {
		limit = rows;
	}
	/* Orbit stays centred independently of its right-hand introduction. */
	int orbit = !strcmp(cinema.loaded_id, "orbit");
	int info_width = skin.welcome_hero && cols >= cinema.width + 34 ? 30 : 0;
	int left = (cols - cinema.width - (!orbit && info_width ? info_width + 4 : 0)) / 2;
	if (orbit && info_width) {
		int available = cols - skin.margin - (left + cinema.width + 4);
		info_width = available < info_width ? available : info_width;
		if (info_width < 16) {
			info_width = 0;
		}
	}
	int info_left = left + cinema.width + 4;
	if (info_width) {
		int x = info_left;
		int y = top + (cinema.height - 1 - skin.hero_line_count) / 2 - scroll;
		char text[512];
		welcome_expand(skin.welcome_title, text, sizeof(text));
		if (*text && y >= 0 && y < limit) {
			welcome_text(y + cinema.origin_y, x, info_width, text, 28, 1);
		}
		for (int i = 0; i < skin.hero_line_count; i++) {
			welcome_expand(skin.hero_lines[i], text, sizeof(text));
			if (y + 1 + i >= 0 && y + 1 + i < limit) {
				welcome_text(y + 1 + i + cinema.origin_y, x, info_width, text, 3, 0);
			}
		}
	}
	cinema.draw_x = left;
	cinema.draw_y = top - scroll + cinema.origin_y;
	for (int y = 0; y < cinema.height; y++) {
		int sy = top + y - scroll;
		if (sy < 0 || sy >= limit) {
			continue;
		}
		for (int x = 0; x < cinema.width; x++) {
			int sx = left + x;
			if (sx < skin.margin || sx >= cols - skin.margin) {
				continue;
			}
			const struct phi_widget_cell *cell = &cinema.cells[y * cinema.width + x];
			wchar_t glyph[2] = {(wchar_t)cell->glyph, 0};
			if (!glyph[0]) {
				glyph[0] = L' ';
			} else if (wcwidth(glyph[0]) != 1) {
				glyph[0] = L'*';
			}
			cchar_t out;
			int pair = has_colors()
				? cinema_pair(cell->foreground, cell->background, !!(cell->flags & 2))
				: 0;
			setcchar(&out, glyph, cell->flags & 1 ? A_BOLD : A_NORMAL, (short)pair, NULL);
			mvadd_wch(sy + cinema.origin_y, sx, &out);
		}
		cinema.visible = 1;
	}
	/* Mask the complete welcome block, including its introduction. Native
       terminal cells have no alpha: a stable ordered dissolve removes cells. */
	if (header_motion.plugin ||
		(header_motion.running &&
			(header_motion.profile.enabled == 2 || header_motion.profile.enabled == 4))) {
		attrset(COLOR_PAIR(27));
		int span = cinema.width + (info_width ? info_width + 4 : 0);
		int mask_left = left;
		header_motion.mask_width = span;
		header_motion.mask_height = cinema.height;
		for (int y = 0; y < cinema.height; y++) {
			int sy = top + y - scroll;
			if (sy < 0 || sy >= limit) {
				continue;
			}
			for (int x = 0; x < span; x++) {
				int sx = mask_left + x;
				if (sx < skin.margin || sx >= cols - skin.margin) {
					continue;
				}
				if (!header_motion_cell_visible(x, y)) {
					mvaddch(sy + cinema.origin_y, sx, ' ');
				}
			}
		}
	}
	attrset(A_NORMAL);
}

static void cinema_palette_reset(void) {

	cinema.pair_count = 0;
}

static void cinema_origin(int y) {

	cinema.origin_y = y;
}

static int cinema_refresh(void) {

	if (!cinema.visible || cinema.viewport_rows < 1) {
		return 0;
	}
	int y, x;
	getyx(stdscr, y, x);
	cinema_draw(cinema.viewport_rows, COLS, cinema.scroll);
	/* Compose transient notices above the changing cells, in the same frame. */
	move(y, x);
	frame_begin();
	wnoutrefresh(stdscr);
	doupdate();
	frame_end();

	return 1;
}

static int cinema_widget_input(const struct phi_widget_input *event) {

	int result = cinema.widget->input(cinema.instance, event);
	if (result & PHI_WIDGET_CHANGED) {
		cinema.revision++;
		dirty = 1;
	}
	if (result & PHI_WIDGET_CAPTURE) {
		cinema.painting = 1;
		transcript_selection_tracking(1);
	}
	if (result & PHI_WIDGET_RELEASE_CAPTURE) {
		cinema.painting = 0;
		transcript_selection_tracking(0);
	}

	return !!(result & PHI_WIDGET_HANDLED);
}

static int cinema_mouse(const MEVENT *event) {

	if (cinema.painting && event->bstate & BUTTON1_RELEASED) {
		struct phi_widget_input release = {.kind = PHI_WIDGET_RELEASE};
		return cinema_widget_input(&release);
	}
	if (!cinema_active() || !cinema.visible || !cinema_live()) {
		return 0;
	}
	struct phi_widget_input e = {.button = 1};
	int rule = welcome_banner_height(conversation_height) - 1 - cinema.scroll + cinema.origin_y;
	if ((used || working) && event->y == rule &&
		event->bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED)) {
		e.kind = PHI_WIDGET_CONTROL;
		e.x = event->x - skin.margin - 2;
		return cinema_widget_input(&e);
	}
	e.x = event->x - cinema.draw_x;
	e.y = event->y - cinema.draw_y;
	int limit = (used || working) ? rule : cinema.viewport_rows + cinema.origin_y;
	if (event->y < cinema.origin_y || event->y >= limit || e.x < 0 || e.x >= cinema.width ||
		e.y < 0 || e.y >= cinema.height) {
		return cinema.painting;
	}
	if (event->bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED)) {
		e.button = 3;
	} else if (event->bstate & (BUTTON2_PRESSED | BUTTON2_CLICKED)) {
		e.button = 2;
	}
	if (event->bstate & (BUTTON1_CLICKED | BUTTON2_CLICKED | BUTTON3_CLICKED)) {
		e.kind = PHI_WIDGET_CLICK;
	} else if (event->bstate & (BUTTON1_PRESSED | BUTTON2_PRESSED | BUTTON3_PRESSED)) {
		e.kind = PHI_WIDGET_PRESS;
	} else if (cinema.painting) {
		e.kind = PHI_WIDGET_DRAG;
	} else {
		return 0;
	}

	return cinema_widget_input(&e);
}

void ui_header_widget_control(const char *action) {

	if (!action || !*action || skin.welcome != SKIN_WELCOME_CINEMA || !cinema_ready() ||
		!cinema.widget->control) {
		return;
	}
	if (cinema.widget->control(cinema.instance, action)) {
		cinema.revision++;
		cinema.next = now() + cinema_delay() / 1000.0;
		dirty = 1;
		draw(0);
	}
}
