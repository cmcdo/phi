#ifndef PHI_TOOL_RESULT_H
#define PHI_TOOL_RESULT_H
/* Whether a tool's saved or live answer is a failure, and its one-line
   summary. Shared by the live card (terminal.c) and resumed sessions
   (resume.inc), so a failed command looks the same in both. */
#include <json-c/json.h>
#include <stdio.h>

static inline int tool_answer_failed(json_object *answer, char *summary, size_t size) {

	json_object *value = NULL;
	const char *error = json_object_object_get_ex(answer, "error", &value) &&
			json_object_is_type(value, json_type_string)
		? json_object_get_string(value)
		: "";
	int timed_out = json_object_get_boolean(json_object_object_get(answer, "timed_out"));
	int aborted = json_object_get_boolean(json_object_object_get(answer, "cancelled"));
	json_object *exit_value = NULL;
	int has_exit = json_object_object_get_ex(answer, "exit_code", &exit_value) && exit_value;
	int code = has_exit ? json_object_get_int(exit_value) : 0;
	int killed = json_object_get_boolean(json_object_object_get(answer, "killed"));
	if (*error) {
		snprintf(summary, size, "%s", error);
	} else if (aborted) {
		snprintf(summary, size, "Cancelled");
	} else if (timed_out) {
		snprintf(summary, size, "Timed out");
	} else if (killed) {
		snprintf(summary, size, "Killed");
	} else {
		snprintf(summary, size, "Exit %d", code);
	}

	return *error || timed_out || aborted || (has_exit && code != 0 && !killed);
}
#endif
