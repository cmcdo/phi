#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "resources.h"
#include "commands.h"
#include "events.h"
#include "extensions.h"
#include "fsutil.h"
#include "json.h"
#include "packages.h"
#include <dirent.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RESOURCES 256
#define FILE_LIMIT (256 * 1024)
#define MAX_DEPTH 5

enum scope {
	SCOPE_USER,
	SCOPE_PROJECT,
	SCOPE_PACKAGE,
	SCOPE_EXTENSION
};

struct skill {
	char name[65], *description, path[4096];
	int model_invocation;
	enum scope scope;
};

struct prompt {
	char command[80], *description, *hint, path[4096];
	enum scope scope;
	struct phi_command info;
};
/* Keep entry addresses stable, but allocate paths/metadata only for actual
   resources instead of reserving two MiB of mostly empty global tables. */
static struct skill *skills[MAX_RESOURCES];
static struct prompt *prompts[MAX_RESOURCES];
static size_t skill_count, prompt_count;
static struct phi_command skill_infos[MAX_RESOURCES];
static char skill_commands[MAX_RESOURCES][72];

/* ---- frontmatter ---------------------------------------------------------- */

/* The YAML subset skills and templates use: "key: value" lines (quoted or
   plain) and folded/literal blocks (> or |) of indented lines. Returns the
   value (caller frees) or NULL. *body receives the text after the block. */
static char *frontmatter(const char *text, const char *key, const char **body) {

	if (body) {
		*body = text;
	}
	if (strncmp(text, "---", 3) || (text[3] != '\n' && text[3] != '\r')) {
		return NULL;
	}
	const char *line = strchr(text, '\n') + 1, *end = NULL;
	for (const char *at = line; *at;
		at = strchr(at, '\n') ? strchr(at, '\n') + 1 : at + strlen(at)) {
		if (!strncmp(at, "---", 3) && (!at[3] || at[3] == '\n' || at[3] == '\r')) {
			end = at;
			break;
		}
	}
	if (!end) {
		return NULL;
	}
	if (body) {
		*body = strchr(end, '\n') ? strchr(end, '\n') + 1 : end + strlen(end);
	}
	size_t key_length = strlen(key);
	for (const char *at = line; at < end;) {
		const char *next = memchr(at, '\n', (size_t)(end - at));
		next = next ? next + 1 : end;
		if (!strncmp(at, key, key_length) && at[key_length] == ':') {
			const char *value = at + key_length + 1;
			while (*value == ' ' || *value == '\t') {
				value++;
			}
			size_t length = (size_t)(next - value);
			while (length &&
				(value[length - 1] == '\n' || value[length - 1] == '\r' ||
					value[length - 1] == ' ')) {
				length--;
			}
			if (length && (*value == '>' || *value == '|')) {
				length = 0, value = next == end ? end : value;
			}
			if (!length) {
				/* Block scalar: following indented lines, joined. */
				int literal = *(at + key_length + 1 + strspn(at + key_length + 1, " \t")) == '|';
				char *out = calloc(1, (size_t)(end - next) + 1);
				if (!out) {
					return NULL;
				}
				size_t used = 0;
				for (const char *row = next; row < end && (*row == ' ' || *row == '\t');) {
					const char *stop = memchr(row, '\n', (size_t)(end - row));
					stop = stop ? stop : end;
					const char *start = row + strspn(row, " \t");
					size_t n = (size_t)(stop - start);
					if (n && start[n - 1] == '\r') {
						n--;
					}
					if (used) {
						out[used++] = literal ? '\n' : ' ';
					}
					memcpy(out + used, start, n);
					used += n;
					row = stop < end ? stop + 1 : end;
				}
				out[used] = 0;
				return out;
			}
			if (length >= 2 && (*value == '"' || *value == '\'') && value[length - 1] == *value) {
				value++, length -= 2;
			}
			char *out = strndup(value, length);
			return out;
		}
		at = next;
	}

	return NULL;
}

static char *read_text(const char *path) {

	return phi_read_file(path, FILE_LIMIT, PHI_READ_FOLLOW, NULL);
}

/* ---- discovery ------------------------------------------------------------ */

