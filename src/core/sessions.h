#ifndef PHI_SESSIONS_H
#define PHI_SESSIONS_H
#include <json-c/json.h>
#include <sys/types.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>

struct session_entry {
	char path[4096], cwd[4096], title[256];
	time_t modified;
	int named, version, starred;
	dev_t device;
	ino_t inode;
};

enum session_node_kind {
	SESSION_ITEM,
	SESSION_COMPACTION
};

struct session_node {
	uint64_t id, parent;
	time_t time;
	unsigned depth, children; /* depth counts actual branch splits, not message ancestry */
	int active, hidden;
	unsigned
		display; /* 1 user, 2 assistant, 4 editable prompt, 8 tool, 16 reasoning, 32 public summary */
	char label[128];
	int bookmarked, annotation_dirty;
	enum session_node_kind kind;
	json_object *data; /* owned by eager trees; NULL for disk-backed records */
	off_t offset;
	size_t bytes;
};

struct session_tree {
	struct session_node *nodes;
	size_t count;
	uint64_t head;
	FILE *source; /* pinned read-only inode for indexed payloads */
	off_t source_bytes;
};

/* Caller frees the list and owns loaded histories/trees. No tools are executed. */
int sessions_list(struct session_entry **entries, size_t *count, char *error, size_t size);
/* The newest saved session started in directory (real paths compared, so
   symlinks match): 1 found, 0 none, -1 error. For --continue. */
int sessions_latest_in(const char *directory, struct session_entry *out, char *error, size_t size);
int session_from_path(const char *path, struct session_entry *entry, char *error, size_t size);
int session_star(struct session_entry *entry, int starred, char *error, size_t size);
json_object *session_load(struct session_entry *entry, char *error, size_t size);
json_object *session_load_ex(struct session_entry *entry, uint64_t *head, char *error, size_t size);
int session_tree_load(
	struct session_entry *entry, struct session_tree *tree, char *error, size_t size);
/* Index records without retaining their JSON payloads. Returned items are owned
   by the caller; appends are allowed, replacement/truncation is not followed. */
int session_tree_index(
	struct session_entry *entry, struct session_tree *tree, char *error, size_t size);
json_object *session_tree_item(
	const struct session_tree *tree, size_t index, char *error, size_t size);
json_object *session_tree_history(
	const struct session_tree *tree, uint64_t head, char *error, size_t size);
void session_tree_free(struct session_tree *tree);
void session_item_text(json_object *item, char *text, size_t size);
/* Case-insensitive fuzzy matching of already loaded visible text; -1 on miss. */
int session_text_score(const char *text, const char *query);
int session_score(const struct session_entry *entry, const char *query);
/* Visible user/assistant text from complete records in the last 64 KiB only. */
int session_recent_text(
	const struct session_entry *entry, char *text, size_t size, char *error, size_t error_size);
/* When a loaded item was saved (0 if unknown, such as an item made this session). */
time_t sessions_item_time(json_object *item);
void sessions_set_item_time(json_object *item, time_t time);
#endif
