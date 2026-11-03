/* Approved native extensions register metadata, never executable theme paths. */
#include <dlfcn.h>
#include "../../include/phi_transition.h"
static void header_motion_cancel(void);
static const char *header_motion_plugin_id(void);

static struct transition_registration {
	char id[64], owner[128], path[4096];
	struct transition_registration *next;
} *native_transitions;

static int transition_id_valid(const char *id) {

	if (!id || !*id || strlen(id) >= 64) {
		return 0;
	}
	for (const char *p = id; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
			return 0;
		}
	}

	return 1;
}

int ui_native_transition_register(
	const char *owner, const char *id, const char *path, char *problem, size_t capacity) {

	if (!owner || !*owner || strlen(owner) >= 128 || !transition_id_valid(id) ||
		!strcmp(id, "none") || !strcmp(id, "instant") || !strcmp(id, "slide") ||
		!strcmp(id, "dissolve") || !strcmp(id, "dither")) {
		snprintf(problem, capacity, "Invalid or reserved transition ID");
		return 0;
	}
	struct transition_registration **slot = &native_transitions;
	int count = 0;
	while (*slot && strcmp((*slot)->id, id)) {
		slot = &(*slot)->next;
	}
	if (*slot && strcmp((*slot)->owner, owner)) {
		snprintf(problem, capacity, "Transition ID belongs to another extension");
		return 0;
	}
	char resolved[4096];
	if (path && (path[0] != '/' || !realpath(path, resolved))) {
		snprintf(problem, capacity, "Transition needs an existing absolute library path");
		return 0;
	}
	if (path && !*slot) {
		for (struct transition_registration *p = native_transitions; p; p = p->next) {
			count++;
		}
		if (count >= 16) {
			snprintf(problem, capacity, "Too many registered transitions");
			return 0;
		}
		*slot = calloc(1, sizeof(**slot));
		if (!*slot) {
			snprintf(problem, capacity, "Out of memory");
			return 0;
		}
		snprintf((*slot)->id, sizeof((*slot)->id), "%s", id);
		snprintf((*slot)->owner, sizeof((*slot)->owner), "%s", owner);
	}
	if (!strcmp(header_motion_plugin_id(), id)) {
		header_motion_cancel();
	}
	if (path) {
		snprintf((*slot)->path, sizeof((*slot)->path), "%s", resolved);
	} else if (*slot) {
		struct transition_registration *old = *slot;
		*slot = old->next;
		free(old);
	}
	dirty = 1;

	return 1;
}

static void ui_native_transition_forget(const char *owner) {

	struct transition_registration **slot = &native_transitions;
	while (*slot) {
		struct transition_registration *p = *slot;
		if (!owner || !strcmp(owner, p->owner)) {
			if (!strcmp(header_motion_plugin_id(), p->id)) {
				header_motion_cancel();
			}
			*slot = p->next;
			free(p);
			dirty = 1;
		} else {
			slot = &p->next;
		}
	}
}
