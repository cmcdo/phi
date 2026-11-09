#define _POSIX_C_SOURCE 200809L
#include "terminal.h"
#include "json.h"
#include "sessions.h"
#include "auth.h"
#include "settings.h"
#include "codex.h"
#include "extensions.h"
#include "events.h"
#include "chats.h"
#include "../tools.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>
#include "usage.h"
#include "surfaces.h"
#include "tool-result.h"
#ifdef PHI_UI
#include "../ui/ui.h"
#else
/* The console binary does not link the ncurses UI. tui is always 0 there, so
   these stubs are never reached; they let the adapter below read as plain
   code instead of a #ifdef around every call. */
static inline int ui_open(const char *m, const char *e) {

	(void)m;
	(void)e;

	return 0;
}

static inline void ui_close(void) {

}

static inline void ui_set_temporary(int temporary) {

	(void)temporary;
}

static inline void ui_set_tool_visibility(int (*hidden)(const char *)) {

	(void)hidden;
}

static inline int ui_tool_hidden(const char *name) {

	(void)name;
	return 0;
}

static inline int ui_board(void) {

	return 0;
}

static inline void ui_append(const char *t) {

	(void)t;
}

static inline void ui_shell_output(const char *t, int c, int b) {

	(void)t;
	(void)c;
	(void)b;
}

static inline void ui_assistant(const char *t) {

	(void)t;
}

static inline void ui_thinking(const char *t, int c) {

	(void)t;
	(void)c;
}

static inline void ui_user(const char *t) {

	(void)t;
}

static inline void ui_tool_begin(const char *n, const char *d) {

	(void)n;
	(void)d;
}

static inline void ui_tool_invocation(json_object *c) {

	(void)c;
}

static inline void ui_tool_stream(const char *t) {

	(void)t;
}

static inline void ui_tool_answer(json_object *a) {

	(void)a;
}

static inline void ui_tool_arguments(json_object *a) {

	(void)a;
}

static inline void ui_tool_title(const char *t) {

	(void)t;
}

static inline void ui_tool_line(const char *t) {

	(void)t;
}

static inline void ui_set_tool_renderer(
	json_object *(*r)(json_object *, const char *, const char *)) {

	(void)r;
}

static inline void ui_tool_result(json_object *a) {

	(void)a;
}

static inline void ui_tool_end(const char *s, int f) {

	(void)s;
	(void)f;
}

static inline void ui_extension_message(const char *t) {

	(void)t;
}

static inline void ui_extension_result(json_object *a) {

	(void)a;
}

static inline void ui_extension_status(const char *t) {

	(void)t;
}

static inline void ui_diff_highlight(
	char k, size_t l, const char *t, size_t s, size_t f, size_t e) {

	(void)k;
	(void)l;
	(void)t;
	(void)s;
	(void)f;
	(void)e;
}

static inline void ui_write_preview(const char *t) {

	(void)t;
}

static inline void ui_read_preview(const char *t) {

	(void)t;
}

static inline char *ui_input(void) {

	return NULL;
}

static inline const char *ui_peek_queued(void) {

	return NULL;
}

static inline char *ui_take_queued(void) {

	return NULL;
}

static inline void ui_busy(int b) {

	(void)b;
}

static inline void ui_stopped(int t) {

	(void)t;
}

static inline void ui_pending_tool(json_object *c) {

	(void)c;
}

static inline int ui_tool_guard(json_object *c, int a) {

	(void)c;
	(void)a;

	return 0;
}

static inline int ui_cancel(void) {

	return 0;
}

static inline int ui_cancel_copy(const char *text, int (*copy)(const char *)) {

	(void)text;
	(void)copy;

	return 0;
}

static inline void ui_request_cancel(void) {

}

static inline void ui_clear(void) {

}

static inline void ui_reload_theme(void) {

}

static inline void ui_set_turn_command(int (*h)(const char *)) {

	(void)h;
}

static inline void ui_side_begin(int i, const char *t) {

	(void)i;
	(void)t;
}

static inline void ui_side_text(int i, const char *t) {

	(void)i;
	(void)t;
}

static inline void ui_side_end(int i, const char *t, const char *e) {

	(void)i;
	(void)t;
	(void)e;
}

static inline void ui_quick_ticks(int on) {

	(void)on;
}

static inline void ui_turn_done(void) {

}

static inline void ui_theme_transcript(const char *p, int inherit) {

	(void)p;
	(void)inherit;
}

static inline int ui_theme_save(const char *n, char *e, size_t s) {

	(void)n;
	snprintf(e, s, "no terminal UI");

	return 0;
}

static inline int ui_theme_save_defaults(const char *n, char *e, size_t s) {

	return ui_theme_save(n, e, s);
}

static inline void ui_configure(const char *m, const char *e) {

	(void)m;
	(void)e;
}

static inline void ui_context_usage(int64_t i, int64_t o) {

	(void)i;
	(void)o;
}

static inline void ui_set_idle(void (*c)(void)) {

	(void)c;
}

static inline void ui_set_wakeup(int (*c)(void)) {

	(void)c;
}

static inline void ui_account_usage(const char *t) {

	(void)t;
}

static inline const char *ui_effort(void) {

	return "none";
}

static inline int ui_columns(void) {

	return 80;
}

static inline void ui_cwd_changed(void) {

}

static inline int ui_resume(struct session_entry *e, size_t c) {

	(void)e;
	(void)c;

	return -1;
}

static inline uint64_t ui_tree(struct session_tree *t, int u) {

	(void)t;
	(void)u;

	return 0;
}

static inline int ui_set_draft(const char *t) {

	(void)t;

	return 0;
}

static inline void ui_restore_history(json_object *h) {

	(void)h;
}

static inline int ui_confirm(const char *t, const char *m, int w, int d) {

	(void)t;
	(void)m;
	(void)w;
	(void)d;

	return -1;
}

static inline int ui_select(const char *t, const char *const *o, size_t c, int w) {

	(void)t;
	(void)o;
	(void)c;
	(void)w;

	return -1;
}

static inline char *ui_ask_text(const char *t, const char *p, const char *v, int m, int w) {

	(void)t;
	(void)p;
	(void)v;
	(void)m;
	(void)w;

	return NULL;
}

static inline void ui_scene(const char *name) {

	(void)name;
}

static inline int ui_history_open(void) {

	return 0;
}

static inline void ui_notice(const char *t, int l) {

	(void)t;
	(void)l;
}

static inline int ui_widget(const char *o, const char *k, const char *const *l, size_t c, int b) {

	(void)o;
	(void)k;
	(void)l;
	(void)c;
	(void)b;

	return 1;
}

static inline void ui_header(const char *o, const char *const *l, size_t c) {

	(void)o;
	(void)l;
	(void)c;
}

static inline void ui_footer(const char *o, const char *t) {

	(void)o;
	(void)t;
}

static inline void ui_working(const char *m, const char *d, const char *t, int v) {

	(void)m;
	(void)d;
	(void)t;
	(void)v;
}

static inline void ui_title(const char *t) {

	(void)t;
}

static inline void ui_forget_owner(const char *o) {

	(void)o;
}

static inline void ui_set_extension_hooks(
	int (*k)(const char *), json_object *(*c)(const char *, const char *)) {

	(void)k;
	(void)c;
}

static inline char *ui_editor_text(void) {

	return NULL;
}

static inline void ui_insert_text(const char *t) {

	(void)t;
}

static inline int ui_tools_expanded(void) {

	return 0;
}

static inline void ui_set_tools_expanded(int e) {

	(void)e;
}

static inline json_object *ui_theme_get(void) {

	return json_object_new_object();
}

static inline json_object *ui_theme_list(void) {

	return json_object_new_array();
}

static inline int ui_theme_set(const char *n, json_object *c, char *e, size_t s) {

	(void)n;
	(void)c;
	snprintf(e, s, "no terminal UI");

	return 0;
}

static inline void ui_card(const char *t, const char *x) {

	(void)t;
	(void)x;
}

static inline void ui_stream_begin(const char *t) {

	(void)t;
}

static inline void ui_stream_text(const char *t) {

	(void)t;
}

static inline void ui_stream_end(void) {

}

static inline void ui_set_markdown_hook(char *(*h)(const char *)) {

	(void)h;
}
#endif

/* This adapter owns terminal state; the agent and tools only see callbacks. */
static int tui;
static struct termios saved_terminal;
static int busy_terminal;
static int cancelled;
static const char *login_url;
static int turn_tools;
static const char *initial_effort;
static int approval_mode, turn_active, extension_activity_depth;
static void (*abort_extension_compile)(void);
static void (*service_pump)(void);
static int (*service_pending)(void);
static json_object *approval_context;

void terminal_set_service(void (*pump)(void), int (*pending)(void)) {

	service_pump = pump;
	service_pending = pending;
	ui_set_wakeup(pending);
}

static int service_wait_input(int fd, int yield) {

	if (!service_pump) {
		return 1;
	}
	for (;;) {
		service_pump();
		if (!yield && cancelled) {
			return 0;
		}
		if (yield && service_pending && service_pending()) {
			return 0;
		}
		struct pollfd p = {fd, POLLIN, 0};
		int ready = poll(&p, 1, 250);
		if (ready > 0 || (ready < 0 && errno != EINTR)) {
			return 1;
		}
	}
}

void terminal_show_origin(const char *via) {

	if (!via || !*via) {
		return;
	}
	char label[180];
	snprintf(label, sizeof(label), "\n[via %s]\n", via);
	if (tui) {
		ui_append(label);
	} else {
		fputs(label, stderr);
	}
}

void terminal_set_extension_abort(void (*callback)(void)) {

	abort_extension_compile = callback;
}

int terminal_extension_cancel(void);

