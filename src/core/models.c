#define _POSIX_C_SOURCE 200809L
#include "models.h"
#include "fsutil.h"
#include <json-c/json.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#define CATALOG_LIMIT (4 * 1024 * 1024)
#define CATALOG_MODELS 128
static struct phi_catalog_model catalog[CATALOG_MODELS];
static size_t count;
/* Context windows and effort levels for every cached model, including
   hidden ones that can still be selected manually with /model <id>.
   levels: the model's supported_reasoning_levels as the server lists them
   (lightest first); efforts: a bit per phi_effort_order entry among them. */
#define WINDOW_MODELS 256

static struct {
	char id[96];
	int64_t window;
	unsigned efforts;
	struct phi_effort_level *levels;
	size_t level_count;
	int default_level;
} windows[WINDOW_MODELS];

static size_t window_count;
static char source_path[4096];
static struct stat source_stat;
static int source_valid;
static long long fetched;

/* Phi's own refreshed cache wins; otherwise read the Codex CLI's cache.
   Phi never writes the Codex CLI's file. */
int phi_catalog_cache_path(char *path, size_t size) {

	return phi_xdg_path(path, size, "XDG_CACHE_HOME", ".cache", "phi/models_cache.json");
}

static int path_for_catalog(char *path, size_t size) {

	if (phi_catalog_cache_path(path, size) && !access(path, F_OK)) {
		return 1;
	}
	const char *codex = getenv("CODEX_HOME"), *home = getenv("HOME");
	int n = codex && *codex == '/' ? snprintf(path, size, "%s/models_cache.json", codex)
		: home && *home == '/'     ? snprintf(path, size, "%s/.codex/models_cache.json", home)
								   : -1;

	return n >= 0 && (size_t)n < size;
}

static const char *string_field(json_object *item, const char *name) {

	json_object *value = json_object_object_get(item, name);
	if (!json_object_is_type(value, json_type_string)) {
		return NULL;
	}
	const char *text = json_object_get_string(value);

	return strlen(text) == (size_t)json_object_get_string_len(value) ? text : NULL;
}

static int valid_id(const char *id) {

	size_t n = id ? strlen(id) : 0;

	return n && n < sizeof(catalog[0].id) &&
		strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/") == n;
}

static void windows_clear(void) {

	for (size_t i = 0; i < window_count; i++) {
		free(windows[i].levels);
		windows[i].levels = NULL;
		windows[i].level_count = 0;
	}
	window_count = 0;
}

/* A level name as sent to the server: a short lowercase word. */
static int valid_level(const char *name) {

	size_t n = name ? strlen(name) : 0;

	return n && n < sizeof(((struct phi_effort_level *)0)->name) &&
		strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-_") == n;
}

