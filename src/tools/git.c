#define _GNU_SOURCE
#include "internal.h"
#include <ctype.h>
#include <sys/file.h>

struct repository {
	char *root;
	char *gitdir;
	char *common;
	char *index;
	int lock;
};

static void repository_close(struct repository *repo) {

	if (repo->lock >= 0) {
		close(repo->lock);
	}
	free(repo->root);
	free(repo->gitdir);
	free(repo->common);
	free(repo->index);
	*repo = (struct repository){.lock = -1};
}

/* Remove Git's line terminator, not newline characters belonging to a path. */
static void trim_terminator(char *text) {

	size_t size = strlen(text);
	if (size && text[size - 1] == '\n') {
		text[size - 1] = '\0';
	}
}

static int run_git(const char *cwd, char *const argv[], const struct tool_env *env,
	const char *input, char **output) {

	int code = -1;
	if (tool_git_run(cwd, argv, env, input, input ? strlen(input) : 0, 30, output, NULL, &code)) {
		return -1;
	}
	if (code) {
		errno = code == 127 ? ENOENT : EIO;
		return -1;
	}

	return 0;
}

static char *git_line(const char *cwd, char *const argv[]) {

	char *output = NULL;
	if (run_git(cwd, argv, NULL, NULL, &output)) {
		free(output);
		return NULL;
	}
	trim_terminator(output);

	return output;
}