/* ui_prompt_start/ui_prompt_end around an approval the user must answer. */
static void prompt_event(const char *event, const char *kind, const char *name, int approved) {

	if (!events_wanted(event)) {
		return;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "kind", json_object_new_string(kind));
	json_object_object_add(payload, "name", json_object_new_string(name));
	if (!strcmp(event, "ui_prompt_end")) {
		json_object_object_add(payload, "approved", json_object_new_boolean(approved));
	} else {
		json_object_object_add(payload, "question", json_object_new_string(name));
		if (approval_context) {
			json_object_object_add(payload, "call", json_object_get(approval_context));
		}
	}
	events_emit(event, payload, NULL, NULL);
	json_object_put(payload);
}

/* session_before_* gates: returns 0 (with a notice) if an extension cancels. */
static int session_gate(
	const char *event, const char *key, const char *value, const char *key2, int64_t value2) {

	if (!events_wanted(event)) {
		return 1;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, key, json_object_new_string(value));
	if (key2) {
		json_object_object_add(payload, key2, json_object_new_int64(value2));
	}
	if (events_allowed(event, payload)) {
		return 1;
	}
	tool_report(stdout, "\nAn extension kept the current session.\n");

	return 0;
}

/* A cancelled preview must not produce an end event for a card never opened. */
static int tool_presented;
static int provider_tool_presented;
static int tool_concealed;
static json_object *concealed_call;
static struct usage_client *usage_client;

static void flush_displays(void);

static void refresh_usage(void) {

	if (service_pump) {
		service_pump();
	}
	flush_displays();
	events_tick();
	/* Side requests (complete_async) advance here, idle or mid-turn. */
	codex_side_pump();
	chats_activity(extension_activity_depth || codex_side_active());
	ui_quick_ticks(codex_side_active() || surfaces_remote_active());
	char status[128];
	usage_poll(usage_client, status, sizeof(status));
	ui_account_usage(status);
}

/* Headless modes (print/JSON/RPC): stdout belongs to the protocol or the
   final answer, the terminal is never reconfigured, and interaction goes
   through the mode's hooks. */
static const struct headless_ops *headless;
static int tool_running;

void terminal_set_headless(const struct headless_ops *ops) {

	headless = ops;
}

/* Output routing: plain console output or the corresponding ncurses view. */
static void styled(const char *text, int color, int bold) {

	if (headless) {
		return;
	}
	if (tui) {
		if (tool_running && tool_concealed) {
			return;
		}
		if (tool_running) {
			ui_tool_stream(text);
		}
		ui_shell_output(text, color, bold);
		return;
	}
	(void)color;
	(void)bold;
	fputs(text, stderr);
	fflush(stderr);
}

static void display(const char *text, FILE *destination) {

	if (headless) {
		/* stdout belongs to the mode's output; diagnostics (failures,
           retries) stay on stderr, but tool chatter is dropped. */
		if (destination == stderr && !tool_running) {
			fputs(text, stderr);
			fflush(stderr);
		}
		return;
	}
	if (tui) {
		if (tool_running && tool_concealed) {
			return;
		}
		if (tool_running) {
			ui_tool_stream(text);
		}
		ui_append(text);
		return;
	}
	fputs(text, destination);
	fflush(destination);
}

void terminal_scene(const char *name) {

	if (tui && !headless) {
		ui_scene(name);
	} else {
		tool_report(stderr, "Scenes are available in phi-ui.\n");
	}
}

void terminal_saved_history(void) {

	if (!tui || headless) {
		fprintf(stderr, "Saved-history browsing needs the interactive UI.\n");
		return;
	}
	if (!ui_history_open()) {
		ui_notice("No saved history is available.", 1);
	}
}

void terminal_notice(const char *text, int level) {

	if (!text || !*text) {
		return;
	}
	if (tui && !headless) {
		ui_notice(text, level);
	} else {
		tool_report(level ? stderr : stdout, "\n%s\n", text);
	}
}

static void pending_tool(json_object *call) {

	if (tui && !cancelled) {
		ui_pending_tool(call);
	}
}

static void context_usage(int64_t input_tokens, int64_t output_tokens) {

	if (tui) {
		ui_context_usage(input_tokens, output_tokens);
	}
}

static void assistant_text(const char *text) {

	if (headless) {
		return;
	}
	if (tui) {
		ui_assistant(text);
		return;
	}
	display(text, stdout);
}

static void thinking_text(const char *text, int complete) {

	if (tui && !headless) {
		ui_thinking(text, complete);
	}
}

/* Console mode temporarily disables line buffering so Esc can cancel a turn. */
static void restore_terminal(void) {

	if (!busy_terminal) {
		return;
	}
	tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
	busy_terminal = 0;
}

static int check_cancel(void) {

	if (service_pump) {
		service_pump();
	}
	if (headless) {
		if (headless->poll) {
			headless->poll();
		}
		return cancelled;
	}
	if (tui) {
		if (ui_cancel_copy(login_url, terminal_copy)) {
			cancelled = 1;
		}
		return cancelled;
	}
	if (busy_terminal) {
		struct pollfd input = {STDIN_FILENO, POLLIN, 0};
		if (poll(&input, 1, 0) > 0 && (input.revents & POLLIN)) {
			char keys[128];
			ssize_t count = read(STDIN_FILENO, keys, sizeof(keys));
			for (ssize_t i = 0; i < count; i++) {
				if (keys[i] == '\033' || keys[i] == 3) {
					cancelled = 1;
				} else if (login_url && (keys[i] == 'c' || keys[i] == 'C')) {
					int ok = terminal_copy(login_url);
					tool_report(stderr, ok ? "Login link copied to clipboard.\n"
						: "Could not copy the login link.\n");
				}
			}
		}
	}

	return cancelled;
}

const char *terminal_effort(void) {

	if (tui) {
		return ui_effort();
	}

	return initial_effort;
}

/* A submitted draft while work is active steers at the next tool boundary. */
static char *take_steer(void) {

	if (tui) {
		if (check_cancel()) {
			return NULL;
		}
		const char *queued = ui_peek_queued();
		/* Preserve local commands/shell input for the normal main loop. */
		if (!queued || queued[0] == '/' || queued[0] == '!') {
			return NULL;
		}
		char *message = ui_take_queued();
		if (message) {
			ui_user(message);
		}
		return message;
	}

	return NULL;
}

void terminal_editor_host(int enabled) {

	static int saved_tui, saved_approval, saved_sandbox, entered;
	static const char *saved_effort;
	if (enabled && !entered) {
		saved_tui = tui;
		saved_effort = initial_effort;
		saved_approval = approval_mode;
		saved_sandbox = tools_sandbox_mode();
		tui = 1;
		entered = 1;
	} else if (!enabled && entered) {
		tui = saved_tui;
		initial_effort = saved_effort;
		approval_mode = saved_approval;
		tools_set_sandbox(saved_sandbox);
		entered = 0;
	}
}

void terminal_configure(const char *model, const char *effort) {

	initial_effort = effort;
	if (tui) {
		ui_configure(model, effort);
	}
}

void terminal_configure_safety(int approval, int sandbox) {

	approval_mode = approval;
	tools_set_sandbox(sandbox);
}

static int tool_needs_approval(json_object *call) {

	if (approval_mode == APPROVAL_OFF) {
		return 0;
	}
	if (approval_mode != APPROVAL_DANGEROUS) {
		return 1;
	}

	/* Explicitly whitelist inspection. New/unknown tool kinds fail closed. */
	const char *name = field(call, "name");
	if (!strcmp(name, "read") || !strcmp(name, "bash_poll")) {
		return 0;
	}
	if (!strcmp(name, "extensions")) {
		json_object *args = json_tokener_parse(field(call, "arguments"));
		const char *action = field(args, "action");
		int inspection = !strcmp(action, "list") || !strcmp(action, "api");
		if (args) {
			json_object_put(args);
		}
		return !inspection;
	}
	if (!strcmp(name, "git")) {
		json_object *args = json_tokener_parse(field(call, "arguments"));
		const char *action = field(args, "action");
		int inspection = !strcmp(action, "status") || !strcmp(action, "diff");
		if (args) {
			json_object_put(args);
		}
		return !inspection; /* Checkpoints can execute configured Git filters. */
	}

	return 1;
}

static void print_approval_text(FILE *out, const char *text) {

	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p < 32 || *p == 127) {
			fprintf(out, "\\x%02x", *p);
		} else {
			fputc(*p, out);
		}
	}
}

static int console_approval(json_object *call) {

	if (headless) {
		char title[256];
		snprintf(title, sizeof(title), "Approve tool %.200s?", field(call, "name"));
		if (headless->confirm) {
			return headless->confirm(title, field(call, "arguments"));
		}
		tool_report(stderr,
			"Tool denied: approval needs an interactive session (use /approval off for unattended runs).\n");
		return 0;
	}
	/* Never interpret piped model input or a redirected file as permission. */
	FILE *tty = fopen("/dev/tty", "r+");
	if (!tty) {
		tool_report(stderr, "Tool denied: approval requires an interactive terminal.\n");
		return 0;
	}
	int was_busy = busy_terminal;
	restore_terminal();
	tcflush(fileno(tty), TCIFLUSH);
	fputs("\nApprove tool ", tty);
	print_approval_text(tty, field(call, "name"));
	fputc(' ', tty);
	print_approval_text(tty, field(call, "arguments"));
	fputs("? [y/N] ", tty);
	fflush(tty);

	char line[16];
	int yes = 0;
	if (service_wait_input(fileno(tty), 0) && fgets(line, sizeof(line), tty)) {
		size_t length = strcspn(line, "\r\n");
		/* Consume an overlong response so it cannot become the next command. */
		if (line[length] == '\0') {
			int ch;
			while ((ch = fgetc(tty)) != '\n' && ch != EOF) {
			}
		} else {
			line[length] = '\0';
			yes = !strcmp(line, "y") || !strcmp(line, "Y") || !strcmp(line, "yes") ||
				!strcmp(line, "YES");
		}
	}
	fclose(tty);
	if (was_busy) {
		struct termios mode = saved_terminal;
		mode.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
		mode.c_cc[VMIN] = mode.c_cc[VTIME] = 0;
		if (!tcsetattr(STDIN_FILENO, TCSANOW, &mode)) {
			busy_terminal = 1;
		}
	}

	return yes;
}

