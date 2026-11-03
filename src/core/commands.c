#include "commands.h"
#include "extensions.h"
#include "models.h"
#include "agent.h"
#include "settings.h"
#include "terminal.h"
#include "auth.h"
#include "json.h"
#include "export.h"
#include "events.h"
#include "resources.h"
#include "../tools.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static char *last_assistant(json_object *history) {

	for (size_t i = json_object_array_length(history); i > 0; i--) {
		json_object *item = json_object_array_get_idx(history, i - 1);
		if (strcmp(field(item, "role"), "assistant")) {
			continue;
		}
		json_object *content = json_object_object_get(item, "content");
		if (json_object_is_type(content, json_type_string)) {
			const char *text = json_object_get_string(content);
			size_t size = strlen(text) + 1;
			char *copy = malloc(size);
			if (copy) {
				memcpy(copy, text, size);
			}
			return copy;
		}
		if (!json_object_is_type(content, json_type_array)) {
			continue;
		}

		size_t total = 0;
		for (size_t j = 0; j < json_object_array_length(content); j++) {
			json_object *part = json_object_array_get_idx(content, j);
			if (!strcmp(field(part, "type"), "output_text")) {
				total += strlen(field(part, "text"));
			}
		}
		char *out = malloc(total + 1);
		if (!out) {
			return NULL;
		}
		size_t used = 0;
		for (size_t j = 0; j < json_object_array_length(content); j++) {
			json_object *part = json_object_array_get_idx(content, j);
			if (strcmp(field(part, "type"), "output_text")) {
				continue;
			}
			const char *text = field(part, "text");
			size_t size = strlen(text);
			memcpy(out + used, text, size);
			used += size;
		}
		out[used] = 0;
		return out;
	}

	return NULL;
}

/* " (gpt-6.1-sol takes low, medium, ...)", in the server's order, or ""
   for a model the catalogue does not describe. */
static void effort_levels_text(const char *model, char *out, size_t size) {

	const struct phi_effort_level *levels;
	size_t count = phi_catalog_levels(model, &levels, NULL), n = 0;
	out[0] = 0;
	if (!count) {
		return;
	}
	n += (size_t)snprintf(out, size, " (%s takes", model);
	for (size_t i = 0; i < count && n < size; i++) {
		n += (size_t)snprintf(out + n, size - n, "%s %s", i ? "," : "",
			!strcmp(levels[i].name, "none") ? "off" : levels[i].name);
	}
	if (n < size) {
		snprintf(out + n, size - n, ")");
	}
}

/* /effort on its own: the level in use, and each the model offers with the
   server's own description. */
static void effort_report(const char *model, const char *current) {

	const struct phi_effort_level *levels;
	int preferred;
	size_t count = phi_catalog_levels(model, &levels, &preferred);
	tool_report(stdout, "\nEffort: %s\n", !strcmp(current, "none") ? "off" : current);
	if (!count) {
		tool_report(stdout, "Levels: off, low, medium, high\n");
		return;
	}
	tool_report(stdout, "%s levels:\n", model);
	for (size_t i = 0; i < count; i++) {
		tool_report(stdout, "  %-8s %s%s%s\n",
			!strcmp(levels[i].name, "none") ? "off" : levels[i].name, levels[i].description,
			(int)i == preferred ? " (default)" : "",
			!strcmp(levels[i].name, current) ? "  ← in use" : "");
	}
}

