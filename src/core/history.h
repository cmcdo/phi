#ifndef PHI_HISTORY_H
#define PHI_HISTORY_H
#include <json-c/json.h>
void history_add_user(json_object *history, const char *text);
/* Phi-private hidden messages are persisted in history but stripped to ordinary
   user messages before provider serialization. */
int history_add_hidden_user(
	json_object *history, const char *text, const char *extension, const char *custom_type);
int history_is_hidden(json_object *item);
const char *history_hidden_extension(json_object *item);
const char *history_hidden_type(json_object *item);
/* Returns an owned array suitable for the provider. */
json_object *history_request_input(json_object *history);
void history_add_output(json_object *history, json_object *output);
/* Moves image_url out of answer into an untrusted image attachment. */
void history_add_tool(json_object *history, json_object *call, json_object *answer);
#endif