int terminal_extension_cancel(void) {

	return check_cancel();
}

void terminal_extension_activity(int busy) {

	if (busy) {
		extension_activity_depth++;
	} else if (extension_activity_depth) {
		extension_activity_depth--;
	}
	busy = extension_activity_depth != 0;
	chats_activity(busy || codex_side_active());
	if (turn_active) {
		return; /* The enclosing agent turn owns its terminal state. */
	}
	if (busy) {
		cancelled = 0;
	}
	if (headless) {
		if (!busy) {
			cancelled = 0;
		}
		return;
	}
	if (tui) {
		ui_busy(busy);
		if (!busy) {
			cancelled = 0;
		}
		return;
	}
	if (!busy) {
		restore_terminal();
		cancelled = 0;
	} else if (isatty(STDIN_FILENO) && !tcgetattr(STDIN_FILENO, &saved_terminal)) {
		struct termios mode = saved_terminal;
		mode.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
		mode.c_cc[VMIN] = mode.c_cc[VTIME] = 0;
		if (!tcsetattr(STDIN_FILENO, TCSANOW, &mode)) {
			busy_terminal = 1;
		}
	}
}

int terminal_native_approval(const char *name, const char *path, const char *key) {

	json_object *call = json_object_new_object(), *arguments = json_object_new_object();
	if (!call || !arguments) {
		if (call) {
			json_object_put(call);
		}
		if (arguments) {
			json_object_put(arguments);
		}
		return 0;
	}
	json_object_object_add(arguments, "warning",
		json_object_new_string(
			"TRUSTED NATIVE CODE: runs inside Phi, outside its sandbox; can access credentials or crash Phi."));
	json_object_object_add(arguments, "name", json_object_new_string(name));
	json_object_object_add(arguments, "source", json_object_new_string(path));
	json_object_object_add(arguments, "sha256", json_object_new_string(key));
	json_object_object_add(call, "name", json_object_new_string("native_extension_load"));
	json_object_object_add(call, "arguments",
		json_object_new_string(json_object_to_json_string_ext(arguments, JSON_C_TO_STRING_PLAIN)));
	json_object_put(arguments);
	if (tui) {
		char notice[4096];
		snprintf(notice, sizeof(notice),
			"%s\nSource: %s\nTrusted native code runs inside Phi, outside the sandbox.", name,
			path);
		ui_extension_message(notice);
	} else {
		tool_report(stdout,
			"\nNative extension: %s\nSource: %s\nTrust applies to native code, not a sandboxed tool.\n",
			name, path);
	}
	int permitted;
	approval_context = call;
	prompt_event("ui_prompt_start", "native_approval", name, 0);
	if (tui) {
		permitted = ui_tool_guard(call, 1);
	} else {
		permitted = console_approval(call);
	}
	prompt_event("ui_prompt_end", "native_approval", name, permitted);
	approval_context = NULL;
	json_object_put(call);

	return permitted;
}

static void present_tool(json_object *call);

static int tool_visibility_hidden(const char *name) {

	const struct phi_tool *tool = tool_find(name);
	return tool && tool->hide_success;
}

static int begin_tool(json_object *call) {

	if (tui) {
		tool_presented = 0;
		provider_tool_presented = 0;
		tool_concealed = 0;
		json_object_put(concealed_call);
		concealed_call = NULL;
		/* Keep the pre-execution preview/cancel window before any tool runs. */
		if (cancelled) {
			return 0;
		}
		int ask = tool_needs_approval(call);
		approval_context = call;
		if (ask) {
			prompt_event("ui_prompt_start", "tool_approval", field(call, "name"), 0);
		}
		int allowed = ui_tool_guard(call, ask);
		if (ask) {
			prompt_event("ui_prompt_end", "tool_approval", field(call, "name"), allowed);
		}
		approval_context = NULL;
		if (!allowed) {
			return 0;
		}
		turn_tools++;
		if (ui_tool_hidden(field(call, "name"))) {
			/* Keep only the invocation needed to show a later failure. Do not
			   open a UI card, so hidden successes never enter activity counts. */
			tool_concealed = 1;
			concealed_call = json_object_get(call);
		} else {
			present_tool(call);
		}
		tool_running = 1;
		return 1;
	}
	if (cancelled) {
		return 0;
	}
	if (tool_needs_approval(call)) {
		approval_context = call;
		prompt_event("ui_prompt_start", "tool_approval", field(call, "name"), 0);
		int allowed = console_approval(call);
		prompt_event("ui_prompt_end", "tool_approval", field(call, "name"), allowed);
		approval_context = NULL;
		if (!allowed) {
			return 0;
		}
	}
	turn_tools++;
	tool_running = 1;

	return 1;
}

/* tool_render: a handler may give the card a title and up to 8 lines. */
static int tool_render_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	(void)state;
	json_object *title = json_object_object_get(result, "title"),
				*lines = json_object_object_get(result, "lines");
	if (json_object_is_type(title, json_type_string) && json_object_get_string_len(title) <= 200) {
		json_object_object_add(payload, "title", json_object_get(title));
	}
	if (json_object_is_type(lines, json_type_array) && json_object_array_length(lines) <= 8) {
		json_object_object_add(payload, "lines", json_object_get(lines));
	}

	return 0;
}

/* tool_render: ask handlers for a card's title and lines. NULL when
   nobody is listening. restored: a card rebuilt when a session resumes,
   possibly of a tool that is not registered (yet). */
static json_object *render_query(
	const char *name, const char *heading, const char *detail, json_object *args, int restored) {

	if (!events_wanted("tool_render") || events_blocked()) {
		return NULL;
	}
	json_object *render = json_object_new_object();
	json_object_object_add(render, "name", json_object_new_string(name));
	json_object_object_add(render, "label", json_object_new_string(heading));
	json_object_object_add(render, "detail", json_object_new_string(detail));
	json_object_object_add(render, "arguments", args ? json_object_get(args) : NULL);
	if (restored) {
		json_object_object_add(render, "restored", json_object_new_boolean(1));
	}
	events_emit("tool_render", render, tool_render_apply, NULL);

	return render;
}

static json_object *restored_render(json_object *call, const char *heading, const char *detail) {

	json_object *args = json_tokener_parse(field(call, "arguments"));
	json_object *render = render_query(field(call, "name"), heading, detail, args, 1);
	if (args) {
		json_object_put(args);
	}

	return render;
}

/* The tool card heading: label and detail argument. */
static void present_tool(json_object *call) {

	const char *name = field(call, "name");
	json_object *args = json_tokener_parse(field(call, "arguments"));
	const struct phi_tool *tool = tool_find(name);
	int provider_tool = tool && !tool->run;
	const char *detail = args && tool && tool->detail_key ? field(args, tool->detail_key) : "";
	char job_detail[32];
	if (!strcmp(name, "bash_poll") && args) {
		snprintf(job_detail, sizeof(job_detail), "%lld",
			(long long)json_object_get_int64(json_object_object_get(args, "job_id")));
		detail = job_detail;
	}
	const char *heading = tool && tool->label ? tool->label : name;
	json_object *render = NULL, *lines = NULL;
	if ((render = render_query(name, heading, detail, args, 0))) {
		if (*field(render, "title")) {
			ui_tool_title(field(render, "title"));
		}
		lines = json_object_object_get(render, "lines");
	}
	ui_tool_begin(heading, provider_tool && !*detail ? "[extension]" : detail);
	ui_tool_invocation(call);
	if (lines) {
		for (size_t i = 0; i < json_object_array_length(lines); i++) {
			json_object *line = json_object_array_get_idx(lines, i);
			if (json_object_is_type(line, json_type_string)) {
				ui_tool_line(json_object_get_string(line));
			}
		}
	} else if (provider_tool && args) {
		ui_tool_arguments(args);
	}
	if (render) {
		json_object_put(render);
	}
	tool_presented = 1;
	provider_tool_presented = provider_tool;
	if (args) {
		json_object_put(args);
	}
}

static void end_tool(json_object *answer) {

	tool_running = 0;
	if (!tui) {
		return;
	}

	char summary[768];
	int failed = concealed_call ? tool_hidden_answer_failed(answer, summary, sizeof(summary))
		: tool_answer_failed(answer, summary, sizeof(summary));
	if (concealed_call && failed) {
		/* Visibility cannot suppress failed, cancelled or malformed results. */
		present_tool(concealed_call);
	}
	json_object_put(concealed_call);
	concealed_call = NULL;
	tool_concealed = 0;
	if (!tool_presented) {
		return;
	}
	if (provider_tool_presented && !failed) {
		ui_tool_result(answer);
	}

	const char *spill = field(answer, "output_file");
	if (*spill) {
		ui_append("\nOutput saved: ");
		ui_append(spill);
		ui_append("\n");
	}
	ui_tool_answer(answer);
	ui_tool_end(summary, failed);
	tool_presented = 0;
	provider_tool_presented = 0;
}

/* A call an extension blocked: a card with the reason, nothing executed. */
static void refused_tool(json_object *call, json_object *answer) {

	if (tui) {
		present_tool(call);
		end_tool(answer);
	} else {
		char text[768];
		snprintf(text, sizeof(text), "\n%s: %s\n", field(call, "name"), field(answer, "error"));
		display(text, stderr);
	}
}

