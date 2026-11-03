#ifndef PHI_UI_FRAME_H
#define PHI_UI_FRAME_H
/* One terminal-independent plan for core chrome. Measurements contain no
   windows, providers or editor ownership; arranging never paints or mutates UI. */
#include "ui-layout.h"
#include <limits.h>

enum {
	PHI_FRAME_COMPOSER,
	PHI_FRAME_TRANSCRIPT,
	PHI_FRAME_FOOTER,
	PHI_FRAME_PANELS
};

struct phi_frame_measure {
	struct phi_rect owners[PHI_FRAME_PANELS]; /* physical screen rectangles */
	struct phi_rect input_columns; /* editor-local x/width */
	int split, footer_detached, margin;
	int footer_visible, footer_rows, footer_lines;
	int composer_min_rows, composer_max_rows, composer_max_percent;
	int draft_rows, caret_row, input_view;
	int above_rows, below_rows, header_rows, side_rows, queue_rows;
	int panels[PHI_FRAME_PANELS]; /* intrinsic, already resource-bounded */
};

struct phi_frame_plan {
	struct phi_rect owners[PHI_FRAME_PANELS];
	/* Rectangles are local to their owner: header/view to transcript;
       input/widgets/side/queue/chrome to composer; footer_text to footer. */
	struct phi_rect input, view, header, footer_text;
	struct phi_rect above, below, side, queue, chrome;
	struct phi_rect panels[PHI_FRAME_PANELS];
	int input_view, border_bottom, status_row;
};

static inline int phi_frame_rows(int requested, int available) {

	return requested < 1 || available < 1 ? 0 : requested < available ? requested : available;
}

static inline struct phi_rect phi_frame_input_columns(
	int width, int rounded, int marker, int padding, int compact) {

	int left = (rounded ? 2 : 0) + (marker ? 2 : 0) + padding;
	if (left > width - 4) {
		left = width - 4;
	}
	if (left < 0) {
		left = 0;
	}
	int columns = width - left - padding - (rounded ? 3 : compact ? 0 : 2);
	if (columns < 4) {
		columns = 4;
	}

	return (struct phi_rect){left, 0, columns, 0};
}

static inline struct phi_rect phi_frame_screen_rect(struct phi_rect local, struct phi_rect owner) {

	local.x += owner.x;
	local.y += owner.y;

	return local;
}

static inline int phi_frame_hit(struct phi_rect box, int *x, int *y) {

	if (!phi_rect_contains(box, *x, *y)) {
		return 0;
	}
	*x -= box.x;
	*y -= box.y;

	return 1;
}

static inline int phi_frame_editor_rows(
	int owner_rows, int available, int minimum, int maximum, int percent, int draft) {

	int cap = (int)((long long)owner_rows * percent / 100);
	if (cap > available) {
		cap = available;
	}
	if (cap < 1) {
		cap = 1;
	}
	if (cap > maximum) {
		cap = maximum;
	}
	int floor = phi_frame_rows(minimum, cap), visible = draft < cap ? draft : cap;

	return visible < floor ? floor : visible;
}

