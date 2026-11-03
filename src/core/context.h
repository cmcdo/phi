#ifndef PHI_CONTEXT_H
#define PHI_CONTEXT_H
#include <stdint.h>
/* Default configured window, not an advertised optional maximum; 0 if unknown. */
int64_t context_window(const char *model);
/* Lets extension-provided models report their window without linking the
   context lookup to the extension loader. */
void context_set_provider_lookup(const char *(*lookup)(const char *model, int64_t *context_window));
#endif
