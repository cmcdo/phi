#define _POSIX_C_SOURCE 200809L
#include "settings.h"
#include "models.h"
#include "fsutil.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

const char *settings_approval_name(int value) {

	return value == APPROVAL_DANGEROUS ? "dangerous" : value == APPROVAL_ALL ? "all" : "off";
}

const char *settings_sandbox_name(int value) {

	return value == SANDBOX_WORKSPACE ? "workspace" : value == SANDBOX_STRICT ? "strict" : "off";
}

int settings_approval_value(const char *value) {

	if (!strcmp(value, "off")) {
		return APPROVAL_OFF;
	}
	if (!strcmp(value, "dangerous")) {
		return APPROVAL_DANGEROUS;
	}
	if (!strcmp(value, "all") || !strcmp(value, "on")) {
		return APPROVAL_ALL;
	}

	return -1;
}

int settings_sandbox_value(const char *value) {

	if (!strcmp(value, "off")) {
		return SANDBOX_OFF;
	}
	if (!strcmp(value, "workspace") || !strcmp(value, "on")) {
		return SANDBOX_WORKSPACE;
	}
	if (!strcmp(value, "strict")) {
		return SANDBOX_STRICT;
	}

	return -1;
}

/* A level as saved: Phi's names ("off" is "none"), or another short
   lowercase word, which a model's catalogue may list (phi_effort_for
   turns it into one the model takes before it is sent). */
const char *settings_effort(const char *value) {

	if (!strcmp(value, "off")) {
		return "none";
	}
	int rank = phi_effort_rank(value);
	if (rank >= 0) {
		return phi_effort_order[rank];
	}
	size_t n = strlen(value);

	return n && n < sizeof(((struct settings *)0)->effort) &&
			strspn(value, "abcdefghijklmnopqrstuvwxyz0123456789-_") == n
		? value
		: NULL;
}

int settings_model_valid(const char *value) {

	value = phi_model_id(value);
	size_t n = strlen(value);
	if (!n || n >= sizeof(((struct settings *)0)->model)) {
		return 0;
	}

	return strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/") ==
		n;
}

static int settings_path(char *path, size_t size) {

	return phi_xdg_path(path, size, "XDG_CONFIG_HOME", ".config", "phi/settings.json");
}

int settings_load(struct settings *s, char *error, size_t size) {

	*s = (struct settings){.model = "gpt-5.6-luna", .effort = "none", .approval = 0, .sandbox = 0};
	char path[4096];
	if (!settings_path(path, sizeof(path))) {
		goto io_error;
	}
	size_t used = 0;
	char *text = phi_read_file(path, 8192, 0, &used);
	if (!text) {
		if (errno == ENOENT) {
			return 0;
		}
		if (errno == EINVAL || errno == EFBIG) {
			goto invalid;
		}
		goto io_error;
	}
	json_tokener *parser = json_tokener_new();
	if (!parser) {
		free(text);
		goto invalid;
	}
	json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
	json_object *object = json_tokener_parse_ex(parser, text, (int)used + 1);
	int valid = json_tokener_get_error(parser) == json_tokener_success &&
		json_object_is_type(object, json_type_object);
	json_tokener_free(parser);
	free(text);
	struct settings next = *s;
	if (valid) {
		json_object_object_foreach(object, key, value) {
			if (!strcmp(key, "approval") || !strcmp(key, "sandbox")) {
				/* Backward compatibility with the original boolean safety settings. */
				if (json_object_is_type(value, json_type_boolean)) {
					int enabled = json_object_get_boolean(value);
					if (!strcmp(key, "approval")) {
						next.approval = enabled ? APPROVAL_ALL : APPROVAL_OFF;
					} else {
						next.sandbox = enabled ? SANDBOX_WORKSPACE : SANDBOX_OFF;
					}
					continue;
				}
				if (!json_object_is_type(value, json_type_string)) {
					valid = 0;
					break;
				}
				const char *mode = json_object_get_string(value);
				if (strlen(mode) != (size_t)json_object_get_string_len(value)) {
					valid = 0;
					break;
				}
				int selected = !strcmp(key, "approval") ? settings_approval_value(mode)
														: settings_sandbox_value(mode);
				if (selected < 0) {
					valid = 0;
					break;
				}
				if (!strcmp(key, "approval")) {
					next.approval = selected;
				} else {
					next.sandbox = selected;
				}
				continue;
			}
			if (!json_object_is_type(value, json_type_string)) {
				valid = 0;
				break;
			}
			const char *str = json_object_get_string(value);
			if (strlen(str) != (size_t)json_object_get_string_len(value)) {
				valid = 0;
				break;
			}
			if (!strcmp(key, "model") && settings_model_valid(str)) {
				snprintf(next.model, sizeof(next.model), "%s", phi_model_id(str));
			} else if (!strcmp(key, "effort") && settings_effort(str)) {
				snprintf(next.effort, sizeof(next.effort), "%s", settings_effort(str));
			} else {
				valid = 0;
				break;
			}
		}
	}
	if (object) {
		json_object_put(object);
	}
	if (!valid) {
		goto invalid;
	}
	*s = next;
	return 1;
invalid:
	snprintf(error, size, "Invalid settings file.");
	return -1;
io_error:
	snprintf(error, size, "Cannot load settings: %s", strerror(errno));

	return -1;
}

int settings_save(const struct settings *s, char *error, size_t size) {

	char path[4096], directory[4096];
	if (!settings_model_valid(s->model) || !settings_effort(s->effort) ||
		s->approval < APPROVAL_OFF || s->approval > APPROVAL_ALL || s->sandbox < SANDBOX_OFF ||
		s->sandbox > SANDBOX_STRICT) {
		errno = EINVAL;
		goto failed;
	}
	if (!settings_path(path, sizeof(path))) {
		goto failed;
	}
	snprintf(directory, sizeof(directory), "%s", path);
	*strrchr(directory, '/') = 0;
	if (!phi_make_directories(directory)) {
		goto failed;
	}
	json_object *object = json_object_new_object();
	if (!object) {
		errno = ENOMEM;
		goto failed;
	}
	json_object_object_add(object, "model", json_object_new_string(phi_model_id(s->model)));
	json_object_object_add(object, "effort", json_object_new_string(settings_effort(s->effort)));
	json_object_object_add(
		object, "approval", json_object_new_string(settings_approval_name(s->approval)));
	json_object_object_add(
		object, "sandbox", json_object_new_string(settings_sandbox_name(s->sandbox)));
	const char *json = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PRETTY);
	size_t length = strlen(json);
	char *text = malloc(length + 2);
	int ok = text != NULL;
	if (ok) {
		memcpy(text, json, length);
		text[length] = '\n';
		text[length + 1] = 0;
		ok = phi_write_file_atomic(path, text, length + 1);
	} else {
		errno = ENOMEM;
	}
	free(text);
	json_object_put(object);
	if (!ok) {
		goto failed;
	}
	return 1;
failed:
	snprintf(error, size, "Settings unchanged: cannot save (%s).", strerror(errno));

	return 0;
}