static void catalog_reload(void) {

	char path[4096];
	struct stat st;
	if (!path_for_catalog(path, sizeof(path))) {
		windows_clear();
		count = source_valid = 0;
		return;
	}
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		windows_clear();
		count = source_valid = 0;
		return;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_size < 1 ||
		st.st_size > CATALOG_LIMIT) {
		close(fd);
		windows_clear();
		count = source_valid = 0;
		return;
	}
	if (source_valid && !strcmp(path, source_path) && st.st_dev == source_stat.st_dev &&
		st.st_ino == source_stat.st_ino && st.st_size == source_stat.st_size &&
		st.st_mtim.tv_sec == source_stat.st_mtim.tv_sec &&
		st.st_mtim.tv_nsec == source_stat.st_mtim.tv_nsec) {
		close(fd);
		return;
	}
	source_valid = 0;
	windows_clear();
	count = 0;
	fetched = 0;
	char *text = malloc((size_t)st.st_size + 1);
	if (!text) {
		close(fd);
		return;
	}
	size_t used = 0;
	while (used < (size_t)st.st_size) {
		ssize_t n = read(fd, text + used, (size_t)st.st_size - used);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		used += (size_t)n;
	}
	close(fd);
	if (used != (size_t)st.st_size || memchr(text, 0, used)) {
		free(text);
		return;
	}
	text[used] = 0;
	struct json_tokener *parser = json_tokener_new();
	if (!parser) {
		free(text);
		return;
	}
	json_object *root = json_tokener_parse_ex(parser, text, (int)used + 1);
	int parsed = json_tokener_get_error(parser) == json_tokener_success;
	json_tokener_free(parser);
	free(text);
	json_object *models = root ? json_object_object_get(root, "models") : NULL;
	if (!parsed || !json_object_is_type(models, json_type_array)) {
		if (root) {
			json_object_put(root);
		}
		return;
	}
	/* Cache metadata may use RFC3339; report filesystem modification time
       rather than misrepresenting it as a verified server fetch time. */
	fetched = (long long)st.st_mtime;
	for (size_t i = 0; i < json_object_array_length(models); i++) {
		json_object *model = json_object_array_get_idx(models, i);
		const char *id = string_field(model, "slug"),
				   *visibility = string_field(model, "visibility");
		json_object *window = json_object_object_get(model, "context_window");
		if (valid_id(id) && window_count < WINDOW_MODELS) {
			int64_t value =
				json_type_int == json_object_get_type(window) ? json_object_get_int64(window) : 0;
			unsigned efforts = 0;
			struct phi_effort_level found[PHI_LEVELS];
			size_t found_count = 0;
			json_object *levels = json_object_object_get(model, "supported_reasoning_levels");
			const char *preferred = string_field(model, "default_reasoning_level");
			int preferred_index = -1;
			/* The API's own list (phi_api_levels, learnt by /refresh-models)
               when known, described in the model list's words; else the
               model list without "ultra", which is Codex's task delegation,
               not a level the API takes. */
			json_object *api = json_object_object_get(model, "phi_api_levels");
			int from_api =
				json_object_is_type(api, json_type_array) && json_object_array_length(api) > 0;
			json_object *names = from_api ? api : levels;
			for (size_t j = 0; json_object_is_type(names, json_type_array) &&
				j < json_object_array_length(names) && found_count < PHI_LEVELS;
				j++) {
				json_object *item = json_object_array_get_idx(names, j);
				const char *level = from_api
					? (json_object_is_type(item, json_type_string) ? json_object_get_string(item)
																   : NULL)
					: string_field(item, "effort");
				const char *about = from_api ? NULL : string_field(item, "description");
				if (!valid_level(level) || (!from_api && !strcmp(level, "ultra"))) {
					continue;
				}
				for (size_t k = 0; from_api && json_object_is_type(levels, json_type_array) &&
					k < json_object_array_length(levels);
					k++) {
					const char *listed =
						string_field(json_object_array_get_idx(levels, k), "effort");
					if (listed && !strcmp(listed, level)) {
						about = string_field(json_object_array_get_idx(levels, k), "description");
					}
				}
				if (!about) {
					about = !strcmp(level, "none")  ? "No reasoning"
						: !strcmp(level, "minimal") ? "Minimal reasoning"
													: "";
				}
				int rank = phi_effort_rank(level);
				if (rank >= 0) {
					efforts |= 1u << rank;
				}
				struct phi_effort_level *entry = &found[found_count];
				snprintf(entry->name, sizeof(entry->name), "%s", level);
				snprintf(entry->description, sizeof(entry->description), "%s", about ? about : "");
				for (char *p = entry->description; *p; p++) {
					if ((unsigned char)*p < 32 || *p == 127) {
						*p = ' ';
					}
				}
				if (preferred && !strcmp(preferred, level)) {
					preferred_index = (int)found_count;
				}
				found_count++;
			}
			if ((value > 0 && value <= 1000000000) || found_count) {
				snprintf(windows[window_count].id, sizeof(windows[window_count].id), "%s", id);
				windows[window_count].window = value > 0 && value <= 1000000000 ? value : 0;
				windows[window_count].efforts = efforts;
				windows[window_count].levels =
					found_count ? malloc(found_count * sizeof(*found)) : NULL;
				if (windows[window_count].levels) {
					memcpy(windows[window_count].levels, found, found_count * sizeof(*found));
				}
				windows[window_count].level_count = windows[window_count].levels ? found_count : 0;
				windows[window_count++].default_level = preferred_index;
			}
		}
		if (count >= CATALOG_MODELS) {
			continue;
		}
		json_object *api = json_object_object_get(model, "supported_in_api");
		if (!valid_id(id) || !visibility || strcmp(visibility, "list") ||
			(api &&
				(!json_object_is_type(api, json_type_boolean) || !json_object_get_boolean(api)))) {
			continue;
		}
		int duplicate = 0;
		for (size_t j = 0; j < count; j++) {
			if (!strcmp(catalog[j].id, id)) {
				duplicate = 1;
			}
		}
		if (duplicate) {
			continue;
		}
		struct phi_catalog_model *entry = &catalog[count++];
		snprintf(entry->id, sizeof(entry->id), "%s", id);
		snprintf(entry->command, sizeof(entry->command), "/model %s", id);
		const char *label = string_field(model, "display_name");
		snprintf(entry->label, sizeof(entry->label), "%s",
			label && *label && strlen(label) < sizeof(entry->label) ? label : id);
		for (char *p = entry->label; *p; p++) {
			if ((unsigned char)*p < 32 || *p == 127) {
				*p = ' ';
			}
		}
	}
	json_object_put(root);
	snprintf(source_path, sizeof(source_path), "%s", path);
	source_stat = st;
	source_valid = 1;
}

