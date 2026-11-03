#ifndef PHI_EXTENSION_BUILD_H
#define PHI_EXTENSION_BUILD_H
#include <stddef.h>
#include "extension_platform.h"
#define PHI_EXTENSION_SOURCE_LIMIT (256u * 1024u)
extern const char phi_extension_sdk[];
extern const char phi_extension_guide[];

struct extension_source {
	char *text;
	size_t length;
	char key[65];
};

int extension_name_valid(const char *name);
int extension_paths(char **sources, char **cache, char *error, size_t size);
int extension_state_directory(char **state, char *error, size_t size);
int extension_directory(const char *path, int create, int private_mode, char *error, size_t size);
int extension_source_read(const char *directory, const char *name, struct extension_source *source,
	char *error, size_t size);
int extension_source_matches(const char *directory, const char *name, const char *key);
/* Cache misses return 0. Invalid/corrupt entries also return 0 with an error.
   Only a matching source+SDK+platform key AND verified approved binary may load. */
int extension_cached(const char *cache, const char *name, const char *key, char **artifact,
	char *error, size_t size);
int extension_build(const char *cache, const char *name, const struct extension_source *source,
	const char *compiler, int (*cancel)(void), char **artifact, char binary_hash[65],
	struct phi_compile_result *result, char *error, size_t size);
int extension_approve(const char *cache, const char *name, const char *key, const char *binary_hash,
	char *error, size_t size);
int extension_disabled(const char *cache, const char *name);
int extension_set_disabled(
	const char *cache, const char *name, int disabled, char *error, size_t size);
#endif