static int valid_skill_name(const char *name) {

	size_t length = strlen(name);
	if (!length || length > 64 || name[0] == '-' || name[length - 1] == '-') {
		return 0;
	}
	for (size_t i = 0; i < length; i++) {
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
			return 0;
		}
		if (c == '-' && name[i + 1] == '-') {
			return 0;
		}
	}

	return 1;
}

static void add_skill(const char *path, const char *fallback_name, enum scope scope) {

	if (skill_count == MAX_RESOURCES) {
		return;
	}
	char *text = read_text(path);
	if (!text) {
		return;
	}
	char *name = frontmatter(text, "name", NULL),
		 *description = frontmatter(text, "description", NULL);
	char *hidden = frontmatter(text, "disable-model-invocation", NULL);
	free(text);
	const char *chosen = name && *name ? name : fallback_name;
	int duplicate = 0;
	for (size_t i = 0; i < skill_count; i++) {
		if (!strcmp(skills[i]->name, chosen)) {
			duplicate = 1;
		}
	}
	if (!duplicate && description && *description && valid_skill_name(chosen)) {
		struct skill *skill = calloc(1, sizeof(*skill));
		if (skill) {
			skills[skill_count++] = skill;
			snprintf(skill->name, sizeof(skill->name), "%s", chosen);
			if (strlen(description) > 1024) {
				description[1024] = 0;
			}
			skill->description = description;
			description = NULL;
			snprintf(skill->path, sizeof(skill->path), "%s", path);
			skill->model_invocation = !(hidden && !strcmp(hidden, "true"));
			skill->scope = scope;
		}
	} else if (!duplicate && name) {
		fprintf(stderr, "Skipped skill %s: %s\n", path,
			description && *description ? "invalid name" : "missing description");
	}
	free(name);
	free(description);
	free(hidden);
}

/* A directory with SKILL.md is a skill (not searched further); otherwise
   subdirectories are searched. Top-level *.md files are standalone skills. */
static void scan_skills(const char *directory, enum scope scope, int depth) {

	char path[4096];
	snprintf(path, sizeof(path), "%s/SKILL.md", directory);
	if (!access(path, R_OK)) {
		const char *base = strrchr(directory, '/');
		add_skill(path, base ? base + 1 : directory, scope);
		return;
	}
	if (depth > MAX_DEPTH) {
		return;
	}
	DIR *dir = opendir(directory);
	if (!dir) {
		return;
	}
	char *names[512];
	size_t count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) && count < 512) {
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "node_modules")) {
			continue;
		}
		names[count] = strdup(entry->d_name);
		if (names[count]) {
			count++;
		}
	}
	closedir(dir);
	qsort(names, count, sizeof(*names), (int (*)(const void *, const void *))strcmp);
	for (size_t i = 0; i < count; i++) {
		snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
		struct stat info;
		size_t length = strlen(names[i]);
		if (!stat(path, &info) && S_ISDIR(info.st_mode)) {
			scan_skills(path, scope, depth + 1);
		} else if (!depth && length > 3 && !strcmp(names[i] + length - 3, ".md")) {
			names[i][length - 3] = 0;
			add_skill(path, names[i], scope);
		}
		free(names[i]);
	}
}

/* A path given explicitly: a SKILL.md file, a skill directory or a root. */
static void add_skill_path(const char *path, enum scope scope) {

	struct stat info;
	if (stat(path, &info)) {
		return;
	}
	if (S_ISDIR(info.st_mode)) {
		scan_skills(path, scope, 0);
		return;
	}
	char copy[4096];
	snprintf(copy, sizeof(copy), "%s", path);
	char *slash = strrchr(copy, '/');
	const char *base = slash ? slash + 1 : copy;
	if (!strcmp(base, "SKILL.md") && slash) {
		*slash = 0;
		slash = strrchr(copy, '/');
		add_skill(path, slash ? slash + 1 : copy, scope);
	} else {
		size_t length = strlen(base);
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(length > 3 ? length - 3 : length), base);
		add_skill(path, name, scope);
	}
}