size_t phi_catalog_count(void) {

	catalog_reload();

	return count;
}

int64_t phi_catalog_context_window(const char *id) {

	catalog_reload();
	for (size_t i = 0; i < window_count; i++) {
		if (!strcmp(windows[i].id, id)) {
			return windows[i].window;
		}
	}

	return 0;
}

const char *const phi_effort_order[PHI_EFFORTS] = {
	"none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"};

int phi_effort_rank(const char *effort) {

	for (int i = 0; effort && i < PHI_EFFORTS; i++) {
		if (!strcmp(effort, phi_effort_order[i])) {
			return i;
		}
	}

	return -1;
}

unsigned phi_catalog_efforts(const char *id) {

	catalog_reload();
	for (size_t i = 0; id && i < window_count; i++) {
		if (!strcmp(windows[i].id, id)) {
			return windows[i].efforts;
		}
	}

	return 0;
}

size_t phi_catalog_levels(
	const char *id, const struct phi_effort_level **levels, int *default_level) {

	catalog_reload();
	for (size_t i = 0; id && i < window_count; i++) {
		if (!strcmp(windows[i].id, id)) {
			if (levels) {
				*levels = windows[i].levels;
			}
			if (default_level) {
				*default_level = windows[i].default_level;
			}
			return windows[i].level_count;
		}
	}
	if (levels) {
		*levels = NULL;
	}
	if (default_level) {
		*default_level = -1;
	}

	return 0;
}

const char *phi_effort_for(const char *id, const char *effort) {

	const struct phi_effort_level *levels;
	int preferred;
	size_t listed = phi_catalog_levels(id, &levels, &preferred);
	if (!listed) {
		return effort; /* a model the catalogue does not describe */
	}
	for (size_t i = 0; i < listed; i++) {
		if (effort && !strcmp(levels[i].name, effort)) {
			return effort;
		}
	}
	unsigned efforts = phi_catalog_efforts(id);
	int rank = phi_effort_rank(effort);
	/* A name Phi cannot place: the model's default, else its first. */
	if (rank < 0 || !efforts) {
		return levels[preferred >= 0 ? preferred : 0].name;
	}
	/* The nearest level the model takes: the next one up, else the highest
       below (so "none" becomes the lightest, and "max" the deepest). */
	for (int i = rank + 1; i < PHI_EFFORTS; i++) {
		if (efforts & (1u << i)) {
			return phi_effort_order[i];
		}
	}
	for (int i = rank - 1; i >= 0; i--) {
		if (efforts & (1u << i)) {
			return phi_effort_order[i];
		}
	}

	return effort;
}

const struct phi_catalog_model *phi_catalog_at(size_t index) {

	catalog_reload();

	return index < count ? catalog + index : NULL;
}

int phi_catalog_source(char *source, size_t size, long long *fetched_at) {

	catalog_reload();
	if (source && size) {
		snprintf(source, size, "%s", source_valid ? source_path : "");
	}
	if (fetched_at) {
		*fetched_at = fetched;
	}

	return source_valid;
}
