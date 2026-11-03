#ifndef PHI_UI_LAYOUT_H
#define PHI_UI_LAYOUT_H
/* Bounded, terminal-independent composition geometry. No provider loading here. */
#include <json-c/json.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

/* Minimal scrolling for a selected row in a bounded list. */
static inline size_t phi_list_scroll(size_t first, size_t selected, size_t rows, size_t count) {

	if (selected < first) {
		first = selected;
	} else if (rows && selected - first >= rows) {
		first = selected - rows + 1;
	}
	if (first > count || rows > count - first) {
		first = count > rows ? count - rows : 0;
	}

	return first;
}

#define PHI_LAYOUT_NODES 48

struct phi_rect {
	int x, y, width, height;
};

static inline int phi_rect_contains(struct phi_rect box, int x, int y) {

	return box.width > 0 && box.height > 0 && x >= box.x && y >= box.y &&
		(long long)x - box.x < box.width && (long long)y - box.y < box.height;
}

enum phi_layout_host {
	PHI_HOST_HEADER,
	PHI_HOST_FOOTER,
	PHI_HOST_ACTIVITY,
	PHI_HOST_QUEUE,
	PHI_HOST_COUNT
};

enum phi_layout_kind {
	PHI_LAYOUT_CHAT,
	PHI_LAYOUT_ROW,
	PHI_LAYOUT_COLUMN,
	PHI_LAYOUT_TEXT,
	PHI_LAYOUT_PROGRAM,
	PHI_LAYOUT_TRANSCRIPT,
	PHI_LAYOUT_COMPOSER,
	PHI_LAYOUT_HOST
};

struct phi_layout_node {
	int kind, parent, size, weight, gap, padding, host;
	char id[64], text[512], provider[64];
};

/* Empty layouts allocate nothing; valid layouts own exactly count nodes. */
struct phi_layout {
	int count, chat, transcript, composer, hosts[PHI_HOST_COUNT];
	struct phi_layout_node *nodes;
};

static inline void phi_layout_free(struct phi_layout *tree) {

	free(tree->nodes);
	*tree = (struct phi_layout){0};
}

/* Both layouts are initialized; replacement is atomic on allocation failure. */
static inline int phi_layout_clone(struct phi_layout *out, const struct phi_layout *source) {

	struct phi_layout copy = *source;
	copy.nodes = NULL;
	if (source->count) {
		copy.nodes = malloc((size_t)source->count * sizeof(*copy.nodes));
		if (!copy.nodes) {
			return 0;
		}
		memcpy(copy.nodes, source->nodes, (size_t)source->count * sizeof(*copy.nodes));
	}
	phi_layout_free(out);
	*out = copy;

	return 1;
}

static inline int phi_layout_equal(const struct phi_layout *a, const struct phi_layout *b) {

	return a->count == b->count && a->chat == b->chat && a->transcript == b->transcript &&
		a->composer == b->composer && !memcmp(a->hosts, b->hosts, sizeof(a->hosts)) &&
		(!a->count || !memcmp(a->nodes, b->nodes, (size_t)a->count * sizeof(*a->nodes)));
}

/* Count before allocating, with the same depth/node bounds as validation. */
static inline int phi_layout_node_count(json_object *o, int depth, int *count) {

	if (depth > 12 || !json_object_is_type(o, json_type_object) || ++*count > PHI_LAYOUT_NODES) {
		return 0;
	}
	json_object *children = NULL;
	if (json_object_object_get_ex(o, "children", &children)) {
		if (!json_object_is_type(children, json_type_array) ||
			json_object_array_length(children) > PHI_LAYOUT_NODES) {
			return 0;
		}
		for (size_t i = 0; i < json_object_array_length(children); i++) {
			if (!phi_layout_node_count(json_object_array_get_idx(children, i), depth + 1, count)) {
				return 0;
			}
		}
	}

	return 1;
}

static inline int phi_layout_integer(
	json_object *o, const char *key, int fallback, int maximum, int *valid) {

	json_object *v = NULL;
	if (!json_object_object_get_ex(o, key, &v)) {
		return fallback;
	}
	if (!json_object_is_type(v, json_type_int) || json_object_get_int64(v) < 0 ||
		json_object_get_int64(v) > maximum) {
		*valid = 0;
		return 0;
	}

	return json_object_get_int(v);
}

