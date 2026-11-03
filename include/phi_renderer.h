#ifndef PHI_RENDERER_H
#define PHI_RENDERER_H
#include "phi_widget.h"
#include <stddef.h>
#include <stdint.h>
#define PHI_RENDERER_ABI 1u
#define PHI_RENDERER_MAX_CELLS 32768u
#define PHI_RENDERER_MAX_ROWS 256
#define PHI_RENDERER_MAX_JSON 65536u

/* Optional entry in a library registered through the approved surface registry.
   Pure, bounded, synchronous snapshot projection: no terminal I/O, UI re-entry,
   threads, tool execution or permission decisions. Borrowed inputs expire on
   return. No retained state. The same accepted frame supplies measurement,
   painting and selectable text; Phi caches it across both layout passes.
   Return occupied rows (1..max_rows), or 0 to use Phi's compatibility renderer.
   Cells use phi_widget_cell colours/flags. Glyph 0 is a blank/continuation;
   double-width glyphs need a zero continuation cell. Combining glyphs are not
   supported in this initial ABI. Custom hit/action maps are not exposed yet. */
struct phi_renderer_frame {
	uint32_t size;
	int width, max_rows;
	const char *context_json; /* schema 1: immutable tool or versioned message snapshots */
};

struct phi_renderer_v1 {
	uint32_t abi;
	size_t size;
	int (*render)(const struct phi_renderer_frame *, struct phi_widget_cell *, size_t capacity);
};
const struct phi_renderer_v1 *phi_renderer_entry(void);
#endif
