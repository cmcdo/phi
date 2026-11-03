#define _POSIX_C_SOURCE 200809L
#include "core/proctitle.h"
#include "core/agent.h"
#include "core/auth.h"
#include "core/terminal.h"
#include "core/settings.h"
#include "core/commands.h"
#include "core/json.h"
#include "tools.h"
#include "core/extensions.h"
#include "core/instructions.h"
#include "core/extension_platform.h"
#include "core/context.h"
#include "core/events.h"
#include "core/host.h"
#include "core/modes.h"
#include "core/packages.h"
#include "core/resources.h"
#include "core/chats.h"
#include "core/bridge.h"
#include "core/usage.h"
#include "core/surfaces.h"
#include "core/sessions.h"
#ifdef PHI_UI
#include "ui/ui.h"
#include "ui/board.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static void extension_message(const char *text) {

	terminal_extension_message(text);
}

/* Host services for extensions. Model calls use a separate connection,
   opened on first use, so they work even from inside a running request. */
static struct agent *service_agent;
static struct codex *service_client;
#ifdef PHI_UI
static int saved_history_index(struct session_tree *tree, char *error, size_t size) {

	*error = 0;
	if (!service_agent || !*service_agent->transcript.path) {
		return 0;
	}
	struct session_entry entry;
	if (!session_from_path(service_agent->transcript.path, &entry, error, size) ||
		!session_tree_index(&entry, tree, error, size)) {
		return 0;
	}
	uint64_t head = service_agent->transcript.head;
	if (head > tree->count) {
		session_tree_free(tree);
		snprintf(error, size, "Saved history changed.");
		return 0;
	}
	tree->head = head;
	for (size_t i = 0; i < tree->count; i++) {
		tree->nodes[i].active = 0;
	}
	for (uint64_t id = head; id; id = tree->nodes[id - 1].parent) {
		tree->nodes[id - 1].active = 1;
	}

	return 1;
}
#endif
/* Streamed replies go to the extension's callback and, with "show", to a
   live card; either can stop the request early. */
struct completion_stream {
	int (*on_text)(const char *, void *);
	void *state;
	int shown;
};

static int completion_stopped;

static int completion_cancel(void) {

	return completion_stopped || terminal_extension_cancel();
}

static void completion_text(const char *text, void *context) {

	struct completion_stream *stream = context;
	if (stream->shown) {
		terminal_stream_text(text);
	}
	if (stream->on_text && stream->on_text(text, stream->state)) {
		completion_stopped = 1;
	}
}

static json_object *extension_complete(
	json_object *request, int (*on_text)(const char *, void *), void *state) {

	if (!service_client && !(service_client = codex_open())) {
		return NULL;
	}
	const char *show = field(request, "show");
	struct completion_stream stream = {on_text, state, *show != 0};
	if (stream.shown) {
		terminal_stream_begin(show);
	}
	completion_stopped = 0;
	terminal_extension_activity(1);
	json_object *answer =
		codex_complete_stream(service_client, request, service_agent->model, terminal_effort(),
			completion_cancel, on_text || stream.shown ? completion_text : NULL, &stream);
	terminal_extension_activity(0);
	if (stream.shown) {
		terminal_stream_end(field(answer, "text"));
	}

	return answer;
}

/* complete_async: a side request beside the running turn. With "show" its
   reply streams into a panel above the prompt (terminal_side_*). */
struct side_bridge {
	int (*on_text)(const char *, void *);
	void (*on_done)(json_object *, void *);
	void *state, *owner;
	int id, shown;
	struct side_bridge *next;
};
static struct side_bridge *side_bridges;

static void side_bridge_forget(struct side_bridge *bridge) {

	for (struct side_bridge **at = &side_bridges; *at; at = &(*at)->next) {
		if (*at == bridge) {
			*at = bridge->next;
			free(bridge);
			return;
		}
	}
}

static int side_bridge_text(const char *text, void *context) {

	struct side_bridge *bridge = context;
	if (bridge->shown) {
		terminal_side_text(bridge->id, text);
	}

	return bridge->on_text ? bridge->on_text(text, bridge->state) : 0;
}

static void side_bridge_done(json_object *answer, void *context) {

	struct side_bridge *bridge = context;
	if (bridge->shown) {
		terminal_side_end(bridge->id, field(answer, "text"), field(answer, "error"));
	}
	if (bridge->on_done) {
		bridge->on_done(answer, bridge->state);
	}
	side_bridge_forget(bridge);
}

