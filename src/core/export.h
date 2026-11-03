#ifndef PHI_EXPORT_H
#define PHI_EXPORT_H
#include <json-c/json.h>
#include <stddef.h>
/* Export the active in-memory branch, not the transcript's inactive branches.
   Never overwrite an existing file; the caller owns the error buffer. */
int phi_export_html(json_object *history, const char *path, char *error, size_t error_size);
#endif
