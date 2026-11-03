#ifndef PHI_EXTENSION_PLATFORM_H
#define PHI_EXTENSION_PLATFORM_H
#include <stddef.h>

/* This is the only dynamic-loader/compiler process boundary. The current
   implementation is POSIX; a Windows port needs DLL and process backends. */
void *phi_dynlib_open(const char *path, char *error, size_t size);
void *phi_dynlib_symbol(void *library, const char *name, char *error, size_t size);
void phi_dynlib_close(void *library);
const char *phi_dynlib_suffix(void);
void phi_abort_extension_compile(void); /* signal-safe best-effort group kill */
const char *phi_extension_build_id(void);

#define PHI_EXTENSION_DIAGNOSTICS 65536

struct phi_compile_result {
	char text[PHI_EXTENSION_DIAGNOSTICS + 1];
	int exit_code, truncated, cancelled, timed_out;
};

/* source/header/output are staged in a private directory by the caller.
   No shell expansion, inherited credentials, package lookup or daemon. */
int phi_compile_extension(const char *directory, const char *compiler, int timeout_seconds,
	int (*cancel)(void), struct phi_compile_result *result);
#endif