/* ---- extension UI (phase 5) ----------------------------------------------- */

/* Keyboard shortcuts extensions registered (ui_shortcut). Keys Phi's editor
   or background chats use are refused. */
#define SHORTCUT_LIMIT 32

static struct {
	char owner[49], key[16];
} shortcuts[SHORTCUT_LIMIT];

static size_t shortcut_count;

static int shortcut_allowed(const char *key) {

	if (key[0] == 'f') {
		int n = atoi(key + 1);
		return n >= 1 && n <= 12 && strlen(key) <= 3;
	}
	if (strncmp(key, "ctrl+", 5) || strlen(key) != 6 || key[5] < 'a' || key[5] > 'z') {
		return 0;
	}

	return !strchr("cdfghijmorvyz", key[5]); /* the editor's own Ctrl keys */
}

/* A key typed in the editor: a registered shortcut fires "shortcut";
   otherwise terminal_input handlers may consume it. */
static int consume_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	(void)source;
	if (!json_object_get_boolean(json_object_object_get(result, "consume"))) {
		return 0;
	}
	*(int *)state = 1;

	return 1;
}

static int extension_key(const char *name) {

	for (size_t i = 0; i < shortcut_count; i++) {
		if (!strcmp(shortcuts[i].key, name)) {
			json_object *payload = json_object_new_object();
			json_object_object_add(payload, "key", json_object_new_string(name));
			json_object_object_add(
				payload, "extension", json_object_new_string(shortcuts[i].owner));
			events_emit("shortcut", payload, NULL, NULL);
			json_object_put(payload);
			return 1;
		}
	}
	if (!events_wanted("terminal_input")) {
		return 0;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "key", json_object_new_string(name));
	int consumed = 0;
	events_emit("terminal_input", payload, consume_apply, &consumed);
	json_object_put(payload);

	return consumed;
}

/* Tab: completions from autocomplete handlers, in load order. */
static int completion_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	(void)source;
	json_object *items = json_object_object_get(result, "items");
	for (size_t i = 0;
		json_object_is_type(items, json_type_array) && i < json_object_array_length(items); i++) {
		json_object *item = json_object_array_get_idx(items, i);
		if (json_object_is_type(item, json_type_string)) {
			json_object *wrapped = json_object_new_object();
			json_object_object_add(wrapped, "value", json_object_get(item));
			json_object_array_add(state, wrapped);
		} else if (*field(item, "value")) {
			json_object_array_add(state, json_object_get(item));
		}
	}

	return 0;
}

static json_object *extension_completions(const char *text, const char *word) {

	if (!events_wanted("autocomplete")) {
		return NULL;
	}
	json_object *payload = json_object_new_object(), *items = json_object_new_array();
	json_object_object_add(payload, "text", json_object_new_string(text));
	json_object_object_add(payload, "word", json_object_new_string(word));
	events_emit("autocomplete", payload, completion_apply, items);
	json_object_put(payload);

	return items;
}

static json_object *ui_answer(const char *key, json_object *value, int was_cancelled) {

	json_object *answer = json_object_new_object();
	if (key) {
		json_object_object_add(answer, key, value);
	}
	if (was_cancelled) {
		json_object_object_add(answer, "cancelled", json_object_new_boolean(1));
	}

	return answer;
}

static int notice_level(const char *level) {

	return !strcmp(level, "error") ? 2 : !strcmp(level, "warning") ? 1 : 0;
}

/* Console (phi-cli): plain prompts on the terminal, when there is one. */
static char *console_line(const char *prompt) {

	if (!isatty(STDIN_FILENO)) {
		return NULL;
	}
	fprintf(stderr, "%s", prompt);
	fflush(stderr);
	char line[4096];
	if (!fgets(line, sizeof(line), stdin)) {
		return NULL;
	}
	line[strcspn(line, "\n")] = 0;

	return strdup(line);
}

static json_object *console_ui(const char *method, json_object *request) {

	const char *title = field(request, "title");
	if (!strcmp(method, "ui_confirm")) {
		char prompt[4400];
		json_object *preferred = json_object_object_get(request, "default");
		int default_yes =
			json_object_is_type(preferred, json_type_boolean) && json_object_get_boolean(preferred);
		snprintf(prompt, sizeof(prompt), "\n%s\n%s %s ", title, field(request, "message"),
			default_yes ? "[Y/n]" : "[y/N]");
		char *line = console_line(prompt);
		int yes = line && (line[0] == 'y' || line[0] == 'Y' || (default_yes && !line[0]));
		json_object *answer = ui_answer("confirmed", json_object_new_boolean(yes), !line);
		free(line);
		return answer;
	}
	if (!strcmp(method, "ui_select")) {
		json_object *options = json_object_object_get(request, "options");
		fprintf(stderr, "\n%s\n", title);
		for (size_t i = 0; i < json_object_array_length(options); i++) {
			fprintf(stderr, "  %zu. %s\n", i + 1,
				json_object_get_string(json_object_array_get_idx(options, i)));
		}
		char *line = console_line("Choose a number: ");
		long choice = line ? strtol(line, NULL, 10) : 0;
		free(line);
		if (choice < 1 || (size_t)choice > json_object_array_length(options)) {
			return ui_answer("value", NULL, 1);
		}
		json_object *answer = ui_answer(
			"value", json_object_get(json_object_array_get_idx(options, (size_t)choice - 1)), 0);
		json_object_object_add(answer, "index", json_object_new_int((int)choice - 1));
		return answer;
	}
	if (!strcmp(method, "ui_input") || !strcmp(method, "ui_editor")) {
		char prompt[4200];
		snprintf(prompt, sizeof(prompt), "\n%s: ", title);
		char *line = console_line(prompt);
		json_object *answer = ui_answer("value", line ? json_object_new_string(line) : NULL, !line);
		free(line);
		return answer;
	}

	return NULL;
}

static int streaming_card;

void terminal_stream_begin(const char *title) {

	streaming_card = 1;
	if (tui) {
		ui_stream_begin(title);
	} else if (!headless) {
		tool_report(stdout, "\n%s\n", title);
	}
}

void terminal_stream_text(const char *text) {

	if (!streaming_card) {
		return;
	}
	if (tui) {
		ui_stream_text(text);
	} else if (!headless) {
		fputs(text, stdout);
		fflush(stdout);
	}
}

void terminal_stream_end(const char *full_text) {

	if (!streaming_card) {
		return;
	}
	streaming_card = 0;
	if (tui) {
		ui_stream_end();
	} else if (headless) {
		if (headless->notify && full_text && *full_text) {
			headless->notify(full_text);
		}
	} else {
		tool_report(stdout, "\n");
	}
}

int terminal_has_ui(void) {

	return tui;
}

int terminal_restart_ready(void) {

	if (turn_active || extension_activity_depth || codex_side_active()) {
		return 0;
	}
	if (tui) {
		char *draft = ui_editor_text();
		int ready = draft && !*draft && !ui_peek_queued();
		free(draft);
		return ready;
	}

	return 0;
}

void terminal_help(void) {

	static const char *const keys[] = {
		"i — enter INSERT mode; Esc — return to NORMAL",
		"Enter — send; Ctrl+J — insert a newline",
		"Esc twice in NORMAL — cancel a running response",
		"NORMAL, empty prompt: Left — background (twice first time)",
		"Ctrl+B — go to the board (keeps draft and running work)",
		"Ctrl+Left / Ctrl+Right — move by word in the prompt",
		"Ctrl+\\ — detach from chat; /board — open the board",
		"Ctrl+C — cancel; twice — quit",
		"Up/Down past the edge — earlier messages",
		"PgUp/PgDn — scroll conversation",
		"Ctrl+F — search conversation",
		"Ctrl+Z / Ctrl+Y — undo / redo",
		"NORMAL: u / Ctrl+R — undo / redo; g- / g+ — undo history",
		"NORMAL: U — open undo tree",
		"Shift+Tab — cycle thinking effort",
		"Ctrl+V — paste (NORMAL: visual block)",
		"Ctrl+G — external editor",
		"Alt+M — commands; Tab — complete paths; @ — attach files",
		"!command — shell; !!command — shell without model context",
		"Board: j/k — move; Enter — open; i — new task; ? — board keys",
		"phi --help — command-line modes and options"};
	enum { KEY_COUNT = sizeof(keys) / sizeof(*keys) };
	char commands[PHI_COMMAND_COUNT][256];
	const char *items[KEY_COUNT + PHI_COMMAND_COUNT];
	for (size_t i = 0; i < KEY_COUNT; i++) {
		items[i] = keys[i];
	}
	for (size_t i = 0; i < PHI_COMMAND_COUNT; i++) {
		snprintf(commands[i], sizeof(commands[i]), "%s — %s", phi_commands[i].name,
			phi_commands[i].description);
		items[KEY_COUNT + i] = commands[i];
	}
	if (tui && !headless) {
		ui_select("Phi help — keybindings and commands", items,
			sizeof(items) / sizeof(*items), 0);
	} else {
		for (size_t i = 0; i < sizeof(items) / sizeof(*items); i++) {
			tool_report(stdout, "%s\n", items[i]);
		}
	}
}

void terminal_board(void) {

	if (!tui || !ui_board()) {
		terminal_notice("The board requires an interactive hosted chat.", 0);
	}
}

/* /btw and other during_turn commands, typed while a turn runs. Output
   shows as a side panel, so it never splits the reply being written. */
static int turn_command(const char *message) {

	if (!strncmp(message, "/board", 6) && !message[6 + strspn(message + 6, " \t\r\n")]) {
		terminal_board();
		return 1;
	}
	char output[4096];
	if (!extensions_command_during_turn(message, output, sizeof(output))) {
		return 0;
	}
	if (*output) {
		static int next = 1000000;
		char title[96];
		snprintf(title, sizeof(title), "%.*s", (int)strcspn(message, " \t"), message);
		ui_side_begin(++next, title);
		ui_side_end(next, output, NULL);
	}

	return 1;
}