static void add_prompt(const char *path, enum scope scope) {

	if (prompt_count == MAX_RESOURCES) {
		return;
	}
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	size_t length = strlen(base);
	if (length <= 3 || length > 70 || strcmp(base + length - 3, ".md")) {
		return;
	}
	char command[80];
	snprintf(command, sizeof(command), "/%.*s", (int)(length - 3), base);
	if (strchr(command + 1, ' ') || strchr(command + 1, ':')) {
		return;
	}
	for (size_t i = 0; i < prompt_count; i++) {
		if (!strcmp(prompts[i]->command, command)) {
			return;
		}
	}
	char *text = read_text(path);
	if (!text) {
		return;
	}
	const char *body;
	char *description = frontmatter(text, "description", &body);
	char *hint = frontmatter(text, "argument-hint", NULL);
	if (!description) {
		/* First non-empty line of the body. */
		const char *line = body + strspn(body, " \t\r\n");
		description = strndup(line, strcspn(line, "\r\n"));
		if (description && strlen(description) > 80) {
			strcpy(description + 77, "...");
		}
	}
	free(text);
	struct prompt *prompt = calloc(1, sizeof(*prompt));
	if (!prompt) {
		free(description);
		free(hint);
		return;
	}
	prompts[prompt_count++] = prompt;
	snprintf(prompt->command, sizeof(prompt->command), "%s", command);
	snprintf(prompt->path, sizeof(prompt->path), "%s", path);
	prompt->description = description;
	prompt->hint = hint;
	prompt->scope = scope;
	prompt->info =
		(struct phi_command){prompt->command, prompt->description ? prompt->description : "", 1};
}

static void scan_prompts(const char *path, enum scope scope) {

	struct stat info;
	if (stat(path, &info)) {
		return;
	}
	if (!S_ISDIR(info.st_mode)) {
		add_prompt(path, scope);
		return;
	}
	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}
	char *names[512];
	size_t count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) && count < 512) {
		if (entry->d_name[0] != '.' && (names[count] = strdup(entry->d_name))) {
			count++;
		}
	}
	closedir(dir);
	qsort(names, count, sizeof(*names), (int (*)(const void *, const void *))strcmp);
	for (size_t i = 0; i < count; i++) {
		char file[4096];
		snprintf(file, sizeof(file), "%s/%s", path, names[i]);
		if (!stat(file, &info) && S_ISREG(info.st_mode)) {
			add_prompt(file, scope);
		}
		free(names[i]);
	}
}

/* Paths from a manifest list (globs allowed) or a conventional directory. */
static void package_paths(const char *root, json_object *list, const char *fallback,
	void (*add)(const char *path, enum scope scope)) {

	char path[4096];
	if (!json_object_is_type(list, json_type_array)) {
		snprintf(path, sizeof(path), "%s/%s", root, fallback);
		add(path, SCOPE_PACKAGE);
		return;
	}
	for (size_t i = 0; i < json_object_array_length(list); i++) {
		const char *entry = json_object_get_string(json_object_array_get_idx(list, i));
		if (!entry || !*entry || *entry == '!' || strstr(entry, "..")) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", root, entry);
		glob_t found;
		if (glob(path, 0, NULL, &found)) {
			continue;
		}
		for (size_t j = 0; j < found.gl_pathc; j++) {
			add(found.gl_pathv[j], SCOPE_PACKAGE);
		}
		globfree(&found);
	}
}

static void add_skill_root(const char *path, enum scope scope) {

	add_skill_path(path, scope);
}

static void scan_prompts(const char *path, enum scope scope);

static void add_prompt_path(const char *path, enum scope scope) {

	scan_prompts(path, scope);
}

static void load_packages(void) {

	json_object *installed = packages_installed();
	for (size_t i = 0; i < json_object_array_length(installed); i++) {
		json_object *package = json_object_array_get_idx(installed, i);
		const char *root = field(package, "path");
		json_object *manifest = packages_manifest(root);
		package_paths(root, json_object_object_get(manifest, "skills"), "skills", add_skill_root);
		package_paths(
			root, json_object_object_get(manifest, "prompts"), "prompts", add_prompt_path);
		if (manifest) {
			json_object_put(manifest);
		}
	}
	if (installed) {
		json_object_put(installed);
	}
}

