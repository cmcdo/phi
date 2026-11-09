#ifndef PHI_TRANSCRIPT_H
#define PHI_TRANSCRIPT_H
#include <json-c/json.h>
#include <stdio.h>
#include <stdint.h>

struct transcript {
	FILE *file;
	char path[4096];
	size_t saved;
	int failed;
	uint64_t head, next_id;
	long long offset; /* file size after our last write: more means another writer */
	char *text; /* streamed reply text not yet written, see transcript_text() */
	size_t text_used, text_capacity;
};

void transcript_begin(struct transcript *t, const char *model, const char *effort);
/* Continue writing an existing v2 session file in place (resume). head is
   the node to continue from, next_id one past the file's last node, and
   saved the length of the history being continued. Several Phi processes
   may append to one file: each write takes a lock and first adopts ids
   another writer used, so their messages become sibling branches. */
int transcript_reopen(
	struct transcript *t, const char *path, uint64_t head, uint64_t next_id, size_t saved);
/* --no-session: never create transcript files (writes become no-ops). */
void transcript_disable(int disabled);
int transcript_disabled(void);
/* --temp: process-lifetime, set before agent/UI/extension startup. */
void transcript_set_temporary(void);
int transcript_temporary(void);
void transcript_sync(struct transcript *t, json_object *history);
void transcript_compact(struct transcript *t, json_object *history);
/* Copy an existing v2 node verbatim into a fresh continuation file. */
int transcript_import_node(
	struct transcript *t, const char *type, json_object *data, uint64_t id, uint64_t parent);
/* Select an already-saved tree node (0 is the root). */
int transcript_select(struct transcript *t, uint64_t head, size_t saved);
/* Record ancestry when a new session file is created from another one. */
void transcript_link(struct transcript *t, const char *kind, const char *path, uint64_t entry);
void transcript_note(struct transcript *t, const char *type, const char *text);
/* Persist an explicit one-line name in an existing owned transcript. */
int transcript_name(const char *path, const char *name);
/* Append a non-conversation record (e.g. an extension's custom entry). */
int transcript_entry(struct transcript *t, const char *type, json_object *data);
int transcript_annotate(struct transcript *t, uint64_t id, const char *label, int bookmarked);
void transcript_text(const char *text, void *context);
void transcript_close(struct transcript *t);
#endif
