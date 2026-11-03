#ifndef PHI_AGENT_H
#define PHI_AGENT_H
#include "codex.h"
#include "transcript.h"

struct agent_host {
	struct response_sink response;
	int (*tool_begin)(json_object *call); /* nonzero permits execution */
	/* Interception happens before tool_begin, so approval sees actual rewritten
       arguments. A rewritten call is newly owned by the caller. */
	int (*tool_intercept)(json_object *call, json_object **effective, char *note, size_t note_size);
	void (*tool_end)(json_object *answer);
	/* A call an extension refused before it could run: shown, not executed. */
	void (*tool_refused)(json_object *call, json_object *answer);
	const char *(*effort)(void);
	void (*event)(const char *name, json_object *payload); /* borrowed, observational */
	/* Called exactly once for each accepted user message, after that visible
       message enters history and before the next provider request. */
	size_t (*turn_messages)(json_object *history, const char *user_message);
	char *(*steer)(void); /* queued user input at a safe boundary; caller frees */
	/* After each round of tool calls: add queued hidden context to history;
       nonzero asks for another request. */
	int (*boundary)(json_object *history);
	/* Presentation-only transcript identity; never enters model history. */
	void (*transcript_changed)(const char *path, int inherit);
	json_object *(*user_origin)(void); /* owned metadata for the next visible user entry, or NULL */
};

struct agent {
	struct codex *client;
	json_object *history;
	const char *model, *effort; /* borrowed for the agent's lifetime */
	char announced_model[96], announced_effort[16]; /* last model/effort reported to extensions */
	struct agent_host host;
	struct transcript transcript;
};

int agent_open(struct agent *agent, const char *model, const char *effort);
void agent_close(struct agent *agent);
void agent_clear(struct agent *agent);
struct session_entry;
int agent_resume(struct agent *agent, struct session_entry *entry, char *error, size_t size);
int agent_navigate(
	struct agent *agent, json_object *history, uint64_t head, char *error, size_t size);
int agent_branch(struct agent *agent, json_object *history, const char *kind, uint64_t source_entry,
	char *error, size_t size);
void agent_turn(struct agent *agent, const char *message);
/* Run the agent on the current history without adding a user message
   (after an extension added hidden context and asked for a turn). */
void agent_continue(struct agent *agent);
/* execute_tool: run a tool from inside a running tool; see agent.c. */
json_object *agent_execute_nested(struct agent *agent, const char *name, json_object *arguments);
int agent_compact(struct agent *agent, const char *focus, char *error, size_t size);
/* Report a model or effort change to extensions (model_select /
   thinking_level_select) if it differs from the last one reported. */
void agent_note_selection(struct agent *agent, const char *source);
/* While a response streams: its text so far ("" otherwise), and the tool
   call being written (borrowed; NULL if none). */
const char *agent_streaming_text(void);
json_object *agent_streaming_call(void);
#endif
