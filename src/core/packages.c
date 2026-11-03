#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "packages.h"
#include "extension_build.h"
#include "fsutil.h"
#include "json.h"
#include <ftw.h>
#include <glob.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

static int config_path(char *path, size_t size, const char *leaf) {

	return phi_xdg_path(path, size, "XDG_CONFIG_HOME", ".config", leaf);
}

static int data_path(char *path, size_t size, const char *leaf) {

	return phi_xdg_path(path, size, "XDG_DATA_HOME", ".local/share", leaf);
}

static json_object *load_record(void) {

	char path[4096];
	json_object *record = NULL;
	if (config_path(path, sizeof(path), "phi/packages.json")) {
		char *text = phi_read_file(path, 1 << 20, PHI_READ_OWNED, NULL);
		if (text) {
			record = json_tokener_parse(text);
		}
		free(text);
	}
	if (!json_object_is_type(json_object_object_get(record, "packages"), json_type_array)) {
		if (record) {
			json_object_put(record);
		}
		record = json_object_new_object();
		json_object_object_add(record, "packages", json_object_new_array());
	}

	return record;
}

static int save_record(json_object *record) {

	char path[4096], directory[4096];
	if (!config_path(path, sizeof(path), "phi/packages.json") ||
		!config_path(directory, sizeof(directory), "phi")) {
		return 0;
	}
	if (!phi_make_directories(directory)) {
		return 0;
	}
	const char *text = json_object_to_json_string_ext(
		record, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);

	return phi_write_file_atomic(path, text, strlen(text));
}

json_object *packages_installed(void) {

	json_object *record = load_record();
	json_object *list = json_object_get(json_object_object_get(record, "packages"));
	json_object_put(record);

	return list;
}

json_object *packages_manifest(const char *root) {

	char path[4096];
	snprintf(path, sizeof(path), "%s/package.json", root);
	char *text = phi_read_file(path, 1 << 20, PHI_READ_FOLLOW, NULL);
	json_object *package = text ? json_tokener_parse(text) : NULL;
	free(text);
	json_object *manifest = json_object_object_get(package, "phi");
	if (!manifest) {
		manifest = json_object_object_get(package, "pi");
	}
	manifest = json_object_is_type(manifest, json_type_object) ? json_object_get(manifest) : NULL;
	if (package) {
		json_object_put(package);
	}

	return manifest;
}

/* Run a program (no shell) and wait for it. */
static int run(char *const argv[]) {

	pid_t pid;
	if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ)) {
		return 0;
	}
	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return 0;
		}
	}

	return WIFEXITED(status) && !WEXITSTATUS(status);
}

struct source {
	char kind[8], url[4096], ref[256], name[64], path[4096];
};

/* Pi's source forms: git:host/path[@ref], https://…[@ref], ssh/git@…, or a
   local path. npm packages are not supported (Phi extensions are C). */
