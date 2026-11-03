/* User-selected PixelPlanets artwork, baked into native terminal cells.
   No image decoding, terminal escapes, subprocesses or render loop. */
#include "../../include/phi_widget.h"
#include <stdlib.h>
#include "planet-frames.inc"

struct orbit {
	unsigned frame;
	int dirty;
	struct phi_widget_cell cells[34 * 18];
};

static void *create(void) {

	struct orbit *s = calloc(1, sizeof(*s));
	if (s) {
		s->dirty = 1;
	}

	return s;
}

static void destroy(void *p) {

	free(p);
}

static int tick(void *p) {

	struct orbit *s = p;
	s->frame = (s->frame + 1) % PLANET_FRAMES;
	s->dirty = 1;

	return 1;
}

static int delay(void *p) {

	return planet_delay[((struct orbit *)p)->frame] * 8 / 5;
}

/* Sample the baked 32x32 pixels with transparent padding. */
static unsigned pixel(unsigned frame, int x, int y) {

	if (x < 0 || x >= 32 || y < 0 || y >= 32) {
		return 0;
	}
	unsigned packed = planet_frames[frame][(y / 2) * 32 + x];
	return (packed >> ((y & 1) * 8)) & 255;
}

static unsigned shaded_pixel(unsigned frame, int x, int y) {

	unsigned color = pixel(frame, x, y);
	if (!color) {
		return 0;
	}
	/* A fixed light source: retain the artwork on the day side, with a
	   stepped, neutral night-side shadow in the original pixel-art style. */
	int dx = x - 15, dy = y - 15;
	if (dx * dx + dy * dy > 180 && x + y > 36) {
		return 235;
	}
	if (x + y > 43) {
		return 238;
	}
	return color;
}

static const struct phi_widget_cell *render(void *p) {

	struct orbit *s = p;
	if (!s->dirty) {
		return s->cells;
	}
	for (int i = 0; i < 34 * 18; i++) {
		int x = i % 34 - 1, y = (i / 34) * 2 - 2;
		unsigned foreground = shaded_pixel(s->frame, x, y);
		unsigned background = shaded_pixel(s->frame, x, y + 1);
		s->cells[i] = (struct phi_widget_cell){
			foreground == background ? ' ' : 0x2580u, (uint8_t)foreground, (uint8_t)background, 2};
	}
	s->dirty = 0;

	return s->cells;
}

static int input(void *p, const struct phi_widget_input *event) {

	(void)p;
	(void)event;

	return 0;
}
static const struct phi_widget_v1 widget = {PHI_WIDGET_ABI, sizeof(struct phi_widget_v1), 34, 18,
	"", create, destroy, tick, delay, render, input, NULL};

const struct phi_widget_v1 *phi_widget_entry(void) {

	return &widget;
}