void terminal_side_begin(int id, const char *title) {

	if (tui) {
		ui_side_begin(id, title);
	}
}

void terminal_side_text(int id, const char *text) {

	if (tui) {
		ui_side_text(id, text);
	}
}

void terminal_side_end(int id, const char *text, const char *error) {

	if (tui) {
		ui_side_end(id, text, error);
	}
}

/* Replies as they are drawn: render_markdown handlers may return the text
   to show instead (the saved reply is unchanged). */
static int markdown_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	(void)state;
	json_object *text = json_object_object_get(result, "text");
	if (json_object_is_type(text, json_type_string)) {
		json_object_object_add(payload, "text", json_object_get(text));
	}

	return 0;
}

static char *markdown_transform(const char *text) {

	if (!events_wanted("render_markdown")) {
		return NULL;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "text", json_object_new_string(text));
	events_emit("render_markdown", payload, markdown_apply, NULL);
	const char *shown = field(payload, "text");
	char *copy = strcmp(shown, text) ? strdup(shown) : NULL;
	json_object_put(payload);

	return copy;
}

/* send_message / append_entry with display:true. A render_message handler
   may supply the text; otherwise the content is shown as it is. */
static int render_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	(void)state;
	json_object *text = json_object_object_get(result, "text");
	if (!json_object_is_type(text, json_type_string)) {
		return 0;
	}
	json_object_object_add(payload, "content", json_object_get(text));

	return 1;
}

/* Shown later when sent from inside an extension callback, where events
   (render_message) cannot run; drawn at the next idle tick. */
static json_object *pending_displays;
static void show_message(json_object *payload);

static void flush_displays(void) {

	if (!pending_displays || events_blocked()) {
		return;
	}
	json_object *queue = pending_displays;
	pending_displays = NULL;
	for (size_t i = 0; i < json_object_array_length(queue); i++) {
		show_message(json_object_array_get_idx(queue, i));
	}
	json_object_put(queue);
}

void terminal_display_message(const char *extension, const char *type, const char *content) {

	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "extension", json_object_new_string(extension));
	json_object_object_add(payload, "custom_type", json_object_new_string(type));
	json_object_object_add(payload, "content", json_object_new_string(content));
	if (tui && events_blocked()) {
		if (!pending_displays) {
			pending_displays = json_object_new_array();
		}
		json_object_array_add(pending_displays, payload);
		return;
	}
	show_message(payload);
	json_object_put(payload);
}

static void show_message(json_object *payload) {

	if (events_wanted("render_message")) {
		events_emit("render_message", payload, render_apply, NULL);
	}
	const char *shown = field(payload, "content"), *type = field(payload, "custom_type");
	if (headless) {
		if (headless->notify) {
			headless->notify(shown);
		}
	} else if (tui) {
		ui_card(type, shown);
	} else {
		tool_report(stdout, "\n%s\n%s\n", type, shown);
	}
}

json_object *terminal_ui(const char *extension, const char *method, json_object *request) {

	if (!strcmp(method, "ui_composer")) {
		json_object *answer = json_object_new_object();
		int ok = 0;
#ifdef PHI_UI
		json_object *label = json_object_object_get(request, "label"),
					*hint = json_object_object_get(request, "hint");
		if (tui) {
			ok = ui_composer(extension,
				json_object_is_type(label, json_type_string) ? json_object_get_string(label) : NULL,
				json_object_is_type(hint, json_type_string) ? json_object_get_string(hint) : NULL);
		}
#endif
		if (!ok) {
			json_object_object_add(answer, "error",
				json_object_new_string("Composer unavailable, occupied, or text too long"));
		}
		return answer;
	}
	if (!strcmp(method, "ui_register_surface") || !strcmp(method, "ui_unregister_surface")) {
		json_object *answer = json_object_new_object();
		char problem[256];
		if (!surfaces_register(extension, field(request, "id"),
				!strcmp(method, "ui_register_surface") ? field(request, "path") : NULL, problem,
				sizeof(problem))) {
			json_object_object_add(answer, "error", json_object_new_string(problem));
		}
		return answer;
	}
	if (!strcmp(method, "ui_register_header_transition") ||
		!strcmp(method, "ui_unregister_header_transition")) {
		json_object *answer = json_object_new_object();
		char problem[256] = "No terminal UI";
		int registered = 0;
#ifdef PHI_UI
		if (tui) {
			registered = ui_native_transition_register(extension, field(request, "id"),
				!strcmp(method, "ui_register_header_transition") ? field(request, "path") : NULL,
				problem, sizeof(problem));
		}
#endif
		if (!registered) {
			json_object_object_add(answer, "error", json_object_new_string(problem));
		}
		return answer;
	}
	if (!strcmp(method, "ui_register_header_widget") ||
		!strcmp(method, "ui_unregister_header_widget")) {
		json_object *answer = json_object_new_object();
		char problem[256] = "No terminal UI";
		int registered = 0;
#ifdef PHI_UI
		if (tui) {
			registered = ui_native_widget_register(extension, field(request, "id"),
				!strcmp(method, "ui_register_header_widget") ? field(request, "path") : NULL,
				problem, sizeof(problem));
		}
#endif
		if (!registered) {
			json_object_object_add(answer, "error", json_object_new_string(problem));
		}
		return answer;
	}
	if (!strcmp(method, "ui_notify")) {
		const char *message = field(request, "message");
		if (headless && headless->ui) {
			return headless->ui(method, request);
		}
		if (headless) {
			if (headless->notify) {
				headless->notify(message);
			}
		} else if (tui) {
			ui_notice(message, notice_level(field(request, "level")));
		} else {
			tool_report(stderr, "\n%s\n", message);
		}
		return json_object_new_object();
	}
	/* Display methods: widgets, header, footer, working message, title. */
	/* Themes. */
	if (!strcmp(method, "ui_get_theme") || !strcmp(method, "ui_list_themes") ||
		!strcmp(method, "ui_set_theme")) {
		if (!tui) {
			json_object *answer = json_object_new_object();
			if (!strcmp(method, "ui_list_themes")) {
				json_object_object_add(answer, "themes", json_object_new_array());
			} else if (!strcmp(method, "ui_set_theme")) {
				json_object_object_add(answer, "error", json_object_new_string("No terminal UI"));
			}
			return answer;
		}
		if (!strcmp(method, "ui_get_theme")) {
			return ui_theme_get();
		}
		if (!strcmp(method, "ui_list_themes")) {
			json_object *answer = json_object_new_object();
			json_object_object_add(answer, "themes", ui_theme_list());
			return answer;
		}
		char problem[256] = "";
		json_object *answer = json_object_new_object();
		if (!ui_theme_set(field(request, "name"), json_object_object_get(request, "colors"),
				problem, sizeof(problem))) {
			json_object_object_add(answer, "error", json_object_new_string(problem));
		}
		return answer;
	}
	/* The prompt, tool cards and shortcuts. */
	if (!strcmp(method, "ui_get_editor_text") || !strcmp(method, "ui_set_editor_text") ||
		!strcmp(method, "ui_paste") || !strcmp(method, "ui_get_tools_expanded") ||
		!strcmp(method, "ui_set_tools_expanded") || !strcmp(method, "ui_shortcut")) {
		json_object *answer = json_object_new_object();
		if (!strcmp(method, "ui_shortcut")) {
			const char *key = field(request, "key");
			int known = 0;
			for (size_t i = 0; i < shortcut_count; i++) {
				if (!strcmp(shortcuts[i].key, key)) {
					known = 1;
				}
			}
			if (!shortcut_allowed(key)) {
				json_object_object_add(answer, "error",
					json_object_new_string(
						"Shortcut must be f1-f12 or ctrl+<letter> other than c, d, f, g, h, i, j, m, o, r, v, y, z"));
			} else if (known) {
				json_object_object_add(
					answer, "error", json_object_new_string("That key is already taken"));
			} else if (shortcut_count == SHORTCUT_LIMIT) {
				json_object_object_add(
					answer, "error", json_object_new_string("Too many shortcuts"));
			} else {
				snprintf(
					shortcuts[shortcut_count].owner, sizeof(shortcuts[0].owner), "%s", extension);
				snprintf(shortcuts[shortcut_count].key, sizeof(shortcuts[0].key), "%s", key);
				shortcut_count++;
			}
			return answer;
		}
		if (headless) {
			if (headless->ui && !strcmp(method, "ui_set_editor_text")) {
				json_object_put(answer);
				return headless->ui(method, request);
			}
			if (!strcmp(method, "ui_get_editor_text")) {
				json_object_object_add(answer, "text", json_object_new_string(""));
			}
			if (!strcmp(method, "ui_get_tools_expanded")) {
				json_object_object_add(answer, "expanded", json_object_new_boolean(0));
			}
			return answer;
		}
		if (!strcmp(method, "ui_get_editor_text")) {
			char *text = tui ? ui_editor_text() : NULL;
			json_object_object_add(answer, "text", json_object_new_string(text ? text : ""));
			free(text);
		} else if (!strcmp(method, "ui_set_editor_text")) {
			if (tui) {
				ui_set_draft(field(request, "text"));
			}
		} else if (!strcmp(method, "ui_paste")) {
			if (tui) {
				ui_insert_text(field(request, "text"));
			}
		} else if (!strcmp(method, "ui_get_tools_expanded")) {
			json_object_object_add(
				answer, "expanded", json_object_new_boolean(tui && ui_tools_expanded()));
		} else if (tui) {
			ui_set_tools_expanded(
				json_object_get_boolean(json_object_object_get(request, "expanded")));
		}
		return answer;
	}
	int display = !strcmp(method, "ui_widget") || !strcmp(method, "ui_header") ||
		!strcmp(method, "ui_footer") || !strcmp(method, "ui_working") ||
		!strcmp(method, "ui_title") || !strcmp(method, "ui_reset");
	if (display) {
		if (!strcmp(method, "ui_reset")) {
			surfaces_forget(extension);
		}
		if (!strcmp(method, "ui_reset")) {
			for (size_t i = shortcut_count; i-- > 0;) {
				if (!strcmp(shortcuts[i].owner, extension)) {
					shortcuts[i] = shortcuts[--shortcut_count];
				}
			}
		}
		if (headless) {
			return headless->ui ? headless->ui(method, request) : json_object_new_object();
		}
		if (!tui) {
			return json_object_new_object(); /* the console binary has no layout */
		}
		json_object *lines = json_object_object_get(request, "lines");
		size_t count =
			json_object_is_type(lines, json_type_array) ? json_object_array_length(lines) : 0;
		if (count > 16) {
			count = 16;
		}
		const char *texts[16];
		for (size_t i = 0; i < count; i++) {
			texts[i] = json_object_get_string(json_object_array_get_idx(lines, i));
			if (!texts[i]) {
				texts[i] = "";
			}
		}
		const char *placement = field(request, "placement");
		int below = !strcmp(placement, "below_editor") || !strcmp(placement, "belowEditor");
		json_object *answer = json_object_new_object();
		if (!strcmp(method, "ui_widget")) {
			if (!*field(request, "key")) {
				json_object_object_add(
					answer, "error", json_object_new_string("ui_widget needs a key"));
			} else if (!ui_widget(extension, field(request, "key"), texts, count, below)) {
				json_object_object_add(answer, "error", json_object_new_string("Too many widgets"));
			}
		} else if (!strcmp(method, "ui_header")) {
			ui_header(extension, texts, count);
		} else if (!strcmp(method, "ui_footer")) {
			json_object *text = json_object_object_get(request, "text");
			ui_footer(extension,
				json_object_is_type(text, json_type_string) ? json_object_get_string(text) : NULL);
		} else if (!strcmp(method, "ui_working")) {
			json_object *visible = json_object_object_get(request, "visible");
			ui_working(field(request, "message"), field(request, "detail"), field(request, "tip"),
				visible ? json_object_get_boolean(visible) : 1);
		} else if (!strcmp(method, "ui_title")) {
			ui_title(field(request, "title"));
		} else {
			ui_forget_owner(extension);
			for (size_t i = shortcut_count; i-- > 0;) {
				if (!strcmp(shortcuts[i].owner, extension)) {
					shortcuts[i] = shortcuts[--shortcut_count];
				}
			}
		}
		return answer;
	}
	int dialog = !strcmp(method, "ui_confirm") || !strcmp(method, "ui_select") ||
		!strcmp(method, "ui_input") || !strcmp(method, "ui_editor");
	if (!dialog) {
		json_object *answer = json_object_new_object();
		json_object_object_add(answer, "error", json_object_new_string("Unknown UI method"));
		return answer;
	}
	if (headless) {
		/* RPC asks its client; print and JSON modes have no one to ask. */
		if (headless->ui) {
			return headless->ui(method, request);
		}
		return ui_answer(!strcmp(method, "ui_confirm") ? "confirmed" : "value",
			!strcmp(method, "ui_confirm") ? json_object_new_boolean(0) : NULL, 1);
	}
	const char *title = field(request, "title");
	int timeout = json_object_get_int(json_object_object_get(request, "timeout_ms"));
	prompt_event("ui_prompt_start", "extension_dialog", extension, 0);
	json_object *answer = NULL;
	if (!tui) {
		answer = console_ui(method, request);
	} else if (!strcmp(method, "ui_confirm")) {
		/* "default": false starts on No, for destructive actions. */
		json_object *preferred = json_object_object_get(request, "default");
		int result = ui_confirm(title, field(request, "message"), timeout,
			!json_object_is_type(preferred, json_type_boolean) ||
				json_object_get_boolean(preferred));
		answer = ui_answer("confirmed", json_object_new_boolean(result == 1), result < 0);
	} else if (!strcmp(method, "ui_select")) {
		json_object *options = json_object_object_get(request, "options");
		size_t count =
			json_object_is_type(options, json_type_array) ? json_object_array_length(options) : 0;
		if (count > 1000) {
			count = 1000;
		}
		const char **texts = count ? calloc(count, sizeof(*texts)) : NULL;
		for (size_t i = 0; texts && i < count; i++) {
			texts[i] = json_object_get_string(json_object_array_get_idx(options, i));
			if (!texts[i]) {
				texts[i] = "";
			}
		}
		int chosen = texts ? ui_select(title, texts, count, timeout) : -1;
		answer = ui_answer(
			"value", chosen >= 0 ? json_object_new_string(texts[chosen]) : NULL, chosen < 0);
		if (chosen >= 0) {
			json_object_object_add(answer, "index", json_object_new_int(chosen));
		}
		free(texts);
	} else {
		int editor = !strcmp(method, "ui_editor");
		char *text = ui_ask_text(
			title, field(request, "placeholder"), field(request, "value"), editor, timeout);
		answer = ui_answer("value", text ? json_object_new_string(text) : NULL, !text);
		free(text);
	}
	if (!answer) {
		answer = ui_answer("value", NULL, 1);
	}
	int given = !json_object_get_boolean(json_object_object_get(answer, "cancelled"));
	prompt_event("ui_prompt_end", "extension_dialog", extension, given);

	return answer;
}

