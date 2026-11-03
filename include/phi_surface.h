#ifndef PHI_SURFACE_H
#define PHI_SURFACE_H
#include <stddef.h>
#include <stdint.h>
#include "phi_widget.h"
#define PHI_SURFACE_ABI 1u
#define PHI_SURFACE_MAX_CELLS 32768u

/* Approved native code; callbacks run on Phi's existing loop. No terminal I/O,
   blocking, UI re-entry, threads or private render loops. Borrowed inputs expire
   on return. Phi owns cells and clips output; glyph 0 is transparent. */
struct phi_surface_frame {
	uint32_t size;
	const char *slot; /* composition instance ID or legacy composer/transcript/footer/board slot */
	int width, height;
	double time; /* monotonic seconds */
	const char *context_json; /* immutable, slot-specific snapshot; never auth */
};

struct phi_surface_event {
	int kind, x, y, button;
}; /* PHI_WIDGET_* pointer events only */

struct phi_surface_v1 {
	uint32_t abi;
	size_t size;
	void *(*create)(void);
	void (*destroy)(void *);
	int (*render)(
		void *, const struct phi_surface_frame *, struct phi_widget_cell *, size_t capacity);
	int (*delay_ms)(void *); /* 0: context/event changes only; otherwise capped at 20 FPS */
	int (*input)(
		void *, const struct phi_surface_frame *, const struct phi_surface_event *); /* optional */
};
const struct phi_surface_v1 *phi_surface_entry(void);
#endif
