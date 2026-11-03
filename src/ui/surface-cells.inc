/* Shared ncurses adapter: no layout/input ownership. Palette 256..383 is
   separate from transcript styles and header-widget pairs (128..255). */
#include "../core/surfaces.h"

static struct {
	short fg, bg;
} surface_pairs[128];

static int surface_pair_count;

static short surface_color(unsigned color, short background, int indexed) {

	if (!color) {
		return background;
	}
	if (indexed) {
		return color < (unsigned)COLORS ? (short)color : background;
	}
	unsigned r = (color >> 4) & 3, g = (color >> 2) & 3, b = color & 3;

	return COLORS >= 256 ? (short)(16 + (r * 5 / 3) * 36 + (g * 5 / 3) * 6 + b * 5 / 3)
						 : (short)(color % (unsigned)(COLORS > 0 ? COLORS : 1));
}

static short surface_pair(struct phi_widget_cell cell, int base_pair) {

	short fg = -1, bg = -1;
	pair_content((short)base_pair, &fg, &bg);
	int indexed = !!(cell.flags & 2);
	short front = surface_color(cell.foreground, bg, indexed),
		  back = surface_color(cell.background, bg, indexed);
	for (int i = 0; i < surface_pair_count; i++) {
		if (surface_pairs[i].fg == front && surface_pairs[i].bg == back) {
			return (short)(256 + i);
		}
	}
	if (surface_pair_count >= 128 || COLOR_PAIRS <= 256 + surface_pair_count) {
		return (short)base_pair;
	}
	int at = surface_pair_count++;
	surface_pairs[at].fg = front;
	surface_pairs[at].bg = back;
	init_pair((short)(256 + at), front, back);

	return (short)(256 + at);
}

static void surface_paint(
	struct surface_client *client, int y, int x, int width, int height, int base_pair) {

	const struct phi_widget_cell *cells = surface_client_cells(client);
	if (!cells) {
		return;
	}
	for (int row = 0; row < height; row++) {
		for (int col = 0; col < width; col++) {
			int sy = y + row, sx = x + col;
			if (sy < 0 || sy >= LINES || sx < 0 || sx >= COLS) {
				continue;
			}
			struct phi_widget_cell cell = cells[(size_t)row * (size_t)width + (size_t)col];
			if (!cell.glyph || cell.glyph > 0x10ffff ||
				(cell.glyph >= 0xd800 && cell.glyph <= 0xdfff)) {
				continue;
			}
			wchar_t glyph[2] = {(wchar_t)cell.glyph, 0};
			if (wcwidth(glyph[0]) != 1) {
				continue;
			}
			cchar_t value;
			if (setcchar(&value, glyph, (cell.flags & 1) ? A_BOLD : A_NORMAL,
					surface_pair(cell, base_pair), NULL) == OK) {
				mvadd_wch(sy, sx, &value);
			}
		}
	}
	attrset(A_NORMAL);
}

static int surface_rows(
	const struct surface_settings *settings, int available, int width, int limit) {

	if (!*settings->id || available < 1 || limit < 1 || width < 1 || width > 1024) {
		return 0;
	}
	int rows = settings->percent ? available * settings->percent / 100 : settings->rows;
	if (rows > limit) {
		rows = limit;
	}
	if (rows > 512) {
		rows = 512;
	}
	if ((size_t)rows * (size_t)width > PHI_SURFACE_MAX_CELLS) {
		rows = (int)(PHI_SURFACE_MAX_CELLS / (unsigned)width);
	}

	return rows > 0 ? rows : 0;
}