static inline int phi_frame_arrange(const struct phi_frame_measure *m, struct phi_frame_plan *p) {

	*p = (struct phi_frame_plan){0};
	struct phi_rect editor = m->owners[PHI_FRAME_COMPOSER];
	struct phi_rect transcript = m->owners[PHI_FRAME_TRANSCRIPT];
	struct phi_rect footer = m->owners[PHI_FRAME_FOOTER];
	int rows = editor.height, cols = editor.width;
	if (rows < 6 || cols < 20 || transcript.width < 20 || transcript.height < (m->split ? 4 : 6) ||
		footer.width < 1 || footer.height < 1 || rows > INT_MAX / 8 ||
		transcript.height > INT_MAX / 8 || m->composer_max_percent < 0 ||
		m->composer_max_percent > 100 || m->composer_max_rows < 1 || m->draft_rows < 1 ||
		m->caret_row < 0 || m->caret_row >= m->draft_rows || m->margin < 0 ||
		m->margin > cols / 2 || m->input_columns.x < 0 || m->input_columns.width < 1 ||
		m->input_columns.x > cols || m->input_columns.width > cols - m->input_columns.x) {
		return 0;
	}
	memcpy(p->owners, m->owners, sizeof(p->owners));
	int footer_rows = m->footer_visible ? m->footer_rows : 0;
	int lines = phi_frame_rows(m->footer_lines, 7);
	if (m->footer_visible && footer_rows < lines + 1) {
		footer_rows = lines + 1;
	}
	if (footer_rows > 8) {
		footer_rows = 8;
	}
	footer_rows = phi_frame_rows(footer_rows, m->footer_detached ? footer.height : rows - 10);
	if (m->footer_visible && footer_rows < 1) {
		footer_rows = 1;
	}
	p->footer_text = (struct phi_rect){0, footer.height - footer_rows, footer.width, footer_rows};
	int footer_panel = phi_frame_rows(m->panels[PHI_FRAME_FOOTER],
		m->footer_detached ? footer.height - footer_rows : rows - footer_rows - 8);
	int reserved_footer = m->footer_detached ? 0 : footer_rows + footer_panel;
	p->panels[PHI_FRAME_FOOTER] =
		(struct phi_rect){m->margin, m->footer_detached ? 0 : rows - reserved_footer,
			footer.width > 2 * m->margin ? footer.width - 2 * m->margin : 0, footer_panel};
	int visible = phi_frame_editor_rows(rows, rows - reserved_footer - (m->split ? 3 : 7),
		m->composer_min_rows, m->composer_max_rows, m->composer_max_percent, m->draft_rows);
	int below = phi_frame_rows(m->below_rows, rows);
	int above = phi_frame_rows(m->above_rows, rows);
	int side = phi_frame_rows(m->side_rows, rows);
	int queue = phi_frame_rows(m->queue_rows, rows);
	int header = phi_frame_rows(m->header_rows, transcript.height);
	above += side + queue;
	int transcript_panel = phi_frame_rows(m->panels[PHI_FRAME_TRANSCRIPT],
		m->split ? transcript.height - header - 4
				 : rows - reserved_footer - visible - above - header - 7);
	header += transcript_panel;
	if (m->split && header > transcript.height - 4) {
		header = 0;
		transcript_panel = 0;
	}
	int composer_panel = phi_frame_rows(
		m->panels[PHI_FRAME_COMPOSER], rows - reserved_footer - visible - above - header - 7);
	above += composer_panel;
	int bottom = rows - 1 - reserved_footer;
	if (bottom - below - visible - 2 - above - (m->split ? 0 : header) < (m->split ? 0 : 4)) {
		below = above = side = queue = composer_panel = 0;
		if (!m->split) {
			header = 0;
			transcript_panel = 0;
		}
	}
	int top = bottom - below - visible, content_bottom = top - 2;
	int height = m->split ? transcript.height - header : content_bottom - above - header;
	if (height < 1) {
		height = 1;
	}
	p->input = m->input_columns;
	p->input.y = top;
	p->input.height = visible;
	p->view = (struct phi_rect){0, header, transcript.width, height};
	p->header = (struct phi_rect){0, 0, transcript.width, header};
	p->border_bottom = bottom - below;
	p->status_row = top - 2;
	p->chrome = (struct phi_rect){
		m->margin, p->status_row, cols - 2 * m->margin, p->border_bottom - p->status_row + 1};
	p->panels[PHI_FRAME_COMPOSER] =
		(struct phi_rect){m->margin, content_bottom - above, cols - 2 * m->margin, composer_panel};
	p->panels[PHI_FRAME_TRANSCRIPT] = (struct phi_rect){
		m->margin, header - transcript_panel, transcript.width - 2 * m->margin, transcript_panel};
	p->above = (struct phi_rect){
		0, content_bottom - above + composer_panel, cols, above - side - queue - composer_panel};
	p->below = (struct phi_rect){0, bottom + 1 - below, cols, below};
	p->side = (struct phi_rect){0, content_bottom - queue - side, cols, side};
	p->queue = (struct phi_rect){0, content_bottom - queue, cols, queue};
	int view = m->input_view;
	if (view > m->caret_row) {
		view = m->caret_row;
	}
	if ((long long)m->caret_row >= (long long)view + visible) {
		view = m->caret_row - visible + 1;
	}
	int last = m->draft_rows > visible ? m->draft_rows - visible : 0;
	if (view > last) {
		view = last;
	}
	p->input_view = view > 0 ? view : 0;

	return 1;
}
#endif
