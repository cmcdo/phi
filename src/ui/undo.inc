/* The draft's undo tree, as in vim. Every change adds a state as a child of
   the current one, so undoing and then typing starts a new branch instead of
   losing the old one.
     u, Ctrl+Z        back to the parent state
     Ctrl+R, Ctrl+Y   forward to the child visited last (else the newest)
     g-, g+           the previous / next state in the order they were made
     U                the tree itself (undotree.inc)
   Typing is grouped a word at a time: "fix the bug" is three states.
   Up to UNDO_NODES states are kept; the oldest leaves go first. Included by
   ui.c before the editing code. */

#define UNDO_NODES 256

struct undo_node {
	wchar_t *text; /* NULL: a free slot */
	size_t length, cursor;
	int parent, child; /* child: the one to redo into */
	int typed; /* made by typing, so more typing can extend it */
	unsigned seq; /* order of creation, for g-/g+ */
	double when;
	size_t replace_start; /* r's change position + 1; zero for other edits */
};
static struct undo_node undo_nodes[UNDO_NODES];
static int undo_current = -1;
static unsigned undo_seq;
static int history_action; /* the last key moved in the tree; not a change */

static void undo_free(int node) {

	free(undo_nodes[node].text);
	undo_nodes[node] = (struct undo_node){0};
}

/* Make room: drop the oldest leaf, or the root when it has one child. */
static int undo_evict(void) {

	int victim = -1;
	for (int i = 0; i < UNDO_NODES; i++) {
		if (!undo_nodes[i].text || i == undo_current) {
			continue;
		}
		int children = 0;
		for (int j = 0; j < UNDO_NODES; j++) {
			if (undo_nodes[j].text && undo_nodes[j].parent == i) {
				children++;
			}
		}
		int removable = !children || (undo_nodes[i].parent < 0 && children == 1);
		if (removable && (victim < 0 || undo_nodes[i].seq < undo_nodes[victim].seq)) {
			victim = i;
		}
	}
	if (victim < 0) {
		return -1;
	}
	for (int j = 0; j < UNDO_NODES; j++) {
		if (undo_nodes[j].text && undo_nodes[j].parent == victim) {
			undo_nodes[j].parent = -1;
		}
	}
	int parent = undo_nodes[victim].parent;
	if (parent >= 0 && undo_nodes[parent].child == victim) {
		undo_nodes[parent].child = -1;
	}
	undo_free(victim);

	return victim;
}

static int undo_store(const wchar_t *text, size_t n, size_t at, int parent) {

	int slot = -1;
	for (int i = 0; i < UNDO_NODES && slot < 0; i++) {
		if (!undo_nodes[i].text) {
			slot = i;
		}
	}
	if (slot < 0) {
		slot = undo_evict();
	}
	if (slot < 0) {
		return -1;
	}
	wchar_t *copy = malloc((n + 1) * sizeof(*copy));
	if (!copy) {
		return -1;
	}
	memcpy(copy, text, n * sizeof(*copy));
	copy[n] = 0;
	undo_nodes[slot] = (struct undo_node){copy, n, at, parent, -1, 0, ++undo_seq, now(), 0};

	return slot;
}

static void undo_clear(void) {

	for (int i = 0; i < UNDO_NODES; i++) {
		if (undo_nodes[i].text) {
			undo_free(i);
		}
	}
	undo_current = -1;
}

