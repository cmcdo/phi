/* Startup-only presentation; no image protocol, decoder, subprocess or model call. */
#include "../version.h"
static char welcome_path[4096], welcome_greeting[256], welcome_memory[80];

static const char *editor_mode_label(void) {

	return normal_mode && replace_pending ? "REPLACE"
		: visual_mode                     ? "VISUAL"
		: normal_mode                     ? "NORMAL"
										  : "INSERT";
}

static void welcome_identity(void) {

	char name[128] = "";
	const char *override = getenv("PHI_NAME");
	if (override && *override) {
		snprintf(name, sizeof(name), "%s", override);
	}
	if (!*name) {
		char path[4096];
		FILE *file = phi_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config", "phi/name")
			? fopen(path, "r")
			: NULL;
		if (file) {
			if (fgets(name, sizeof(name), file)) {
				name[strcspn(name, "\r\n")] = 0;
			}
			fclose(file);
		}
	}
	if (!*name) {
		/* Local passwd only: avoid an NSS/network lookup on the startup path. */
		FILE *file = fopen("/etc/passwd", "r");
		char line[2048];
		while (file && fgets(line, sizeof(line), file)) {
			char *fields[5], *p = line;
			int count = 0;
			while (count < 5) {
				fields[count++] = p;
				char *end = strchr(p, ':');
				if (!end) {
					break;
				}
				*end = 0;
				p = end + 1;
			}
			if (count != 5) {
				continue;
			}
			char *end;
			unsigned long uid = strtoul(fields[2], &end, 10);
			if (!*fields[2] || *end || uid != (unsigned long)getuid()) {
				continue;
			}
			fields[4][strcspn(fields[4], ",")] = 0;
			snprintf(name, sizeof(name), "%s", *fields[4] ? fields[4] : fields[0]);
			name[strcspn(name, " \t")] = 0;
			break;
		}
		if (file) {
			fclose(file);
		}
	}
	time_t stamp = time(NULL);
	struct tm local;
	int hour = localtime_r(&stamp, &local) ? local.tm_hour : 18;
	const char *period = hour >= 5 && hour < 12 ? "morning"
		: hour >= 12 && hour < 18               ? "afternoon"
												: "evening";
	if (*name) {
		snprintf(welcome_greeting, sizeof(welcome_greeting), "Good %s, %s.", period, name);
	} else {
		snprintf(welcome_greeting, sizeof(welcome_greeting), "Good %s.", period);
	}

	/* Snapshot resident memory once; never invent the mockup's number. */
	snprintf(welcome_memory, sizeof(welcome_memory), "Startup memory unavailable");
	FILE *memory = fopen("/proc/self/statm", "r");
	unsigned long pages;
	long page_size = sysconf(_SC_PAGESIZE);
	if (memory) {
		if (fscanf(memory, "%*u %lu", &pages) == 1 && page_size > 0) {
			snprintf(welcome_memory, sizeof(welcome_memory), "%.1f MiB at startup",
				(double)pages * page_size / (1024 * 1024));
		}
		fclose(memory);
	}
}

static int welcome_scroll, welcome_clip = -1;

/* The working directory as the header and footer show it: ~ for $HOME. */
static void welcome_set_path(void) {

	char cwd[4096];
	const char *home = getenv("HOME");
	if (!getcwd(cwd, sizeof(cwd))) {
		snprintf(cwd, sizeof(cwd), "Working directory unavailable");
	}
	size_t home_size = home ? strlen(home) : 0;
	if (home_size > 1 && !strncmp(cwd, home, home_size) &&
		(cwd[home_size] == '/' || !cwd[home_size])) {
		snprintf(welcome_path, sizeof(welcome_path), "~%s", cwd + home_size);
	} else {
		snprintf(welcome_path, sizeof(welcome_path), "%s", cwd);
	}
}

static void welcome_init(void) {

	welcome_identity();
	welcome_set_path();
}

