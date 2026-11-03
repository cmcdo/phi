#ifndef PHI_SETTINGS_H
#define PHI_SETTINGS_H
#include <stddef.h>
#include "../safety.h"

struct settings {
	char model[96], effort[16];
	int approval, sandbox;
};

const char *settings_approval_name(int value);
const char *settings_sandbox_name(int value);
int settings_approval_value(const char *value);
int settings_sandbox_value(const char *value);
const char *settings_effort(const char *value);
int settings_model_valid(const char *value);
/* Load: 1 loaded, 0 defaults, -1 invalid/unreadable. Save: 1 success, 0 failure. */
int settings_load(struct settings *s, char *error, size_t size);
int settings_save(const struct settings *s, char *error, size_t size);
#endif
