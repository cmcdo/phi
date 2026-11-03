#ifndef PHI_CODEX_H
#define PHI_CODEX_H
#include "stream.h"
struct codex;
struct codex *codex_open(void);
void codex_close(struct codex *client);
/* Both functions borrow their inputs and return newly owned JSON, or NULL. */
json_object *codex_request_body(json_object *history, const char *model, const char *effort);
json_object *codex_summary_body(json_object *history, const char *model, const char *effort);
json_object *codex_custom_body(
	json_object *history, const char *model, const char *effort, const char *instructions);
/* Extension host service: {"messages":[...], "system", "model", "effort"} ->
   {"text", "model", "usage"}, or {"error"}. No tools; not streamed. */
json_object *codex_complete_request(struct codex *client, json_object *request,
	const char *default_model, const char *default_effort, int (*cancel)(void));
/* The same, passing text to on_text as it arrives. */
json_object *codex_complete_stream(struct codex *client, json_object *request,
	const char *default_model, const char *default_effort, int (*cancel)(void),
	void (*on_text)(const char *text, void *state), void *state);
/* Why the latest request failed (NULL if it succeeded or the reason is unknown), and its HTTP status. */
const char *codex_last_error(struct codex *client);
long codex_last_status(struct codex *client);
json_object *codex_respond(struct codex *client, json_object *history, const char *model,
	const char *effort, struct response_sink sink);
json_object *codex_respond_summary(struct codex *client, json_object *history, const char *model,
	const char *effort, struct response_sink sink);
/* Side requests: a complete request (as above) that runs alongside the main
   turn, on its own connection. Returns its id (> 0), or 0 with error set.
   codex_side_pump advances them (call it often; it never blocks); on_text
   (nonzero stops: on_done then gets {"error":"Stopped"}) and on_done run
   from it, never from codex_side_start. on_done borrows the
   answer ({"text",...} or {"error"}). Built-in endpoint only. */
int codex_side_start(json_object *request, const char *default_model, const char *default_effort,
	int (*on_text)(const char *text, void *state),
	void (*on_done)(json_object *answer, void *state), void *state, void *owner, char *error,
	size_t size);
void codex_side_pump(void);
/* An answer found another way (an extension provider, run at once), handed
   to on_done at the next pump like any other: returns its id, or 0. */
int codex_side_later(json_object *answer, void (*on_done)(json_object *answer, void *state),
	void *state, void *owner);
int codex_side_active(void);
/* One request by id, or (id 0) every request of owner; no callbacks run. */
void codex_side_cancel(int id, void *owner);
#endif