static int config_path(char *path, size_t size, const char *leaf) {

	return phi_xdg_path(path, size, "XDG_CONFIG_HOME", ".config", leaf);
}

struct discover_state {
	json_object *skills, *prompts;
};

static int discover_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	(void)source;
	struct discover_state *found = state;
	const char *keys[2][2] = {{"skill_paths", "skillPaths"}, {"prompt_paths", "promptPaths"}};
	for (int k = 0; k < 2; k++) {
		for (int alias = 0; alias < 2; alias++) {
			json_object *list = json_object_object_get(result, keys[k][alias]);
			for (size_t i = 0;
				json_object_is_type(list, json_type_array) && i < json_object_array_length(list);
				i++) {
				json_object *path = json_object_array_get_idx(list, i);
				if (json_object_is_type(path, json_type_string)) {
					json_object_array_add(
						k ? found->prompts : found->skills, json_object_get(path));
				}
			}
		}
	}

	return 0;
}

struct trust_state {
	int denied;
};

static int trust_apply(json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	(void)source;
	json_object *trusted = json_object_object_get(result, "trusted");
	if (trusted && !json_object_get_boolean(trusted)) {
		((struct trust_state *)state)->denied = 1;
		return 1;
	}

	return 0;
}

static void clear(void) {

	for (size_t i = 0; i < skill_count; i++) {
		free(skills[i]->description);
		free(skills[i]);
		skills[i] = NULL;
	}
	for (size_t i = 0; i < prompt_count; i++) {
		free(prompts[i]->description);
		free(prompts[i]->hint);
		free(prompts[i]);
		prompts[i] = NULL;
	}
	skill_count = prompt_count = 0;
}

void resources_close(void) {

	clear();
}

/* Project directories: cwd and its ancestors up to the repository root. */
static size_t project_directories(char directories[][4096], size_t capacity) {

	char cwd[4096];
	if (!getcwd(cwd, sizeof(cwd))) {
		return 0;
	}
	size_t count = 0;
	for (;;) {
		if (count < capacity) {
			snprintf(directories[count++], 4096, "%s", cwd);
		}
		char git[4200];
		snprintf(git, sizeof(git), "%s/.git", cwd);
		if (!access(git, F_OK)) {
			break;
		}
		char *slash = strrchr(cwd, '/');
		if (!slash || slash == cwd) {
			break;
		}
		*slash = 0;
	}

	return count;
}

