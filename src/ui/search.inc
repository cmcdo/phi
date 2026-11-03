/* Search the rendered, retained conversation, never the editor draft. */
struct conversation_hit {
	int row, col, cells;
};
static struct conversation_hit *conversation_hits;
static size_t conversation_count, conversation_selected;
static wchar_t conversation_query[128];
static int conversation_search_active;

static size_t conversation_capacity;

/* Adds hits from a pad showing transcript rows first..; a NULL pad resets. */
static void conversation_collect(WINDOW *pad, int first) {

	if (!pad) {
		free(conversation_hits);
		conversation_hits = NULL;
		conversation_count = conversation_capacity = 0;
		return;
	}
	size_t n = wcslen(conversation_query), capacity = conversation_capacity;
	if (!n) {
		return;
	}
	int cols = getmaxx(pad);
	wchar_t *line = calloc((size_t)cols + 1, sizeof(*line));
	if (!line) {
		return;
	}
	for (int row = 0; row < getmaxy(pad); row++) {
		memset(line, 0, ((size_t)cols + 1) * sizeof(*line));
		mvwinnwstr(pad, row, 0, line, cols);
		int col = 0;
		for (size_t at = 0; line[at]; at++) {
			size_t j = 0;
			while (j < n && line[at + j] &&
				towlower(line[at + j]) == towlower(conversation_query[j])) {
				j++;
			}
			if (j == n) {
				if (conversation_count == capacity) {
					size_t next = capacity ? capacity * 2 : 64;
					struct conversation_hit *p = realloc(conversation_hits, next * sizeof(*p));
					if (!p) {
						free(line);
						return;
					}
					conversation_hits = p;
					capacity = conversation_capacity = next;
				}
				int cells = wcswidth(line + at, n);
				if (cells < 1) {
					cells = 1;
				}
				conversation_hits[conversation_count++] =
					(struct conversation_hit){first + row, col, cells};
			}
			int cells = wcwidth(line[at]);
			if (cells > 0) {
				col += cells;
			}
		}
	}
	free(line);
}

/* Hit rows are transcript rows; pad row 0 shows transcript row `first`. */
static void conversation_highlight(WINDOW *pad, int first) {

	if (!conversation_search_active) {
		return;
	}
	for (size_t i = 0; i < conversation_count; i++) {
		struct conversation_hit h = conversation_hits[i];
		if (h.row >= first && h.row - first < getmaxy(pad) && h.col < getmaxx(pad)) {
			mvwchgat(pad, h.row - first, h.col, h.cells,
				i == conversation_selected ? A_REVERSE | A_BOLD : A_UNDERLINE, 28, NULL);
		}
	}
}
