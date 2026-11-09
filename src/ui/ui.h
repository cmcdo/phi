#ifndef PHI_UI_H
#define PHI_UI_H
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <json-c/json.h>
struct phi_command;
void ui_set_extension_commands(size_t (*count)(void), const struct phi_command *(*at)(size_t));
void ui_set_extension_models(size_t (*count)(void),
	int (*at)(size_t, const char **, const char **, const char **, int64_t *));
struct session_entry;
struct session_tree;
/* Configure before ui_open; temporary chats never claim draft recovery slots. */
void ui_set_temporary(int temporary);
int ui_open(const char *model, const char *effort);
int ui_native_transition_register(
	const char *owner, const char *id, const char *path, char *problem, size_t capacity);
void ui_close(void);
int ui_board(void); /* Detach a hosted chat without cancelling work. */
/* Trusted core hosts can embed the normal editor; no extension input privilege.
   Host owns the surrounding view. No chat transcript or recovery file is bound. */
int ui_editor_attach(const char *model, const char *effort, const char *theme_path,
	void (*redraw)(void *), void (*feedback)(const char *, void *), void *context);
int ui_editor_attached(void);
void ui_editor_refresh(void);
int ui_editor_rows(int cols, int maximum);
void ui_editor_paint(int top, int rows, int cols, const char *label, int focus);
int ui_editor_key(int kind, wint_t key); /* normal edit actions: 1 submit, 2 quit */
int ui_editor_read(wint_t *key, int wait_ms);
int ui_editor_normal(void);
int ui_editor_pasting(void);
void ui_editor_focus(void);
void ui_editor_remember(const char *text);
int ui_editor_reset(const char *text);
int ui_editor_paste_event(int kind, wint_t key); /* 1 begin, -1 end, 0 other */
int ui_editor_activation(int kind, wint_t key);
void ui_editor_scope(int workspace);
void ui_editor_theme(const char *path);
void ui_editor_theme_source(char *path, size_t size);
int ui_theme_save_defaults(const char *name, char *error, size_t size);
void ui_append(const char *text);
void ui_shell_output(const char *text, int ansi_color, int bold);
void ui_assistant(const char *text);
/* Provider-visible reasoning summaries only; complete seals the current block. */
void ui_thinking(const char *text, int complete);
void ui_user(const char *text);
/* Tool-owned display policy; errors and approvals must still render. */
void ui_set_tool_visibility(int (*hidden)(const char *name));
int ui_tool_hidden(const char *name);
void ui_tool_begin(const char *name, const char *detail);
/* Snapshot actual invocation/result for semantic rendering; never execute them. */
void ui_tool_invocation(json_object *call);
/* Actual tool-sink text, before compatibility formatting. Not transcript text. */
void ui_tool_stream(const char *text);
void ui_tool_answer(json_object *answer);
void ui_tool_arguments(json_object *arguments);
/* tool_render: a heading (with markup) for the next ui_tool_begin, and body lines. */
void ui_tool_title(const char *title);
void ui_tool_line(const char *text);
/* tool_render for cards rebuilt on resume: render returns an owned object
   with optional "title" and "lines" (as the live event), or NULL. */
void ui_set_tool_renderer(
	json_object *(*render)(json_object *call, const char *heading, const char *detail));
void ui_tool_result(json_object *answer);
void ui_tool_end(const char *summary, int failed);
void ui_extension_message(const char *text);
void ui_extension_result(json_object *answer);
void ui_extension_status(const char *text);
/* line is a file line number, or 0 when unavailable (e.g. saved snippets). */
void ui_diff_line(char kind, size_t line, const char *text, size_t size);
void ui_diff_highlight(
	char kind, size_t line, const char *text, size_t size, size_t first, size_t end);