/* 0: ordinary message; 1: local command handled; 2: quit. */
int command_run(struct agent *agent, struct settings *settings, const char *message) {

	if (*message != '/') {
		return 0;
	}
	size_t n = strcspn(message, " \t\r\n");
	int command = -1;
	for (size_t i = 0; i < PHI_COMMAND_COUNT; i++) {
		if (strlen(phi_commands[i].name) == n && !strncmp(message, phi_commands[i].name, n)) {
			command = (int)i;
		}
	}
	if (command < 0 && agent && extensions_command(message)) {
		return 1;
	}
	/* Clipboard images are pasted as absolute paths. An unrecognised first
       token with another slash or a dot is a path, not a command typo.
       Check only that token: /unknown /tmp/image.png is still a command.
       Recognised commands above retain precedence over path-like names. */
	if (command < 0 && command_input_path(message)) {
		return 0;
	}
	if (command < 0) {
		tool_report(stdout, "\nUnknown command. Type / for suggestions.\n");
		return 1;
	}
	if (!agent && !command_workspace_allowed((size_t)command)) {
		tool_report(stdout,
			"\n%s needs an open chat. Board commands never target the selected agent implicitly.\n",
			phi_commands[command].name);
		return 1;
	}
	const char *argument = message + n;
	argument += strspn(argument, " \t");
	size_t size = strlen(argument);
	while (size && strchr(" \t\r\n", argument[size - 1])) {
		size--;
	}
	char value[1024];
	if (size >= sizeof(value) || (size && !phi_commands[command].argument)) {
		tool_report(stdout, "\nInvalid arguments for %s.\n", phi_commands[command].name);
		return 1;
	}
	memcpy(value, argument, size);
	value[size] = 0;
	const char *effort = terminal_effort();
	const char *shown_effort = !strcmp(effort, "none") ? "off" : effort;
	switch (command) {
	case CMD_QUIT:
		return 2;
	case CMD_BOARD:
		if (agent) {
			terminal_board();
		} else {
			tool_report(stdout, "\nAlready on the board.\n");
		}
		break;
	case CMD_CLEAR:
	case CMD_NEW: {
		if (!agent) {
			tool_report(stdout, "\nReady for a new task. Existing chats are unchanged.\n");
			break;
		}
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "reason", json_object_new_string("new"));
		json_object_object_add(payload, "target_file", json_object_new_string(""));
		if (!events_allowed("session_before_switch", payload)) {
			tool_report(stdout, "\nAn extension kept the current session.\n");
			break;
		}
		agent_clear(agent);
		terminal_clear();
		break;
	}
	case CMD_RELOAD:
		terminal_reload();
		resources_load("reload");
		break;
	case CMD_THEME:
		if (agent) {
			terminal_theme(value);
		} else {
			terminal_theme_defaults(value);
		}
		break;
	case CMD_HISTORY:
		terminal_saved_history();
		break;
	case CMD_RESOURCES: {
		char *text = resources_describe();
		tool_report(stdout, "\n%s\n",
			text && *text
				? text
				: "No skills or prompt templates. Add them under ~/.config/phi/skills and ~/.config/phi/prompts.\n");
		free(text);
		break;
	}
	case CMD_REFRESH_MODELS: {
		char error[256];
		if (phi_catalog_refresh(error, sizeof(error))) {
			tool_report(stdout,
				"\nModel catalogue refreshed: %zu models listed. Type /model for details.\n",
				phi_catalog_count());
		} else {
			tool_report(stderr, "\n%s\n", error);
		}
		break;
	}
	case CMD_EXTENSIONS:
	case CMD_RELOAD_EXTENSIONS: {
		const char *action = "list", *name = "";
		if (command == CMD_RELOAD_EXTENSIONS) {
			action = "reload";
			name = value;
		} else if (!strcmp(value, "api")) {
			action = "api";
		} else if (!strncmp(value, "disable ", 8)) {
			action = "disable";
			name = value + 8;
		} else if (*value) {
			tool_report(
				stderr, "\nUsage: /extensions [api | disable NAME] or /reload-extensions [NAME]\n");
			break;
		}
		json_object *arguments = json_object_new_object();
		json_object_object_add(arguments, "action", json_object_new_string(action));
		json_object_object_add(arguments, "name", json_object_new_string(name));
		json_object *answer = extensions_manage(arguments);
		if (answer) {
			terminal_extension_result(answer);
			json_object_put(answer);
		}
		json_object_put(arguments);
		break;
	}
	case CMD_RESUME:
		if (agent) {
			terminal_resume(agent);
		} else {
			tool_report(stdout,
				"\nUse :resume on the board (or /resume in its composer) to browse saved chats.\n");
		}
		break;
	case CMD_TREE:
		terminal_tree(agent);
		break;
	case CMD_FORK:
		terminal_fork(agent);
		break;
	case CMD_CLONE:
		terminal_clone(agent);
		break;
	case CMD_LOGIN: {
		const char *provider = *value ? value : NULL;
		char picked[128] = "";
		if (!provider) {
			json_object *list = extensions_provider_list();
			size_t count = list ? json_object_array_length(list) : 0;
			if (!count) {
				if (list) {
					json_object_put(list);
				}
				terminal_login();
				break;
			}
			json_object *options = json_object_new_array();
			json_object_array_add(options, json_object_new_string("OpenAI"));
			for (size_t i = 0; i < count; i++) {
				json_object_array_add(options,
					json_object_new_string(field(json_object_array_get_idx(list, i), "name")));
			}
			json_object *request = json_object_new_object();
			json_object_object_add(
				request, "title", json_object_new_string("Log in with which provider?"));
			json_object_object_add(request, "options", options);
			json_object *answer = terminal_ui("phi", "ui_select", request);
			json_object_put(request);
			const char *chosen = "";
			int dismissed =
				answer && json_object_get_boolean(json_object_object_get(answer, "cancelled"));
			if (answer && !dismissed) {
				chosen = field(answer, "value");
			}
			if (dismissed) {
				json_object_put(answer);
				json_object_put(list);
				break;
			} /* Esc: no login */
			if (!*chosen || !strcmp(chosen, "OpenAI")) {
				provider = "openai";
			} else {
				for (size_t i = 0; i < count; i++) {
					json_object *entry = json_object_array_get_idx(list, i);
					if (!strcmp(field(entry, "name"), chosen)) {
						snprintf(picked, sizeof(picked), "%s", field(entry, "id"));
						provider = picked;
						break;
					}
				}
			}
			if (answer) {
				json_object_put(answer);
			}
			json_object_put(list);
			if (!provider) {
				break;
			}
		}
		if (!strcmp(provider, "openai") || !strcmp(provider, "codex")) {
			terminal_login();
		} else {
			terminal_extension_activity(1);
			extensions_provider_login(provider);
			terminal_extension_activity(0);
		}
		break;
	}
	case CMD_LOGOUT:
		if (!*value || !strcmp(value, "openai") || !strcmp(value, "codex")) {
			codex_auth_logout();
		} else {
			extensions_provider_logout(value);
		}
		break;
	case CMD_NAME:
		if (!*value) {
			tool_report(stdout, "\nUsage: /name <session name>\n");
		} else {
			transcript_begin(&agent->transcript, agent->model, effort);
			if (agent->transcript.file && !agent->transcript.failed) {
				transcript_note(&agent->transcript, "name", value);
				events_notify("session_info_changed", "name", value);
				tool_report(stdout, "\nSession named: %s\n", value);
			} else {
				tool_report(stderr, "\nSession name was not saved.\n");
			}
		}
		break;
	case CMD_SESSION:
		tool_report(stdout, "\nSession: %s\nMessages: %zu\nModel: %s\nEffort: %s\n",
			*agent->transcript.path ? agent->transcript.path : "not saved yet",
			json_object_array_length(agent->history), agent->model, shown_effort);
		break;
	case CMD_COPY: {
		char *text = last_assistant(agent->history);
		if (text && *text && terminal_copy(text)) {
			terminal_notice("Copied last assistant response.", 0);
		} else {
			terminal_notice("No assistant response to copy.", 1);
		}
		free(text);
		break;
	}
	case CMD_HELP:
		terminal_help();
		break;
	case CMD_COMPACT: {
		char error[256];
		terminal_extension_activity(1);
		int compacted = agent_compact(agent, value, error, sizeof(error));
		terminal_extension_activity(0);
		if (compacted) {
			terminal_notice("Context compacted.", 0);
		} else {
			tool_report(stderr, "\n%s\n", error);
		}
		break;
	}
	case CMD_EXPORT: {
		if (!*value) {
			tool_report(stdout, "\nUsage: /export PATH.html\n");
			break;
		}
		char error[256];
		if (phi_export_html(agent->history, value, error, sizeof(error))) {
			tool_report(stdout, "\nExported active session to %s\n", value);
		} else {
			tool_report(stderr, "\n%s\n", error);
		}
		break;
	}
	case CMD_TRANSCRIPT:
		if (*agent->transcript.path) {
			tool_report(stdout, "\nTranscript%s: %s\n",
				agent->transcript.failed ? " (saving failed; may be incomplete)" : "",
				agent->transcript.path);
		} else {
			tool_report(stdout,
				agent->transcript.failed
					? "\nTranscript saving is unavailable.\n"
					: "\nNo transcript yet; saving starts with your first message.\n");
		}
		break;
	case CMD_SETTINGS:
		tool_report(stdout,
			"\nModel: %s\nEffort: %s\nApproval: %s\nSandbox: %s\nUse /approval off|dangerous|all and /sandbox off|workspace|strict.\n",
			settings->model, shown_effort, settings_approval_name(settings->approval),
			settings_sandbox_name(settings->sandbox));
		break;
	case CMD_APPROVAL:
	case CMD_SANDBOX: {
		int *current = command == CMD_APPROVAL ? &settings->approval : &settings->sandbox;
		const char *label = command == CMD_APPROVAL ? "Approval" : "Sandbox";
		if (!*value) {
			tool_report(stdout, "\n%s: %s\n", label,
				command == CMD_APPROVAL ? settings_approval_name(*current)
										: settings_sandbox_name(*current));
			break;
		}
		int selected = command == CMD_APPROVAL ? settings_approval_value(value)
											   : settings_sandbox_value(value);
		if (selected < 0) {
			tool_report(stdout,
				command == CMD_APPROVAL ? "\nExpected off, dangerous, or all.\n"
										: "\nExpected off, workspace, or strict.\n");
			break;
		}
		struct settings next = *settings;
		if (command == CMD_APPROVAL) {
			next.approval = selected;
		} else {
			next.sandbox = selected;
		}
		char error[256];
		if (!settings_save(&next, error, sizeof(error))) {
			tool_report(stderr, "\n%s\n", error);
			break;
		}
		*settings = next;
		terminal_configure_safety(settings->approval, settings->sandbox);
		tool_report(stdout, "\n%s: %s\n", label,
			command == CMD_APPROVAL ? settings_approval_name(selected)
									: settings_sandbox_name(selected));
		break;
	}
	case CMD_MODEL:
	case CMD_EFFORT: {
		if (!*value) {
			if (command == CMD_MODEL) {
				char source[4096];
				long long modified = 0;
				int available = phi_catalog_source(source, sizeof(source), &modified);
				tool_report(stdout, "\nCurrent model: %s\n", settings->model);
				if (!available) {
					tool_report(stdout,
						"No readable local Codex model catalogue. Use /refresh-models to fetch one, or /model <id> to enter a model manually.\n");
				} else {
					char date[32] = "unknown";
					time_t stamp = (time_t)modified;
					struct tm *when = localtime(&stamp);
					if (when) {
						strftime(date, sizeof(date), "%Y-%m-%d %H:%M", when);
					}
					tool_report(stdout, "Locally advertised models (cache modified %s):\n", date);
					for (size_t i = 0; i < phi_catalog_count(); i++) {
						const struct phi_catalog_model *model = phi_catalog_at(i);
						if (model) {
							tool_report(stdout, "  %s  %s%s\n", model->id, model->label,
								!strcmp(settings->model, model->id) ? "  (current)" : "");
						}
					}
					if (!phi_catalog_count()) {
						tool_report(stdout, "  (none listed)\n");
					}
					tool_report(stdout,
						"Cache: %s\nAvailability may change; this does not verify account access.\n",
						source);
				}
				size_t extension_models = extensions_model_count();
				if (extension_models) {
					tool_report(stdout, "Extension models:\n");
					for (size_t i = 0; i < extension_models; i++) {
						const char *id, *name, *provider;
						if (!extensions_model_at(i, &id, &name, &provider, NULL)) {
							continue;
						}
						if (provider) {
							tool_report(stdout, "  %s  %s  (%s)%s\n", id, name, provider,
								!strcmp(settings->model, id) ? "  (current)" : "");
						} else {
							tool_report(stdout, "  %s  %s  (virtual)%s\n", id, name,
								!strcmp(settings->model, id) ? "  (current)" : "");
						}
					}
				}
			} else {
				effort_report(agent ? agent->model : settings->model, effort);
			}
			break;
		}
		struct settings next = *settings;
		snprintf(next.effort, sizeof(next.effort), "%s", effort);
		if (command == CMD_MODEL) {
			if (!settings_model_valid(value)) {
				tool_report(stdout, "\nInvalid model name.\n");
				break;
			}
			const char *id = phi_model_id(value);
			memcpy(next.model, id, strlen(id) + 1);
		} else {
			/* Phi's names, or one the model's catalogue lists. */
			const struct phi_effort_level *levels;
			size_t count =
					   phi_catalog_levels(agent ? agent->model : settings->model, &levels, NULL),
				   listed = 0;
			for (size_t i = 0; i < count; i++) {
				if (!strcmp(levels[i].name, value)) {
					listed = 1;
				}
			}
			const char *selected = strcmp(value, "off") && phi_effort_rank(value) < 0 && !listed
				? NULL
				: settings_effort(value);
			if (!selected) {
				char offered[160];
				effort_levels_text(
					agent ? agent->model : settings->model, offered, sizeof(offered));
				tool_report(stdout, "\nUnknown effort: %s%s\n", value,
					*offered ? offered : " (off, minimal, low, medium, high, xhigh, max, ultra)");
				break;
			}
			snprintf(next.effort, sizeof(next.effort), "%s", selected);
		}
		/* A level the model does not take becomes its nearest, said aloud. */
		const char *usable = phi_effort_for(next.model, next.effort);
		char adjusted[160] = "";
		if (usable != next.effort && strcmp(usable, next.effort)) {
			snprintf(adjusted, sizeof(adjusted), " (%s has no %s)", next.model,
				!strcmp(next.effort, "none") ? "off" : next.effort);
			snprintf(next.effort, sizeof(next.effort), "%s", usable);
		}
		char error[256];
		if (!settings_save(&next, error, sizeof(error))) {
			tool_report(stderr, "\n%s\n", error);
			break;
		}
		*settings = next;
		if (agent) {
			agent->model = settings->model;
			agent->effort = settings->effort;
		}
		terminal_configure(settings->model, settings->effort);
		if (agent) {
			agent_note_selection(agent, "command");
			transcript_note(&agent->transcript, "model", settings->model);
		}
		char notice[512];
		snprintf(notice, sizeof(notice), "Saved: %s · %s%s", settings->model,
			!strcmp(settings->effort, "none") ? "off" : settings->effort, adjusted);
		terminal_notice(notice, 0);
		break;
	}
	}

	return 1;
}