/* Clip by terminal cells, not bytes; never wrap into another UI region. */
static void welcome_text(int y, int x, int available, const char *text, int pair, int bold) {

	y -= welcome_scroll;
	int limit = welcome_clip < 0 ? LINES : welcome_clip;
	if (available <= 0 || y < 0 || y >= limit || x < 0 || x >= COLS) {
		return;
	}
	if (available > COLS - x) {
		available = COLS - x;
	}
	attrset(COLOR_PAIR(pair) | (bold ? A_BOLD : 0));
	mbstate_t state = {0};
	int cells = 0;
	while (*text) {
		wchar_t ch;
		size_t n = mbrtowc(&ch, text, strlen(text), &state);
		if (n == (size_t)-1 || n == (size_t)-2) {
			ch = L'?';
			n = 1;
			memset(&state, 0, sizeof(state));
		}
		if (!n) {
			break;
		}
		int width = wcwidth(ch);
		if (width < 0) {
			ch = L'?';
			width = 1;
		}
		if (cells + width > available) {
			break;
		}
		mvaddnwstr(y, x + cells, &ch, 1);
		cells += width;
		text += n;
	}
	attrset(A_NORMAL);
}

static int welcome_title_height(int height) {

	(void)height;

	return 1;
}

static int welcome_banner_height(int height) {

	if (skin.welcome == SKIN_WELCOME_NONE) {
		return 0;
	}
	if ((used || working) && skin.welcome == SKIN_WELCOME_CINEMA && header_motion_gone()) {
		return skin.welcome_message_row ? skin.welcome_message_row : skin.welcome_message_padding;
	}
	if ((used || working) && skin.welcome_message_row) {
		int reserved = skin.welcome_message_row +
			(skin.welcome == SKIN_WELCOME_CINEMA ? header_motion_offset() : 0);
		return reserved > 0 ? reserved : 1;
	}
	if (skin.welcome == SKIN_WELCOME_SCENE) {
		return used ? 0 : height;
	}
	if (skin.welcome == SKIN_WELCOME_CINEMA) {
		if (!used && !working) {
			return height;
		}
		int reserved = cinema_banner_rows(height) + header_motion_offset();
		return reserved > 0 ? reserved : 1;
	}
	int text = skin.welcome == SKIN_WELCOME_FETCH
		? 2 + skin.welcome_field_count
		: welcome_title_height(height) + skin.welcome_line_count;

	return (height >= 5 ? 1 : 0) + (text > skin.art_count ? text : skin.art_count) + 1;
}

/* A skin's welcome line with {greeting}, {path}, {memory}, {version},
   {date} ("Sat, 03 Oct") and {time} ("15:31"). */
static void welcome_expand(const char *line, char *out, size_t size) {

	static const char *const names[] = {"{greeting}", "{path}", "{memory}", "{version}", "{date}",
		"{time}", "{model}", "{effort}", "{theme}", "{mode}", "{context}", "{usage}", "{status}",
		"{queued}", "{state}", "{project}"};
	char date[32] = "", clock[16] = "";
	time_t moment = time(NULL);
	struct tm local;
	if (localtime_r(&moment, &local)) {
		strftime(date, sizeof(date), "%a, %d %b", &local);
		strftime(clock, sizeof(clock), "%H:%M", &local);
	}
	char context[96], queue_label[48] = "";
	context_summary(context, sizeof(context));
	if (queued_count) {
		snprintf(queue_label, sizeof(queue_label), "%zu queued", queued_count);
	}
	const char *project = strrchr(welcome_path, '/');
	project = project && project[1] ? project + 1 : welcome_path;
	const char *values[] = {welcome_greeting, welcome_path, welcome_memory, PHI_VERSION, date,
		clock, footer_model, !strcmp(current_effort, "none") ? "off" : current_effort,
		*theme_current_name ? theme_current_name : "default", editor_mode_label(), context,
		account_usage, extension_footer, queue_label, working ? "working" : "ready", project};
	size_t n = 0;
	out[0] = 0;
	while (*line && n + 1 < size) {
		size_t i = 0;
		while (i < sizeof(names) / sizeof(*names) && strncmp(line, names[i], strlen(names[i]))) {
			i++;
		}
		if (i < sizeof(names) / sizeof(*names)) {
			n += (size_t)snprintf(out + n, size - n, "%s", values[i]);
			if (n >= size) {
				n = size - 1;
				break;
			}
			line += strlen(names[i]);
		} else {
			out[n++] = *line++;
		}
	}
	out[n] = 0;
}