void terminal_extension_message(const char *text) {

	if (headless) {
		if (headless->notify && text) {
			headless->notify(text);
		}
		return;
	}
	if (tui) {
		ui_extension_message(text);
		return;
	}
	if (text) {
		tool_report(stdout, "%s", text);
	}
}

void terminal_extension_result(json_object *answer) {

	if (headless) {
		if (headless->notify && answer) {
			headless->notify(json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN));
		}
		return;
	}
	if (tui) {
		ui_extension_result(answer);
		return;
	}
	if (answer) {
		tool_report(
			stdout, "\n%s\n", json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PRETTY));
	}
}

void terminal_extension_status(const char *text) {

	if (headless) {
		if (headless->status) {
			headless->status(text ? text : "");
		}
		return;
	}
	if (tui) {
		ui_extension_status(text ? text : "");
	}
}

void terminal_close(void) {

	restore_terminal();
	ui_set_idle(NULL);
	surfaces_forget(NULL);
	usage_close(usage_client);
	usage_client = NULL;
	if (tui) {
		ui_close();
		tui = 0;
	}
	tools_set_host(NULL);
}

/* Terminal state from before Phi changed anything, for the signal path. */
static struct termios original_terminal;
static int original_terminal_saved;

/* Only async-signal-safe calls here: kill(), write(), tcsetattr(), _exit().
   ncurses and curl cleanup are not safe inside a handler, so the terminal is
   reset directly instead of through terminal_close(). */
static void exit_on_signal(int number) {

	if (abort_extension_compile) {
		abort_extension_compile();
	}
	tools_abort_jobs();
	if (tui) {
		/* Undo the modes ui_open() enabled, leave the alternate screen and
           show a normal cursor. */
		static const char reset[] = "\033[<u\033[?2004l\033[?1004l\033[?1003l\033[?1002l\033[?1000l"
									"\033[?1006l\033[?1049l\033[?25h\033[0 q\033[0m";
		ssize_t ignored = write(STDOUT_FILENO, reset, sizeof(reset) - 1);
		(void)ignored;
	}
	if (busy_terminal) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
	} else if (original_terminal_saved) {
		tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal);
	}
	_exit(128 + number);
}

static void theme_transcript_changed(const char *path, int inherit) {

	if (tui && !headless) {
		ui_theme_transcript(path, inherit);
	}
}

static void visible_diff(char kind, size_t line, const char *text, size_t size,
	size_t first, size_t end) {

	if (!tool_concealed) ui_diff_highlight(kind, line, text, size, first, end);
}

static void visible_write_preview(const char *text) {

	if (!tool_concealed) ui_write_preview(text);
}

static void visible_read_preview(const char *text) {

	if (!tool_concealed) ui_read_preview(text);
}

struct agent_host terminal_open(const char *model, const char *effort) {

	initial_effort = effort;
	cancelled = 0;
	original_terminal_saved = isatty(STDIN_FILENO) && !tcgetattr(STDIN_FILENO, &original_terminal);
	atexit(terminal_close);
	signal(SIGINT, exit_on_signal);
	signal(SIGTERM, exit_on_signal);

	struct tool_host host = {
		.display = display, .styled = styled, .cancel = check_cancel, .banners = 1};
	ui_set_temporary(transcript_temporary());
	tui = !headless && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && ui_open(model, effort);
	if (headless) {
		host.banners = 0;
	}
	if (tui) {
		usage_client = usage_open();
		ui_account_usage("wk --");
		ui_set_idle(refresh_usage);
		ui_set_tool_renderer(restored_render);
		ui_set_tool_visibility(tool_visibility_hidden);
		ui_set_turn_command(turn_command);
		ui_set_extension_hooks(extension_key, extension_completions);
		ui_set_markdown_hook(markdown_transform);
		host.banners = 0;
		host.diff = visible_diff;
		host.write_preview = visible_write_preview;
		host.read_preview = visible_read_preview;
	}
	tools_set_host(&host);

	return (struct agent_host){.response = {.text = assistant_text,
								   .thinking = tui ? thinking_text : NULL,
								   .cancel = check_cancel,
								   .usage = context_usage,
								   .tool_pending = pending_tool},
		.transcript_changed = theme_transcript_changed,
		.tool_begin = begin_tool,
		.tool_end = end_tool,
		.tool_refused = refused_tool,
		.effort = terminal_effort,
		.steer = take_steer};
}

