/* In-file history navigation. Selection never runs tools or sends a prompt. */
#include "../core/sessions.h"
#include "../core/json.h"
#include "../core/history.h"

static int tree_visible(const struct session_node *node, int users_only) {

	if (node->kind == SESSION_COMPACTION) {
		return !users_only;
	}
	if (!node->data) {
		return !node->hidden && (node->display & (users_only ? 4u : 3u));
	}
	if (history_is_hidden(node->data)) {
		return 0;
	}
	const char *role = field(node->data, "role");
	if (users_only) {
		return !strcmp(role, "user") &&
			json_object_is_type(json_object_object_get(node->data, "content"), json_type_string);
	}
	/* Default to Pi's useful "no tools" view: conversation points + compactions. */

	return !strcmp(role, "user") || !strcmp(role, "assistant");
}

static void tree_label_data(const struct session_node *node, char *text, size_t size) {

	if (node->kind == SESSION_COMPACTION) {
		snprintf(text, size, "context compaction");
		return;
	}
	const char *type = field(node->data, "type"), *role = field(node->data, "role");
	char content[512] = "";
	session_item_text(node->data, content, sizeof(content));
	for (char *p = content; *p; p++) {
		if (*p == '\n' || *p == '\r' || *p == '\t') {
			*p = ' ';
		}
	}
	if (!*type || !strcmp(type, "message")) {
		snprintf(text, size, "%s%s%s", !strcmp(role, "user") ? "you: " : "assistant: ", content,
			*content ? "" : "(empty)");
	} else if (!strcmp(type, "function_call")) {
		snprintf(text, size, "tool: %s", field(node->data, "name"));
	} else if (!strcmp(type, "function_call_output")) {
		snprintf(text, size, "tool result: %s", field(node->data, "call_id"));
	} else if (!strcmp(type, "reasoning")) {
		snprintf(text, size, "reasoning");
	} else if (!strcmp(type, "image_generation_call")) {
		snprintf(text, size, "image generation");
	} else {
		snprintf(text, size, "%s", type);
	}
}

static void tree_label(
	const struct session_tree *tree, const struct session_node *node, char *text, size_t size) {

	if (node->kind == SESSION_COMPACTION) {
		snprintf(text, size, "context compaction");
		return;
	}
	char error[160];
	json_object *data = session_tree_item(tree, (size_t)(node - tree->nodes), error, sizeof(error));
	if (!data) {
		snprintf(text, size, "[saved record unavailable]");
		return;
	}
	struct session_node copy = *node;
	copy.data = data;
	tree_label_data(&copy, text, size);
	json_object_put(data);
}

static int tree_match(const char *label, const char *query) {

	while (*query) {
		while (*label && tolower((unsigned char)*label) != tolower((unsigned char)*query)) {
			label++;
		}
		if (!*label) {
			return 0;
		}
		label++;
		query++;
	}

	return 1;
}

/* Depth counts branch splits, not ancestry; follow parent IDs for folding. */
static int tree_hidden(const struct session_tree *tree, size_t index, const unsigned char *folded) {

	uint64_t parent = tree->nodes[index].parent;
	for (size_t i = index; i > 0 && parent; i--) {
		if (tree->nodes[i - 1].id != parent) {
			continue;
		}
		if (folded[i - 1]) {
			return 1;
		}
		parent = tree->nodes[i - 1].parent;
	}

	return 0;
}

/* Project hidden tool records out, then walk actual ancestry rather than
   append order. Linear conversation stays flat; only forks add indentation. */
static void tree_branches(
	const struct session_tree *tree, size_t *visible, size_t count, char (*prefix)[128]) {

	if (!count) {
		return;
	}
	size_t *memory = malloc((tree->count + 1 + count * 5) * sizeof(size_t));
	if (!memory) {
		return;
	}
	size_t *map = memory, *parent = map + tree->count + 1, *child = parent + count;
	size_t *next = child + count, *last = next + count, *order = last + count;
	for (size_t i = 0; i <= tree->count; i++) {
		map[i] = count;
	}
	for (size_t i = 0; i < count; i++) {
		map[tree->nodes[visible[i]].id] = i;
		child[i] = next[i] = last[i] = count;
	}
	size_t root = count, root_last = count;
	for (size_t i = 0; i < count; i++) {
		uint64_t id = tree->nodes[visible[i]].parent;
		while (id && map[id] == count) {
			id = tree->nodes[id - 1].parent;
		}
		parent[i] = id ? map[id] : count;
		size_t p = parent[i];
		if (p == count) {
			if (root == count) {
				root = i;
			} else {
				next[root_last] = i;
			}
			root_last = i;
		} else {
			if (child[p] == count) {
				child[p] = i;
			} else {
				next[last[p]] = i;
			}
			last[p] = i;
		}
	}
	size_t at = root, out = 0;
	while (at != count) {
		order[out] = visible[at];
		/* Collect branch ancestors, nearest first; at most 12 visible levels. */
		size_t branches[12], n = 0;
		for (size_t a = at; a != count; a = parent[a]) {
			size_t p = parent[a], first_child = p == count ? root : child[p];
			if (first_child != count && next[first_child] != count && n < 12) {
				branches[n++] = a;
			}
		}
		prefix[out][0] = 0;
		while (n) {
			size_t a = branches[--n];
			const char *part =
				a == at ? (next[a] == count ? "└─ " : "├─ ") : (next[a] == count ? "   " : "│  ");
			strcat(prefix[out], part);
		}
		out++;
		if (child[at] != count) {
			at = child[at];
			continue;
		}
		while (at != count && next[at] == count) {
			at = parent[at];
		}
		if (at != count) {
			at = next[at];
		}
	}
	memcpy(visible, order, count * sizeof(*visible));
	free(memory);
}