#include "mascot-motion.inc"
/* The alternate poses are opt-in and must preserve every row's cell width. */
static double welcome_blink_deadline;
static int welcome_blink_closed;
static uint32_t welcome_blink_rng;
static int welcome_gesture, welcome_gesture_frame, welcome_intro_cursor;

static int welcome_blink_enabled(void) {

	if (used || working || modal_active || !skin.art_count ||
		skin.art_blink_count != skin.art_count ||
		(skin.welcome != SKIN_WELCOME_PHI && skin.welcome != SKIN_WELCOME_FETCH)) {
		return 0;
	}
	for (int i = 0; i < skin.art_count; i++) {
		if (skin_width(skin.art[i]) != skin_width(skin.art_blink[i])) {
			return 0;
		}
	}

	return 1;
}

static int welcome_blink_update(double stamp) {

	if (!welcome_blink_enabled()) {
		int changed = welcome_blink_closed;
		welcome_blink_closed = 0;
		welcome_blink_deadline = 0;
		welcome_intro_cursor = skin.art_intro_count;
		return changed;
	}
	int intro = mascot_intro_step(&skin, stamp, &welcome_blink_deadline, &welcome_blink_closed,
		&welcome_blink_rng, &welcome_gesture, &welcome_gesture_frame, &welcome_intro_cursor);
	if (intro >= 0) {
		return intro;
	}

	return mascot_step(&skin, stamp, &welcome_blink_deadline, &welcome_blink_closed,
		&welcome_blink_rng, &welcome_gesture, &welcome_gesture_frame);
}

static int welcome_blink_wait(void) {

	if (!welcome_blink_enabled() || !welcome_blink_deadline) {
		return -1;
	}
	double remaining = welcome_blink_deadline - now();

	return remaining > 0 ? (int)(remaining * 1000) + 1 : 0;
}

static const char *welcome_art_line(int row) {

	return mascot_line(&skin, welcome_blink_closed && welcome_blink_enabled(), welcome_gesture,
		welcome_gesture_frame, row);
}

#include "brand-header.inc"

static const char *welcome_brand_art(int row, void *context) {

	(void)context;

	return welcome_art_line(row);
}

static void welcome_brand_expand(const char *text, char *out, size_t size, void *context) {

	(void)context;
	welcome_expand(text, out, size);
}

static void welcome_brand_paint(
	int y, int x, int width, const char *text, int pair, int bold, void *context) {

	(void)context;
	welcome_text(y, x, width, text, pair, bold);
}

static void welcome_fetch(int height, int cols, int top) {

	int art_width = skin.art_count ? skin.art_width : 0;
	int side = art_width && cols - skin.margin * 2 >= art_width + 34;
	int x = skin.margin + (side ? art_width + 4 : 0);
	int available = cols - x - skin.margin;
	if (side) {
		for (int i = 0; i < skin.art_count; i++) {
			welcome_text(top + i, skin.margin, art_width, welcome_art_line(i), 28, 0);
		}
	}
	char heading[160];
	welcome_expand(skin.welcome_title, heading, sizeof(heading));
	welcome_text(top, x, available, heading, 28, 1);
	welcome_text(top + 1, x, available, "────────────────────────", 3, 0);
	for (int i = 0; i < skin.welcome_field_count; i++) {
		const char *field = skin.welcome_fields[i], *value = "", *label = "";
		if (!strcmp(field, "model")) {
			label = "Model";
			value = footer_model;
		} else if (!strcmp(field, "effort")) {
			label = "Effort";
			value = !strcmp(current_effort, "none") ? "off" : current_effort;
		} else if (!strcmp(field, "path")) {
			label = "Project";
			value = welcome_path;
		} else if (!strcmp(field, "theme")) {
			label = "Theme";
			value = *theme_current_name ? theme_current_name : "default";
		} else if (!strcmp(field, "memory")) {
			label = "Memory";
			value = welcome_memory;
		} else if (!strcmp(field, "version")) {
			label = "Version";
			value = PHI_VERSION;
		}
		char key[24];
		snprintf(key, sizeof(key), "%-8s", label);
		welcome_text(top + 2 + i, x, available, key, 28, 1);
		welcome_text(top + 2 + i, x + 9, available - 9, *value ? value : "—", 3, 0);
	}
	(void)height;
}

