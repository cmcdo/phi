#ifndef PHI_COMPACTION_H
#define PHI_COMPACTION_H
#include <json-c/json.h>
#include <stddef.h>
struct codex;
struct response_sink;
/* Pi-compatible defaults: 16k response reserve and 20k recent context. */
#define PHI_COMPACTION_RESERVE 16384
#define PHI_COMPACTION_KEEP 20000
int64_t compaction_estimate(json_object *history);
/* Number of leading items a compaction would summarize (0: nothing to do). */
size_t compaction_boundary(json_object *history);
/* Replace items [0, cut) with a checkpoint holding summary; 0 on failure. */
int compaction_replace(json_object **history, size_t cut, const char *summary);
/* Replaces old context with a checkpoint plus a recent, call-safe tail. */
int compaction_run(struct codex *client, json_object **history, const char *model,
	const char *effort, const char *focus, struct response_sink sink, char *error,
	size_t error_size);
#endif
