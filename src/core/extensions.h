#ifndef PHI_EXTENSIONS_H
#define PHI_EXTENSIONS_H
#include <stddef.h>
#include <json-c/json.h>
#include "../tools.h"
#include "commands.h"
#include "stream.h"

struct extension_host {
	int (*confirm_native)(const char *name, const char *path, const char *key);
	int (*cancel)(void);
	void (*activity)(int busy);
	void (*message)(const char *text);
	void (*status)(const char *text);
	int (*reserved_tool)(const char *name);
	/* Host services offered to extensions (optional). Each receives a parsed
       request object and returns a newly owned result object, or NULL. */
	/* on_text (optional) receives the reply as it streams; nonzero stops it. */
	json_object *(*complete)(
		json_object *request, int (*on_text)(const char *text, void *state), void *state);
	/* Concurrent complete: on_done (once) and on_text run later, from Phi's
       loop. Returns an id > 0, or 0 with error. owner tags the requests an
       unloading extension must stop (complete_cancel with id 0). */
	int (*complete_async)(json_object *request, int (*on_text)(const char *text, void *state),
		void (*on_done)(json_object *answer, void *state), void *state, void *owner, char *error,
		size_t size);
	int (*complete_cancel)(int id, void *owner);
	json_object *(*exec)(json_object *request);
	/* Session control (api->call); extension is the caller's name. */
	json_object *(*call)(const char *extension, const char *method, json_object *request);
	/* Extension UI (api->call "ui_*": dialogs, notices, widgets, ...). */
	json_object *(*ui)(const char *extension, const char *method, json_object *request);
};

/* Single process-wide manager. No directory, compiler, watcher or helper is
   created when there are no extensions. Loading native code is always trusted. */
void extensions_open(const struct extension_host *host);
void extensions_open_cli(
	const struct extension_host *host, const char *command); /* approved owning module only */
void extensions_close(void);
json_object *extensions_manage(json_object *arguments);
int extensions_command(const char *message); /* 0 unknown; 1 handled */
int extensions_cli_known(const char *name); /* discovery index only, not authority to load code */
int extensions_cli_execute(int argc, char **argv); /* -1 unknown; otherwise exit status */
/* A command marked during_turn (set_command_options), typed while a turn
   runs: 1 if it was one and ran (its output, if any, in output), else 0. */
int extensions_command_during_turn(const char *message, char *output, size_t size);
size_t extensions_command_count(void);
const struct phi_command *extensions_command_at(size_t index);
const struct phi_tool *extensions_tool_find(const char *name);
int extensions_append_schema(json_object *array);
json_object *extensions_execute(const char *name, json_object *arguments);
/* Model-tool interception occurs before ordinary approval/execution. On an
   argument rewrite, *effective receives a newly owned call; caller releases it.
   Return nonzero to proceed, zero to deny; note explains rewrites/denials. */
int extensions_intercept_tool(
	json_object *call, json_object **effective, char *note, size_t note_size);
/* Caller frees. NULL means no dynamic extension context for this request. */
char *extensions_context(void);
/* Appends model-visible/UI-hidden messages generated once for this accepted user
   input. Returns the number appended; provider failures are omitted. */
size_t extensions_append_turn_messages(json_object *history, const char *user_message);
void extensions_event(const char *event, json_object *payload);

/* Provider/model extension layer. Requests are normalized JSON objects. The
   fallback callback implements Phi's built-in provider and is invoked when no
   extension owns the selected model. Returned JSON is a newly owned output
   array, or NULL on failure/cancellation. */
typedef json_object *(*extensions_provider_fallback_fn)(
	json_object *request, struct response_sink sink, void *userdata);
json_object *extensions_provider_dispatch(json_object *request, struct response_sink sink,
	extensions_provider_fallback_fn fallback, void *userdata);
/* Returns provider id for an extension-owned model, or NULL for the built-in
   provider. context_window is optional and receives 0 when unspecified. */
const char *extensions_provider_for_model(const char *model, int64_t *context_window);
json_object *extensions_provider_list(void);
/* Every model advertised by loaded providers, followed by virtual models.
   Returned strings are borrowed until the extension set changes. */
size_t extensions_model_count(void);
int extensions_model_at(size_t index, const char **id, const char **name, const char **provider,
	int64_t *context_window);
int extensions_provider_login(const char *id);
int extensions_provider_logout(const char *id);
#endif