void resources_load(const char *reason) {

	clear();
	char path[4096], cwd[4096];
	if (!getcwd(cwd, sizeof(cwd))) {
		cwd[0] = 0;
	}

	/* Project resources first in a scratch pass, to ask project_trust. */
	char directories[32][4096];
	size_t directory_count = project_directories(directories, 32);
	json_object *project_skills = json_object_new_array(),
				*project_prompts = json_object_new_array();
	for (size_t i = 0; i < directory_count; i++) {
		const char *names[] = {".phi/skills", ".agents/skills"};
		for (int j = 0; j < 2; j++) {
			snprintf(path, sizeof(path), "%.4000s/%.20s", directories[i], names[j]);
			if (!access(path, R_OK)) {
				json_object_array_add(project_skills, json_object_new_string(path));
			}
		}
	}
	snprintf(path, sizeof(path), "%.4000s/.phi/prompts", cwd);
	if (*cwd && !access(path, R_OK)) {
		json_object_array_add(project_prompts, json_object_new_string(path));
	}
	int project_trusted = 1;
	if ((json_object_array_length(project_skills) || json_object_array_length(project_prompts)) &&
		events_wanted("project_trust")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "cwd", json_object_new_string(cwd));
		json_object_object_add(payload, "skill_paths", json_object_get(project_skills));
		json_object_object_add(payload, "prompt_paths", json_object_get(project_prompts));
		struct trust_state trust = {0};
		events_emit("project_trust", payload, trust_apply, &trust);
		json_object_put(payload);
		project_trusted = !trust.denied;
	}

	if (config_path(path, sizeof(path), "phi/skills")) {
		scan_skills(path, SCOPE_USER, 0);
	}
	const char *home = getenv("HOME");
	if (home && *home == '/') {
		snprintf(path, sizeof(path), "%s/.agents/skills", home);
		scan_skills(path, SCOPE_USER, 0);
	}
	for (size_t i = 0; project_trusted && i < json_object_array_length(project_skills); i++) {
		scan_skills(
			json_object_get_string(json_object_array_get_idx(project_skills, i)), SCOPE_PROJECT, 0);
	}
	if (config_path(path, sizeof(path), "phi/prompts")) {
		scan_prompts(path, SCOPE_USER);
	}
	for (size_t i = 0; project_trusted && i < json_object_array_length(project_prompts); i++) {
		scan_prompts(
			json_object_get_string(json_object_array_get_idx(project_prompts, i)), SCOPE_PROJECT);
	}
	json_object_put(project_skills);
	json_object_put(project_prompts);
	load_packages();

	if (events_wanted("resources_discover")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "cwd", json_object_new_string(cwd));
		json_object_object_add(
			payload, "reason", json_object_new_string(reason ? reason : "startup"));
		struct discover_state found = {json_object_new_array(), json_object_new_array()};
		events_emit("resources_discover", payload, discover_apply, &found);
		json_object_put(payload);
		for (size_t i = 0; i < json_object_array_length(found.skills); i++) {
			add_skill_path(json_object_get_string(json_object_array_get_idx(found.skills, i)),
				SCOPE_EXTENSION);
		}
		for (size_t i = 0; i < json_object_array_length(found.prompts); i++) {
			scan_prompts(json_object_get_string(json_object_array_get_idx(found.prompts, i)),
				SCOPE_EXTENSION);
		}
		json_object_put(found.skills);
		json_object_put(found.prompts);
	}
	for (size_t i = 0; i < skill_count; i++) {
		snprintf(skill_commands[i], sizeof(skill_commands[i]), "/skill:%.64s", skills[i]->name);
		skill_infos[i] = (struct phi_command){skill_commands[i], skills[i]->description, 1};
	}
}

/* ---- commands ------------------------------------------------------------- */

size_t resources_command_count(void) {

	return prompt_count + skill_count;
}

const struct phi_command *resources_command_at(size_t index) {

	if (index < prompt_count) {
		return &prompts[index]->info;
	}
	index -= prompt_count;

	return index < skill_count ? &skill_infos[index] : NULL;
}

/* Shell-like words: whitespace separated, with '…', "…" and \ escapes. */
static size_t split_arguments(const char *text, char ***out) {

	size_t count = 0, capacity = 8;
	char **words = malloc(capacity * sizeof(*words));
	char *word = malloc(strlen(text) + 1);
	if (!words || !word) {
		free(words);
		free(word);
		*out = NULL;
		return 0;
	}
	for (const char *at = text;;) {
		while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
			at++;
		}
		if (!*at) {
			break;
		}
		size_t used = 0;
		char quote = 0;
		for (; *at; at++) {
			if (quote) {
				if (*at == quote) {
					quote = 0;
				} else if (quote == '"' && *at == '\\' && at[1]) {
					word[used++] = *++at;
				} else {
					word[used++] = *at;
				}
			} else if (*at == '\'' || *at == '"') {
				quote = *at;
			} else if (*at == '\\' && at[1]) {
				word[used++] = *++at;
			} else if (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
				break;
			} else {
				word[used++] = *at;
			}
		}
		word[used] = 0;
		if (count == capacity) {
			char **grown = realloc(words, (capacity *= 2) * sizeof(*words));
			if (!grown) {
				break;
			}
			words = grown;
		}
		words[count++] = strdup(word);
	}
	free(word);
	*out = words;

	return count;
}

struct buffer {
	char *text;
	size_t used, capacity;
};

static void put(struct buffer *out, const char *text, size_t length) {

	if (out->used + length + 1 > out->capacity) {
		size_t capacity = (out->used + length + 1) * 2;
		char *grown = realloc(out->text, capacity);
		if (!grown) {
			return;
		}
		out->text = grown;
		out->capacity = capacity;
	}
	memcpy(out->text + out->used, text, length);
	out->used += length;
	out->text[out->used] = 0;
}

