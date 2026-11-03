#ifndef PHI_COMMANDS_H
#define PHI_COMMANDS_H
#include <stddef.h>
#include <string.h>
struct agent;
struct settings;
/* NULL agent: workspace/new-chat defaults; session-only commands fail closed. */
int command_run(struct agent *agent, struct settings *settings, const char *message);

/* Shared by local dispatch and the input suggestions. */
enum {
	CMD_SETTINGS,
	CMD_MODEL,
	CMD_EFFORT,
	CMD_APPROVAL,
	CMD_SANDBOX,
	CMD_RELOAD,
	CMD_TRANSCRIPT,
	CMD_SESSION,
	CMD_NAME,
	CMD_NEW,
	CMD_RESUME,
	CMD_TREE,
	CMD_FORK,
	CMD_CLONE,
	CMD_LOGIN,
	CMD_LOGOUT,
	CMD_COPY,
	CMD_HELP,
	CMD_COMPACT,
	CMD_EXPORT,
	CMD_CLEAR,
	CMD_EXTENSIONS,
	CMD_RELOAD_EXTENSIONS,
	CMD_REFRESH_MODELS,
	CMD_RESOURCES,
	CMD_THEME,
	CMD_HISTORY,
	CMD_BOARD,
	CMD_QUIT
};

struct phi_command {
	const char *name, *description;
	int argument;
};
static const struct phi_command phi_commands[] = {{"/settings", "Show current settings", 0},
	{"/model", "Show locally advertised models or switch", 1},
	{"/effort", "Set the thinking level (the model's own: low … max)", 1},
	{"/approval", "Set off, dangerous, or all", 1},
	{"/sandbox", "Set off, workspace, or strict", 1},
	{"/reload", "Reload theme colours, skills and prompt templates", 0},
	{"/transcript", "Show the saved transcript path", 0},
	{"/session", "Show current session information", 0}, {"/name", "Name the current session", 1},
	{"/new", "Start a new session", 0}, {"/resume", "Find and resume a saved session", 0},
	{"/tree", "Navigate branches in this session", 0},
	{"/fork", "Fork from an earlier user prompt", 0}, {"/clone", "Clone the current branch", 0},
	{"/login", "Sign in to OpenAI or an extension provider in your browser", 1},
	{"/logout", "Remove OpenAI or extension provider credentials", 1},
	{"/copy", "Copy the last assistant response", 0}, {"/help", "Show keybindings and command help", 0},
	{"/compact", "Summarize old context; optional focus", 1},
	{"/export", "Export active session as HTML: /export PATH", 1},
	{"/clear", "Start a fresh conversation", 0},
	{"/extensions", "List, inspect API, or disable native extensions", 1},
	{"/reload-extensions", "Reload changed C extensions; optional name", 1},
	{"/refresh-models", "Fetch Codex models for /model hints", 0},
	{"/resources", "List skills and prompt templates", 0},
	{"/theme", "List themes, or save this transcript's theme: /theme NAME", 1},
	{"/history", "Browse saved history from disk without loading the whole transcript", 0},
	{"/board", "Open the chat board without stopping this chat", 0}, {"/quit", "Exit Phi", 0}};
#define PHI_COMMAND_COUNT (sizeof(phi_commands) / sizeof(*phi_commands))

static inline const char *command_description(const struct phi_command *command, int workspace) {

	if (workspace) {
		if (!strcmp(command->name, "/resume")) {
			return "Browse saved chats on the board; Enter reopens one";
		}
		if (!strcmp(command->name, "/theme")) {
			return "Theme for this board and new chats";
		}
		if (!strcmp(command->name, "/model")) {
			return "Model default for new chats";
		}
		if (!strcmp(command->name, "/effort")) {
			return "Thinking-level default for new chats";
		}
		if (!strcmp(command->name, "/approval")) {
			return "Approval default for new chats";
		}
		if (!strcmp(command->name, "/sandbox")) {
			return "Sandbox default for new chats";
		}
		if (!strcmp(command->name, "/new") || !strcmp(command->name, "/clear")) {
			return "Clear the new-task composer; keep existing chats";
		}
		if (!strcmp(command->name, "/quit")) {
			return "Leave the board; agents keep running";
		}
	}

	return command->description;
}

static inline int command_input_path(const char *message) {

	size_t n = strcspn(message, " \t\r\n");

	return message[0] == '/' && n > 1 &&
		(memchr(message + 1, '/', n - 1) || memchr(message + 1, '.', n - 1));
}

/* A workspace has settings and a composer, not an implicit selected session. */
static inline int command_workspace_allowed(size_t id) {

	return id == CMD_SETTINGS || id == CMD_MODEL || id == CMD_EFFORT || id == CMD_APPROVAL ||
		id == CMD_SANDBOX || id == CMD_RELOAD || id == CMD_THEME || id == CMD_LOGIN ||
		id == CMD_LOGOUT || id == CMD_HELP || id == CMD_RESOURCES || id == CMD_REFRESH_MODELS ||
		id == CMD_RESUME || id == CMD_BOARD || id == CMD_QUIT || id == CMD_NEW || id == CMD_CLEAR;
}
#endif
