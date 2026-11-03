#ifndef PHI_WIDGET_H
#define PHI_WIDGET_H
#include <stddef.h>
#include <stdint.h>

/* Experimental trusted native header-widget ABI. Phi owns ncurses output,
   clipping, timers and pointer capture. Callbacks run on its UI event loop.
   No terminal escapes, subprocesses, threads or private render loops.
   Colour 0 inherits the theme background; other colours use the RGB cube
   r*16+g*4+b (components 0..3). Cell flag bit 0 is bold; bit 1 selects
   xterm-256 colour indices instead of the cube. Zero still inherits the
   background. Glyphs must occupy one terminal cell. */
#define PHI_WIDGET_ABI 2u

enum {
	PHI_WIDGET_PRESS,
	PHI_WIDGET_DRAG,
	PHI_WIDGET_RELEASE,
	PHI_WIDGET_CLICK,
	PHI_WIDGET_CONTROL
};

enum {
	PHI_WIDGET_HANDLED = 1,
	PHI_WIDGET_CHANGED = 2,
	PHI_WIDGET_CAPTURE = 4,
	PHI_WIDGET_RELEASE_CAPTURE = 8
};

struct phi_widget_cell {
	uint32_t glyph;
	uint8_t foreground, background;
	uint16_t flags;
};

struct phi_widget_input {
	int kind, x, y, button;
}; /* widget-local terminal cells */

struct phi_widget_v1 {
	uint32_t abi;
	size_t size;
	int width, height;
	const char *controls;
	void *(*create)(void);
	void (*destroy)(void *);
	int (*tick)(void *);
	int (*delay_ms)(void *); /* 0: no timer */
	const struct phi_widget_cell *(*render)(void *); /* borrowed width*height cells */
	int (*input)(void *, const struct phi_widget_input *);
	int (*control)(void *, const char *);
};
const struct phi_widget_v1 *phi_widget_entry(void);
#endif