static void put_str(struct buffer *out, const char *text) {

	put(out, text, strlen(text));
}

static void put_range(struct buffer *out, char **words, size_t count, size_t start, size_t length) {

	for (size_t i = start, n = 0; i < count && n < length; i++, n++) {
		if (n) {
			put_str(out, " ");
		}
		put_str(out, words[i]);
	}
}

/* Pi's template substitutions. */
static char *substitute(const char *body, char **words, size_t count) {

	struct buffer out = {0};
	put(&out, "", 0);
	for (const char *at = body; *at;) {
		if (*at != '$') {
			size_t n = strcspn(at, "$");
			put(&out, at, n);
			at += n;
			continue;
		}
		if (!strncmp(at, "$ARGUMENTS", 10)) {
			put_range(&out, words, count, 0, count);
			at += 10;
			continue;
		}
		if (at[1] == '@') {
			put_range(&out, words, count, 0, count);
			at += 2;
			continue;
		}
		if (at[1] >= '1' && at[1] <= '9') {
			char *end;
			long n = strtol(at + 1, &end, 10);
			if ((size_t)n <= count) {
				put_str(&out, words[n - 1]);
			}
			at = end;
			continue;
		}
		if (at[1] == '{') {
			const char *close = strchr(at, '}');
			if (!close) {
				put(&out, at, 1);
				at++;
				continue;
			}
			char inner[256];
			snprintf(inner, sizeof(inner), "%.*s", (int)(close - at - 2), at + 2);
			char *fallback = strstr(inner, ":-");
			if (fallback) {
				*fallback = 0;
				fallback += 2;
			}
			if (!strcmp(inner, "@")) {
				if (count) {
					put_range(&out, words, count, 0, count);
				} else if (fallback) {
					put_str(&out, fallback);
				}
			} else if (!strncmp(inner, "@:", 2)) {
				char *end;
				long start = strtol(inner + 2, &end, 10);
				long length = *end == ':' ? strtol(end + 1, NULL, 10) : (long)count;
				if (start < 1) {
					start = 1;
				}
				put_range(&out, words, count, (size_t)start - 1, length < 0 ? 0 : (size_t)length);
			} else if (inner[0] >= '1' && inner[0] <= '9') {
				size_t n = (size_t)strtoul(inner, NULL, 10);
				if (n <= count && *words[n - 1]) {
					put_str(&out, words[n - 1]);
				} else if (fallback) {
					put_str(&out, fallback);
				}
			} else {
				put(&out, at, (size_t)(close - at + 1));
			}
			at = close + 1;
			continue;
		}
		put(&out, at, 1);
		at++;
	}

	return out.text;
}

static int shadowed(const char *command, size_t length) {

	for (size_t i = 0; i < PHI_COMMAND_COUNT; i++) {
		if (strlen(phi_commands[i].name) == length &&
			!strncmp(phi_commands[i].name, command, length)) {
			return 1;
		}
	}
	for (size_t i = 0; i < extensions_command_count(); i++) {
		const struct phi_command *extension = extensions_command_at(i);
		if (!extension) {
			continue;
		}
		const char *name = extension->name + (extension->name[0] == '/');
		if (strlen(name) == length - 1 && !strncmp(name, command + 1, length - 1)) {
			return 1;
		}
	}

	return 0;
}