static int extension_complete_async(json_object *request, int (*on_text)(const char *, void *),
	void (*on_done)(json_object *, void *), void *state, void *owner, char *error, size_t size) {

	const char *model = *field(request, "model") ? field(request, "model") : service_agent->model;
	const char *show = field(request, "show");
	/* Without the terminal UI nothing runs between inputs: answer now. */
	if (!terminal_has_ui()) {
		json_object *answer = extension_complete(request, on_text, state);
		if (on_done) {
			on_done(answer, state);
		}
		if (answer) {
			json_object_put(answer);
		}
		return -1;
	}
	struct side_bridge *bridge = calloc(1, sizeof(*bridge));
	if (!bridge) {
		snprintf(error, size, "out of memory");
		return 0;
	}
	*bridge = (struct side_bridge){on_text, on_done, state, owner, 0, 0, side_bridges};
	side_bridges = bridge;
	int id;
	if (extensions_provider_for_model(model, NULL)) {
		/* An extension's provider runs its own way, at once; the answer
           still arrives later, like the others. */
		json_object *answer = extension_complete(request, on_text, state);
		id = answer ? codex_side_later(answer, side_bridge_done, bridge, owner) : 0;
		if (answer) {
			json_object_put(answer);
		}
		if (!id) {
			snprintf(error, size, "the request could not run");
		}
	} else {
		bridge->shown = *show != 0;
		id = codex_side_start(request, service_agent->model, terminal_effort(), side_bridge_text,
			side_bridge_done, bridge, owner, error, size);
	}
	if (!id) {
		side_bridge_forget(bridge);
		return 0;
	}
	bridge->id = id;
	chats_activity(1); /* Hidden side requests are work too, even after the main turn ends. */
	if (bridge->shown) {
		terminal_side_begin(id, show);
	}

	return id;
}

/* One request (id), or every request of owner (id 0): no callbacks. */
static int extension_complete_cancel(int id, void *owner) {

	int found = 0;
	for (struct side_bridge **at = &side_bridges; *at;) {
		struct side_bridge *bridge = *at;
		if (id ? bridge->id == id : bridge->owner == owner) {
			if (bridge->shown) {
				terminal_side_end(bridge->id, NULL, "Cancelled");
			}
			*at = bridge->next;
			free(bridge);
			found = 1;
		} else {
			at = &bridge->next;
		}
	}
	codex_side_cancel(id, owner);

	return found ? 0 : 1;
}

static json_object *extension_exec(json_object *request) {

	return tools_exec_request(request);
}

static json_object *extension_host_call(
	const char *extension, const char *method, json_object *request) {

	json_object *answer = bridge_call(extension, method, request);

	return answer ? answer : host_call(extension, method, request);
}

static void cli_message(const char *text) {

	fputs(text, stderr);
}

static int extension_cli(int argc, char **argv) {

	if (argc < 2 || !extensions_cli_known(argv[1])) {
		return -1;
	}
	/* No model, session, terminal UI or native approval prompt in CLI mode.
       Redirect init/shutdown diagnostics, leaving callback stdout untouched. */
	fflush(stdout);
	int saved = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
	if (saved < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
		if (saved >= 0) {
			close(saved);
		}
		return 1;
	}
	struct extension_host cli_host = {.message = cli_message, .call = extension_host_call};
	extensions_open_cli(&cli_host, argv[1]);
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	int status = extensions_cli_execute(argc - 1, argv + 1);
	if (status < 0) {
		fprintf(stderr,
			"CLI command %s is unavailable; approve/enable its extension in an interactive chat.\n",
			argv[1]);
		status = 2;
	}
	fflush(stdout);
	dup2(STDERR_FILENO, STDOUT_FILENO);
	extensions_close();
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);

	return status;
}

/* Steering: a message an extension queued mid-run comes first. */
static char *(*terminal_steer)(void);

static char *steer(void) {

	bridge_pump();
	char *message = bridge_take();
	if (message) {
		terminal_show_origin(bridge_origin_label());
		terminal_show_user(message);
		return message;
	}
	message = host_take_steer();
	if (message) {
		terminal_show_user(message);
		return message;
	}

	return terminal_steer ? terminal_steer() : NULL;
}

