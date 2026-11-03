#ifndef PHI_BRIDGE_H
#define PHI_BRIDGE_H
#include <stddef.h>
#include <json-c/json.h>
/* Local chat control. Requests/results are owned by callers. No method can
   approve a tool or interpret a delivered message as a terminal command. */
json_object *bridge_call(const char *caller, const char *method, json_object *request);
int bridge_cli(int argc, char **argv); /* -1: not a bridge command */
int bridge_open(void);
void bridge_set_cancel(int (*cancel)(void));
void bridge_set_close_handler(int (*handler)(int force, char *error, size_t size));
json_object *bridge_request_close(const char *id, int force);
void bridge_close(void);
void bridge_pump(void);
int bridge_pending(void);
char *bridge_take(void); /* caller frees; sets origin for the next user entry */
json_object *bridge_take_origin(void); /* owned, consumed once by the agent */
const char *bridge_origin_label(void);
const char *bridge_socket(void);
#endif