char *resources_expand(const char *message) {

	if (!message || message[0] != '/') {
		return NULL;
	}
	size_t length = strcspn(message, " \t\r\n");
	const char *arguments = message + length;
	arguments += strspn(arguments, " \t\r\n");
	if (!strncmp(message, "/skill:", 7)) {
		for (size_t i = 0; i < skill_count; i++) {
			if (strlen(skills[i]->name) != length - 7 ||
				strncmp(skills[i]->name, message + 7, length - 7)) {
				continue;
			}
			char *text = read_text(skills[i]->path);
			if (!text) {
				return NULL;
			}
			const char *body;
			free(frontmatter(text, "name", &body));
			char directory[4096];
			snprintf(directory, sizeof(directory), "%s", skills[i]->path);
			char *slash = strrchr(directory, '/');
			if (slash) {
				*slash = 0;
			}
			struct buffer out = {0};
			put_str(&out, "<skill name=\"");
			put_str(&out, skills[i]->name);
			put_str(&out, "\" location=\"");
			put_str(&out, skills[i]->path);
			put_str(&out, "\">\nReferences are relative to ");
			put_str(&out, directory);
			put_str(&out, ".\n\n");
			put_str(&out, body);
			put_str(&out, "\n</skill>");
			if (*arguments) {
				put_str(&out, "\n\n");
				put_str(&out, arguments);
			}
			free(text);
			return out.text;
		}
		return NULL;
	}
	if (shadowed(message, length)) {
		return NULL;
	}
	for (size_t i = 0; i < prompt_count; i++) {
		if (strlen(prompts[i]->command) != length ||
			strncmp(prompts[i]->command, message, length)) {
			continue;
		}
		char *text = read_text(prompts[i]->path);
		if (!text) {
			return NULL;
		}
		const char *body;
		free(frontmatter(text, "description", &body));
		char **words;
		size_t count = split_arguments(arguments, &words);
		char *expanded = substitute(body, words, count);
		for (size_t j = 0; j < count; j++) {
			free(words[j]);
		}
		free(words);
		free(text);
		if (expanded) {
			size_t n = strlen(expanded);
			while (n && (expanded[n - 1] == '\n' || expanded[n - 1] == '\r')) {
				expanded[--n] = 0;
			}
		}
		return expanded;
	}

	return NULL;
}

/* ---- prompt and listing --------------------------------------------------- */

static void put_escaped(struct buffer *out, const char *text) {

	for (; *text; text++) {
		if (*text == '<') {
			put_str(out, "&lt;");
		} else if (*text == '>') {
			put_str(out, "&gt;");
		} else if (*text == '&') {
			put_str(out, "&amp;");
		} else {
			put(out, text, 1);
		}
	}
}

char *resources_system_prompt(void) {

	size_t shown = 0;
	for (size_t i = 0; i < skill_count; i++) {
		shown += skills[i]->model_invocation;
	}
	if (!shown) {
		return NULL;
	}
	struct buffer out = {0};
	put_str(&out,
		"The following skills provide specialized instructions for specific tasks.\n"
		"Use the read tool to load a skill's file when the task matches its description.\n"
		"When a skill file references a relative path, resolve it against the skill directory "
		"(the directory containing its file) and use that absolute path in tool calls.\n\n"
		"<available_skills>\n");
	for (size_t i = 0; i < skill_count; i++) {
		if (!skills[i]->model_invocation) {
			continue;
		}
		put_str(&out, "  <skill>\n    <name>");
		put_escaped(&out, skills[i]->name);
		put_str(&out, "</name>\n    <description>");
		put_escaped(&out, skills[i]->description);
		put_str(&out, "</description>\n    <location>");
		put_escaped(&out, skills[i]->path);
		put_str(&out, "</location>\n  </skill>\n");
	}
	put_str(&out, "</available_skills>");

	return out.text;
}

char *resources_describe(void) {

	static const char *scopes[] = {"user", "project", "package", "extension"};
	struct buffer out = {0};
	put(&out, "", 0);
	for (size_t i = 0; i < skill_count; i++) {
		char line[4400];
		snprintf(line, sizeof(line), "skill  /skill:%.64s  (%s%s)  %.4000s\n", skills[i]->name,
			scopes[skills[i]->scope], skills[i]->model_invocation ? "" : ", command only",
			skills[i]->path);
		put_str(&out, line);
	}
	for (size_t i = 0; i < prompt_count; i++) {
		char line[4400];
		snprintf(line, sizeof(line), "prompt %.80s%s%.200s  (%s)  %.4000s\n", prompts[i]->command,
			prompts[i]->hint ? " " : "", prompts[i]->hint ? prompts[i]->hint : "",
			scopes[prompts[i]->scope], prompts[i]->path);
		put_str(&out, line);
	}

	return out.text;
}
