#ifndef PHI_SURFACES_H
#define PHI_SURFACES_H
#include <json-c/json.h>
#include "../../include/phi_surface.h"
#include "../../include/phi_renderer.h"
unsigned surfaces_generation(void);
/* Local, approved, pure semantic renderer. Returns rows, or zero for fallback. */
int surfaces_render_semantic(const char *provider, int width, int max_rows, json_object *context,
	struct phi_widget_cell *cells, size_t capacity);
int surfaces_register(
	const char *owner, const char *id, const char *path, char *error, size_t capacity);
void surfaces_forget(const char *owner); /* NULL: all providers and instances */
void surfaces_set_discovery(json_object *(*snapshot)(void)); /* returns owned chat array */
int surfaces_remote_active(void);
json_object *surfaces_serve(
	json_object *request, int peer_pid); /* owned cells, not library paths */
struct surface_client;
struct surface_client *surface_client_new(void);
void surface_client_free(struct surface_client *client);
/* Nonblocking; returns 1 if the cached frame/presence changed. */
int surface_client_poll(struct surface_client *, const char *provider, const char *slot, int width,
	int height, json_object *context);
const struct phi_widget_cell *surface_client_cells(const struct surface_client *);
int surface_client_wait(const struct surface_client *); /* -1: no timer */
/* Drain only an in-flight exchange; never invokes a local renderer or starts
   another request. Hidden/blocked hosts use only its pending deadline. */
int surface_client_service(struct surface_client *);
int surface_client_pending_wait(const struct surface_client *);
/* Passive readiness/deadline check: never renders or advances a request. */
int surface_client_due(const struct surface_client *);
int surface_client_event(struct surface_client *, const struct phi_surface_event *);
#endif