/* Run the agent on a prompt (or, with NULL, on hidden context already added). */
static void run_agent(struct agent *agent, const char *message) {

	if (message) {
		terminal_show_origin(bridge_origin_label());
	}
	terminal_begin_turn(message);
	host_set_busy(1);
	if (message) {
		agent_turn(agent, message);
	} else {
		agent_continue(agent);
	}
	host_set_busy(0);
	terminal_end_turn();
}

#ifdef PHI_UI
/* Completion lists extension commands, then prompt templates and skills. */
static size_t command_count(void) {

	return extensions_command_count() + resources_command_count();
}

static const struct phi_command *command_at(size_t index) {

	size_t extensions = extensions_command_count();

	return index < extensions ? extensions_command_at(index)
							  : resources_command_at(index - extensions);
}
#endif

/* A prompt template or /skill:NAME becomes the prompt text it expands to. */
static char *expand_resources(char *message) {

	char *expanded = resources_expand(message);
	if (!expanded) {
		return message;
	}
	free(message);

	return expanded;
}

static void close_extensions(void) {

	instructions_set_dynamic(NULL);
	instructions_set_skills(NULL);
	resources_close();
	context_set_provider_lookup(NULL);
	extensions_close();
	codex_close(service_client);
	service_client = NULL;
	tools_set_provider(NULL);
	terminal_set_extension_abort(NULL);
#ifdef PHI_UI
	ui_set_extension_commands(NULL, NULL);
#endif
}

static int usage(const char *program, int error) {

	fprintf(error ? stderr : stdout,
		"Usage: %s [options] [TRANSCRIPT.jsonl] [PROMPT...]\n"
		"\n"
		"  -p, --print             run the prompts, print the final answer, exit\n"
		"      --mode text|json|rpc  output mode (json: JSONL events; rpc: JSONL protocol on stdin/stdout)\n"
		"      --model ID          model for this run (not saved)\n"
		"      --thinking LEVEL    off|minimal|low|medium|high|xhigh|max|ultra (alias --effort);\n"
		"                          a level the model lacks becomes its nearest\n"
		"      --no-session        do not save a transcript\n"
		"  -c, --continue          continue the newest session in this directory\n"
		"  -r, --resume [FILE]     resume a transcript (a picker in phi-ui without FILE)\n"
		"      --session FILE      resume FILE\n"
		"      --login             sign in to ChatGPT\n"
		"      --bg                start the chat in the background (phi attach ID opens it)\n"
		"      --no-host           run in this terminal only (no detach/reattach)\n"
		"      --NAME[=VALUE]      an option an extension registered (register_flag)\n"
		"\n"
		"Codex quota: phi usage [--json]\n"
		"Chats: phi ls;  phi attach [ID];  phi send ID TEXT;  phi close ID... [--force] or --idle;  phi rm ID\n"
		"       Left on an empty prompt, or Ctrl+\\, detaches from a chat.\n"
		"\n"
		"Packages: phi install git:host/user/repo[@ref] | URL | PATH;  phi remove NAME;\n"
		"          phi list;  phi update [NAME]  (docs/resources.md)\n"
		"\n"
		"Prompts on the command line start an interactive chat with them, or imply\n"
		"--print when stdout is not a terminal (piped stdin joins the prompt). See docs/modes.md.\n",
		program);

	return error;
}

struct input_state {
	int handled;
};

static int input_apply(json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	const char *action = field(result, "action");
	if (!strcmp(action, "handled")) {
		((struct input_state *)state)->handled = 1;
		return 1;
	}
	if (!strcmp(action, "transform") &&
		json_object_is_type(json_object_object_get(result, "text"), json_type_string)) {
		json_object_object_add(
			payload, "text", json_object_get(json_object_object_get(result, "text")));
	}

	return 0;
}

/* input event: extensions may rewrite what was typed or consume it. Returns
   the (possibly new) message, or NULL when an extension handled it. */
static const char *input_source = "interactive";

static char *filter_input(char *message) {

	if (!events_wanted("input")) {
		return message;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "text", json_object_new_string(message));
	json_object_object_add(payload, "source", json_object_new_string(input_source));
	struct input_state state = {0};
	events_emit("input", payload, input_apply, &state);
	char *result = state.handled ? NULL : strdup(field(payload, "text"));
	json_object_put(payload);
	free(message);

	return result;
}

