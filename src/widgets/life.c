#include "../../include/phi_widget.h"
#include <stdlib.h>
#include <string.h>

struct life {
	unsigned char cells[64 * 40], previous[64 * 40];
	struct phi_widget_cell surface[128 * 40];
	int paused, speed, painting, paint_value, dirty;
};

static void seed(struct life *s, const char *pattern, int ox, int oy) {

	int x = ox, y = oy;
	for (; *pattern; pattern++) {
		if (*pattern == '\n') {
			y++;
			x = ox;
		} else {
			if (*pattern == 'O') {
				s->cells[y * 64 + x] = 1;
			}
			x++;
		}
	}
}

static void reset(struct life *s) {

	memset(s->cells, 0, sizeof(s->cells));
	const char *p =
		"..OOO...OOO..\n.............\nO....O.O....O\nO....O.O....O\nO....O.O....O\n..OOO...OOO..\n.............\n..OOO...OOO..\nO....O.O....O\nO....O.O....O\nO....O.O....O\n.............\n..OOO...OOO..";
	seed(s, p, 3, 3);
	seed(s, p, 47, 23);
	seed(s, "..O....O..\nOO.OOOO.OO\n..O....O..", 27, 18);
	seed(s, "OOO", 8, 31);
	seed(s, "OOO", 52, 8);
	memcpy(s->previous, s->cells, sizeof(s->cells));
	s->paused = 0;
	s->dirty = 1;
}

static void *create(void) {

	struct life *s = calloc(1, sizeof(*s));
	if (s) {
		s->speed = 140;
		reset(s);
	}

	return s;
}

static void destroy(void *data) {

	free(data);
}

static int step(void *data) {

	struct life *s = data;
	unsigned char next[64 * 40];
	for (int y = 0; y < 40; y++) {
		for (int x = 0; x < 64; x++) {
			int n = 0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					if (dx || dy) {
						n += s->cells[((y + dy + 40) % 40) * 64 + (x + dx + 64) % 64];
					}
				}
			}
			next[y * 64 + x] = n == 3 || (n == 2 && s->cells[y * 64 + x]);
		}
	}
	memcpy(s->previous, s->cells, sizeof(s->cells));
	memcpy(s->cells, next, sizeof(next));
	s->dirty = 1;

	return 1;
}

static int delay(void *data) {

	struct life *s = data;

	return s->paused ? 0 : s->speed;
}

static const struct phi_widget_cell *render(void *data) {

	struct life *s = data;
	if (!s->dirty) {
		return s->surface;
	}
	for (int y = 0; y < 40; y++) {
		for (int x = 0; x < 64; x++) {
			int at = y * 64 + x;
			int color = s->cells[at] ? (s->previous[at] ? 10 : 61) : s->previous[at] ? 5 : 0;
			struct phi_widget_cell cell = {color ? 0x2588u : ' ', (uint8_t)color, 0, 0};
			/* Two columns per Life square correct terminal-cell aspect ratio. */
			s->surface[y * 128 + x * 2] = cell;
			s->surface[y * 128 + x * 2 + 1] = cell;
		}
	}
	s->dirty = 0;

	return s->surface;
}

static int control(void *data, const char *action) {

	struct life *s = data;
	if (!strcmp(action, "pause")) {
		s->paused = !s->paused;
	} else if (!strcmp(action, "resume")) {
		s->paused = 0;
	} else if (!strcmp(action, "step")) {
		s->paused = 1;
		step(s);
	} else if (!strcmp(action, "reset")) {
		reset(s);
	} else if (!strcmp(action, "clear")) {
		memset(s->cells, 0, sizeof(s->cells));
		memset(s->previous, 0, sizeof(s->previous));
		s->paused = 1;
	} else if (!strcmp(action, "slower")) {
		if (s->speed < 2000) {
			s->speed += 25;
		}
	} else if (!strcmp(action, "faster")) {
		if (s->speed > 50) {
			s->speed -= 25;
		}
	} else if (!strncmp(action, "speed ", 6)) {
		char *end;
		long value = strtol(action + 6, &end, 10);
		if (*end || value < 50 || value > 2000) {
			return 0;
		}
		s->speed = (int)value;
	} else {
		return 0;
	}
	if (s->speed < 50) {
		s->speed = 50;
	}
	if (s->speed > 2000) {
		s->speed = 2000;
	}
	s->dirty = 1;

	return 1;
}

static int input(void *data, const struct phi_widget_input *event) {

	struct life *s = data;
	if (event->kind == PHI_WIDGET_RELEASE && s->painting) {
		s->painting = 0;
		return PHI_WIDGET_HANDLED | PHI_WIDGET_RELEASE_CAPTURE;
	}
	if (event->kind == PHI_WIDGET_CONTROL) {
		const char *action = event->x < 6 ? "pause"
			: event->x < 11               ? "step"
			: event->x < 17               ? "reset"
			: event->x < 23               ? "clear"
			: event->x < 25               ? "slower"
										  : "faster";
		if (event->x < 0 || event->x >= 26) {
			return 0;
		}
		return control(s, action) ? PHI_WIDGET_HANDLED | PHI_WIDGET_CHANGED : 0;
	}
	if (event->x < 0 || event->x >= 128 || event->y < 0 || event->y >= 40) {
		return s->painting ? PHI_WIDGET_HANDLED : 0;
	}
	if (event->button == 3 &&
		(event->kind == PHI_WIDGET_PRESS || event->kind == PHI_WIDGET_CLICK)) {
		control(s, "pause");
		return PHI_WIDGET_HANDLED | PHI_WIDGET_CHANGED;
	}
	if (event->button == 2 &&
		(event->kind == PHI_WIDGET_PRESS || event->kind == PHI_WIDGET_CLICK)) {
		control(s, "step");
		return PHI_WIDGET_HANDLED | PHI_WIDGET_CHANGED;
	}
	int at = event->y * 64 + event->x / 2, capture = 0;
	if (event->kind == PHI_WIDGET_CLICK && event->button == 1) {
		s->cells[at] = !s->cells[at];
		s->previous[at] = s->cells[at];
		s->dirty = 1;
		return PHI_WIDGET_HANDLED | PHI_WIDGET_CHANGED;
	}
	if (event->kind == PHI_WIDGET_PRESS && event->button == 1 && !s->painting) {
		s->painting = 1;
		s->paint_value = !s->cells[at];
		capture = PHI_WIDGET_CAPTURE;
	}
	if (!s->painting) {
		return 0;
	}
	int changed = s->cells[at] != s->paint_value || s->previous[at] != s->paint_value;
	s->cells[at] = (unsigned char)s->paint_value;
	s->previous[at] = s->cells[at];
	s->dirty |= changed;

	return PHI_WIDGET_HANDLED | capture | (changed ? PHI_WIDGET_CHANGED : 0);
}
static const struct phi_widget_v1 widget = {PHI_WIDGET_ABI, sizeof(struct phi_widget_v1), 128, 40,
	"Pause Step Reset Clear - +", create, destroy, step, delay, render, input, control};

const struct phi_widget_v1 *phi_widget_entry(void) {

	return &widget;
}
