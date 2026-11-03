#ifndef PHI_MODES_H
#define PHI_MODES_H
/* Non-interactive interfaces, following Pi's:
     print  run the prompts, write the final assistant text, exit
     json   run the prompts, write JSONL session events, exit
     rpc    JSONL commands on stdin; responses and events on stdout
   See docs/modes.md for the protocol. */
struct agent;
struct settings;

enum phi_mode {
	MODE_INTERACTIVE,
	MODE_PRINT,
	MODE_JSON,
	MODE_RPC
};

/* Install output routing and event translation before terminal_open(). */
void modes_start(enum phi_mode mode, struct agent *agent, struct settings *settings);
void modes_header(void); /* JSON mode's session header record */
/* RPC: read and handle commands for up to timeout_ms (-1 waits). Returns 0
   when stdin has closed (orderly shutdown). */
int modes_rpc_wait(int timeout_ms);
int modes_failed(void); /* the last prompt did not complete */
char *modes_last_text(void); /* last assistant text; caller frees */
void modes_stop(void);
#endif