/* The draft changed from before to the current input: add a state. */
static void undo_record(const wchar_t *before, size_t before_length, size_t before_cursor) {

	struct undo_node *current = undo_current >= 0 ? &undo_nodes[undo_current] : NULL;
	if (!current || current->length != before_length ||
		wmemcmp(current->text, before, before_length)) {
		/* The first change, or one made without passing through here. */
		int base = undo_store(before, before_length, before_cursor, undo_current);
		if (base < 0) {
			return;
		}
		if (undo_current >= 0) {
			undo_nodes[undo_current].child = base;
		}
		undo_current = base;
	}
	/* One character typed at the cursor: extend the state being typed,
       unless it starts a new word or you have undone past it. */
	current = &undo_nodes[undo_current];
	int typed = length == before_length + 1 && cursor == before_cursor + 1 &&
		!wmemcmp(input, before, before_cursor) &&
		!wmemcmp(input + cursor, before + before_cursor, before_length - before_cursor);
	wchar_t added = typed ? input[before_cursor] : 0;
	int new_word =
		iswspace((wint_t)added) && before_cursor && !iswspace((wint_t)before[before_cursor - 1]);
	if (typed && current->typed && current->cursor == before_cursor && !new_word &&
		current->parent >= 0) {
		int has_children = 0;
		for (int i = 0; i < UNDO_NODES && !has_children; i++) {
			has_children = undo_nodes[i].text && undo_nodes[i].parent == undo_current;
		}
		wchar_t *grown =
			has_children ? NULL : realloc(current->text, (length + 1) * sizeof(*grown));
		if (grown) {
			memcpy(grown, input, (length + 1) * sizeof(*grown));
			current->text = grown;
			current->length = length;
			current->cursor = cursor;
			current->when = now();
			return;
		}
	}
	int node = undo_store(input, length, cursor, undo_current);
	if (node < 0) {
		return;
	}
	undo_nodes[node].typed = typed;
	if (replace_changed) {
		undo_nodes[node].replace_start = replace_start + 1;
	}
	undo_nodes[undo_current].child = node;
	undo_current = node;
}

/* Show state node in the draft. */
static void undo_go(int node) {

	if (node < 0 || !undo_nodes[node].text) {
		return;
	}
	struct undo_node *state = &undo_nodes[node];
	if (!input_reserve(state->length)) {
		return;
	}
	memcpy(input, state->text, (state->length + 1) * sizeof(*input));
	length = state->length;
	cursor = state->cursor <= length ? state->cursor : length;
	visual_mode = delete_pending = 0;
	input_view = 0;
	undo_current = node;
	history_action = 1;
	/* Redo retraces the path to where you are now, as in vim. */
	for (int at = node; undo_nodes[at].parent >= 0; at = undo_nodes[at].parent) {
		undo_nodes[undo_nodes[at].parent].child = at;
	}
}

/* -1: to the parent (undo). +1: to the child visited last, else the newest. */
static void undo_step(int direction) {

	if (undo_current < 0) {
		return;
	}
	if (direction < 0) {
		size_t at = undo_nodes[undo_current].replace_start;
		int parent = undo_nodes[undo_current].parent;
		undo_go(parent);
		if (at && parent >= 0 && undo_current == parent) {
			cursor = at - 1;
			if (normal_mode) {
				normal_cursor();
			}
		}
		return;
	}
	int next = undo_nodes[undo_current].child;
	if (next < 0 || !undo_nodes[next].text || undo_nodes[next].parent != undo_current) {
		next = -1;
		for (int i = 0; i < UNDO_NODES; i++) {
			if (undo_nodes[i].text && undo_nodes[i].parent == undo_current &&
				(next < 0 || undo_nodes[i].seq > undo_nodes[next].seq)) {
				next = i;
			}
		}
	}
	undo_go(next);
	if (next >= 0 && undo_current == next && undo_nodes[next].replace_start) {
		cursor = undo_nodes[next].replace_start - 1;
		if (normal_mode) {
			normal_cursor();
		}
	}
}

/* g- / g+: the state made just before / after the current one. */
static void undo_time(int direction) {

	if (undo_current < 0) {
		return;
	}
	unsigned seq = undo_nodes[undo_current].seq;
	int best = -1;
	for (int i = 0; i < UNDO_NODES; i++) {
		if (!undo_nodes[i].text) {
			continue;
		}
		unsigned s = undo_nodes[i].seq;
		if (direction < 0 ? s < seq && (best < 0 || s > undo_nodes[best].seq)
						  : s > seq && (best < 0 || s < undo_nodes[best].seq)) {
			best = i;
		}
	}
	if (best < 0) {
		return;
	}
	undo_go(best);
}

static void undo_tree_view(void);
