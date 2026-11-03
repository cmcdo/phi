#ifndef PHI_TRANSITION_H
#define PHI_TRANSITION_H
#include <stdint.h>
#define PHI_TRANSITION_ABI 1u

/* Trusted, in-process pure callbacks. No terminal output, UI calls, blocking,
   retained frame pointers, or threads. Phi owns clipping and scheduling. */
struct phi_transition_frame {
	uint32_t size;
	double time, progress; /* normalized elapsed time and theme-shaped progress */
	int source_offset, rows, columns;
};

struct phi_transition_result {
	int offset_rows; /* -200..500; vertical welcome placement */
	double coverage; /* 0..1; shared image/text visibility */
	int transcript_visible; /* 0 or 1; forced visible at completion */
	int hide_welcome; /* 0 or 1; collapse to message padding once complete */
};

struct phi_transition_v1 {
	uint32_t abi, size;
	void (*sample)(const struct phi_transition_frame *, struct phi_transition_result *);
	/* Optional custom mask. Local coordinates cover the image AND intro text.
       Return nonzero to retain a cell. Dimensions describe that whole block.
       When absent, Phi uses its ordered dither at result.coverage. */
	int (*cell_visible)(const struct phi_transition_frame *, const struct phi_transition_result *,
		int x, int y, int width, int height);
};

/* Export: const struct phi_transition_v1 *phi_transition_entry(void); */
#endif
