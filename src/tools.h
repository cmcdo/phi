#ifndef PHI_TOOLS_H
#define PHI_TOOLS_H
#include <json-c/json.h>
#include <stddef.h>
#include <stdio.h>
#include "safety.h"

/* run borrows a parsed argument object and returns a newly owned JSON result.
   Return {"error":"..."} for failures; validate arguments before side effects. */
struct phi_tool {
	const char *name;
	const char *schema; /* Responses API function definition */
	const char *detail_key; /* argument shown in the card heading, or NULL */
	json_object *(*run)(json_object *args);
	const char *label; /* card heading instead of the name, or NULL */
	/* MCP-style hints as a JSON object ({"readOnlyHint":true,...}), or NULL.
       Not verified: a permission extension may use them to decide what to confirm. */
	const char *annotations;
	int hide_success; /* suppress routine TUI presentation, not errors or approvals */
};

/* Optional presentation/cancellation hooks. Tools do not depend on ncurses. */
struct tool_host {
	void (*display)(const char *text, FILE *destination);
	void (*styled)(const char *text, int ansi_color, int bold);
	int (*cancel)(void);
	void (*diff)(char kind, size_t line, const char *text, size_t size, size_t first, size_t end);
	void (*write_preview)(const char *text);
	void (*read_preview)(const char *text);
	int banners;
};

void tools_set_host(const struct tool_host *host);
void tools_get_host(struct tool_host *host);
/* Receives every piece of text a running tool displays (for progress events). */
void tools_set_update_hook(void (*hook)(const char *text, void *context), void *context);
void tools_get_update_hook(void (**hook)(const char *text, void *context), void **context);

/* A provider owns its descriptors/schemas until it is replaced at a safe boundary.
   Dynamic descriptors use execute() rather than phi_tool.run. */
struct tool_provider {
	const struct phi_tool *(*find)(const char *name);
	int (*append_schema)(json_object *array);
	json_object *(*execute)(const char *name, json_object *args);
	void (*event)(const char *name, json_object *payload);
};

void tools_set_provider(const struct tool_provider *provider);
int tool_builtin_exists(const char *name);
void tools_reap_jobs(void); /* nonblocking maintenance for persistent jobs */
int tools_jobs_pending(void); /* running jobs, collectors, or unread output */
void tools_abort_jobs(void); /* signal-path best-effort termination */
void tools_shutdown(void); /* terminate and reap persistent background jobs */
void tools_user_pty(int columns); /* next/current user shell: 0 restores pipe mode */
void tools_set_sandbox(int mode);
int tools_sandbox_mode(void);
int tools_sandbox_enabled(void);
int tool_report(FILE *destination, const char *format, ...);
const struct phi_tool *tool_find(const char *name);
/* Caller owns the returned array; its cached schema elements are read-only. */
json_object *tools_schema(void);
/* Active-tool filter: names is an array of tool names, or NULL for all.
   Inactive tools are left out of tools_schema() and refused by tools_execute(). */
int tools_set_active(json_object *names);
int tool_is_active(const char *name);
json_object *tools_active_names(void); /* owned array, or NULL when all are active */
json_object *tools_all_names(void); /* owned array of every registered tool name */
/* Owned array of {name, description, parameters, annotations, active, source}
   for every registered tool (get_all_tools {details:true}); active_only keeps
   the tools execute_tool can call. */
json_object *tools_info(int active_only);
json_object *tools_execute(json_object *call); /* validates envelope and dispatches */
/* Extension host service: {"argv"|"command", cwd, stdin, timeout} -> {exit_code, output}. */
json_object *tools_exec_request(json_object *request);
#endif