static long scene_tick = -1;

static int welcome_scene_animated(void) {

	if (skin.welcome != SKIN_WELCOME_SCENE || used || modal_active || working) {
		return 0;
	}
	for (int i = 0; i < skin.scene_count; i++) {
		if (skin.scene[i].frame_count > 1) {
			return 1;
		}
	}

	return 0;
}

static void welcome_scene_render(const struct skin *scene, int height, int cols) {

	for (int i = 0; i < scene->scene_count; i++) {
		const struct welcome_element *e = &scene->scene[i];
		int frame = e->frame_count > 1 ? (long)(now() * 1000 / e->interval) % e->frame_count : 0;
		char text[8192];
		welcome_expand(e->frames[frame], text, sizeof(text));
		int lines = 1, width = 0;
		for (char *p = text; *p; p++) {
			if (*p == '\n') {
				lines++;
			}
		}
		char *p = text;
		while (p) {
			char *end = strchr(p, '\n');
			if (end) {
				*end = 0;
			}
			int w = skin_width(p);
			if (w > width) {
				width = w;
			}
			if (end) {
				*end = '\n';
				p = end + 1;
			} else {
				p = NULL;
			}
		}
		int x = e->x +
			(e->anchor == 1          ? (cols - width) / 2
					: e->anchor == 2 ? cols - width - skin.margin
									 : skin.margin);
		int y = e->y +
			(e->anchor == 1          ? (height - lines) / 2
					: e->anchor == 2 ? height - lines
									 : 0);
		p = text;
		while (p) {
			char *end = strchr(p, '\n');
			if (end) {
				*end = 0;
			}
			int draw_x = x;
			const char *visible = p;
			mbstate_t state = {0};
			while (draw_x < 0 && *visible) {
				wchar_t ch;
				size_t n = mbrtowc(&ch, visible, strlen(visible), &state);
				if (n == (size_t)-1 || n == (size_t)-2 || !n) {
					break;
				}
				int cells = wcwidth(ch);
				draw_x += cells > 0 ? cells : 0;
				visible += n;
			}
			if (draw_x >= 0 && draw_x < cols) {
				welcome_text(y, draw_x, cols - draw_x, visible, e->color, e->bold);
			}
			y++;
			p = end ? end + 1 : NULL;
		}
	}
}

static void welcome_header(int height, int cols, int scroll) {

	welcome_scroll = scroll;
	welcome_clip = height;
	int x = skin.margin;
	int top = height >= 5 ? 1 : 0;
	if (skin.welcome == SKIN_WELCOME_NONE) {
		welcome_scroll = 0;
		welcome_clip = -1;
		return;
	}
	if (skin.welcome == SKIN_WELCOME_FETCH) {
		welcome_fetch(height, cols, top);
		welcome_scroll = 0;
		welcome_clip = -1;
		return;
	}
	if (skin.welcome == SKIN_WELCOME_CINEMA) {
		if (used || working) {
			int banner = welcome_banner_height(height);
			if (!cinema_ready()) {
				char heading[256];
				welcome_expand(skin.welcome_title, heading, sizeof(heading));
				welcome_text((banner - 2) / 2, (cols - 16) / 2, 24, heading, 28, 1);
			}
			int rule = banner - 1 - scroll;
			if (!header_motion_gone() && header_motion_body_visible() && rule >= 0 &&
				rule < height) {
				attrset(COLOR_PAIR(5));
				mvhline(rule, skin.margin, ACS_HLINE, cols - skin.margin * 2);
				attrset(A_NORMAL);
				if (skin.welcome_life && cinema_ready()) {
					/* rule is already scroll-adjusted; welcome_text adjusts it. */
					welcome_text(rule + scroll, skin.margin + 2, cols - skin.margin * 2 - 4,
						cinema_controls(), 3, 0);
				}
			}
		} else if (!cinema_ready()) {
			int y = height / 2 - 1;
			char heading[128];
			welcome_expand(skin.welcome_title, heading, sizeof(heading));
			welcome_text(y, (cols - 16) / 2, 24, heading, 28, 1);
			welcome_text(y + 2, (cols - 24) / 2, 32, "Type a message to begin", 3, 0);
		}
		welcome_scroll = 0;
		welcome_clip = -1;
		return;
	}
	if (skin.welcome == SKIN_WELCOME_SCENE) {
		if (!used) {
			welcome_scene_render(&skin, height, cols);
		}
		welcome_scroll = 0;
		welcome_clip = -1;
		return;
	}
	brand_header_draw(&skin, top, x, cols - x - 2, brand_header_rows(&skin), 28, 27, 3,
		welcome_brand_art, welcome_brand_expand, welcome_brand_paint, NULL);
	welcome_scroll = 0;
	welcome_clip = -1;
}