static inline int phi_layout_string(json_object *o, const char *key, char *out, size_t capacity) {

	json_object *v = NULL;
	if (!json_object_object_get_ex(o, key, &v)) {
		return 1;
	}
	if (!json_object_is_type(v, json_type_string)) {
		return 0;
	}
	const char *s = json_object_get_string(v);
	size_t bytes = (size_t)json_object_get_string_len(v);
	if (bytes >= capacity || strlen(s) != bytes) {
		return 0;
	}
	memcpy(out, s, bytes + 1);

	return 1;
}

static inline int phi_layout_identifier(const char *id) {

	if (!*id) {
		return 0;
	}
	for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
		if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') {
			return 0;
		}
	}

	return 1;
}

static inline int phi_layout_parse_node(
	struct phi_layout *tree, json_object *o, int parent, int depth, int capacity) {

	if (depth > 12 || tree->count >= capacity || !json_object_is_type(o, json_type_object)) {
		return 0;
	}
	json_object_object_foreach(o, key, value) {
		(void)value;
		if (strcmp(key, "type") && strcmp(key, "id") && strcmp(key, "size") &&
			strcmp(key, "weight") && strcmp(key, "gap") && strcmp(key, "padding") &&
			strcmp(key, "text") && strcmp(key, "provider") && strcmp(key, "children")) {
			return 0;
		}
	}
	char type[16] = "";
	if (!phi_layout_string(o, "type", type, sizeof(type))) {
		return 0;
	}
	int index = tree->count++;
	struct phi_layout_node *n = &tree->nodes[index];
	n->parent = parent;
	if (!strcmp(type, "chat")) {
		n->kind = PHI_LAYOUT_CHAT;
		if (tree->chat >= 0) {
			return 0;
		}
		tree->chat = index;
	} else if (!strcmp(type, "transcript")) {
		n->kind = PHI_LAYOUT_TRANSCRIPT;
		if (tree->transcript >= 0) {
			return 0;
		}
		tree->transcript = index;
	} else if (!strcmp(type, "composer")) {
		n->kind = PHI_LAYOUT_COMPOSER;
		if (tree->composer >= 0) {
			return 0;
		}
		tree->composer = index;
	} else if (!strcmp(type, "row")) {
		n->kind = PHI_LAYOUT_ROW;
	} else if (!strcmp(type, "column")) {
		n->kind = PHI_LAYOUT_COLUMN;
	} else if (!strcmp(type, "text")) {
		n->kind = PHI_LAYOUT_TEXT;
	} else if (!strcmp(type, "program")) {
		n->kind = PHI_LAYOUT_PROGRAM;
	} else {
		static const char *names[] = {"header", "footer", "activity", "queue"};
		int host = 0;
		while (host < PHI_HOST_COUNT && strcmp(type, names[host])) {
			host++;
		}
		if (host == PHI_HOST_COUNT || tree->hosts[host] >= 0) {
			return 0;
		}
		n->kind = PHI_LAYOUT_HOST;
		n->host = host;
		tree->hosts[host] = index;
	}
	int valid = 1;
	n->size = phi_layout_integer(o, "size", 0, 4096, &valid);
	n->weight = phi_layout_integer(o, "weight", 1, 100, &valid);
	n->gap = phi_layout_integer(o, "gap", 0, 8, &valid);
	n->padding = phi_layout_integer(o, "padding", 0, 8, &valid);
	if (!valid || (!n->size && !n->weight) || !phi_layout_string(o, "id", n->id, sizeof(n->id)) ||
		!phi_layout_string(o, "text", n->text, sizeof(n->text)) ||
		!phi_layout_string(o, "provider", n->provider, sizeof(n->provider))) {
		return 0;
	}
	if (!*n->id) {
		snprintf(n->id, sizeof(n->id), "node-%d", index);
	}
	if (!phi_layout_identifier(n->id) || (*n->provider && !phi_layout_identifier(n->provider))) {
		return 0;
	}
	for (int i = 0; i < index; i++) {
		if (!strcmp(n->id, tree->nodes[i].id)) {
			return 0;
		}
	}
	json_object *children = NULL;
	int has_children = json_object_object_get_ex(o, "children", &children);
	if (n->kind == PHI_LAYOUT_ROW || n->kind == PHI_LAYOUT_COLUMN) {
		if (!has_children || !json_object_is_type(children, json_type_array) ||
			!json_object_array_length(children)) {
			return 0;
		}
		size_t child_count = json_object_array_length(children);
		if (child_count > PHI_LAYOUT_NODES) {
			return 0;
		}
		for (size_t i = 0; i < child_count; i++) {
			if (!phi_layout_parse_node(
					tree, json_object_array_get_idx(children, i), index, depth + 1, capacity)) {
				return 0;
			}
		}
	} else if (has_children || (n->kind == PHI_LAYOUT_PROGRAM && !*n->provider)) {
		return 0;
	}

	return 1;
}