void ui_write_preview(const char *text);
void ui_read_preview(const char *text);
char *ui_input(void);
const char *ui_peek_queued(void);
char *ui_take_queued(void); /* queued while the agent is working; caller frees */
void ui_busy(int busy);
void ui_stopped(int tools_started);
void ui_pending_tool(json_object *call);
/* Interactive pre-execution window; returns nonzero on cancellation. */
int ui_tool_guard(json_object *call, int approval); /* 1 permit, 0 deny */
int ui_cancel(void);
/* Scoped login shortcut: plain c/C copies text without editing the draft. */
int ui_cancel_copy(const char *text, int (*copy)(const char *));
void ui_request_cancel(void); /* host cancellation also unwinds approval/dialog waits */
void ui_clear(void);
void ui_reload_theme(void);
/* A turn finished (not stopped): the "Worked for 14s" line. */
void ui_turn_done(void);
/* Nonzero while side requests run: the idle loop wakes every 30 ms. */
void ui_quick_ticks(int on);
/* Enter on a /command while a turn runs: handler returns 1 if it ran it now. */
void ui_set_turn_command(int (*handler)(const char *message));
/* Side requests' replies: a panel above the prompt while they stream; when
   done (error: the reason) each becomes a card, after the turn if one runs. */
void ui_side_begin(int id, const char *title);
void ui_side_text(int id, const char *text);
void ui_side_end(int id, const char *text, const char *error);
/* /theme NAME: save NAME as theme.json's extends; every window follows it. */
int ui_theme_save(const char *name, char *error, size_t size);
/* inherit: first creation/fork keeps the current choice; resume/new resets. */
void ui_theme_transcript(const char *path, int inherit);
void ui_configure(const char *model, const char *effort);
void ui_context_usage(int64_t input_tokens, int64_t output_tokens);
void ui_set_idle(void (*callback)(void));
void ui_set_wakeup(int (*pending)(void));
void ui_account_usage(const char *text);
const char *ui_effort(void);
int ui_columns(void);
void ui_cwd_changed(void);
int ui_resume(struct session_entry *entries, size_t count);
uint64_t ui_tree(struct session_tree *tree, int users_only);
void ui_set_history_reader(int (*reader)(struct session_tree *, char *, size_t));
int ui_history_open(void);
int ui_set_draft(const char *text);
void ui_restore_history(json_object *history);
/* Extension dialogs and inline feedback (dialog.inc). timeout_ms <= 0: none. */
int ui_confirm(const char *title, const char *message, int timeout_ms,
	int default_yes); /* 1 yes, 0 no, -1 cancelled */
int ui_select(
	const char *title, const char *const *options, size_t count, int timeout_ms); /* index or -1 */
char *ui_ask_text(
	const char *title, const char *placeholder, const char *value, int multiline, int timeout_ms);
void ui_header_widget_control(const char *action);
void ui_scene(const char *name);
void ui_notice(
	const char *text, int level); /* inline compatibility feedback: 0 info, 1 warning, 2 error */
/* Extension widgets (widgets.inc). count 0 removes; below: under the prompt. */
int ui_widget(
	const char *owner, const char *key, const char *const *lines, size_t count, int below);
void ui_header(const char *owner, const char *const *lines, size_t count);
void ui_footer(const char *owner, const char *text); /* NULL restores Phi's footer */
/* The line shown while a prompt runs: message replaces the word, detail
   the "(12s · ↓ 1k tokens)" part, tip the line under it. "" = the skin's. */
void ui_working(const char *message, const char *detail, const char *tip, int visible);
void ui_title(const char *title);
void ui_forget_owner(const char *owner);
int ui_composer(const char *owner, const char *label, const char *hint);
/* Only approved native-extension callers may register library paths. */
int ui_native_widget_register(
	const char *owner, const char *id, const char *path, char *problem, size_t capacity);
/* Extension input hooks: a key by name (return 1 to consume it), and Tab
   completion for the word before the cursor ([{value, label?}]). */
void ui_set_extension_hooks(
	int (*keys)(const char *name), json_object *(*complete)(const char *text, const char *word));
char *ui_editor_text(void); /* the prompt being typed; caller frees */
void ui_insert_text(const char *text); /* at the cursor */
int ui_tools_expanded(void);
void ui_set_tools_expanded(int expanded);
/* Themes for extensions: current colours, named themes, apply for this session. */
json_object *ui_theme_get(void);
json_object *ui_theme_list(void);
int ui_theme_set(const char *name, json_object *colors, char *error, size_t size);
void ui_card(const char *title, const char *text); /* a displayed extension message */
/* A model reply streaming into a card; replies as drawn (render_markdown). */
void ui_stream_begin(const char *title);
void ui_stream_text(const char *text);
void ui_stream_end(void);
void ui_set_markdown_hook(char *(*hook)(const char *text));
#endif
