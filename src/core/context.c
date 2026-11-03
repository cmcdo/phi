#define _POSIX_C_SOURCE 200809L
#include "context.h"
#include "models.h"

static const char *(*provider_lookup)(const char *model, int64_t *context_window);

void context_set_provider_lookup(
	const char *(*lookup)(const char *model, int64_t *context_window)) {

	provider_lookup = lookup;
}

int64_t context_window(const char *model) {

	model = phi_model_id(model);
	if (provider_lookup) {
		int64_t extension_window = 0;
		if (provider_lookup(model, &extension_window) && extension_window > 0) {
			return extension_window;
		}
	}
	/* The local model cache is authoritative when it names the model; the
       built-in table covers the aliases when no cache exists. */
	int64_t cached = phi_catalog_context_window(model);
	if (cached > 0) {
		return cached;
	}
	for (size_t i = 0; i < PHI_MODEL_COUNT; i++) {
		if (!strcmp(model, phi_models[i].id)) {
			return phi_models[i].context_window;
		}
	}
	/* A newer model of a known family (gpt-6.1-sol, not yet in the cache):
       its family's window. Without one, nothing ever compacts automatically. */
	const char *dash = strrchr(model, '-');
	for (size_t i = 0; dash && i < PHI_MODEL_COUNT; i++) {
		if (!strcmp(dash + 1, phi_models[i].name)) {
			return phi_models[i].context_window;
		}
	}

	return 0;
}