static inline int phi_layout_parse(json_object *o, struct phi_layout *out) {

	int capacity = 0;
	if (!phi_layout_node_count(o, 0, &capacity)) {
		return 0;
	}
	struct phi_layout tree = {.chat = -1, .transcript = -1, .composer = -1};
	tree.nodes = calloc((size_t)capacity, sizeof(*tree.nodes));
	if (!tree.nodes) {
		return 0;
	}
	for (int i = 0; i < PHI_HOST_COUNT; i++) {
		tree.hosts[i] = -1;
	}
	if (!phi_layout_parse_node(&tree, o, -1, 0, capacity)) {
		goto invalid;
	}
	for (int i = 0; i < PHI_HOST_COUNT; i++) {
		if (tree.chat >= 0 && tree.hosts[i] >= 0) {
			goto invalid;
		}
	}
	if (tree.chat >= 0 ? (tree.transcript >= 0 || tree.composer >= 0)
					   : (tree.transcript < 0 || tree.composer < 0)) {
		goto invalid;
	}
	phi_layout_free(out);
	*out = tree;
	return 1;
invalid:
	phi_layout_free(&tree);

	return 0;
}

static inline void phi_layout_arrange_node(
	const struct phi_layout *tree, int index, struct phi_rect rect, struct phi_rect *boxes) {

	const struct phi_layout_node *n = &tree->nodes[index];
	int px = n->padding < rect.width / 2 ? n->padding : rect.width / 2,
		py = n->padding < rect.height / 2 ? n->padding : rect.height / 2;
	rect.x += px;
	rect.y += py;
	rect.width -= 2 * px;
	rect.height -= 2 * py;
	boxes[index] = rect;
	if (n->kind != PHI_LAYOUT_ROW && n->kind != PHI_LAYOUT_COLUMN) {
		return;
	}
	int extent = n->kind == PHI_LAYOUT_ROW ? rect.width : rect.height, children = 0, fixed = 0,
		weights = 0;
	for (int i = index + 1; i < tree->count; i++) {
		if (tree->nodes[i].parent == index) {
			children++;
			fixed += tree->nodes[i].size;
			if (!tree->nodes[i].size) {
				weights += tree->nodes[i].weight;
			}
		}
	}
	int gap = children > 1 ? n->gap : 0;
	if (children > 1 && gap * (children - 1) > extent) {
		gap = extent / (children - 1);
	}
	int available = extent - gap * (children - 1), flex = available > fixed ? available - fixed : 0,
		position = 0;
	for (int i = index + 1; i < tree->count; i++) {
		if (tree->nodes[i].parent == index) {
			const struct phi_layout_node *child = &tree->nodes[i];
			int span = child->size;
			if (!span) {
				span = weights ? flex * child->weight / weights : 0;
				flex -= span;
				weights -= child->weight;
			}
			if (span > available) {
				span = available;
			}
			available -= span;
			struct phi_rect box = rect;
			if (n->kind == PHI_LAYOUT_ROW) {
				box.x += position;
				box.width = span;
			} else {
				box.y += position;
				box.height = span;
			}
			phi_layout_arrange_node(tree, i, box, boxes);
			position += span + gap;
		}
	}
}

static inline int phi_layout_arrange(
	const struct phi_layout *tree, int width, int height, struct phi_rect *boxes) {

	memset(boxes, 0, sizeof(*boxes) * PHI_LAYOUT_NODES);
	if (!tree->count || width < 1 || height < 1) {
		return 0;
	}
	phi_layout_arrange_node(tree, 0, (struct phi_rect){0, 0, width, height}, boxes);
	struct phi_rect chat = boxes[tree->chat >= 0 ? tree->chat : tree->transcript];
	if (tree->chat >= 0) {
		return chat.width >= 20 && chat.height >= 6;
	}
	for (int i = 0; i < PHI_HOST_COUNT; i++) {
		if (tree->hosts[i] >= 0) {
			struct phi_rect host = boxes[tree->hosts[i]];
			if (host.width < 1 || host.height < 1) {
				return 0;
			}
		}
	}
	struct phi_rect editor = boxes[tree->composer];

	return chat.width >= 20 && chat.height >= 4 && editor.width >= 20 && editor.height >= 6;
}
#endif
