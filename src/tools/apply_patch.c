#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#define PATCH_MAX_BYTES (4 * 1024 * 1024)

/* Git interprets diff paths relative to the repository root, even when run
   below it. Prefix them explicitly so this tool consistently uses the CWD.
   Outside a repository, git apply already uses the current directory. */
static int patch_directory(char **option) {

	char *output = NULL;
	int code = -1;
	char *arguments[] = {"rev-parse", "--show-prefix", NULL};
	*option = NULL;
	if (tool_git_run(NULL, arguments, NULL, NULL, 0, 15, &output, NULL, &code)) {
		return -1;
	}
	if (!code) {
		size_t size = strlen(output);
		if (size && output[size - 1] == '\n') {
			output[--size] = '\0';
		}
		if (size) {
			*option = malloc(sizeof("--directory=") + size);
			if (!*option) {
				free(output);
				return -1;
			}
			snprintf(*option, sizeof("--directory=") + size, "--directory=%s", output);
		}
	}
	free(output);

	return 0;
}

static json_object *run(json_object *args) {

	json_object *value = NULL;
	size_t size = 0;
	const char *patch = NULL;
	if (!args || !json_object_object_get_ex(args, "patch", &value) ||
		!(patch = tool_plain_string(value, &size)) || !size || size > PATCH_MAX_BYTES) {
		return tool_error("Expected a nonempty unified diff up to 4 MiB.");
	}

	char *directory = NULL, *output = NULL;
	json_object *answer = NULL;
	int code = -1;
	if (patch_directory(&directory)) {
		return tool_error(tool_process_error());
	}

	/* Combine validation and the preview in one Git process. --numstat also
       reveals silently skipped patches; a successful no-op is not success. */
	char *check[] = {"apply", "--check", "--numstat", "--whitespace=nowarn", NULL, NULL, NULL};
	size_t at = 4;
	if (directory) {
		check[at++] = directory;
	}
	check[at] = "-";
	if (tool_git_run(NULL, check, NULL, patch, size, 15, &output, NULL, &code)) {
		answer = tool_error(tool_process_error());
		goto done;
	}
	if (code || !*output) {
		answer = tool_error(*output ? output : "Patch changes no files in the current directory.");
		goto done;
	}
	if (tool_show_banners()) {
		tool_report(stderr, "[apply_patch]\n%s", output);
	} else {
		tool_display(output, stderr);
	}
	free(output);
	output = NULL;

	char *apply[] = {"apply", "--whitespace=nowarn", NULL, NULL, NULL};
	at = 2;
	if (directory) {
		apply[at++] = directory;
	}
	apply[at] = "-";
	if (tool_git_run(NULL, apply, NULL, patch, size, 30, &output, NULL, &code)) {
		answer = tool_error(tool_process_error());
	} else if (code) {
		answer =
			tool_error(*output ? output : "Patch application failed; inspect the working tree.");
	} else {
		answer = json_object_new_object();
		json_object_object_add(answer, "applied", json_object_new_boolean(1));
	}
done:
	free(directory);
	free(output);

	return answer;
}

const struct phi_tool phi_tool_apply_patch = {.name = "apply_patch",
	.schema =
		"{\"type\":\"function\",\"name\":\"apply_patch\","
		"\"description\":\"Apply an ordinary unified diff with paths relative to the current working directory. All hunks are checked before application. Requires Git but not a repository. Application across multiple files is not a crash-atomic transaction; inspect the tree after interruption or an I/O failure.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"patch\":{\"type\":\"string\"}},\"required\":[\"patch\"],"
		"\"additionalProperties\":false},\"strict\":true}",
	.detail_key = NULL,
	.run = run};