char *terminal_input(void) {

	if (tui) {
		return ui_input();
	}
	tool_report(stdout, "> ");
	if (!service_wait_input(STDIN_FILENO, 1)) {
		return strdup("");
	}
	char *message = NULL;
	size_t capacity = 0;
	if (getline(&message, &capacity, stdin) == -1) {
		free(message);
		return NULL;
	}
	size_t length = strlen(message);
	while (length && (message[length - 1] == '\n' || message[length - 1] == '\r')) {
		message[--length] = 0;
	}

	return message;
}

void terminal_begin_turn(const char *message) {

	turn_active = 1;
	chats_run(1);
	cancelled = 0;
	turn_tools = 0;
	if (tui) {
		if (message) {
			ui_user(message);
		}
		ui_busy(1);
		return;
	}
	if (headless) {
		return;
	}
	if (isatty(STDIN_FILENO) && !tcgetattr(STDIN_FILENO, &saved_terminal)) {
		struct termios mode = saved_terminal;
		mode.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
		mode.c_cc[VMIN] = 0;
		mode.c_cc[VTIME] = 0;
		if (!tcsetattr(STDIN_FILENO, TCSANOW, &mode)) {
			busy_terminal = 1;
		}
	}
}

void terminal_end_turn(void) {

	turn_active = 0;
	chats_run(0);
	restore_terminal();
	if (tui) {
		if (cancelled) {
			ui_stopped(turn_tools != 0);
		} else {
			ui_turn_done();
			ui_busy(0);
		}
		return;
	}
	if (cancelled) {
		tool_report(stderr,
			turn_tools ? "Stopped. Completed tool actions are not undone.\n" : "Stopped.\n");
	}
}

void terminal_clear(void) {

	if (tui) {
		ui_clear();
	}
}

static int resume_selected(struct agent *agent, struct session_entry *entry) {

	char error[256];
	if (events_wanted("session_before_switch")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "reason", json_object_new_string("resume"));
		json_object_object_add(payload, "target_file", json_object_new_string(entry->path));
		if (!events_allowed("session_before_switch", payload)) {
			tool_report(stdout, "\nAn extension kept the current session.\n");
			return 0;
		}
	}
	if (!agent_resume(agent, entry, error, sizeof(error))) {
		tool_report(stderr, "\nResume failed: %s\n", error);
		return 0;
	}
	if (tui) {
		ui_restore_history(agent->history);
	}

	return 1;
}

int terminal_resume_path(struct agent *agent, const char *path) {

	struct session_entry entry;
	char error[256];
	if (!session_from_path(path, &entry, error, sizeof(error))) {
		tool_report(stderr, "\n%s\n", error);
		return 0;
	}

	return resume_selected(agent, &entry);
}

int terminal_resume(struct agent *agent) {

	struct session_entry *entries;
	size_t count;
	char error[256];
	if (!sessions_list(&entries, &count, error, sizeof(error))) {
		tool_report(stderr, "\n%s\n", error);
		return 0;
	}
	if (*error) {
		tool_report(stderr, "\n%s\n", error);
	}

	int selected = -1;
	if (tui) {
		selected = ui_resume(entries, count);
	}
	if (selected < 0 && !count) {
		tool_report(stdout, "\nNo saved sessions yet.\n");
	}
	if (!tui && count) {
		tool_report(stdout,
			"\nThe fuzzy session picker requires phi-ui; use --continue for the newest session.\n");
	}
	int resumed = selected >= 0 && resume_selected(agent, entries + selected);
	free(entries);

	return resumed;
}

/* --continue: the newest session started in this directory. */
int terminal_continue(struct agent *agent) {

	struct session_entry entry;
	char error[256], cwd[4096];
	int found =
		getcwd(cwd, sizeof(cwd)) ? sessions_latest_in(cwd, &entry, error, sizeof(error)) : 0;
	if (found < 0) {
		tool_report(stderr, "\n%s\n", error);
		return 0;
	}
	if (!found) {
		tool_report(
			stdout, "\nNo saved session in this directory yet; /resume lists all of them.\n");
		return 0;
	}

	return resume_selected(agent, &entry);
}

static int current_tree(struct agent *agent, struct session_entry *entry, struct session_tree *tree,
	char *error, size_t size) {

	if (!agent->transcript.file || agent->transcript.failed || !*agent->transcript.path) {
		snprintf(error, size, "No saved current session yet.");
		return 0;
	}
	/* Open the current file directly rather than scanning every transcript. */
	if (!session_from_path(agent->transcript.path, entry, error, size)) {
		return 0;
	}

	return session_tree_index(entry, tree, error, size);
}

static int save_tree_annotations(struct agent *agent, struct session_tree *tree) {

	for (size_t i = 0; i < tree->count; i++) {
		struct session_node *node = tree->nodes + i;
		if (!node->annotation_dirty) {
			continue;
		}
		if (!transcript_annotate(&agent->transcript, node->id, node->label, node->bookmarked)) {
			tool_report(
				stderr, "\nCould not save all tree labels/bookmarks; navigation cancelled.\n");
			return 0;
		}
		node->annotation_dirty = 0;
	}

	return 1;
}

static const struct session_node *tree_node(const struct session_tree *tree, uint64_t id) {

	return id && id <= tree->count ? tree->nodes + id - 1 : NULL;
}

int terminal_tree(struct agent *agent) {

	if (!tui) {
		tool_report(stdout, "\n/tree requires phi-ui.\n");
		return 0;
	}
	struct session_entry entry;
	struct session_tree tree;
	char error[256];
	if (!current_tree(agent, &entry, &tree, error, sizeof(error))) {
		tool_report(stderr, "\nTree unavailable: %s\n", error);
		return 0;
	}
	uint64_t selected = ui_tree(&tree, 0);
	if (!save_tree_annotations(agent, &tree)) {
		session_tree_free(&tree);
		return 0;
	}
	if (!selected) {
		session_tree_free(&tree);
		return 0;
	}
	const struct session_node *node = tree_node(&tree, selected);
	if (node &&
		!(tree.nodes[selected - 1].data =
				session_tree_item(&tree, (size_t)selected - 1, error, sizeof(error)))) {
		session_tree_free(&tree);
		tool_report(stderr, "\nTree selection unavailable: %s\n", error);
		return 0;
	}
	uint64_t head = selected;
	const char *draft = NULL;
	if (node && node->kind == SESSION_ITEM && !strcmp(field(node->data, "role"), "user")) {
		json_object *content = json_object_object_get(node->data, "content");
		if (json_object_is_type(content, json_type_string)) {
			head = node->parent;
			draft = json_object_get_string(content);
		}
	}
	uint64_t previous = agent->transcript.head;
	if (events_wanted("session_before_tree")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "target_id", json_object_new_int64((int64_t)head));
		json_object_object_add(payload, "current_id", json_object_new_int64((int64_t)previous));
		if (!events_allowed("session_before_tree", payload)) {
			tool_report(stdout, "\nAn extension kept the current position.\n");
			session_tree_free(&tree);
			return 0;
		}
	}
	json_object *history = session_tree_history(&tree, head, error, sizeof(error));
	if (!history) {
		session_tree_free(&tree);
		tool_report(stderr, "\nTree switch failed: %s\n", error);
		return 0;
	}
	char *draft_copy = draft ? strdup(draft) : strdup("");
	if (!draft_copy) {
		json_object_put(history);
		session_tree_free(&tree);
		tool_report(stderr, "\nTree switch failed: out of memory.\n");
		return 0;
	}
	if (!ui_set_draft(draft_copy)) {
		json_object_put(history);
		free(draft_copy);
		session_tree_free(&tree);
		tool_report(
			stderr, "\nTree switch cancelled: selected prompt is too large for the editor.\n");
		return 0;
	}
	if (!agent_navigate(agent, history, head, error, sizeof(error))) {
		json_object_put(history);
		ui_set_draft("");
		free(draft_copy);
		session_tree_free(&tree);
		tool_report(stderr, "\nTree switch failed: %s\n", error);
		return 0;
	}
	ui_restore_history(agent->history);
	free(draft_copy);
	session_tree_free(&tree);
	if (events_wanted("session_tree")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "previous_id", json_object_new_int64((int64_t)previous));
		json_object_object_add(payload, "current_id", json_object_new_int64((int64_t)head));
		events_emit("session_tree", payload, NULL, NULL);
		json_object_put(payload);
	}

	return 1;
}