uint64_t ui_tree(struct session_tree *tree, int users_only) {

	if (!tree || !tree->count) {
		return 0;
	}
	size_t *visible = malloc(tree->count * sizeof(*visible));
	if (!visible) {
		return 0;
	}
	size_t count = 0;
	for (size_t i = 0; i < tree->count; i++) {
		if (tree_visible(tree->nodes + i, users_only)) {
			visible[count++] = i;
		}
	}
	if (!count) {
		free(visible);
		return 0;
	}

	size_t selected = 0, first = 0;
	/* Default to the most recent visible point on the active branch. */
	for (size_t i = count; i > 0; i--) {
		if (tree->nodes[visible[i - 1]].active) {
			selected = i - 1;
			break;
		}
	}
	unsigned char *folded = calloc(tree->count, 1);
	char(*prefix)[128] = calloc(tree->count, sizeof(*prefix));
	if (!folded || !prefix) {
		free(prefix);
		free(folded);
		free(visible);
		return 0;
	}
	char query[128] = "";
	size_t query_length = 0;
	int bookmarks_only = 0, editing_label = 0;
	size_t label_node = 0, label_length = 0;
	char label_edit[128] = "";
	uint64_t result = 0;
	modal_active = 1;
	for (;;) {
		uint64_t selected_id = count ? tree->nodes[visible[selected]].id : 0;
		count = 0;
		for (size_t i = 0; i < tree->count; i++) {
			if (!tree_visible(tree->nodes + i, users_only)) {
				continue;
			}
			if (bookmarks_only && !tree->nodes[i].bookmarked) {
				continue;
			}
			if (*query) {
				char text[768], label[1024];
				tree_label(tree, tree->nodes + i, text, sizeof(text));
				snprintf(label, sizeof(label), "%s %s", tree->nodes[i].label, text);
				if (!tree_match(label, query)) {
					continue;
				}
			} else if (!bookmarks_only && tree_hidden(tree, i, folded)) {
				continue;
			}
			visible[count++] = i;
		}
		memset(prefix, 0, tree->count * sizeof(*prefix));
		tree_branches(tree, visible, count, prefix);
		selected = 0;
		for (size_t i = 0; i < count; i++) {
			if (tree->nodes[visible[i]].id == selected_id) {
				selected = i;
			}
		}
		if (idle_callback) {
			idle_callback();
		}
		draw(1);
		welcome_scroll = 0;
		welcome_clip = -1;
		int width = COLS - 8, height = LINES - 4;
		if (width > 108) {
			width = 108;
		}
		if (height > 28) {
			height = 28;
		}
		int wanted = (int)count + 6;
		if (wanted < 12) {
			wanted = 12;
		}
		if (height > wanted) {
			height = wanted;
		}
		int usable = width >= 44 && height >= 12;
		if (usable) {
			int x = (COLS - width) / 2, y = (LINES - height) / 2;
			for (int row = 0; row < height; row++) {
				attrset(A_NORMAL);
				mvhline(y + row, x, ' ', width);
			}
			attrset(COLOR_PAIR(5));
			mvhline(y, x, ACS_HLINE, width);
			mvhline(y + height - 1, x, ACS_HLINE, width);
			mvvline(y, x, ACS_VLINE, height);
			mvvline(y, x + width - 1, ACS_VLINE, height);
			mvaddch(y, x, ACS_ULCORNER);
			mvaddch(y, x + width - 1, ACS_URCORNER);
			mvaddch(y + height - 1, x, ACS_LLCORNER);
			mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
			welcome_text(y, x + 2, users_only ? 10 : 10, users_only ? " Fork " : " Tree ", 28, 1);
			char status[64];
			snprintf(status, sizeof(status), " %zu entries ", count);
			welcome_text(y, x + width - (int)strlen(status) - 2, (int)strlen(status), status, 3, 0);

			char search[160];
			snprintf(search, sizeof(search), "%s%s",
				editing_label        ? "Label (Enter saves): "
					: bookmarks_only ? "Bookmarks: "
									 : "Search: ",
				editing_label ? label_edit : query);
			welcome_text(y + 1, x + 2, width - 4, search, 3, 0);
			size_t rows = (size_t)(height - 5);
			first = phi_list_scroll(first, selected, rows, count);
			for (size_t row = 0; row < rows && first + row < count; row++) {
				const struct session_node *node = tree->nodes + visible[first + row];
				int highlighted = first + row == selected;
				char label[768];
				tree_label(tree, node, label, sizeof(label));
				char line[1400];
				snprintf(line, sizeof(line), "%s%s%s%s%s%s%s", prefix[first + row],
					node->bookmarked ? "★ " : "", node->label, *node->label ? " — " : "", label,
					folded[visible[first + row]] ? " [folded]" : "",
					node->id == tree->head ? " ← current" : "");
				welcome_text(y + 2 + (int)row, x + 2, width - 4, line,
					highlighted ? 28 : (node->active ? 27 : 3), highlighted);
			}
			const char *note = users_only
				? "Enter forks before selected prompt; prompt is placed in editor."
				: "Enter select · Esc close · Ctrl+B bookmark · Ctrl+L label · ←/→ fold";
			welcome_text(y + height - 2, x + 2, width - 4, note, 3, 0);
		} else {
			welcome_text(
				LINES / 2, 1, COLS - 2, "Enlarge terminal for session tree · Esc cancels", 3, 0);
		}
		curs_set(0);
		attrset(A_NORMAL);
		frame_begin();
		wnoutrefresh(stdscr);
		doupdate();
		frame_end();
		wint_t key;
		int kind = read_key(&key, 250);
		if (kind == ERR) {
			continue;
		}
		if (kind == KEY_CODE_YES && key == KEY_RESIZE) {
			title_resized();
			continue;
		}
		if (editing_label) {
			if ((kind == OK && (key == 27 || key == 3)) ||
				(kind == KEY_CODE_YES && (key == KEY_ESCAPE || key == KEY_INTERRUPT))) {
				editing_label = 0;
				continue;
			}
			if ((kind == OK && key == '\r') || (kind == KEY_CODE_YES && key == KEY_ENTER)) {
				snprintf(tree->nodes[label_node].label, sizeof(tree->nodes[label_node].label), "%s",
					label_edit);
				tree->nodes[label_node].annotation_dirty = 1;
				editing_label = 0;
				continue;
			}
			if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
				(kind == OK && (key == 127 || key == 8))) {
				if (label_length) {
					label_edit[--label_length] = 0;
				}
				continue;
			}
			if (kind == OK && key >= 32 && key < 127 && label_length + 1 < sizeof(label_edit)) {
				label_edit[label_length++] = (char)key;
				label_edit[label_length] = 0;
			}
			continue;
		}
		if (kind == OK && key == 6) {
			bookmarks_only = !bookmarks_only;
			continue;
		}
		if (count && kind == OK && key == 2) {
			struct session_node *node = tree->nodes + visible[selected];
			node->bookmarked = !node->bookmarked;
			node->annotation_dirty = 1;
			continue;
		}
		if (count && kind == OK && key == 12) {
			label_node = visible[selected];
			snprintf(label_edit, sizeof(label_edit), "%s", tree->nodes[label_node].label);
			label_length = strlen(label_edit);
			editing_label = 1;
			continue;
		}
		if ((kind == OK && (key == 27 || key == 3 || key == 4)) ||
			(kind == KEY_CODE_YES &&
				(key == KEY_ESCAPE || key == KEY_INTERRUPT || key == KEY_EOF))) {
			break;
		}
		if (usable && count &&
			((kind == OK && key == '\r') || (kind == KEY_CODE_YES && key == KEY_ENTER))) {
			result = tree->nodes[visible[selected]].id;
			break;
		}
		if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
			(kind == OK && (key == 127 || key == 8))) {
			if (query_length) {
				query[--query_length] = 0;
			}
			continue;
		}
		if (kind == OK && key >= 32 && key < 127 && query_length + 1 < sizeof(query)) {
			query[query_length++] = (char)key;
			query[query_length] = 0;
			continue;
		}
		if (!count) {
			continue;
		}
		if (kind == KEY_CODE_YES && (key == KEY_LEFT || key == KEY_RIGHT)) {
			folded[visible[selected]] = key == KEY_LEFT;
			continue;
		}
		if ((kind == KEY_CODE_YES && (key == KEY_DOWN || key == KEY_NEWLINE)) ||
			(kind == OK && key == 10)) {
			selected = (selected + 1) % count;
		} else if ((kind == KEY_CODE_YES && key == KEY_UP) || (kind == OK && key == 11)) {
			selected = (selected + count - 1) % count;
		}
	}
	free(prefix);
	free(folded);
	free(visible);
	modal_active = 0;
	dirty = 1;
	cursor_shape = -1;
	draw(1);

	return result;
}
