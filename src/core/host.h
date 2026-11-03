#ifndef PHI_HOST_H
#define PHI_HOST_H
/* Session control for extensions (api->call). Methods that start work while
   Phi is idle queue it for the main loop; while a prompt is running, user
   messages become steering input and hidden context is added at the next
   tool boundary. */
#include <json-c/json.h>
struct agent;
struct settings;

void host_attach(struct agent *agent, struct settings *settings);
void host_set_busy(int busy);
json_object *host_call(const char *extension, const char *method, json_object *request);

/* Main-loop work queued by extensions. */
enum host_work {
	HOST_NONE,
	HOST_USER_MESSAGE,
	HOST_CONTINUE
};
enum host_work host_next_work(char **message); /* message owned by caller */
char *host_take_steer(void); /* visible message queued while busy */
int host_boundary(json_object *history); /* add queued hidden context; >0 asks for a turn */
int host_shutdown_requested(void);
int host_take_reload(void);
void host_reset(void); /* drop queued work (for tests and exit) */
/* RPC prompts: steer the running prompt, or (follow_up) wait until it ends. */
int host_enqueue_prompt(const char *text, int follow_up);
size_t host_pending_count(void);
json_object *host_clear_queue(void); /* {steering:[...], followUp:[...]} */
json_object *host_queue_snapshot(void); /* same shape, queue unchanged */
int host_busy(void);
void host_set_mode(const char *mode);
/* Extension command-line flags: phi.c passes the --name[=value] options it
   did not know; register_flag claims one, get_flag reads it. */
void host_set_flags(char **flags, size_t count);
/* Messages and entries sent with display:true are shown through this. */
void host_set_display(
	void (*display)(const char *extension, const char *type, const char *content));
const char *host_unclaimed_flag(void);
/* one no extension registered, or NULL */ /* interactive, print, json or rpc (get_session) */
#endif