static int user_bash_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	(void)source;
	if (!json_object_get_boolean(json_object_object_get(result, "handled"))) {
		return 0;
	}
	json_object *answer = json_object_new_object();
	json_object_object_add(answer, "output", json_object_new_string(field(result, "output")));
	json_object_object_add(answer, "exit_code",
		json_object_new_int(json_object_get_int(json_object_object_get(result, "exit_code"))));
	*(json_object **)state = answer;

	return 1;
}

/* ! shares a command's result with the model; !! is a local-only command.
   Explicit user shell commands bypass approval, but retain sandbox policy. */
static void run_user_shell(struct agent *agent, const char *message) {

	int send = message[1] != '!';
	const char *shell = message + (send ? 1 : 2);
	shell += strspn(shell, " \t");
	json_object *call = json_object_new_object();
	json_object *arguments = json_object_new_object();
	if (!call || !arguments) {
		if (call) {
			json_object_put(call);
		}
		if (arguments) {
			json_object_put(arguments);
		}
		tool_report(stderr, "Cannot allocate shell request.\n");
		return;
	}
	json_object_object_add(call, "name", json_object_new_string("bash"));
	json_object_object_add(arguments, "command", json_object_new_string(shell));
	json_object_object_add(call, "arguments",
		json_object_new_string(json_object_to_json_string_ext(arguments, JSON_C_TO_STRING_PLAIN)));
	json_object_put(arguments);

	if (send) {
		terminal_begin_turn(message);
	}
	terminal_shell_begin(shell, send);
	json_object *answer = NULL;
	if (events_wanted("user_bash")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "command", json_object_new_string(shell));
		json_object_object_add(payload, "send", json_object_new_boolean(send));
		events_emit("user_bash", payload, user_bash_apply, &answer);
		json_object_put(payload);
	}
	if (!answer) {
		answer = tools_execute(call);
	}
	terminal_shell_end(answer);
	const char *changed_cwd = field(answer, "working_directory");
	if (!send && *changed_cwd) {
		transcript_note(&agent->transcript, "cwd", changed_cwd);
	}
	if (send && answer) {
		const char *encoded = json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PRETTY);
		size_t size = strlen(shell) + strlen(encoded) + 96;
		char *combined = malloc(size);
		if (combined) {
			snprintf(combined, size,
				"I ran this shell command:\n```sh\n%s\n```\nResult:\n```json\n%s\n```", shell,
				encoded);
			host_set_busy(1);
			agent_turn(agent, combined);
			host_set_busy(0);
			free(combined);
		} else {
			tool_report(stderr,
				"Shell command completed, but its result could not be added to model context.\n");
		}
	}
	if (send) {
		terminal_end_turn();
	}
	if (answer) {
		json_object_put(answer);
	}
	json_object_put(call);
}

#ifdef PHI_UI
#define HOSTED_BY_DEFAULT 1
#else
#define HOSTED_BY_DEFAULT 0 /* the console binary is for pipes and scripts */
#endif

static int getenv_flag(const char *name) {

	const char *value = getenv(name);

	return value && *value && strcmp(value, "0");
}