static int repository_open(struct repository *repo) {

	*repo = (struct repository){.lock = -1};
	char *root[] = {"rev-parse", "--show-toplevel", NULL};
	char *gitdir[] = {"rev-parse", "--absolute-git-dir", NULL};
	char *common[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", NULL};
	char *index[] = {"rev-parse", "--path-format=absolute", "--git-path", "index", NULL};
	repo->root = git_line(NULL, root);
	if (!repo->root) {
		return -1;
	}
	if (!tool_path_allowed(repo->root)) {
		errno = EPERM;
		return -1;
	}
	repo->gitdir = git_line(repo->root, gitdir);
	repo->common = git_line(repo->root, common);
	repo->index = git_line(repo->root, index);
	if (!repo->gitdir || !repo->common || !repo->index) {
		return -1;
	}
	if (!tool_path_allowed(repo->gitdir) || !tool_path_allowed(repo->common) ||
		!tool_path_allowed(repo->index)) {
		errno = EPERM;
		return -1;
	}

	return 0;
}

static int repository_lock(struct repository *repo) {

	char *output = NULL;
	int code = -1;
	char *sparse[] = {"config", "--bool", "--get", "core.sparseCheckout", NULL};
	if (tool_git_run(repo->root, sparse, NULL, NULL, 0, 15, &output, NULL, &code)) {
		return -1;
	}
	int unsupported = !code && !strcmp(output, "true\n");
	free(output);
	if (unsupported) {
		errno = ENOTSUP;
		return -1;
	}
	if (code != 0 && code != 1) {
		errno = EIO;
		return -1;
	}

	char *path = NULL;
	if (asprintf(&path, "%s/phi-operation.lock", repo->gitdir) < 0) {
		return -1;
	}
	repo->lock = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	free(path);
	if (repo->lock < 0) {
		return -1;
	}
	struct stat status;
	if (fstat(repo->lock, &status) || !S_ISREG(status.st_mode)) {
		errno = EINVAL;
		return -1;
	}

	return flock(repo->lock, LOCK_EX | LOCK_NB);
}

static int valid_checkpoint(const char *id) {

	if (!id) {
		return 0;
	}
	size_t size = strlen(id);
	if (size != 40 && size != 64) {
		return 0;
	}
	for (size_t i = 0; i < size; i++) {
		if (!isxdigit((unsigned char)id[i])) {
			return 0;
		}
	}

	return 1;
}

/* Seed from the real index, not HEAD: this retains tracked, force-added ignored
   files. The private directory protects both the temporary index and its lock. */
static int copy_index(const char *source, const char *destination) {

	int input = tool_open_read(source);
	if (input < 0) {
		return errno == ENOENT ? 0 : -1;
	}
	struct stat status;
	if (fstat(input, &status) || !S_ISREG(status.st_mode) || status.st_size > 64 * 1024 * 1024) {
		close(input);
		errno = EINVAL;
		return -1;
	}
	int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (output < 0) {
		close(input);
		return -1;
	}
	int result = 1;
	char buffer[65536];
	for (;;) {
		ssize_t size = read(input, buffer, sizeof(buffer));
		if (size < 0 && errno == EINTR) {
			continue;
		}
		if (!size) {
			break;
		}
		if (size < 0 || tool_write_all(output, buffer, (size_t)size)) {
			result = -1;
			break;
		}
	}
	int saved = errno;
	if (close(output) && result > 0) {
		result = -1;
		saved = errno;
	}
	close(input);
	errno = saved;

	return result;
}

static int checkpoint_create(const struct repository *repo, char **checkpoint, char **detail) {

	*checkpoint = NULL;
	*detail = NULL;
	char *directory = NULL, *index = NULL, *output = NULL;
	char *head = NULL, *tree = NULL, *commit = NULL, *ref = NULL;
	int result = -1, created_directory = 0;
	if (asprintf(&directory, "%s/phi-checkpoint-XXXXXX", repo->gitdir) < 0) {
		goto done;
	}
	if (!mkdtemp(directory)) {
		goto done;
	}
	created_directory = 1;
	if (asprintf(&index, "%s/index", directory) < 0) {
		goto done;
	}
	int copied = copy_index(repo->index, index);
	if (copied < 0) {
		goto done;
	}
	const struct tool_env env[] = {{"GIT_INDEX_FILE", index}, {"GIT_AUTHOR_NAME", "Phi"},
		{"GIT_AUTHOR_EMAIL", "phi@local"}, {"GIT_COMMITTER_NAME", "Phi"},
		{"GIT_COMMITTER_EMAIL", "phi@local"}, {NULL, NULL}};
	if (!copied) {
		char *empty[] = {"read-tree", "--empty", NULL};
		if (run_git(repo->root, empty, env, NULL, &output)) {
			goto done;
		}
		free(output);
		output = NULL;
	}
	if (copied) {
		size_t listed_size = 0;
		int code = -1;
		char *files[] = {"ls-files", "-z", NULL};
		if (tool_git_run(repo->root, files, env, NULL, 0, 30, &output, &listed_size, &code) ||
			code) {
			goto done;
		}
		if (listed_size) {
			char *listed = output;
			output = NULL;
			char *clear_flags[] = {"update-index", "--no-assume-unchanged", "-z", "--stdin", NULL};
			int failed = tool_git_run(
				repo->root, clear_flags, env, listed, listed_size, 30, &output, NULL, &code);
			free(listed);
			if (failed || code) {
				goto done;
			}
		}
		free(output);
		output = NULL;
	}
	char *add[] = {"add", "-A", "--", ".", NULL};
	if (run_git(repo->root, add, env, NULL, &output)) {
		goto done;
	}
	free(output);
	output = NULL;
	char *write_tree[] = {"write-tree", NULL};
	if (run_git(repo->root, write_tree, env, NULL, &output)) {
		goto done;
	}
	trim_terminator(output);
	if (!valid_checkpoint(output)) {
		errno = EIO;
		goto done;
	}
	tree = output;
	output = NULL;

	int code = -1;
	char *head_args[] = {"rev-parse", "--verify", "--quiet", "HEAD", NULL};
	if (tool_git_run(repo->root, head_args, NULL, NULL, 0, 30, &output, NULL, &code)) {
		goto done;
	}
	if (!code) {
		trim_terminator(output);
		if (!valid_checkpoint(output)) {
			errno = EIO;
			goto done;
		}
		head = output;
		output = NULL;
	} else {
		if (code != 1) {
			errno = EIO;
			goto done;
		}
		free(output);
		output = NULL;
	}
	char *parented[] = {"commit-tree", tree, "-p", head, NULL};
	char *unborn[] = {"commit-tree", tree, NULL};
	if (run_git(repo->root, head ? parented : unborn, env, "Phi checkpoint\n", &output)) {
		goto done;
	}
	trim_terminator(output);
	if (!valid_checkpoint(output)) {
		errno = EIO;
		goto done;
	}
	commit = output;
	output = NULL;
	if (asprintf(&ref, "refs/phi/checkpoints/%s", commit) < 0) {
		goto done;
	}
	char *update[] = {"update-ref", ref, commit, NULL};
	if (run_git(repo->root, update, NULL, NULL, &output)) {
		goto done;
	}
	*checkpoint = commit;
	commit = NULL;
	result = 0;
done: {
	int saved = errno;
	if (result) {
		*detail = strdup(output && *output ? output : tool_process_error());
	}
	if (index) {
		unlink(index);
		char *lock = NULL;
		if (asprintf(&lock, "%s.lock", index) >= 0) {
			unlink(lock);
			free(lock);
		}
	}
	if (created_directory) {
		rmdir(directory);
	}
	free(directory);
	free(index);
	free(output);
	free(head);
	free(tree);
	free(commit);
	free(ref);
	errno = saved;
	return result;
}
}

static json_object *status_action(const struct repository *repo) {

	char *status = NULL, *branch = NULL;
	char *status_args[] = {"status", "--short", "--branch", "--ignore-submodules=all", NULL};
	char *branch_args[] = {"branch", "--show-current", NULL};
	if (run_git(repo->root, status_args, NULL, NULL, &status) ||
		!(branch = git_line(repo->root, branch_args))) {
		json_object *answer = tool_error(status && *status ? status : tool_process_error());
		free(status);
		free(branch);
		return answer;
	}
	json_object *answer = json_object_new_object();
	json_object_object_add(answer, "branch", json_object_new_string(branch));
	json_object_object_add(answer, "output", json_object_new_string(status));
	free(status);
	free(branch);

	return answer;
}

static json_object *diff_action(const struct repository *repo, int staged) {

	char *output = NULL;
	char *plain[] = {
		"diff", "--no-ext-diff", "--no-textconv", "--ignore-submodules=all", "--", ".", NULL};
	char *cached[] = {"diff", "--cached", "--no-ext-diff", "--no-textconv",
		"--ignore-submodules=all", "--", ".", NULL};
	int failed = run_git(repo->root, staged ? cached : plain, NULL, NULL, &output);
	int too_large = failed && errno == EOVERFLOW;
	if (too_large) {
		/* A full diff over the capture limit is useless as an error. Return
           the per-file summary instead, so the model can narrow its request. */
		char *plain_stat[] = {"diff", "--stat", "--no-ext-diff", "--no-textconv",
			"--ignore-submodules=all", "--", ".", NULL};
		char *cached_stat[] = {"diff", "--cached", "--stat", "--no-ext-diff", "--no-textconv",
			"--ignore-submodules=all", "--", ".", NULL};
		free(output);
		output = NULL;
		failed = run_git(repo->root, staged ? cached_stat : plain_stat, NULL, NULL, &output);
	}
	json_object *answer = failed ? tool_error(output && *output ? output : tool_process_error())
								 : json_object_new_object();
	if (!failed) {
		json_object_object_add(answer, "output", json_object_new_string(output));
		json_object_object_add(answer, "staged", json_object_new_boolean(staged));
		if (too_large) {
			json_object_object_add(answer, "truncated", json_object_new_boolean(1));
			json_object_object_add(answer, "note",
				json_object_new_string(
					"Full diff exceeds 256 KiB; this is the per-file summary. Use bash with "
					"`git diff -- PATH` (optionally piped to head) to view specific files."));
		}
	}
	free(output);

	return answer;
}

static json_object *checkpoint_action(const struct repository *repo) {

	char *id = NULL, *detail = NULL;
	if (checkpoint_create(repo, &id, &detail)) {
		json_object *answer = tool_error(detail ? detail : "Could not create Git checkpoint.");
		free(detail);
		return answer;
	}
	json_object *answer = json_object_new_object();
	json_object_object_add(answer, "checkpoint", json_object_new_string(id));
	json_object_object_add(answer, "note",
		json_object_new_string(
			"Git content snapshot; HEAD and the real index are unchanged. Ignored untracked files and submodule contents are excluded."));
	free(id);

	return answer;
}

static int paths_overlap(const char *a, const char *b) {

	size_t x = strlen(a), y = strlen(b);
	while (x && a[x - 1] == '/') {
		x--;
	}
	while (y && b[y - 1] == '/') {
		y--;
	}
	size_t common = x < y ? x : y;
	if (strncmp(a, b, common)) {
		return 0;
	}

	return x == y || (x < y ? b[x] == '/' : a[y] == '/');
}

static int compare_paths(const void *a, const void *b) {

	return strcmp(*(char *const *)a, *(char *const *)b);
}

static char **sorted_paths(char *data, size_t size, size_t *count) {

	*count = 0;
	if (size && data[size - 1] != '\0') {
		errno = EIO;
		return NULL;
	}
	for (size_t i = 0; i < size; i++) {
		if (!data[i]) {
			(*count)++;
		}
	}
	char **paths = calloc(*count + 1, sizeof(*paths));
	if (!paths) {
		return NULL;
	}
	size_t used = 0;
	for (size_t at = 0; at < size;) {
		size_t length = strlen(data + at);
		paths[used++] = data + at;
		at += length + 1;
		while (length && paths[used - 1][length - 1] == '/') {
			paths[used - 1][--length] = '\0';
		}
	}
	qsort(paths, *count, sizeof(*paths), compare_paths);

	return paths;
}

/* A sorted merge checks ignored/target collisions without a quadratic walk.
   Refuse collisions BEFORE creating an undo snapshot or deleting files. */
static int ignored_conflict(
	const struct repository *repo, const char *commit, char **detail, int *target_empty) {

	char *ignored = NULL, *target = NULL;
	char **ignored_paths = NULL, **target_paths = NULL;
	size_t ignored_size = 0, target_size = 0, ignored_count = 0, target_count = 0;
	int code, result = -1;
	char *ignored_args[] = {
		"ls-files", "--others", "--ignored", "--exclude-standard", "--directory", "-z", NULL};
	char *target_args[] = {"ls-tree", "-r", "-z", "--name-only", (char *)commit, NULL};
	if (tool_git_run(repo->root, ignored_args, NULL, NULL, 0, 30, &ignored, &ignored_size, &code) ||
		code) {
		goto done;
	}
	if (tool_git_run(repo->root, target_args, NULL, NULL, 0, 30, &target, &target_size, &code) ||
		code) {
		goto done;
	}
	*target_empty = target_size == 0;
	ignored_paths = sorted_paths(ignored, ignored_size, &ignored_count);
	target_paths = sorted_paths(target, target_size, &target_count);
	if (!ignored_paths || !target_paths) {
		goto done;
	}
	for (size_t i = 0, j = 0; i < ignored_count && j < target_count;) {
		if (paths_overlap(ignored_paths[i], target_paths[j])) {
			if (asprintf(detail, "Restore would overwrite ignored path '%s'; move it aside first.",
					ignored_paths[i]) < 0) {
				*detail = NULL;
			}
			result = 1;
			goto done;
		}
		if (strcmp(ignored_paths[i], target_paths[j]) < 0) {
			i++;
		} else {
			j++;
		}
	}
	result = 0;
done:
	if (result < 0) {
		*detail = strdup("Could not verify ignored-file safety; restore was not started.");
	}
	free(ignored_paths);
	free(target_paths);
	free(ignored);
	free(target);

	return result;
}

static json_object *restore_action(const struct repository *repo, const char *id) {

	char *ref = NULL, *commit = NULL, *undo = NULL, *detail = NULL, *output = NULL, *source = NULL;
	json_object *answer = NULL;
	if (!id || !valid_checkpoint(id)) {
		return tool_error("restore requires a full checkpoint id returned by Phi.");
	}
	if (asprintf(&ref, "refs/phi/checkpoints/%s^{commit}", id) < 0) {
		goto failed;
	}
	char *verify[] = {"rev-parse", "--verify", ref, NULL};
	commit = git_line(repo->root, verify);
	if (!commit || !valid_checkpoint(commit) || strcasecmp(commit, id)) {
		detail = strdup("Checkpoint was not found in this repository.");
		goto failed;
	}
	int target_empty = 0, index_empty = 0;
	if (ignored_conflict(repo, commit, &detail, &target_empty)) {
		goto failed;
	}
	if (target_empty) {
		char *files[] = {"ls-files", "-z", NULL};
		if (run_git(repo->root, files, NULL, NULL, &output)) {
			goto failed;
		}
		index_empty = !*output;
		free(output);
		output = NULL;
	}
	if (checkpoint_create(repo, &undo, &detail)) {
		goto failed;
	}
	if (asprintf(&source, "--source=%s", commit) < 0) {
		goto failed;
	}

	/* Clean while the CURRENT ignore rules still apply. Cleaning after restoring
       an older .gitignore could delete files that were ignored before restore. */
	char *clean[] = {"clean", "-fd", "--", ".", NULL};
	if (run_git(repo->root, clean, NULL, NULL, &output)) {
		goto failed;
	}
	free(output);
	output = NULL;
	char *restore[] = {
		"restore", source, "--staged", "--worktree", "--no-recurse-submodules", "--", ".", NULL};
	/* An empty index and empty snapshot have no pathspec for git restore.
       The cleanup above was sufficient; do not report a false failure. */
	if ((!target_empty || !index_empty) && run_git(repo->root, restore, NULL, NULL, &output)) {
		goto failed;
	}
	answer = json_object_new_object();
	json_object_object_add(answer, "restored", json_object_new_string(id));
	json_object_object_add(answer, "note",
		json_object_new_string(
			"Index and working tree restored. Ignored untracked files and submodule contents were left alone."));
	goto done;
failed:
	answer = tool_error(detail ? detail : output && *output ? output : tool_process_error());
done:
	/* Keep the recovery handle even when cancellation or an I/O error occurs
       after the destructive phase has started. */
	if (undo) {
		json_object_object_add(answer, "undo_checkpoint", json_object_new_string(undo));
	}
	free(ref);
	free(commit);
	free(undo);
	free(detail);
	free(output);
	free(source);

	return answer;
}

static json_object *run(json_object *args) {

	const char *action = tool_plain_string(json_object_object_get(args, "action"), NULL);
	if (!action ||
		(strcmp(action, "status") && strcmp(action, "diff") && strcmp(action, "checkpoint") &&
			strcmp(action, "restore"))) {
		return tool_error("Expected Git action: status, diff, checkpoint or restore.");
	}
	json_object *checkpoint_value = json_object_object_get(args, "checkpoint");
	const char *checkpoint = checkpoint_value ? tool_plain_string(checkpoint_value, NULL) : NULL;
	if (checkpoint_value && !checkpoint) {
		return tool_error("checkpoint must be a string or null.");
	}
	if (!strcmp(action, "restore") && !valid_checkpoint(checkpoint)) {
		return tool_error("restore requires a full checkpoint id returned by Phi.");
	}
	json_object *staged_value = json_object_object_get(args, "staged");
	if (staged_value && !json_object_is_type(staged_value, json_type_boolean)) {
		return tool_error("staged must be a boolean or null.");
	}
	int staged = json_object_get_boolean(staged_value);

	struct repository repo;
	json_object *answer;
	if (repository_open(&repo)) {
		answer = tool_error(errno == EPERM
				? "Sandbox requires repository root, index and Git metadata inside the workspace."
				: "Cannot open repository. Check Git/bubblewrap availability and run Phi from the repository root in sandbox mode.");
	} else if ((!strcmp(action, "checkpoint") || !strcmp(action, "restore")) &&
		repository_lock(&repo)) {
		answer = tool_error(errno == ENOTSUP
				? "Checkpoint/restore is not supported in a sparse checkout; inspection remains available."
				: "Cannot acquire the Phi Git lock; check repository permissions or another running mutation.");
	} else if (!strcmp(action, "status")) {
		answer = status_action(&repo);
	} else if (!strcmp(action, "diff")) {
		answer = diff_action(&repo, staged);
	} else if (!strcmp(action, "checkpoint")) {
		answer = checkpoint_action(&repo);
	} else {
		answer = restore_action(&repo, checkpoint);
	}
	repository_close(&repo);

	return answer;
}

const struct phi_tool phi_tool_git = {.name = "git",
	.schema =
		"{\"type\":\"function\",\"name\":\"git\","
		"\"description\":\"Inspect status/diff, create a hidden Git content checkpoint without changing HEAD or the real index, or restore a Phi checkpoint. Snapshots include tracked and ordinary untracked files, not ignored untracked files or submodule contents. Git content filters apply. Restore changes index/worktree and returns an undo checkpoint, including on a partial failure.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"action\":{\"type\":\"string\",\"enum\":[\"status\",\"diff\",\"checkpoint\",\"restore\"]},"
		"\"checkpoint\":{\"type\":[\"string\",\"null\"]},"
		"\"staged\":{\"type\":[\"boolean\",\"null\"]}},"
		"\"required\":[\"action\",\"checkpoint\",\"staged\"],"
		"\"additionalProperties\":false},\"strict\":true}",
	.detail_key = "action",
	.run = run};
