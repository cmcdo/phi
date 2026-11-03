#ifndef PHI_MODELS_H
#define PHI_MODELS_H
#include <string.h>

/* Names verified against the local Codex model catalogue. */
static const struct {
	const char *name, *id, *command;
	long context_window;
} phi_models[] = {{"luna", "gpt-5.6-luna", "/model luna", 272000},
	{"terra", "gpt-5.6-terra", "/model terra", 272000},
	{"sol", "gpt-5.6-sol", "/model sol", 272000}, {"astra", "gpt-6-astra", "/model astra", 272000}};

#define PHI_MODEL_COUNT (sizeof(phi_models) / sizeof(*phi_models))

/* Local Codex advertised catalogue. Absence is not evidence of entitlement. */
struct phi_catalog_model {
	char id[96], command[112], label[128];
};

size_t phi_catalog_count(void);
const struct phi_catalog_model *phi_catalog_at(size_t index);
/* Returns 1 if a readable catalogue exists; reports its fetched timestamp. */
int phi_catalog_source(char *source, size_t size, long long *fetched_at);
int phi_catalog_refresh(char *error, size_t capacity);
#include <stdint.h>
/* Context window advertised in the cache for any model id, or 0. */
int64_t phi_catalog_context_window(const char *id);
/* Effort levels, lightest first. A model's levels are a mask of bits in
   this order (0: unknown model, any level goes). */
#define PHI_EFFORTS 8
extern const char *const phi_effort_order[PHI_EFFORTS];
int phi_effort_rank(const char *effort);
unsigned phi_catalog_efforts(const char *id);
/* A model's effort levels as the server lists them, lightest first, with
   its own descriptions; *default_level indexes the model's default (-1:
   none given). 0 for a model the catalogue does not describe. The array
   lasts until the catalogue changes on disk. */
#define PHI_LEVELS 12

struct phi_effort_level {
	char name[16];
	char description[96];
};

size_t phi_catalog_levels(
	const char *id, const struct phi_effort_level **levels, int *default_level);
/* The effort to send: the given one if the model takes it, else the
   nearest it does (by phi_effort_order), else its default. */
const char *phi_effort_for(const char *id, const char *effort);
/* Phi's own cache file, written by /refresh-models. */
int phi_catalog_cache_path(char *path, size_t size);

static inline const char *phi_model_id(const char *name) {

	for (size_t i = 0; i < PHI_MODEL_COUNT; i++) {
		if (!strcmp(name, phi_models[i].name)) {
			return phi_models[i].id;
		}
	}

	return name; /* Full model IDs remain available for manual entry. */
}
#endif
