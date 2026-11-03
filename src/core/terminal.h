#ifndef PHI_TERMINAL_H
#define PHI_TERMINAL_H
#include "agent.h"

/* Print/JSON/RPC modes: set before terminal_open(). Diagnostics go to
   stderr; stdout belongs to the mode. */
struct headless_ops {
	void (*poll)(void); /* handle pending input during a run */
	int (*confirm)(const char *title, const char *message); /* NULL: deny */
	void (*notify)(const char *text);
	void (*status)(const char *text);
	/* Extension UI calls (RPC: Pi's extension_ui_request); NULL: defaults. */
	json_object *(*ui)(const char *method, json_object *request);
};

void terminal_set_headless(const struct headless_ops *ops);
struct agent_host terminal_open(const char *model, const char *effort);
void terminal_close(void);
int terminal_native_approval(const char *name, const char *path, const char *key);
/* api->call "ui_*" from an extension: dialogs, notices and other UI. */
json_object *terminal_ui(const char *extension, const char *method, json_object *request);
void terminal_display_message(const char *extension, const char *type, const char *content);
/* A model reply streamed into a card ("show" on api->complete). */
void terminal_stream_begin(const char *title);
void terminal_stream_text(const char *text);
void terminal_stream_end(const char *full_text);
/* A side request's reply (complete_async with "show"): a panel above the
   prompt while it streams, then a card in the conversation. */
void terminal_side_begin(int id, const char *title);
void terminal_side_text(int id, const char *text);
void terminal_side_end(int id, const char *text, const char *error);
int terminal_has_ui(void);
void terminal_board(void);
void terminal_help(void);
int terminal_extension_cancel(void);
void terminal_extension_activity(int busy);
void terminal_extension_message(const char *text);
void terminal_extension_result(json_object *answer);
void terminal_extension_status(const char *text);
void terminal_set_extension_abort(void (*abort_compile)(void));
char *terminal_input(void); /* newly allocated draft, NULL on EOF/quit */
void terminal_begin_turn(const char *message);
void terminal_end_turn(void);
void terminal_clear(void);
void terminal_reload(void);
void terminal_editor_host(int enabled); /* scoped core editor host, not an extension API */
void terminal_theme_defaults(const char *name);
void terminal_theme(const char *name); /* /theme: list, or save this transcript's theme */
int terminal_login(void);
int terminal_copy(const char *text);
void terminal_shell_begin(const char *command, int send);
void terminal_shell_end(json_object *answer);
int terminal_resume(struct agent *agent);
int terminal_resume_path(struct agent *agent, const char *path);
int terminal_continue(struct agent *agent);
int terminal_tree(struct agent *agent);
int terminal_fork(struct agent *agent);
int terminal_clone(struct agent *agent);
/* For extension session control. */
void terminal_request_cancel(void);
void terminal_show_user(const char *text);
void terminal_show_origin(const char *via);
void terminal_set_service(void (*pump)(void), int (*pending)(void));
int terminal_navigate(struct agent *agent, uint64_t id, char *error, size_t size);
int terminal_fork_at(struct agent *agent, uint64_t id, char *error, size_t size);
void terminal_scene(const char *name);
void terminal_saved_history(void);
void terminal_notice(const char *text, int level);
void terminal_configure(const char *model, const char *effort);
void terminal_configure_safety(int approval, int sandbox);
const char *terminal_effort(void);
#endif