static int is_file(const char *path) {

	struct stat info;

	return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

/* Piped stdin becomes part of the first prompt (print and JSON modes). */
static char *read_stdin(void) {

	size_t used = 0, capacity = 0;
	char *text = NULL;
	for (;;) {
		if (used + 4096 + 1 > capacity) {
			capacity = capacity ? capacity * 2 : 65536;
			char *grown = realloc(text, capacity);
			if (!grown) {
				free(text);
				return NULL;
			}
			text = grown;
		}
		size_t n = fread(text + used, 1, capacity - used - 1, stdin);
		if (!n) {
			break;
		}
		used += n;
	}
	if (text) {
		text[used] = 0;
	}
	while (used && (text[used - 1] == '\n' || text[used - 1] == '\r')) {
		text[--used] = 0;
	}
	if (text && !used) {
		free(text);
		text = NULL;
	}

	return text;
}

static const char *thinking_level(const char *level) {

	return settings_effort(level); /* "off" is "none"; the model's nearest level is sent */
}

/* Run queued extension work (follow-ups, continuations) until none is left. */
static void drain_host_work(struct agent *agent) {

	char *queued = NULL;
	enum host_work work;
	while (!host_shutdown_requested() && (work = host_next_work(&queued)) != HOST_NONE) {
		run_agent(agent, work == HOST_USER_MESSAGE ? queued : NULL);
		free(queued);
		queued = NULL;
	}
}

static int close_chat_control(int force, char *error, size_t size) {

	if (!force && (host_busy() || host_pending_count() || bridge_pending())) {
		snprintf(error, size, "chat is working; use --force");
		return 0;
	}
	chats_preserve_closed_status();
	json_object *request = json_object_new_object(),
				*reply = host_call("chat-control", "shutdown", request);
	json_object_put(request);
	json_object_put(reply);
	terminal_request_cancel(); /* Also unwind idle extension dialogs/approval waits. */

	return 1;
}

int main(int argc, char **argv) {

	argv = proctitle_init(argc, argv);
	chats_prepare(); /* Protect an inherited startup lock before spawning anything. */
	if (chats_hosted()) {
		/* Show up in ps/btop as the chat it is, not as "phi-ui". */
		char title[64];
		snprintf(title, sizeof(title), "phi chat %.16s", getenv("PHI_CHAT_ID"));
		proctitle_set("phi chat", title);
	}
	surfaces_set_discovery(chats_snapshot);
	int usage_status = usage_cli(argc, argv);
	if (usage_status >= 0) {
		return usage_status;
	}
	int bridge_status = bridge_cli(argc, argv);
	if (bridge_status >= 0) {
		return bridge_status;
	}
	int package_status = packages_cli(argc, argv);
	if (package_status >= 0) {
		return package_status;
	}
#ifdef PHI_UI
	chats_set_board(board_run);
#endif
	int chat_status = chats_cli(argc, argv);
	if (chat_status >= 0) {
		return chat_status;
	}
	int cli_status = extension_cli(argc, argv);
	if (cli_status >= 0) {
		return cli_status;
	}
	enum phi_mode mode = MODE_INTERACTIVE;

	enum {
		START_NEW,
		START_CONTINUE,
		START_RESUME,
		START_PATH
	} start = START_NEW;

	const char *transcript_path = NULL, *model = NULL, *thinking = NULL;
	int print = 0, no_session = 0, explicit_mode = 0, background = 0, no_host = 0;
	static char *extension_flags[32];
	size_t extension_flag_count = 0;
	char **prompts = calloc((size_t)argc + 1, sizeof(*prompts));
	size_t prompt_count = 0;
	if (!prompts) {
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *next = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			free(prompts);
			return usage(argv[0], 0);
		} else if (!strcmp(arg, "--login")) {
			free(prompts);
			return codex_auth_login(NULL) ? 0 : 1;
		} else if (!strcmp(arg, "-p") || !strcmp(arg, "--print")) {
			print = 1;
		} else if (!strcmp(arg, "--no-session")) {
			no_session = 1;
		} else if (!strcmp(arg, "--bg")) {
			background = 1;
		} else if (!strcmp(arg, "--no-host")) {
			no_host = 1;
		} else if (!strcmp(arg, "-c") || !strcmp(arg, "--continue")) {
			start = START_CONTINUE;
		} else if (!strcmp(arg, "--mode") && next) {
			explicit_mode = 1;
			if (!strcmp(next, "text")) {
				print = 1;
			} else if (!strcmp(next, "json")) {
				mode = MODE_JSON;
			} else if (!strcmp(next, "rpc")) {
				mode = MODE_RPC;
			} else {
				fprintf(stderr, "Unknown mode: %s\n", next);
				free(prompts);
				return 2;
			}
			i++;
		} else if (!strcmp(arg, "--model") && next) {
			model = next;
			i++;
		} else if ((!strcmp(arg, "--thinking") || !strcmp(arg, "--effort")) && next) {
			thinking = next;
			i++;
		} else if (!strcmp(arg, "--session") && next) {
			start = START_PATH;
			transcript_path = next;
			i++;
		} else if (!strcmp(arg, "-r") || !strcmp(arg, "--resume")) {
			if (next && is_file(next)) {
				start = START_PATH;
				transcript_path = next;
				i++;
			} else {
				start = START_RESUME;
			}
		} else if (!strcmp(arg, "--")) {
			/* "-- FILE" resumes a transcript whose name begins with '-'. */
			if (i + 1 < argc && !prompt_count && start == START_NEW && is_file(argv[i + 1])) {
				start = START_PATH;
				transcript_path = argv[++i];
			}
			while (++i < argc) {
				prompts[prompt_count++] = argv[i];
			}
		} else if (!strncmp(arg, "--", 2) && (arg[2] >= 'a' && arg[2] <= 'z') &&
			extension_flag_count < 32) {
			/* Maybe an extension's flag; checked once extensions are loaded. */
			extension_flags[extension_flag_count++] = argv[i];
		} else if (arg[0] == '-' && arg[1]) {
			fprintf(stderr, "Unknown option: %s\n", arg);
			free(prompts);
			return usage(argv[0], 2);
		} else if (!prompt_count && start == START_NEW && strlen(arg) > 6 &&
			!strcmp(arg + strlen(arg) - 6, ".jsonl") && is_file(arg)) {
			start = START_PATH;
			transcript_path = arg;
		} else {
			prompts[prompt_count++] = argv[i];
		}
	}
	/* The launcher selected and locked this exact transcript. In particular,
       --continue must not select a different 'newest' file during startup. */
	const char *hosted_file = chats_hosted() ? getenv("PHI_CHAT_SESSION_FILE") : NULL;
	if (hosted_file && (start == START_CONTINUE || start == START_PATH)) {
		start = *hosted_file ? START_PATH : START_NEW;
		transcript_path = *hosted_file ? hosted_file : NULL;
	}
	if (mode == MODE_INTERACTIVE && !background &&
		(print || (!explicit_mode && prompt_count && !isatty(STDOUT_FILENO)))) {
		mode = MODE_PRINT;
	}
	if (background && (mode != MODE_INTERACTIVE || print)) {
		fprintf(
			stderr, "--bg starts an interactive chat; it cannot be combined with -p or --mode.\n");
		free(prompts);
		return 2;
	}
	/* Interactive chats run inside phi-host (see core/chats.h), so closing
       the terminal leaves them running. --bg only starts one. */
	if (background ||
		(mode == MODE_INTERACTIVE && !no_host && !chats_hosted() && !getenv_flag("PHI_NO_HOST") &&
			isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && HOSTED_BY_DEFAULT)) {
		char **forwarded = calloc((size_t)argc + 1, sizeof(*forwarded));
		int forwarded_count = 0;
		for (int i = 0; forwarded && i < argc; i++) {
			if (strcmp(argv[i], "--bg") && strcmp(argv[i], "--no-host")) {
				forwarded[forwarded_count++] = argv[i];
			}
		}
		int hosted = forwarded ? chats_host(forwarded_count, forwarded, background,
									 start == START_PATH ? transcript_path : NULL,
									 start == START_CONTINUE, prompts, prompt_count)
							   : 1;
		free(forwarded);
		if (hosted >= 0 || background) {
			free(prompts);
			return hosted < 0 ? 1 : hosted;
		}
		/* No phi-host: run here, as before. */
	}
	char *piped = NULL;
	if ((mode == MODE_PRINT || mode == MODE_JSON) && !isatty(STDIN_FILENO)) {
		piped = read_stdin();
	}
	if ((mode == MODE_PRINT || mode == MODE_JSON) && !prompt_count && !piped) {
		fprintf(stderr, "No prompt: give one on the command line or pipe it on stdin.\n");
		free(prompts);
		return 2;
	}
	if (start == START_RESUME && mode != MODE_INTERACTIVE) {
		fprintf(stderr, "--resume needs a transcript file outside interactive mode.\n");
		free(prompts);
		free(piped);
		return 2;
	}

	struct settings settings;
	char error[256];
	if (settings_load(&settings, error, sizeof(error)) < 0) {
		/* Defaults are for an absent config, not a corrupted safety policy. */
		fprintf(stderr, "%s\nRefusing to start with unreadable or invalid settings.\n", error);
		return 1;
	}
	if (model) {
		if (!settings_model_valid(model)) {
			fprintf(stderr, "Invalid model name: %s\n", model);
			free(prompts);
			free(piped);
			return 2;
		}
		snprintf(settings.model, sizeof(settings.model), "%s", model);
	}
	if (thinking) {
		const char *effort = thinking_level(thinking);
		if (!effort) {
			fprintf(stderr, "Unknown thinking level: %s\n", thinking);
			free(prompts);
			free(piped);
			return 2;
		}
		snprintf(settings.effort, sizeof(settings.effort), "%s", effort);
	}
	if (no_session) {
		transcript_disable(1);
	}
	struct agent agent;
	if (!agent_open(&agent, settings.model, settings.effort)) {
		free(prompts);
		free(piped);
		return 1;
	}
	modes_start(mode, &agent, &settings);
	if (mode != MODE_INTERACTIVE) {
		input_source = mode == MODE_RPC ? "rpc" : "print";
	}
	agent.host = terminal_open(settings.model, settings.effort);
	bridge_set_cancel(terminal_extension_cancel);
	terminal_configure_safety(settings.approval, settings.sandbox);
	struct extension_host extension_host = {.confirm_native = terminal_native_approval,
		.cancel = terminal_extension_cancel,
		.activity = terminal_extension_activity,
		.message = extension_message,
		.status = terminal_extension_status,
		.reserved_tool = tool_builtin_exists,
		.complete = extension_complete,
		.exec = extension_exec,
		.call = extension_host_call,
		.ui = terminal_ui,
		.complete_async = extension_complete_async,
		.complete_cancel = extension_complete_cancel};
	host_attach(&agent, &settings);
	host_set_display(terminal_display_message);
	terminal_steer = agent.host.steer;
	agent.host.steer = steer;
	agent.host.boundary = host_boundary;
	agent.host.user_origin = bridge_take_origin;
	service_agent = &agent;
	host_set_flags(extension_flags, extension_flag_count); /* before init reads them */
	extensions_open(&extension_host);
	struct tool_provider extension_provider = {.find = extensions_tool_find,
		.append_schema = extensions_append_schema,
		.execute = extensions_execute,
		.event = extensions_event};
	tools_set_provider(&extension_provider);
	terminal_set_extension_abort(phi_abort_extension_compile);
	agent.host.event = extensions_event;
	agent.host.turn_messages = extensions_append_turn_messages;
	agent.host.tool_intercept = extensions_intercept_tool;
	instructions_set_dynamic(extensions_context);
	context_set_provider_lookup(extensions_provider_for_model);
#ifdef PHI_UI
	ui_set_history_reader(saved_history_index);
	ui_set_extension_commands(command_count, command_at);
	ui_set_extension_models(extensions_model_count, extensions_model_at);
#endif
	resources_load("startup");
	instructions_set_skills(resources_system_prompt);
	const char *unclaimed = host_unclaimed_flag();
	if (unclaimed && mode == MODE_INTERACTIVE && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
		/* An extension awaiting approval may be the one that claims it. */
		char notice[256];
		snprintf(notice, sizeof(notice),
			"Option %.120s is not registered by any loaded extension.\n"
			"It is kept, in case an extension you approve now registers it.",
			unclaimed);
		terminal_extension_message(notice);
	} else if (unclaimed) {
		close_extensions();
		agent_close(&agent);
		terminal_close();
		modes_stop();
		fprintf(stderr, "Unknown option: %s (no extension registered it)\n", unclaimed);
		free(prompts);
		free(piped);
		return 2;
	}
	extensions_event("session_load", NULL);
	events_notify("session_start", "reason", "startup");
	if (start == START_PATH && !terminal_resume_path(&agent, transcript_path)) {
		close_extensions();
		agent_close(&agent);
		terminal_close();
		modes_stop();
		free(prompts);
		free(piped);
		return 1;
	}
	if (start == START_CONTINUE) {
		terminal_continue(&agent);
	} else if (start == START_RESUME) {
		terminal_resume(&agent);
	}
	modes_header();
	/* Interactive prompts on the command line are the chat's first messages. */
	for (size_t i = 0; mode == MODE_INTERACTIVE && i < prompt_count; i++) {
		char *first = expand_resources(strdup(prompts[i]));
		if (first) {
			host_enqueue_prompt(first, 1);
		}
		free(first);
	}
	if (mode == MODE_INTERACTIVE) {
		bridge_set_close_handler(close_chat_control);
		if (!bridge_open()) {
			fprintf(stderr, "Cannot start the private chat bridge.\n");
			close_extensions();
			agent_close(&agent);
			terminal_close();
			modes_stop();
			return 1;
		}
		terminal_set_service(bridge_pump, bridge_pending);
		chats_begin(&agent);
	}

	int status = 0;
	char *message;
	if (mode == MODE_PRINT || mode == MODE_JSON) {
		/* One-shot: each prompt in turn, then any work extensions queued. */
		for (size_t i = 0; i < (prompt_count ? prompt_count : 1) && !host_shutdown_requested();
			i++) {
			const char *text = prompt_count ? prompts[i] : "";
			if (i == 0 && piped) {
				size_t size = strlen(piped) + strlen(text) + 3;
				message = malloc(size);
				if (message) {
					snprintf(message, size, "%s%s%s", piped, *text ? "\n\n" : "", text);
				}
			} else {
				message = strdup(text);
			}
			if (!message || !(message = filter_input(message))) {
				continue;
			}
			message = expand_resources(message);
			int command = message[0] == '/' ? command_run(&agent, &settings, message) : 0;
			if (command == 2) {
				free(message);
				break;
			}
			if (!command && message[strspn(message, " \t\r\n\v\f")]) {
				run_agent(&agent, message);
			}
			free(message);
			drain_host_work(&agent);
			if (modes_failed()) {
				break;
			}
		}
		status = modes_failed();
		if (mode == MODE_PRINT) {
			char *answer = modes_last_text();
			if (answer && !status) {
				printf("%s\n", answer);
			}
			free(answer);
		}
	} else if (mode == MODE_RPC) {
		/* Commands arrive on stdin; prompts run from the host queue. */
		for (;;) {
			tools_reap_jobs();
			if (host_shutdown_requested()) {
				break;
			}
			if (host_take_reload()) {
				json_object *arguments = json_object_new_object();
				json_object_object_add(arguments, "action", json_object_new_string("reload"));
				json_object *answer = extensions_manage(arguments);
				if (answer) {
					json_object_put(answer);
				}
				json_object_put(arguments);
				resources_load("reload");
			}
			char *queued = NULL;
			enum host_work work = host_next_work(&queued);
			if (work != HOST_NONE) {
				run_agent(&agent, work == HOST_USER_MESSAGE ? queued : NULL);
				free(queued);
				continue;
			}
			if (!modes_rpc_wait(250)) {
				break;
			}
		}
	} else {
		for (;;) {
			tools_reap_jobs();
			bridge_pump();
			if (host_shutdown_requested()) {
				break;
			}
			char *external = bridge_take();
			if (external) {
				run_agent(&agent, external);
				free(external);
				continue;
			}
			if (host_take_reload()) {
				json_object *arguments = json_object_new_object();
				json_object_object_add(arguments, "action", json_object_new_string("reload"));
				json_object *answer = extensions_manage(arguments);
				if (answer) {
					terminal_extension_result(answer);
					json_object_put(answer);
				}
				json_object_put(arguments);
				resources_load("reload");
			}
			char *queued = NULL;
			enum host_work work = host_next_work(&queued);
			if (work != HOST_NONE) {
				run_agent(&agent, work == HOST_USER_MESSAGE ? queued : NULL);
				free(queued);
				continue;
			}
			if (!(message = terminal_input())) {
				break;
			}
			if (!*message) {
				free(message);
				continue;
			} /* external queue wakeup */
			if (!(message = filter_input(message))) {
				continue;
			}
			message = expand_resources(message);
			if (message[0] == '!' && message[1]) {
				run_user_shell(&agent, message);
				free(message);
				continue;
			}
			int command = command_run(&agent, &settings, message);
			if (command == 2) {
				free(message);
				break;
			}
			if (!command && message[strspn(message, " \t\r\n\v\f")]) {
				run_agent(&agent, message);
			}
			free(message);
		}
	}
	if (events_wanted("session_shutdown")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "reason", json_object_new_string("exit"));
		json_object_object_add(
			payload, "session_file", json_object_new_string(agent.transcript.path));
		events_emit("session_shutdown", payload, NULL, NULL);
		json_object_put(payload);
	}
	chats_end();
	terminal_set_service(NULL, NULL);
	bridge_close();
	bridge_set_cancel(NULL);
	close_extensions();
	host_reset();
	tools_shutdown();
	agent_close(&agent);
	terminal_close();
	modes_stop();
	free(prompts);
	free(piped);
	if (mode == MODE_INTERACTIVE) {
		puts("Bye!");
	}

	return status;
}