static int parse_source(const char *text, struct source *source, char *error, size_t size) {

	memset(source, 0, sizeof(*source));
	if (!strncmp(text, "npm:", 4)) {
		snprintf(error, size, "npm packages are not supported; use git or a local path");
		return 0;
	}
	int git = !strncmp(text, "git:", 4) || !strncmp(text, "https://", 8) ||
		!strncmp(text, "http://", 7) || !strncmp(text, "ssh://", 6) || !strncmp(text, "git@", 4);
	if (!git) {
		if (!realpath(text, source->path)) {
			snprintf(error, size, "No such package directory: %s", text);
			return 0;
		}
		struct stat info;
		if (stat(source->path, &info) || !S_ISDIR(info.st_mode)) {
			snprintf(error, size, "Not a directory: %s", text);
			return 0;
		}
		snprintf(source->kind, sizeof(source->kind), "local");
		snprintf(source->url, sizeof(source->url), "%s", source->path);
	} else {
		snprintf(source->kind, sizeof(source->kind), "git");
		const char *rest = !strncmp(text, "git:", 4) ? text + 4 : text;
		if (!strncmp(text, "git:", 4) && !strstr(rest, "://") && strncmp(rest, "git@", 4)) {
			snprintf(source->url, sizeof(source->url), "https://%s", rest);
		} else {
			snprintf(source->url, sizeof(source->url), "%s", rest);
		}
		char *slash = strrchr(source->url, '/'), *at = slash ? strrchr(slash, '@') : NULL;
		if (at) {
			snprintf(source->ref, sizeof(source->ref), "%s", at + 1);
			*at = 0;
		}
	}
	/* Name: last path component, without .git, reduced to [a-z0-9_-]. */
	const char *base = strrchr(source->url, '/');
	base = base ? base + 1 : source->url;
	size_t used = 0;
	for (const char *c = base; *c && used < sizeof(source->name) - 1; c++) {
		if (!strcmp(c, ".git")) {
			break;
		}
		char ch = *c >= 'A' && *c <= 'Z' ? (char)(*c - 'A' + 'a') : *c;
		source->name[used++] =
			(ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ? ch
																						   : '-';
	}
	if (!used || source->name[0] == '-') {
		snprintf(error, size, "Cannot derive a package name from %s", text);
		return 0;
	}

	return 1;
}

static json_object *find(json_object *list, const char *name, size_t *index) {

	for (size_t i = 0; i < json_object_array_length(list); i++) {
		json_object *package = json_object_array_get_idx(list, i);
		if (!strcmp(field(package, "name"), name) || !strcmp(field(package, "source"), name)) {
			if (index) {
				*index = i;
			}
			return package;
		}
	}

	return NULL;
}

/* Who owns extensions/NAME.c, if a package does. */
static const char *extension_owner(json_object *list, const char *extension) {

	for (size_t i = 0; i < json_object_array_length(list); i++) {
		json_object *package = json_object_array_get_idx(list, i);
		json_object *names = json_object_object_get(package, "extensions");
		if (!json_object_is_type(names, json_type_array)) {
			continue;
		}
		for (size_t j = 0; j < json_object_array_length(names); j++) {
			const char *name = json_object_get_string(json_object_array_get_idx(names, j));
			if (name && !strcmp(name, extension)) {
				return field(package, "name");
			}
		}
	}

	return NULL;
}

/* The package's C extension sources. */
static size_t extension_sources(const char *root, glob_t *found) {

	json_object *manifest = packages_manifest(root);
	json_object *list = json_object_object_get(manifest, "extensions");
	char pattern[4200];
	int flags = 0;
	memset(found, 0, sizeof(*found));
	if (json_object_is_type(list, json_type_array)) {
		for (size_t i = 0; i < json_object_array_length(list); i++) {
			const char *entry = json_object_get_string(json_object_array_get_idx(list, i));
			if (!entry || !*entry || *entry == '!' || strstr(entry, "..")) {
				continue;
			}
			snprintf(pattern, sizeof(pattern), "%s/%s", root, entry);
			if (!glob(pattern, flags, NULL, found)) {
				flags = GLOB_APPEND;
			}
		}
	} else {
		snprintf(pattern, sizeof(pattern), "%s/extensions/*.c", root);
		glob(pattern, 0, NULL, found);
	}
	if (manifest) {
		json_object_put(manifest);
	}

	return found->gl_pathc;
}

/* Copy the package's extensions into the extensions directory. Existing
   files owned by another package or by the user are never overwritten. */
static int copy_extensions(json_object *list, const char *package, const char *root,
	json_object *names, char *error, size_t size) {

	struct stat root_info;
	if (stat(root, &root_info) || !S_ISDIR(root_info.st_mode)) {
		snprintf(error, size, "Package directory is unavailable: %.400s", root);
		return 0;
	}
	char directory[4096];
	if (!config_path(directory, sizeof(directory), "phi/extensions") ||
		!phi_make_directories(directory)) {
		snprintf(error, size, "Cannot create the extensions directory");
		return 0;
	}
	glob_t found;
	extension_sources(root, &found);
	int ok = 1;
	for (size_t pass = 0; pass < 2 && ok; pass++) { /* check everything, then copy */
		for (size_t i = 0; i < found.gl_pathc && ok; i++) {
			const char *path = found.gl_pathv[i], *base = strrchr(path, '/');
			base = base ? base + 1 : path;
			size_t length = strlen(base);
			if (length < 3 || strcmp(base + length - 2, ".c")) {
				continue;
			}
			char name[64], target[4200];
			snprintf(name, sizeof(name), "%.*s", (int)(length - 2), base);
			if (!extension_name_valid(name)) {
				snprintf(error, size, "Invalid extension name: %s", name);
				ok = 0;
				break;
			}
			snprintf(target, sizeof(target), "%s/%s.c", directory, name);
			const char *owner = extension_owner(list, name);
			if (!pass) {
				if (!access(target, F_OK) && (!owner || strcmp(owner, package))) {
					snprintf(error, size, "Extension %s.c already exists (%s); remove it first",
						name, owner ? owner : "not from a package");
					ok = 0;
				}
				continue;
			}
			size_t used = 0;
			char *text = phi_read_file(path, 4 << 20, PHI_READ_FOLLOW, &used);
			if (!text || !phi_write_file_atomic(target, text, used)) {
				snprintf(error, size, "Cannot copy %s", path);
				ok = 0;
			} else {
				json_object_array_add(names, json_object_new_string(name));
			}
			free(text);
		}
	}
	globfree(&found);

	return ok;
}

static int remove_entry(const char *path, const struct stat *info, int flag, struct FTW *ftw) {

	(void)info;
	(void)flag;
	(void)ftw;

	return remove(path);
}

static void delete_extensions_except(json_object *package, json_object *keep) {

	char directory[4096], target[4200];
	if (!config_path(directory, sizeof(directory), "phi/extensions")) {
		return;
	}
	json_object *names = json_object_object_get(package, "extensions");
	if (!json_object_is_type(names, json_type_array)) {
		return;
	}
	size_t keep_count =
		json_object_is_type(keep, json_type_array) ? json_object_array_length(keep) : 0;
	for (size_t i = 0; i < json_object_array_length(names); i++) {
		const char *name = json_object_get_string(json_object_array_get_idx(names, i));
		/* Treat persisted metadata as data, never as a deletion path. */
		if (!extension_name_valid(name)) {
			continue;
		}
		int retained = 0;
		for (size_t j = 0; j < keep_count; j++) {
			const char *other = json_object_get_string(json_object_array_get_idx(keep, j));
			if (other && !strcmp(name, other)) {
				retained = 1;
			}
		}
		if (retained) {
			continue;
		}
		snprintf(target, sizeof(target), "%s/%s.c", directory, name);
		unlink(target);
	}
}

static void delete_extensions(json_object *package) {

	delete_extensions_except(package, NULL);
}

static void count_resources(const char *root, int *skills, int *prompts) {

	char pattern[4200];
	glob_t found;
	*skills = *prompts = 0;
	snprintf(pattern, sizeof(pattern), "%s/skills/*/SKILL.md", root);
	if (!glob(pattern, 0, NULL, &found)) {
		*skills = (int)found.gl_pathc;
		globfree(&found);
	}
	snprintf(pattern, sizeof(pattern), "%s/prompts/*.md", root);
	if (!glob(pattern, 0, NULL, &found)) {
		*prompts = (int)found.gl_pathc;
		globfree(&found);
	}
}

static int clone(struct source *source, char *error, size_t size) {

	char directory[3900];
	if (!data_path(directory, sizeof(directory), "phi/packages") ||
		!phi_make_directories(directory)) {
		snprintf(error, size, "Cannot create the packages directory");
		return 0;
	}
	snprintf(source->path, sizeof(source->path), "%s/%s", directory, source->name);
	if (!access(source->path, F_OK)) {
		snprintf(error, size, "%.400s already exists", source->path);
		return 0;
	}
	if (*source->ref) {
		char *branch[] = {"git", "clone", "--quiet", "--depth", "1", "--branch", source->ref, "--",
			source->url, source->path, NULL};
		if (run(branch)) {
			return 1;
		}
		/* A commit rather than a branch or tag: full clone, then check out. */
		nftw(source->path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
		char *full[] = {"git", "clone", "--quiet", "--", source->url, source->path, NULL};
		char *checkout[] = {"git", "-C", source->path, "checkout", "--quiet", source->ref, NULL};
		if (run(full) && run(checkout)) {
			return 1;
		}
	} else {
		char *plain[] = {
			"git", "clone", "--quiet", "--depth", "1", "--", source->url, source->path, NULL};
		if (run(plain)) {
			return 1;
		}
	}
	nftw(source->path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	snprintf(error, size, "git clone failed for %.400s", source->url);

	return 0;
}

static int install(const char *text) {

	char error[512];
	struct source source;
	if (!parse_source(text, &source, error, sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	json_object *record = load_record(), *list = json_object_object_get(record, "packages");
	if (find(list, source.name, NULL)) {
		fprintf(stderr, "A package named %s is already installed (phi remove %s first).\n",
			source.name, source.name);
		json_object_put(record);
		return 1;
	}
	if (!strcmp(source.kind, "git") && !clone(&source, error, sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		json_object_put(record);
		return 1;
	}
	json_object *names = json_object_new_array();
	if (!copy_extensions(list, source.name, source.path, names, error, sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		json_object *partial = json_object_new_object();
		json_object_object_add(partial, "extensions", names);
		delete_extensions(partial);
		json_object_put(partial);
		if (!strcmp(source.kind, "git")) {
			nftw(source.path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
		}
		json_object_put(record);
		return 1;
	}
	json_object *package = json_object_new_object();
	json_object_object_add(package, "name", json_object_new_string(source.name));
	json_object_object_add(package, "source", json_object_new_string(text));
	json_object_object_add(package, "kind", json_object_new_string(source.kind));
	json_object_object_add(package, "path", json_object_new_string(source.path));
	if (*source.ref) {
		json_object_object_add(package, "ref", json_object_new_string(source.ref));
	}
	json_object_object_add(package, "extensions", names);
	json_object_array_add(list, package);
	int saved = save_record(record);
	int skills, prompts;
	count_resources(source.path, &skills, &prompts);
	if (saved) {
		printf("Installed %s: %zu extension(s), %d skill(s), %d prompt template(s).\n%s",
			source.name, json_object_array_length(names), skills, prompts,
			json_object_array_length(names)
				? "Extensions load after you approve them (on start, or /reload-extensions).\n"
				: "");
	} else {
		fprintf(stderr, "Cannot save packages.json\n");
	}
	json_object_put(record);

	return !saved;
}

static int remove_package(const char *name) {

	json_object *record = load_record(), *list = json_object_object_get(record, "packages");
	size_t index;
	json_object *package = find(list, name, &index);
	if (!package) {
		fprintf(stderr, "No installed package %s.\n", name);
		json_object_put(record);
		return 1;
	}
	delete_extensions(package);
	char directory[4096];
	const char *path = field(package, "path");
	size_t length = data_path(directory, sizeof(directory), "phi/packages") ? strlen(directory) : 0;
	/* Only clones Phi made are deleted; local packages stay where they are. */
	if (!strcmp(field(package, "kind"), "git") && length && !strncmp(path, directory, length) &&
		path[length] == '/') {
		const char *leaf = path + length + 1;
		if (*leaf && !strchr(leaf, '/') && strcmp(leaf, ".") && strcmp(leaf, "..") &&
			!strcmp(leaf, field(package, "name"))) {
			nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
		}
	}
	printf("Removed %s.\n", field(package, "name"));
	json_object_array_del_idx(list, index, 1);
	int saved = save_record(record);
	json_object_put(record);

	return !saved;
}

static int update(const char *name) {

	json_object *record = load_record(), *list = json_object_object_get(record, "packages");
	int status = 0, matched = 0;
	for (size_t i = 0; i < json_object_array_length(list); i++) {
		json_object *package = json_object_array_get_idx(list, i);
		if (name && strcmp(field(package, "name"), name) &&
			strcmp(field(package, "source"), name)) {
			continue;
		}
		matched = 1;
		char path[4096];
		snprintf(path, sizeof(path), "%s", field(package, "path"));
		/* Pinned refs stay put; unpinned clones fast-forward. */
		if (!strcmp(field(package, "kind"), "git") && !*field(package, "ref")) {
			char *pull[] = {"git", "-C", path, "pull", "--quiet", "--ff-only", NULL};
			if (!run(pull)) {
				fprintf(stderr, "git pull failed for %s\n", field(package, "name"));
				status = 1;
				continue;
			}
		}
		char error[512];
		json_object *names = json_object_new_array();
		if (!copy_extensions(list, field(package, "name"), path, names, error, sizeof(error))) {
			fprintf(stderr, "%s: %s\n", field(package, "name"), error);
			/* Never delete the working extensions before a failed update.
               Track any partial new copies too, so removal can clean them. */
			json_object *old = json_object_object_get(package, "extensions");
			if (!json_object_is_type(old, json_type_array)) {
				old = json_object_new_array();
				json_object_object_add(package, "extensions", old);
			}
			for (size_t j = 0; j < json_object_array_length(names); j++) {
				json_object *item = json_object_array_get_idx(names, j);
				if (!extension_owner(list, json_object_get_string(item))) {
					json_object_array_add(old, json_object_get(item));
				}
			}
			json_object_put(names);
			status = 1;
			continue;
		}
		delete_extensions_except(package, names);
		json_object_object_add(package, "extensions", names);
		printf("Updated %s.\n", field(package, "name"));
	}
	if (name && !matched) {
		fprintf(stderr, "No installed package %s.\n", name);
		status = 1;
	}
	if (!save_record(record)) {
		status = 1;
	}
	json_object_put(record);

	return status;
}

static int list_packages(void) {

	json_object *list = packages_installed();
	if (!json_object_array_length(list)) {
		printf("No packages installed.\n");
	}
	for (size_t i = 0; i < json_object_array_length(list); i++) {
		json_object *package = json_object_array_get_idx(list, i);
		int skills, prompts;
		count_resources(field(package, "path"), &skills, &prompts);
		printf("%s  %s\n  %s\n  %zu extension(s), %d skill(s), %d prompt template(s)\n",
			field(package, "name"), field(package, "source"), field(package, "path"),
			json_object_array_length(json_object_object_get(package, "extensions")), skills,
			prompts);
	}
	json_object_put(list);

	return 0;
}

int packages_cli(int argc, char **argv) {

	if (argc < 2) {
		return -1;
	}
	const char *command = argv[1];
	if (!strcmp(command, "install") && argc == 3) {
		return install(argv[2]);
	}
	if ((!strcmp(command, "remove") || !strcmp(command, "uninstall")) && argc == 3) {
		return remove_package(argv[2]);
	}
	if (!strcmp(command, "update") && argc <= 3) {
		return update(argc == 3 ? argv[2] : NULL);
	}
	if (!strcmp(command, "list") && argc == 2) {
		return list_packages();
	}

	return -1;
}