int terminal_fork(struct agent *agent) {

	if (!tui) {
		tool_report(stdout, "\n/fork requires phi-ui.\n");
		return 0;
	}
	struct session_entry entry;
	struct session_tree tree;
	char error[256];
	if (!current_tree(agent, &entry, &tree, error, sizeof(error))) {
		tool_report(stderr, "\nFork unavailable: %s\n", error);
		return 0;
	}
	uint64_t selected = ui_tree(&tree, 1);
	if (!save_tree_annotations(agent, &tree)) {
		session_tree_free(&tree);
		return 0;
	}
	if (!selected) {
		session_tree_free(&tree);
		return 0;
	}
	const struct session_node *node = tree_node(&tree, selected);
	if (node &&
		!(tree.nodes[selected - 1].data =
				session_tree_item(&tree, (size_t)selected - 1, error, sizeof(error)))) {
		session_tree_free(&tree);
		tool_report(stderr, "\nFork selection unavailable: %s\n", error);
		return 0;
	}
	json_object *content = node ? json_object_object_get(node->data, "content") : NULL;
	if (!node || node->kind != SESSION_ITEM || strcmp(field(node->data, "role"), "user") ||
		!json_object_is_type(content, json_type_string)) {
		session_tree_free(&tree);
		tool_report(stderr, "\nFork selection is not an editable user prompt.\n");
		return 0;
	}
	char *draft = strdup(json_object_get_string(content));
	json_object *history = session_tree_history(&tree, node->parent, error, sizeof(error));
	if (!draft || !history) {
		free(draft);
		if (history) {
			json_object_put(history);
		}
		session_tree_free(&tree);
		tool_report(stderr, "\nFork failed: %s\n", *error ? error : "out of memory.");
		return 0;
	}
	if (!ui_set_draft(draft)) {
		json_object_put(history);
		free(draft);
		session_tree_free(&tree);
		tool_report(stderr, "\nFork cancelled: selected prompt is too large for the editor.\n");
		return 0;
	}
	if (!session_gate("session_before_fork", "kind", "fork", "entry_id", (int64_t)selected)) {
		json_object_put(history);
		ui_set_draft("");
		free(draft);
		session_tree_free(&tree);
		return 0;
	}
	if (!agent_branch(agent, history, "fork", selected, error, sizeof(error))) {
		json_object_put(history);
		ui_set_draft("");
		free(draft);
		session_tree_free(&tree);
		tool_report(stderr, "\nFork failed: %s\n", error);
		return 0;
	}
	ui_restore_history(agent->history);
	free(draft);
	session_tree_free(&tree);

	return 1;
}

void terminal_request_cancel(void) {

	cancelled = 1;
	if (tui) {
		ui_request_cancel();
	}
}

void terminal_show_user(const char *text) {

	if (tui) {
		ui_user(text);
	} else {
		tool_report(stdout, "\n> %s\n", text);
	}
}

/* Non-interactive tree moves for extensions (api->call navigate_tree/fork). */
static int tree_move(struct agent *agent, uint64_t id, int fork, char *error, size_t size) {

	struct session_entry entry;
	struct session_tree tree;
	if (!current_tree(agent, &entry, &tree, error, size)) {
		return 0;
	}
	if (!id || id > tree.count) {
		session_tree_free(&tree);
		snprintf(error, size, "No such entry in this session.");
		return 0;
	}
	uint64_t previous = agent->transcript.head;
	json_object *history = session_tree_history(&tree, id, error, size);
	session_tree_free(&tree);
	if (!history) {
		return 0;
	}
	int ok = fork ? agent_branch(agent, history, "fork", id, error, size)
				  : agent_navigate(agent, history, id, error, size);
	if (!ok) {
		json_object_put(history);
		return 0;
	}
	if (tui) {
		ui_restore_history(agent->history);
	}
	if (!fork && events_wanted("session_tree")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "previous_id", json_object_new_int64((int64_t)previous));
		json_object_object_add(payload, "current_id", json_object_new_int64((int64_t)id));
		events_emit("session_tree", payload, NULL, NULL);
		json_object_put(payload);
	}

	return 1;
}

int terminal_navigate(struct agent *agent, uint64_t id, char *error, size_t size) {

	return tree_move(agent, id, 0, error, size);
}

int terminal_fork_at(struct agent *agent, uint64_t id, char *error, size_t size) {

	return tree_move(agent, id, 1, error, size);
}

int terminal_clone(struct agent *agent) {

	if (!agent->transcript.file || agent->transcript.failed || !*agent->transcript.path) {
		tool_report(stderr, "\nClone unavailable: no saved current session yet.\n");
		return 0;
	}
	char error[256];
	json_object *history = json_object_get(agent->history);
	if (!history) {
		tool_report(stderr, "\nClone failed: out of memory.\n");
		return 0;
	}
	uint64_t source = agent->transcript.head;
	if (!session_gate("session_before_fork", "kind", "clone", "entry_id", (int64_t)source)) {
		json_object_put(history);
		return 0;
	}
	if (!agent_branch(agent, history, "clone", source, error, sizeof(error))) {
		json_object_put(history);
		tool_report(stderr, "\nClone failed: %s\n", error);
		return 0;
	}
	if (tui) {
		ui_restore_history(agent->history);
		ui_set_draft("");
	}
	tool_report(stdout, "\nCloned current branch into a new session.\n");

	return 1;
}

void terminal_shell_begin(const char *command, int send) {

	if (tui) {
		char detail[4096];
		snprintf(detail, sizeof(detail), "[%s] %s", send ? "send" : "local", command);
		/* The sign distinguishes sent output from a local-only shell. */
		tools_user_pty(send ? ui_columns() : -ui_columns());
		ui_tool_begin("bash", detail);
		tool_presented = 1;
	}
}

void terminal_shell_end(json_object *answer) {

	if (tui) {
		tools_user_pty(0);
		end_tool(answer);
		ui_cwd_changed();
	}
}

int terminal_copy(const char *text) {

	if (!text || !*text || headless) {
		return 0;
	}
	static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char encoded[1024]; /* Four output bytes per three input bytes. */
	size_t length = strlen(text);
	size_t used = 0;

	/* OSC 52 asks the terminal emulator to copy these bytes to its clipboard. */
	fputs("\033]52;c;", stdout);
	for (size_t i = 0; i < length; i += 3) {
		unsigned a = (unsigned char)text[i];
		unsigned b = i + 1 < length ? (unsigned char)text[i + 1] : 0;
		unsigned c = i + 2 < length ? (unsigned char)text[i + 2] : 0;
		encoded[used++] = base64[a >> 2];
		encoded[used++] = base64[((a & 3) << 4) | (b >> 4)];
		encoded[used++] = i + 1 < length ? base64[((b & 15) << 2) | (c >> 6)] : '=';
		encoded[used++] = i + 2 < length ? base64[c & 63] : '=';
		if (used == sizeof(encoded)) {
			fwrite(encoded, 1, used, stdout);
			used = 0;
		}
	}
	if (used) {
		fwrite(encoded, 1, used, stdout);
	}
	fputs("\033\\", stdout);
	fflush(stdout);

	return 1;
}

static void login_link(const char *url) {

	login_url = url;
}

int terminal_login(void) {

	cancelled = 0;
	int was_busy = busy_terminal;
	const int signals[] = {SIGINT, SIGTERM, SIGHUP};
	struct sigaction previous[3], action = {0};
	int guarded[3] = {0};
	if (!tui && !headless && !was_busy) {
		action.sa_handler = exit_on_signal;
		sigemptyset(&action.sa_mask);
		for (size_t i = 0; i < 3; i++) {
			guarded[i] = !sigaction(signals[i], &action, &previous[i]);
		}
	}
	if (tui) {
		ui_busy(1);
	} else if (!headless && !busy_terminal && isatty(STDIN_FILENO) &&
		!tcgetattr(STDIN_FILENO, &saved_terminal)) {
		struct termios mode = saved_terminal;
		mode.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG);
		mode.c_cc[VMIN] = mode.c_cc[VTIME] = 0;
		if (!tcsetattr(STDIN_FILENO, TCSANOW, &mode)) {
			busy_terminal = 1;
		}
	}
	int ok = codex_auth_login(check_cancel, login_link);
	login_url = NULL;
	if (tui) {
		ui_busy(0);
	} else if (!was_busy) {
		restore_terminal();
	}
	for (size_t i = 0; i < 3; i++) {
		if (guarded[i]) {
			sigaction(signals[i], &previous[i], NULL);
		}
	}
	cancelled = 0;

	return ok;
}

static void terminal_theme_list(int defaults) {

	json_object *current = ui_theme_get(), *names = ui_theme_list();
	tool_report(
		stdout, defaults ? "\nBoard view theme: %s\n" : "\nTheme: %s\n", field(current, "name"));
	size_t count = json_object_array_length(names);
	if (!count) {
		tool_report(stdout, "No named themes (~/.config/phi/themes/, or themes/ beside Phi).\n");
	}
	for (size_t i = 0; i < count; i++) {
		tool_report(stdout, "  %s\n", json_object_get_string(json_object_array_get_idx(names, i)));
	}
	if (count) {
		tool_report(stdout,
			defaults
				? "Use /theme NAME: saves the board/new-chat default; existing chats unchanged.\n"
				: "Use /theme NAME: saved for this transcript only.\n");
	}
	json_object_put(current);
	json_object_put(names);
}

void terminal_theme_defaults(const char *name) {

	if (!tui) {
		tool_report(stderr, "Themes need an interactive composer.\n");
		return;
	}
	if (!*name) {
		terminal_theme_list(1);
		return;
	}
	char error[256];
	if (!ui_theme_set(name, NULL, error, sizeof(error))) {
		terminal_notice(error, 2);
	} else if (!ui_theme_save_defaults(name, error, sizeof(error))) {
		ui_reload_theme();
		terminal_notice(error, 2);
	} else {
		terminal_notice("Theme saved for the board and new chats; existing chats unchanged.", 0);
	}
}

void terminal_theme(const char *name) {

	if (!tui) {
		tool_report(stderr, "Themes are available in phi-ui.\n");
		return;
	}
	if (*name) {
		char error[256];
		char notice[512];
		int level = 0;
		if (!ui_theme_set(name, NULL, error, sizeof(error))) {
			snprintf(notice, sizeof(notice), "Theme unchanged: %s.", error);
			level = 2;
		} else if (ui_theme_save(name, error, sizeof(error))) {
			snprintf(notice, sizeof(notice), "Theme: %s (saved for this transcript).", name);
		} else {
			snprintf(notice, sizeof(notice), "Theme: %s (this chat only: %s).", name, error);
			level = 1;
		}
		terminal_notice(notice, level);
		return;
	}
	terminal_theme_list(0);
}

void terminal_reload(void) {

	if (tui) {
		ui_reload_theme();
		return;
	}
	tool_report(stderr, "Theme reload is available in phi-ui.\n");
}
