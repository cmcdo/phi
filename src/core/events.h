#ifndef PHI_EVENTS_H
#define PHI_EVENTS_H
/* Lifecycle event bus between the core and extensions. The core emits named
   events with a JSON payload; each registered handler may return a JSON
   result, which the emitting code applies through `apply` before the next
   handler runs (so handlers compose, as in Pi). With no bus installed, or no
   handler for an event, emitting costs one function call.

   Event names, payloads and results are documented for extension authors in
   include/phi_extension.h. */
#include <json-c/json.h>

/* Apply one handler's result to the payload (and any caller state). source
   is the extension name. Return nonzero to stop later handlers. */
typedef int (*event_apply_fn)(
	json_object *payload, json_object *result, const char *source, void *state);

struct event_bus {
	int (*wanted)(const char *event);
	/* Returns the number of handlers that ran; *failed counts handler errors. */
	int (*emit)(
		const char *event, json_object *payload, event_apply_fn apply, void *state, int *failed);
	/* Nonzero while events cannot be delivered (inside an extension callback). */
	int (*blocked)(void);
	/* The UI is idle enough for periodic work ("tick" subscribers). */
	void (*tick)(void);
};

void events_set_bus(const struct event_bus *bus);
int events_blocked(void);
/* Called from the terminal UI's idle loop; delivers due "tick" events. */
void events_tick(void);
/* An observer (the JSON/RPC output modes) sees every event after handlers
   have run, including internal ones extensions cannot subscribe to
   (auto_retry_start/end). NULL removes it. */
void events_set_observer(void (*observer)(const char *event, json_object *payload));
/* A second observer, for background chat status (core/chats.c). */
void events_set_status_observer(
	void (*observer)(const char *event, json_object *payload), int (*wants)(const char *event));

/* True if any handler listens for event; build payloads only when it is. */
int events_wanted(const char *event);
/* Emit to all handlers in load order. apply may be NULL for notifications.
   Returns handler errors (a failed handler never stops the core). */
int events_emit(const char *event, json_object *payload, event_apply_fn apply, void *state);
/* Notification helper: payload built from one string field, or none. */
void events_notify(const char *event, const char *key, const char *value);
/* Gate helper: emit payload and return 0 if any handler answered
   {"cancel":true}. Takes ownership of payload. */
int events_allowed(const char *event, json_object *payload);
#endif
