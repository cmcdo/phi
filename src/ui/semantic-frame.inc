/* One accepted native frame supplies measurement, paint and selectable glyphs
   for every semantic kind. No callbacks run independently in a paint pass. */
static int semantic_frame_matches(
	const struct semantic_frame_cache *cache, const char *provider, int width, uint64_t version) {

	return cache->width == width && cache->generation == surfaces_generation() &&
		cache->version == version && !strcmp(cache->provider, provider);
}

static int semantic_frame_prepare(struct semantic_frame_cache *cache, const char *provider,
	int width, uint64_t version, json_object *context) {

	free(cache->cells);
	*cache = (struct semantic_frame_cache){
		.width = width, .generation = surfaces_generation(), .version = version};
	snprintf(cache->provider, sizeof(cache->provider), "%s", provider);
	if (width < 1 || width > 1024) {
		return 0;
	}
	int maximum = (int)(PHI_RENDERER_MAX_CELLS / (unsigned)width);
	if (maximum > PHI_RENDERER_MAX_ROWS) {
		maximum = PHI_RENDERER_MAX_ROWS;
	}
	size_t count = (size_t)width * (size_t)maximum;
	struct phi_widget_cell *cells = calloc(count, sizeof(*cells));
	if (!cells) {
		cache->width = 0;
		return 0;
	}
	int rows = surfaces_render_semantic(provider, width, maximum, context, cells, count);
	for (int row = 0; rows && row < rows; row++) {
		for (int x = 0; x < width; x++) {
			uint32_t glyph = cells[(size_t)row * (size_t)width + (size_t)x].glyph;
			if (!glyph) {
				continue;
			}
			int span = wcwidth((wchar_t)glyph);
			if (span < 1 || span > 2 ||
				(span == 2 &&
					(x + 1 >= width || cells[(size_t)row * (size_t)width + (size_t)x + 1].glyph))) {
				rows = 0;
				break;
			}
			x += span - 1;
		}
	}
	if (rows) {
		cache->cells = cells;
		cache->rows = rows;
	} else {
		free(cells);
	}

	return rows;
}

static void semantic_frame_paint(
	WINDOW *pad, int first, int y, const struct semantic_frame_cache *cache) {

	if (!pad) {
		return;
	}
	wattrset(pad, A_NORMAL);
	int width = cache->width;
	for (int row = 0; row < cache->rows; row++) {
		int destination = y + row - first;
		if (destination < 0 || destination >= getmaxy(pad)) {
			continue;
		}
		for (int x = 0; x < width; x++) {
			struct phi_widget_cell cell = cache->cells[(size_t)row * (size_t)width + (size_t)x];
			wchar_t glyph[2] = {(wchar_t)(cell.glyph ? cell.glyph : ' '), 0};
			cchar_t value;
			if (setcchar(&value, glyph, (cell.flags & 1) ? A_BOLD : A_NORMAL,
					surface_pair(cell, 27), NULL) == OK) {
				mvwadd_wch(pad, destination, x, &value);
			}
			if (cell.glyph && wcwidth(glyph[0]) == 2) {
				x++;
			}
		}
	}
	wattrset(pad, A_NORMAL);
}