static void welcome_draw(int height, int cols) {

	welcome_header(height, cols, 0);
}

/* One quiet row, using the same template and UTF-8 clipping primitives as
   other chrome. Reserve the model first so long project names never collide. */
static void chrome_header_row(int cols) {

	brand_compact_draw(&skin, 0, skin.margin, cols - 2 * skin.margin, 28, 3, welcome_brand_expand,
		welcome_brand_paint, NULL);
}

static void welcome_footer(int row, int cols) {

	if (!skin.footer_visible) {
		return;
	}
	char left[4224], right[512], context[96], weekly[128];
	const char *mode = editor_mode_label();
	snprintf(left, sizeof(left), "%s", mode);
	if (*footer_text) { /* an extension replaced the footer */
		strncat(left, " · ", sizeof(left) - strlen(left) - 1);
		strncat(left, footer_text, sizeof(left) - strlen(left) - 1);
		markup_text(row, skin.margin, cols - 2 * skin.margin, left, 3, 0);
		return;
	}
	if (*extension_footer) {
		strncat(left, " · ", sizeof(left) - strlen(left) - 1);
		strncat(left, extension_footer, sizeof(left) - strlen(left) - 1);
	}
	strncat(left, " · ", sizeof(left) - strlen(left) - 1);
	/* Extension status can carry markup; a brace in the path stays a brace. */
	for (const char *c = welcome_path; *c && strlen(left) + 3 < sizeof(left); c++) {
		size_t at = strlen(left);
		left[at] = *c;
		left[at + 1] = 0;
		if (*c == '{' && *extension_footer) {
			strcat(left, "{");
		}
	}
	if (queued_count) {
		char queued_label[64];
		snprintf(queued_label, sizeof(queued_label), " · %zu queued", queued_count);
		strncat(left, queued_label, sizeof(left) - strlen(left) - 1);
	}
	context_summary(context, sizeof(context));
	snprintf(weekly, sizeof(weekly), "%.127s", account_usage);
	snprintf(right, sizeof(right), "%s%s%s%s%s", footer_model, *context ? " · ctx " : "", context,
		*weekly ? " · " : "", weekly);
	if (skin.footer_left_set) {
		welcome_expand(skin.footer_left, left, sizeof(left));
	}
	if (skin.footer_right_set) {
		welcome_expand(skin.footer_right, right, sizeof(right));
	}
	/* These labels are ASCII except for one-cell middle dots. */
	int right_width = skin_width(right);
	if (right_width < 0) {
		right_width = 0;
	}
	if (!skin.footer_right_set && (*context || *weekly) && right_width > cols - 14) {
		snprintf(right, sizeof(right), "%s%s%s", *context ? "ctx " : "", context,
			*weekly ? (*context ? " · " : weekly) : "");
		if (*weekly && *context) {
			strncat(right, weekly, sizeof(right) - strlen(right) - 1);
		}
		right_width = skin_width(right);
		if (right_width < 0) {
			right_width = 0;
		}
	}
	int right_x = cols - skin.margin - right_width;
	int split = right_x > 10;
	int left_width = split ? right_x - skin.margin - 2 : cols - 2 * skin.margin;
	if (*extension_footer && !skin.footer_left_set) {
		markup_text(row, skin.margin, left_width, left, 3, 0);
	} else {
		welcome_text(row, skin.margin, left_width, left, 3, 0);
	}
	if (split) {
		welcome_text(row, right_x, right_width, right, 3, 0);
	}
}
