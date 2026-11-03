#ifndef PHI_CHATS_H
#define PHI_CHATS_H
/* Background chats. An interactive Phi runs on its own terminal held by
   phi-host, so closing the window leaves it running and any terminal (or an
   ssh login) can attach to it again.

     phi                    a new chat, hosted (--no-host or PHI_NO_HOST=1: not)
     phi --bg "task"        start a chat in the background
     phi ls                 chats: needs input, working, ended
     phi board              the chat board (also: Left on an empty prompt)
     phi attach [ID]        show a chat here (an ended one is resumed)
     phi send ID "text"     reply without opening it
     phi rm ID              forget an ended chat

   Inside a chat, Left on an empty prompt (or Ctrl+\) detaches to the
   board; Left or Esc there returns.

   An empty hosted chat keeps only a live status by its socket in
   $XDG_RUNTIME_DIR/phi; it is removed on exit. After the first message,
   its status moves to $XDG_STATE_HOME/phi/chats/ID.json with id, pid, socket,
   session file, has_messages, cwd, title, state, detail, started and updated.
   Opening a completed chat resumes its latest history with the same id,
   title and transcript. --continue and --session reuse that identity too;
   if it is already running they attach to its host instead of duplicating it.
   Sockets also live in $XDG_RUNTIME_DIR/phi. */
#include <stddef.h>
#include <json-c/json.h>
struct agent;

/* Call at process entry, before extension/tool subprocesses can be spawned. */
void chats_prepare(void);

/* Chat subcommands (ls, attach, send, rm). Returns an exit status, or -1
   when argv is not one. */
int chats_cli(int argc, char **argv);
/* Resolve this invocation to an existing conversation host or start one
   (background: start/locate it and return). Concurrent resumes serialize
   through status publication; the chosen transcript is pinned for the child.
   argv is passed to the hosted Phi unchanged.
   Returns an exit status, or -1 if hosting is unavailable (run normally). */
int chats_host(int argc, char **argv, int background, const char *resume_path, int continue_latest,
	char **prompts, size_t prompt_count);
/* Inside a hosted chat: report status for this agent until chats_end(). */
void chats_begin(struct agent *agent);
void chats_end(void);
/* Non-agent work (extensions and side requests), independent of attachment. */
void chats_activity(int active);
/* Hold Working for the whole runtime turn, including lifecycle callbacks. */
void chats_run(int active);
int chats_hosted(void); /* this process is a hosted chat */

/* The board (phi-ui): shown when a chat detaches. It returns what to do
   next; for CHATS_BOARD_OPEN, chosen holds the chat id. */
enum {
	CHATS_BOARD_RETURN,
	CHATS_BOARD_OPEN,
	CHATS_BOARD_QUIT
};

void chats_set_board(int (*board)(const char *current, char *chosen, size_t size));

struct chat_row {
	char id[16], title[128], detail[256], group[16], state[16], place[128], model[96], effort[16],
		session_file[4096];
	long long updated, session_bytes, session_stamp;
	int live;
};

/* Chats grouped (waiting; requires approval; working; saved), newest first
   within each group; caller frees. Chats without a first turn are hidden. */
size_t chats_rows(struct chat_row **rows);
int chats_live(const char *id); /* even when hidden from the board */
void chats_age(long long when, char *out, size_t size);
int chats_send(const char *id, const char *text, char *error, size_t size);
void chats_preserve_closed_status(void);
int chats_close_force(const char *id, int force, char *error, size_t size);
int chats_close(const char *id, char *error, size_t size); /* stop it; the session stays saved */
int chats_forget(const char *id, char *error, size_t size);
int chats_start(const char *task, char *id, size_t size); /* a new background chat */
/* Running chats rename themselves with /name (only when waiting for you);
   ended ones get a name record in their session. */
int chats_rename(const char *id, const char *name, char *error, size_t size);
/* Stable, owned snapshots for the bridge; liveness is recomputed on each query. */
json_object *chats_snapshot(void);
json_object *chats_lookup(const char *id, char *error, size_t size);
int chats_launch(json_object *options, char *id, size_t size, char *error, size_t error_size);
void chats_bridge_status(const char *socket, int queued);
int chats_record_delivery(
	json_object *message); /* durable full payload before queue acknowledgement */
#endif
