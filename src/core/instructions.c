#define _POSIX_C_SOURCE 200809L
#include "instructions.h"
#include "fsutil.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define LIMIT (256 * 1024)
#ifndef PHI_DOCS_DIR
#define PHI_DOCS_DIR "docs"
#endif

/* Phi's harness contract is always present. User/project instruction files add
   policy and project context; they do not erase the tool/safety contract. */
static const char phi_base[] = {
#include "agent-system.inc"
};
static char *(*dynamic_provider)(void), *(*skills_provider)(void);
/* Per-run overrides from before_agent_start handlers. */
static char *run_replace, *run_append;

void instructions_set_run(const char *replace, const char *append) {

	free(run_replace);
	free(run_append);
	run_replace = replace && *replace ? strdup(replace) : NULL;
	run_append = append && *append ? strdup(append) : NULL;
}

void instructions_set_dynamic(char *(*provider)(void)) {

	dynamic_provider = provider;
}

void instructions_set_skills(char *(*provider)(void)) {

	skills_provider = provider;
}

static char *read_small(const char *path) {

	return phi_read_file(path, LIMIT, 0, NULL);
}

static int append(char **out, size_t *used, const char *label, const char *text) {

	if (!text || !*text) {
		return 1;
	}
	size_t heading = strlen(label), length = strlen(text);
	if (*used + heading + length + 3 > LIMIT) {
		return 0;
	}
	char *next = realloc(*out, *used + heading + length + 3);
	if (!next) {
		return 0;
	}
	*out = next;
	memcpy(next + *used, label, heading);
	*used += heading;
	memcpy(next + *used, text, length);
	*used += length;
	next[(*used)++] = '\n';
	next[(*used)++] = '\n';
	next[*used] = 0;

	return 1;
}

static void add_file(char **out, size_t *used, const char *path, const char *kind) {

	char *text = read_small(path);
	if (!text) {
		return;
	}
	char heading[4300];
	snprintf(heading, sizeof(heading), "\n%s from %s:\n", kind, path);
	append(out, used, heading, text);
	free(text);
}

static int config_path(char *path, size_t size, const char *name) {

	char leaf[64];
	snprintf(leaf, sizeof(leaf), "phi/%s", name);

	return phi_xdg_path(path, size, "XDG_CONFIG_HOME", ".config", leaf);
}

char *instructions_load(void) {

	if (run_replace) {
		/* An extension replaced the whole prompt for this run. */
		char *text = strdup(run_replace);
		size_t length = text ? strlen(text) : 0;
		if (text && run_append) {
			append(&text, &length, "\n", run_append);
		}
		return text;
	}
	char *base = strdup(phi_base), *cwd = getcwd(NULL, 0), path[4096];
	if (!base) {
		free(cwd);
		return NULL;
	}
	size_t used = strlen(base);
	/* Advertise locations, not whole manuals, so guides are read on demand. */
	append(&base, &used, "\n<docs>\n",
		"Phi documentation (read when asked about Phi, themes, extensions, or its SDK):\n"
		"- Themes: " PHI_DOCS_DIR "/themes.md\n"
		"- Extensions: " PHI_DOCS_DIR "/extensions.md\n"
		"- Detailed extension recipes: " PHI_DOCS_DIR "/extensions-agent.md\n"
		"- Phi source root: " PHI_DOCS_DIR "/..\n"
		"Read the relevant guide before changing harness behavior. Resolve its relative paths "
		"under the Phi source root, not the user's current project. If these files are "
		"unavailable, ask for their location rather than inventing paths or APIs. "
		"The extensions tool with action api returns the embedded SDK and extension guide.\n"
		"</docs>");
	if (cwd) {
		append(&base, &used, "\n<cwd>\n", cwd);
		append(&base, &used, "", "</cwd>");
	}

	/* Preserve SYSTEM.md precedence: project .phi/SYSTEM.md wins over the
       global file. It now augments Phi's fixed harness contract instead of
       replacing it. */
	char system_path[4096] = {0};
	if (config_path(path, sizeof(path), "SYSTEM.md") && !access(path, R_OK)) {
		snprintf(system_path, sizeof(system_path), "%s", path);
	}
	if (cwd) {
		int n = snprintf(path, sizeof(path), "%s/.phi/SYSTEM.md", cwd);
		if (n >= 0 && (size_t)n < sizeof(path) && !access(path, R_OK)) {
			snprintf(system_path, sizeof(system_path), "%s", path);
		}
	}
	if (*system_path) {
		add_file(&base, &used, system_path, "Additional system instructions");
	}

	if (config_path(path, sizeof(path), "APPEND_SYSTEM.md")) {
		add_file(&base, &used, path, "Additional system instructions");
	}
	if (cwd) {
		snprintf(path, sizeof(path), "%s/.phi/APPEND_SYSTEM.md", cwd);
		add_file(&base, &used, path, "Additional system instructions");
	}
	const char *no_agents = getenv("PHI_NO_AGENTS");
	int load_agents = !no_agents || strcmp(no_agents, "1");
	if (load_agents && config_path(path, sizeof(path), "AGENTS.md")) {
		add_file(&base, &used, path, "Project instructions");
	}
	if (load_agents && cwd) {
		/* Ancestors first; each directory prefers override, AGENTS, CLAUDE. */
		for (char *part = cwd + 1;; part++) {
			if (*part && *part != '/') {
				continue;
			}
			char saved = *part;
			*part = 0;
			const char *names[] = {"AGENTS.override.md", "AGENTS.md", "CLAUDE.md"};
			for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
				snprintf(path, sizeof(path), "%s/%s", cwd, names[i]);
				if (!access(path, R_OK)) {
					add_file(&base, &used, path, "Project instructions");
					break;
				}
			}
			*part = saved;
			if (!saved) {
				break;
			}
		}
	}
	free(cwd);
	if (skills_provider) {
		char *skills = skills_provider();
		if (skills) {
			append(&base, &used, "\n", skills);
			free(skills);
		}
	}
	if (run_append) {
		append(&base, &used, "\n", run_append);
	}
	if (dynamic_provider) {
		char *dynamic = dynamic_provider();
		if (dynamic) {
			append(&base, &used, "\nDynamic native-extension context:\n", dynamic);
			free(dynamic);
		}
	}

	return base;
}
