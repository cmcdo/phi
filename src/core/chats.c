#define _GNU_SOURCE
#include "chats.h"
#include "bridge.h"
#include "chat_start.h"
#include "host_attach.h"
#include "agent.h"
#include "events.h"
#include "fsutil.h"
#include "history.h"
#include "json.h"
#include "sessions.h"
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <time.h>

/* ---- places ---------------------------------------------------------------- */

static int status_directory(char *path, size_t size) {

	return phi_xdg_path(path, size, "XDG_STATE_HOME", ".local/state", "phi/chats");
}

static int socket_directory(char *path, size_t size) {

	const char *runtime = getenv("XDG_RUNTIME_DIR");
	int n = runtime && *runtime == '/' ? snprintf(path, size, "%s/phi", runtime)
									   : snprintf(path, size, "/tmp/phi-%u", (unsigned)getuid());
	if (n < 0 || (size_t)n >= size || !phi_make_directories(path)) {
		return 0;
	}
	struct stat st;

	return !lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() && !chmod(path, 0700);
}

/* phi-host next to this executable, else on PATH. */
static int host_program(char *path, size_t size) {

	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n > 0) {
		self[n] = 0;
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = 0;
			snprintf(path, size, "%.4000s/phi-host", self);
			if (!access(path, X_OK)) {
				return 1;
			}
		}
	}
	const char *search = getenv("PATH");
	for (char *copy = search ? strdup(search) : NULL, *dir = copy ? strtok(copy, ":") : NULL,
			  *keep = copy;
		; dir = strtok(NULL, ":")) {
		if (!dir) {
			free(keep);
			return 0;
		}
		snprintf(path, size, "%.4000s/phi-host", dir);
		if (!access(path, X_OK)) {
			free(keep);
			return 1;
		}
	}
}

/* ---- status file ----------------------------------------------------------- */

static struct agent *agent;
static int preserve_closed_status;

void chats_preserve_closed_status(void) {

	preserve_closed_status = 1;
}

static char chat_id[32], title[128], state[16] = "idle", detail[256], session_file[4096];
static time_t started;
static int startup_lock = -1;
static char bridge_path[4096];
static int bridge_queued, turn_running, auxiliary_running, runtime_running;

static const char *work_state(void) {

	return runtime_running || turn_running || auxiliary_running || bridge_queued ? "working"
																				 : "idle";
}

static char bridge_delivery_file[4096];
static json_object *pending_question;

void chats_prepare(void) {

	startup_lock = chat_start_receive();
}

int chats_hosted(void) {

	return getenv("PHI_CHAT_ID") && *getenv("PHI_CHAT_ID");
}

static int has_messages(void) {

	if (!agent || !agent->history) {
		return 0;
	}
	if (*bridge_delivery_file && !strcmp(bridge_delivery_file, agent->transcript.path)) {
		return 1;
	}
	for (size_t i = 0; i < json_object_array_length(agent->history); i++) {
		json_object *item = json_object_array_get_idx(agent->history, i);
		if (!history_is_hidden(item) &&
			(!strcmp(field(item, "role"), "user") || !strcmp(field(item, "role"), "assistant"))) {
			return 1;
		}
	}

	return 0;
}

/* An unused chat needs a discoverable live status for attach/send, not a
   durable record. Its status lives beside its socket until the first turn. */
static void remove_status(int durable) {

	char directory[4096], path[4200];
	int ok = durable ? status_directory(directory, sizeof(directory))
					 : socket_directory(directory, sizeof(directory));
	if (ok) {
		snprintf(path, sizeof(path), "%s/%s.json", directory, chat_id);
		unlink(path);
	}
}

/* PID identity and zombie detection: kill(pid, 0) alone treats zombies and
   reused PIDs as live. Linux start ticks identify this process instance. */
static uint64_t process_start(pid_t pid, int *zombie) {

	char path[64], buffer[4096];
	snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
	if (zombie) {
		*zombie = 0;
	}
	FILE *file = fopen(path, "re");
	if (!file) {
		return 0;
	}
	char *got = fgets(buffer, sizeof(buffer), file);
	fclose(file);
	char *end = got ? strrchr(buffer, ')') : NULL;
	if (!end || end[1] != ' ' || !end[2]) {
		return 0;
	}
	char *at = end + 2;
	if (zombie) {
		*zombie = *at == 'Z' || *at == 'X';
	}
	for (int field_number = 3; field_number < 22; field_number++) {
		at += strcspn(at, " ");
		while (*at == ' ') {
			at++;
		}
		if (!*at) {
			return 0;
		}
	}

	return strtoull(at, NULL, 10);
}

static int write_status(void) {

	char directory[4096], path[4200], temporary[4300];
	if (!*chat_id) {
		return 0;
	}
	int durable = has_messages() || preserve_closed_status;
	int ready = durable ? status_directory(directory, sizeof(directory))
						: socket_directory(directory, sizeof(directory));
	if (!ready || (durable && !phi_make_directories(directory))) {
		return 0;
	}
	json_object *status = json_object_new_object();
	char cwd[4096];
	json_object_object_add(status, "id", json_object_new_string(chat_id));
	json_object_object_add(status, "pid", json_object_new_int64(getpid()));
	json_object_object_add(
		status, "process_start", json_object_new_int64((int64_t)process_start(getpid(), NULL)));
	json_object_object_add(status, "socket",
		json_object_new_string(getenv("PHI_HOST_SOCKET") ? getenv("PHI_HOST_SOCKET") : ""));
	json_object_object_add(status, "session_file", json_object_new_string(session_file));
	json_object_object_add(status, "has_messages", json_object_new_boolean(has_messages()));
	if (preserve_closed_status) {
		json_object_object_add(status, "closed", json_object_new_boolean(1));
	}
	if (agent && *session_file) {
		json_object_object_add(
			status, "session_head", json_object_new_int64((int64_t)agent->transcript.head));
	}
	json_object_object_add(
		status, "cwd", json_object_new_string(getcwd(cwd, sizeof(cwd)) ? cwd : ""));
	json_object_object_add(status, "title", json_object_new_string(title));
	json_object_object_add(status, "state", json_object_new_string(state));
	json_object_object_add(status, "bridge_socket", json_object_new_string(bridge_path));
	json_object_object_add(status, "queued", json_object_new_int(bridge_queued));
	json_object_object_add(status, "model", json_object_new_string(agent ? agent->model : ""));
	json_object_object_add(status, "thinking",
		json_object_new_string(
			agent ? (agent->host.effort ? agent->host.effort() : agent->effort) : ""));
	if (pending_question) {
		json_object_object_add(status, "approval", json_object_get(pending_question));
	}
	json_object_object_add(status, "detail", json_object_new_string(detail));
	json_object_object_add(status, "started", json_object_new_int64(started));
	json_object_object_add(status, "updated", json_object_new_int64(time(NULL)));
	const char *text = json_object_to_json_string_ext(
		status, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
	/* Written often and cheap to lose: rename for atomicity, no fsync. */
	snprintf(path, sizeof(path), "%s/%s.json", directory, chat_id);
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	int published = 0;
	if (fd >= 0) {
		size_t length = strlen(text);
		int ok = write(fd, text, length) == (ssize_t)length;
		close(fd);
		if (ok && !rename(temporary, path)) {
			published = 1;
			remove_status(!durable); /* Drop the old location only after publishing. */
		} else {
			unlink(temporary);
		}
	}
	json_object_put(status);

	return published;
}

/* One line of text: the last non-empty line, trimmed. */
static void last_line(const char *text, char *out, size_t size) {

	const char *end = text + strlen(text);
	while (end > text && (end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\r')) {
		end--;
	}
	const char *start = end;
	while (start > text && start[-1] != '\n') {
		start--;
	}
	while (start < end && (*start == ' ' || *start == '#' || *start == '*' || *start == '-')) {
		start++;
	}
	size_t length = (size_t)(end - start);
	if (length >= size) {
		length = size - 1;
	}
	memcpy(out, start, length);
	out[length] = 0;
}

static void last_reply(void) {

	detail[0] = 0;
	for (size_t i = agent ? json_object_array_length(agent->history) : 0; i > 0; i--) {
		json_object *item = json_object_array_get_idx(agent->history, i - 1);
		if (strcmp(field(item, "role"), "assistant")) {
			continue;
		}
		char text[8192];
		session_item_text(item, text, sizeof(text));
		last_line(text, detail, sizeof(detail));
		return;
	}
}

static int status_wants(const char *event) {

	static const char *const wanted[] = {"agent_start", "agent_end", "ui_prompt_start",
		"ui_prompt_end", "tool_execution_start", "session_info_changed", "session_start",
		"session_tree", "session_compact", "model_select", "thinking_level_select"};
	for (size_t i = 0; i < sizeof(wanted) / sizeof(*wanted); i++) {
		if (!strcmp(event, wanted[i])) {
			return 1;
		}
	}

	return 0;
}

static void observe(const char *event, json_object *payload) {

	/* The transcript file is created with the first message. */
	if (agent && *agent->transcript.path) {
		snprintf(session_file, sizeof(session_file), "%s", agent->transcript.path);
	}
	if (!strcmp(event, "agent_start")) {
		turn_running = 1;
		snprintf(state, sizeof(state), "working");
		const char *prompt = field(payload, "prompt");
		if (!*title && *prompt) {
			last_line(prompt, title, 61);
		}
		detail[0] = 0;
	} else if (!strcmp(event, "tool_execution_start")) {
		json_object *arguments = json_object_object_get(payload, "arguments");
		const char *argument = field(arguments, "command");
		if (!*argument) {
			argument = field(arguments, "path");
		}
		snprintf(detail, sizeof(detail), "%s%s%.200s", field(payload, "name"),
			*argument ? ": " : "", argument);
	} else if (!strcmp(event, "agent_end")) {
		turn_running = 0;
		snprintf(state, sizeof(state), "%s", work_state());
		last_reply();
	} else if (!strcmp(event, "ui_prompt_start")) {
		snprintf(state, sizeof(state), "needs_input");
		const char *kind = field(payload, "kind");
		if (!strcmp(kind, "tool_approval") || !strcmp(kind, "native_approval")) {
			snprintf(detail, sizeof(detail), "Approve %s?", field(payload, "name"));
		} else {
			snprintf(detail, sizeof(detail), "Input requested: %s", field(payload, "name"));
		}
		if (pending_question) {
			json_object_put(pending_question);
		}
		pending_question = json_object_get(payload);
	} else if (!strcmp(event, "ui_prompt_end")) {
		if (pending_question) {
			json_object_put(pending_question);
		}
		pending_question = NULL;
		snprintf(state, sizeof(state), "%s", work_state());
	} else if (!strcmp(event, "session_info_changed")) {
		snprintf(title, sizeof(title), "%s", field(payload, "name"));
	} else if (!strcmp(event, "session_start")) {
		if (agent) {
			snprintf(session_file, sizeof(session_file), "%s", agent->transcript.path);
		}
	} else if (strcmp(event, "session_tree") && strcmp(event, "session_compact") &&
		strcmp(event, "model_select") && strcmp(event, "thinking_level_select")) {
		return;
	}
	write_status();
}

void chats_begin(struct agent *active) {

	preserve_closed_status = 0;
	if (!chats_hosted()) {
		return;
	}
	agent = active;
	snprintf(chat_id, sizeof(chat_id), "%s", getenv("PHI_CHAT_ID"));
	started = time(NULL);
	const char *initial_title = getenv("PHI_BRIDGE_TITLE");
	if (initial_title) {
		snprintf(title, sizeof(title), "%s", initial_title);
	}
	snprintf(session_file, sizeof(session_file), "%s", agent->transcript.path);
	/* Keep the conversation's name when its host is restarted. */
	char directory[4096], path[4200];
	if (status_directory(directory, sizeof(directory))) {
		snprintf(path, sizeof(path), "%s/%s.json", directory, chat_id);
		json_object *old = json_object_from_file(path);
		if (old && !strcmp(field(old, "session_file"), session_file)) {
			snprintf(title, sizeof(title), "%s", field(old, "title"));
		}
		if (old) {
			json_object_put(old);
		}
	}
	struct session_entry entry;
	char error[256];
	if (*session_file && session_from_path(session_file, &entry, error, sizeof(error)) &&
		entry.named) {
		snprintf(title, sizeof(title), "%.127s", entry.title);
	}
	last_reply();
	/* Resuming an ended chat restores history before chats_begin. Give its
       status a real title instead of making it look like a blank new chat. */
	for (size_t i = 0; agent->history && i < json_object_array_length(agent->history) && !*title;
		i++) {
		json_object *item = json_object_array_get_idx(agent->history, i);
		if (strcmp(field(item, "role"), "user") || history_is_hidden(item)) {
			continue;
		}
		char text[8192];
		session_item_text(item, text, sizeof(text));
		last_line(text, title, 61);
	}
	events_set_status_observer(observe, status_wants);
	/* phi-host forks Phi before binding its socket. Do not announce a ready
       host (or release the startup lock) until that socket exists too. */
	const char *socket = getenv("PHI_HOST_SOCKET");
	for (int attempt = 0; startup_lock >= 0 && socket && access(socket, F_OK) && attempt < 200;
		attempt++) {
		struct timespec pause = {.tv_nsec = 25000000};
		nanosleep(&pause, NULL);
	}
	if (write_status() && (startup_lock < 0 || (socket && !access(socket, F_OK)))) {
		chat_start_release(startup_lock);
		startup_lock = -1;
	} else {
		/* Keep the startup lock rather than permitting a duplicate host. */
		fprintf(
			stderr, "Could not publish chat status; this conversation cannot be attached yet.\n");
	}
}

void chats_end(void) {

	if (!*chat_id) {
		return;
	}
	events_set_status_observer(NULL, NULL);
	snprintf(state, sizeof(state), "ended");
	if (agent) {
		snprintf(session_file, sizeof(session_file), "%s", agent->transcript.path);
	}
	if (preserve_closed_status || (has_messages() && *session_file)) {
		write_status();
	} else {
		remove_status(0);
		remove_status(1);
	}
	agent = NULL;
	if (pending_question) {
		json_object_put(pending_question);
	}
	pending_question = NULL;
}

int chats_record_delivery(json_object *message) {

	if (!agent || transcript_disabled()) {
		return 0;
	}
	transcript_begin(&agent->transcript, agent->model,
		agent->host.effort ? agent->host.effort() : agent->effort);
	if (!transcript_entry(&agent->transcript, "bridge_delivery", message) ||
		!agent->transcript.file || fsync(fileno(agent->transcript.file))) {
		return 0;
	}
	snprintf(bridge_delivery_file, sizeof(bridge_delivery_file), "%s", agent->transcript.path);
	snprintf(session_file, sizeof(session_file), "%s", agent->transcript.path);

	return 1;
}

void chats_run(int active) {

	runtime_running = !!active;
	if (!strcmp(state, "ended") || !strcmp(state, "needs_input") || !strcmp(state, work_state())) {
		return;
	}
	snprintf(state, sizeof(state), "%s", work_state());
	write_status();
}

void chats_activity(int active) {

	active = !!active;
	if (auxiliary_running == active) {
		return;
	}
	auxiliary_running = active;
	/* Approval/dialog state takes priority; detaching never changes activity. */
	if (!strcmp(state, "ended") || !strcmp(state, "needs_input") || !strcmp(state, work_state())) {
		return;
	}
	snprintf(state, sizeof(state), "%s", work_state());
	if (!active && !turn_running) {
		last_reply();
	}
	write_status();
}

void chats_bridge_status(const char *socket, int queued) {

	snprintf(bridge_path, sizeof(bridge_path), "%s", socket ? socket : "");
	bridge_queued = queued;
	if (queued && !strcmp(state, "idle")) {
		snprintf(state, sizeof(state), "working");
	}
	write_status();
}

/* ---- listing --------------------------------------------------------------- */

struct chat {
	json_object *status;
	int live;
};

static int alive(json_object *status) {

	pid_t pid = (pid_t)json_object_get_int64(json_object_object_get(status, "pid"));
	const char *socket_path = field(status, "socket");
	int zombie = 0;
	uint64_t started_at = pid > 0 ? process_start(pid, &zombie) : 0;
	json_object *expected = json_object_object_get(status, "process_start");
	if (zombie ||
		(expected && (!started_at || started_at != (uint64_t)json_object_get_int64(expected)))) {
		return 0;
	}

	return pid > 0 && !kill(pid, 0) && *socket_path && !access(socket_path, F_OK) &&
		strcmp(field(status, "state"), "ended");
}

static void load_chats_from(
	const char *directory, int transient, struct chat **out, size_t *count, size_t *capacity) {

	DIR *dir = opendir(directory);
	if (!dir) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		size_t length = strlen(entry->d_name);
		if (length < 6 || strcmp(entry->d_name + length - 5, ".json")) {
			continue;
		}
		char path[4400];
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		char *text = phi_read_file(path, 64 * 1024 * 1024, PHI_READ_OWNED, NULL);
		json_object *status = text ? json_tokener_parse(text) : NULL;
		free(text);
		if (!json_object_is_type(status, json_type_object)) {
			if (status) {
				json_object_put(status);
			}
			continue;
		}
		int live = alive(status);
		const char *file = field(status, "session_file");
		/* Live empty chats have only a transient record. Stale transient
           records and ended chats without a session cannot be reopened. */
		int closed_empty = json_object_get_boolean(json_object_object_get(status, "closed")) &&
			!strcmp(field(status, "state"), "ended") && !*file;
		if ((!live && (transient || (!closed_empty && (!*file || access(file, R_OK)))))) {
			unlink(path);
			json_object_put(status);
			continue;
		}
		int duplicate = 0;
		for (size_t i = 0; i < *count; i++) {
			if (!strcmp(field((*out)[i].status, "id"), field(status, "id"))) {
				duplicate = 1;
				break;
			}
		}
		if (duplicate) {
			json_object_put(status);
			continue;
		}
		if (*count == *capacity) {
			size_t next = *capacity ? *capacity * 2 : 16;
			struct chat *grown = realloc(*out, next * sizeof(**out));
			if (!grown) {
				json_object_put(status);
				break;
			}
			*out = grown;
			*capacity = next;
		}
		(*out)[*count].status = status;
		(*out)[*count].live = live;
		(*count)++;
	}
	closedir(dir);
}

static size_t load_chats(struct chat **out) {

	char directory[4096];
	size_t count = 0, capacity = 0;
	*out = NULL;
	if (status_directory(directory, sizeof(directory))) {
		load_chats_from(directory, 0, out, &count, &capacity);
	}
	if (socket_directory(directory, sizeof(directory))) {
		load_chats_from(directory, 1, out, &count, &capacity);
	}

	return count;
}

static void free_chats(struct chat *chats, size_t count) {

	for (size_t i = 0; i < count; i++) {
		json_object_put(chats[i].status);
	}
	free(chats);
}

static void mark_seen(const char *id) {

	char directory[4096], path[4200], temporary[4300];
	if (!*id || !status_directory(directory, sizeof(directory)) ||
		!phi_make_directories(directory)) {
		return;
	}
	snprintf(path, sizeof(path), "%.4000s/%s.seen", directory, id);
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	FILE *file = fopen(temporary, "w");
	if (!file) {
		return;
	}
	fprintf(file, "%lld\n", (long long)time(NULL));
	if (fclose(file) || rename(temporary, path)) {
		unlink(temporary);
	}
}

/* Live groups describe actual activity, not unread output. Only permission
   prompts get an approval group; other questions simply wait for the user. */
static const char *group_of(const struct chat *chat) {

	const char *now = field(chat->status, "state");
	if (!chat->live) {
		return "closed"; /* saved history; the board can reopen it */
	}
	if (!strcmp(now, "working")) {
		return "working";
	}
	const char *kind = field(json_object_object_get(chat->status, "approval"), "kind");
	if (!strcmp(now, "needs_input") &&
		(!strcmp(kind, "tool_approval") || !strcmp(kind, "native_approval"))) {
		return "approval";
	}

	return "waiting";
}

static int64_t updated(const struct chat *chat) {

	return json_object_get_int64(json_object_object_get(chat->status, "updated"));
}

static int newest_first(const void *a, const void *b) {

	int64_t x = updated(a), y = updated(b);

	return x < y ? 1 : x > y ? -1 : 0;
}

static void age(int64_t when, char *out, size_t size) {

	int64_t seconds = time(NULL) - when;
	if (seconds < 60) {
		snprintf(out, size, "%llds", (long long)(seconds < 0 ? 0 : seconds));
	} else if (seconds < 3600) {
		snprintf(out, size, "%lldm", (long long)(seconds / 60));
	} else if (seconds < 86400) {
		snprintf(out, size, "%lldh", (long long)(seconds / 3600));
	} else {
		snprintf(out, size, "%lldd", (long long)(seconds / 86400 % 100000));
	}
}

static int list(void) {

	struct chat *chats;
	size_t count = load_chats(&chats);
	if (count > 1) {
		qsort(chats, count, sizeof(*chats), newest_first);
	}
	static const char *groups[][2] = {{"waiting", "Waiting"}, {"approval", "Requires approval"},
		{"working", "Working"}, {"closed", "Closed (saved; phi -c or /resume reopens)"}};
	int any = 0;
	for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
		int heading = 0;
		for (size_t i = 0; i < count; i++) {
			if (strcmp(group_of(&chats[i]), groups[g][0])) {
				continue;
			}
			if (!heading) {
				printf("%s%s\n", any ? "\n" : "", groups[g][1]);
				heading = any = 1;
			}
			char when[32];
			age(updated(&chats[i]), when, sizeof(when));
			const char *name = field(chats[i].status, "title");
			printf("  %-8s  %-32.32s  %-50.50s %4s\n", field(chats[i].status, "id"),
				*name ? name : "new chat", field(chats[i].status, "detail"), when);
		}
	}
	if (!any) {
		printf("No chats. Start one with phi, or phi --bg \"task\".\n");
	}
	free_chats(chats, count);

	return 0;
}

/* A chat by id prefix; with no query, the most recently active live one. */
static json_object *find_chat(const char *query, int *live, char *error, size_t size) {

	struct chat *chats;
	size_t count = load_chats(&chats), matches = 0;
	if (count > 1) {
		qsort(chats, count, sizeof(*chats), newest_first);
	}
	json_object *found = NULL;
	for (size_t i = 0; i < count; i++) {
		int match =
			query ? !strncmp(field(chats[i].status, "id"), query, strlen(query)) : chats[i].live;
		if (!match) {
			continue;
		}
		if (!found) {
			found = json_object_get(chats[i].status);
			*live = chats[i].live;
		}
		matches++;
		if (!query) {
			break;
		}
	}
	free_chats(chats, count);
	if (!found) {
		snprintf(
			error, size, query ? "No chat %s (phi ls lists them)." : "No running chats.", query);
	} else if (matches > 1) {
		snprintf(error, size, "%s matches %zu chats; give more of the id.", query, matches);
		json_object_put(found);
		found = NULL;
	}

	return found;
}

static void snapshot_fields(json_object *item, int live) {

	const char *strings[] = {"id", "title", "state", "detail", "cwd", "model", "thinking",
		"session_file", "bridge_socket"};
	for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); i++) {
		if (!json_object_object_get(item, strings[i])) {
			json_object_object_add(item, strings[i], json_object_new_string(""));
		}
	}
	json_object_object_add(item, "live", json_object_new_boolean(live));
	if (!live) {
		json_object_object_add(item, "state", json_object_new_string("ended"));
	}
}

json_object *chats_snapshot(void) {

	struct chat *items;
	size_t count = load_chats(&items);
	json_object *out = json_object_new_array();
	for (size_t i = 0; i < count; i++) {
		json_object *item = json_object_get(items[i].status);
		snapshot_fields(item, items[i].live);
		json_object_array_add(out, item);
	}
	free_chats(items, count);

	return out;
}

json_object *chats_lookup(const char *id, char *error, size_t size) {

	int live = 0;
	if (!id || !*id) {
		snprintf(error, size, "A chat ID is required.");
		return NULL;
	}
	json_object *item = find_chat(id, &live, error, size);
	if (item) {
		snapshot_fields(item, live);
	}

	return item;
}

/* ---- hosting --------------------------------------------------------------- */

static void new_id(char *out, size_t size) {

	unsigned char bytes[4] = {0};
	if (getrandom(bytes, sizeof(bytes), 0) != (ssize_t)sizeof(bytes)) {
		unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
		for (int i = 0; i < 4; i++) {
			bytes[i] = (unsigned char)(rand_r(&seed) >> 8);
		}
	}
	snprintf(out, size, "%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3]);
}

static int run_program_locked(char *const argv[], int lock) {

	pid_t pid = fork();
	if (pid < 0) {
		return 1;
	}
	if (!pid) {
		signal(SIGWINCH, SIG_DFL);
		if (!chat_start_inherit(lock)) {
			_exit(127);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return 1;
		}
	}

	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* A conversation ID is stable; the socket and PID belong to this host only. */
struct chat_target {
	char id[32], socket[4200];
	int existing;
};

/* Start Phi (with argv's options) in a new host. */
static int start_chat(int argc, char **argv, const char *reuse_id, const char *file, int lock,
	char *id, size_t id_size, char *socket_path, size_t socket_size) {

	char host[4096], self[PATH_MAX], directory[4096];
	if (!host_program(host, sizeof(host)) || !socket_directory(directory, sizeof(directory))) {
		return 0;
	}
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) {
		return 0;
	}
	self[n] = 0;
	if (reuse_id && (strlen(reuse_id) != 8 || strspn(reuse_id, "0123456789abcdef") != 8)) {
		return 0;
	}
	if (reuse_id) {
		snprintf(id, id_size, "%s", reuse_id);
	} else {
		new_id(id, id_size);
	}
	/* A retiring host may still unlink its socket after publishing 'ended'.
       Give the replacement host a new socket, but keep the conversation ID. */
	char socket_id[16];
	if (reuse_id) {
		new_id(socket_id, sizeof(socket_id));
	} else {
		snprintf(socket_id, sizeof(socket_id), "%s", id);
	}
	snprintf(socket_path, socket_size, "%.4000s/%s.sock", directory, socket_id);
	struct winsize size = {.ws_row = 40, .ws_col = 120};
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
	char rows[16], cols[16];
	snprintf(rows, sizeof(rows), "%u", size.ws_row ? size.ws_row : 40);
	snprintf(cols, sizeof(cols), "%u", size.ws_col ? size.ws_col : 120);
	char **host_argv = calloc((size_t)argc + 8, sizeof(*host_argv));
	if (!host_argv) {
		return 0;
	}
	size_t used = 0;
	host_argv[used++] = host;
	host_argv[used++] = "new";
	host_argv[used++] = socket_path;
	host_argv[used++] = rows;
	host_argv[used++] = cols;
	host_argv[used++] = "--";
	host_argv[used++] = self;
	for (int i = 1; i < argc; i++) {
		host_argv[used++] = argv[i];
	}
	const char *variables[] = {"PHI_CHAT_ID", "PHI_HOST_SOCKET", "PHI_CHAT_SESSION_FILE"};
	char *saved_env[3] = {0};
	for (int i = 0; i < 3; i++) {
		const char *value = getenv(variables[i]);
		if (value && !(saved_env[i] = strdup(value))) {
			for (int j = 0; j < i; j++) {
				free(saved_env[j]);
			}
			free(host_argv);
			return 0;
		}
	}
	setenv("PHI_CHAT_ID", id, 1);
	setenv("PHI_HOST_SOCKET", socket_path, 1);
	/* Empty also pins a decision: --continue found no saved conversation. */
	setenv("PHI_CHAT_SESSION_FILE", file ? file : "", 1);
	int status = run_program_locked(host_argv, lock);
	free(host_argv);
	for (int i = 0; i < 3; i++) {
		if (saved_env[i]) {
			setenv(variables[i], saved_env[i], 1);
			free(saved_env[i]);
		} else {
			unsetenv(variables[i]);
		}
	}

	return !status;
}

/* Show a chat in this terminal. Returns 3 when detached, else 0. */
/* Show a chat in this terminal, in this process (no extra attach program):
   3 detached, 4 detached as the chat closes, 0 ended, 1 error. */
static int attach_child(const char *socket_path, int first_foreground) {

	return first_foreground ? phi_host_attach(socket_path, NULL)
							: phi_host_attach_backgrounded(socket_path, NULL);
}

static int (*board_hook)(const char *current, char *chosen, size_t size);

void chats_set_board(int (*board)(const char *current, char *chosen, size_t size)) {

	board_hook = board;
}

/* The transcript identifies the conversation; the host is replaceable. */
static int session_chat(const char *file, int live_only, char *id, size_t id_size,
	char *socket_path, size_t socket_size) {

	struct chat *chats;
	size_t count = load_chats(&chats);
	ssize_t best = -1;
	for (size_t i = 0; i < count; i++) {
		json_object *status = chats[i].status;
		if (live_only && !chats[i].live) {
			continue;
		}
		const char *candidate = field(status, "session_file");
		struct stat a, b;
		if (strcmp(candidate, file) &&
			(stat(candidate, &a) || stat(file, &b) || a.st_dev != b.st_dev ||
				a.st_ino != b.st_ino)) {
			continue;
		}
		if (best < 0 || updated(&chats[i]) > updated(&chats[best])) {
			best = (ssize_t)i;
		}
	}
	if (best >= 0) {
		snprintf(id, id_size, "%s", field(chats[best].status, "id"));
		snprintf(socket_path, socket_size, "%s", field(chats[best].status, "socket"));
	}
	free_chats(chats, count);

	return best >= 0;
}

/* All hosted resume entry points go through this transaction. Resolve only
   after locking, then either attach or launch. The child releases the lock
   after publishing its identity; a second launcher cannot race that handoff. */
static int resume_chat(int argc, char **argv, const char *file, const char *preferred,
	struct chat_target *target, char *error, size_t size) {

	char directory[4096];
	if (!socket_directory(directory, sizeof(directory))) {
		snprintf(error, size, "Cannot open Phi's private runtime directory.");
		return 0;
	}
	int lock = chat_start_lock(directory, file, error, size);
	if (lock < 0) {
		return 0;
	}
	*target = (struct chat_target){0};
	if (session_chat(
			file, 1, target->id, sizeof(target->id), target->socket, sizeof(target->socket))) {
		target->existing = 1;
		close(lock);
		return 1;
	}
	char reuse[32] = "";
	if (preferred && *preferred) {
		snprintf(reuse, sizeof(reuse), "%s", preferred);
	} else {
		session_chat(file, 0, reuse, sizeof(reuse), target->socket, sizeof(target->socket));
	}
	int ok = start_chat(argc, argv, *reuse ? reuse : NULL, file, lock, target->id,
		sizeof(target->id), target->socket, sizeof(target->socket));
	/* Do not LOCK_UN: the child's inherited reference owns the handoff. */
	close(lock);
	if (!ok) {
		snprintf(error, size, "Could not start the saved conversation.");
	}

	return ok;
}

/* Reopen the conversation, not a snapshot of its old completed host. */
static int open_chat(char *id, size_t id_size, char *socket_path, size_t socket_size) {

	int live = 0;
	char error[256] = "";
	json_object *chat = find_chat(id, &live, error, sizeof(error));
	if (!chat) {
		fprintf(stderr, "%s\n", error);
		return 0;
	}
	if (live) {
		snprintf(socket_path, socket_size, "%s", field(chat, "socket"));
		json_object_put(chat);
		return 1;
	}
	const char *file = field(chat, "session_file");
	if (!*file || access(file, R_OK)) {
		fprintf(stderr, "Chat %s ended without a saved session.\n", id);
		json_object_put(chat);
		return 0;
	}
	struct session_entry entry;
	if (!session_from_path(file, &entry, error, sizeof(error)) || entry.version != 2) {
		fprintf(stderr, "Cannot reopen chat %s in place: %s\n", id,
			*error ? error : "legacy transcripts need one-time conversion with /resume");
		json_object_put(chat);
		return 0;
	}
	char copy[4096], reuse[32], self[] = "phi", option[] = "--session",
								*resume[] = {self, option, copy, NULL};
	snprintf(copy, sizeof(copy), "%s", file);
	snprintf(reuse, sizeof(reuse), "%s", field(chat, "id"));
	json_object_put(chat);
	struct chat_target target;
	if (!resume_chat(3, resume, copy, reuse, &target, error, sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		return 0;
	}
	snprintf(id, id_size, "%s", target.id);
	snprintf(socket_path, socket_size, "%s", target.socket);

	return 1;
}

/* The viewer: show a chat; when it detaches, show the board and follow
   what is picked there, until the chat ends or the board is left. */
static int view(const char *first_id, const char *first_socket, int first_foreground) {

	char id[32], socket_path[4200];
	snprintf(id, sizeof(id), "%s", first_id);
	snprintf(socket_path, sizeof(socket_path), "%s", first_socket);
	for (;;) {
		int code = *id ? attach_child(socket_path, first_foreground) : 3;
		first_foreground = 0; /* Every later visit comes back from the board. */
		if (*id) {
			mark_seen(id); /* you have now seen what it said */
		}
		if (code == 4) {
			*id = 0; /* an empty chat closed itself: nothing to return to */
		} else if (code != 3) {
			return 0; /* the chat ended (/quit) */
		}
		if (!board_hook) {
			printf("Detached; chat %s keeps running. phi attach %s returns to it.\n", id, id);
			return 0;
		}
		char chosen[32] = "";
		int action = board_hook(id, chosen, sizeof(chosen));
		if (action == CHATS_BOARD_QUIT) {
			printf(
				"Chats keep running in the background: phi ls lists them, phi attach opens one.\n");
			return 0;
		}
		if (action == CHATS_BOARD_OPEN) {
			snprintf(id, sizeof(id), "%s", chosen);
			if (!open_chat(id, sizeof(id), socket_path, sizeof(socket_path))) {
				return 1;
			}
		}
		/* CHATS_BOARD_RETURN: back to the same chat. */
	}
}

int chats_host(int argc, char **argv, int background, const char *resume_path, int continue_latest,
	char **prompts, size_t prompt_count) {

	char file[4096] = "", error[256];
	struct session_entry entry;
	if (resume_path) {
		if (!session_from_path(resume_path, &entry, error, sizeof(error))) {
			fprintf(stderr, "%s\n", error);
			return 1;
		}
		snprintf(file, sizeof(file), "%s", entry.path);
	}
	if (continue_latest) {
		/* The newest session started in this directory; none starts a new chat. */
		char cwd[4096];
		int found =
			getcwd(cwd, sizeof(cwd)) ? sessions_latest_in(cwd, &entry, error, sizeof(error)) : 0;
		if (found < 0) {
			fprintf(stderr, "%s\n", error);
			return 1;
		}
		if (found) {
			snprintf(file, sizeof(file), "%s", entry.path);
		}
	}
	struct chat_target target = {0};
	if (*file) {
		if (!resume_chat(argc, argv, file, NULL, &target, error, sizeof(error))) {
			fprintf(stderr, "%s\n", error);
			return 1; /* Never turn a failed/busy resume into a fresh chat. */
		}
	} else {
		int launched = start_chat(argc, argv, NULL, NULL, -1, target.id, sizeof(target.id),
			target.socket, sizeof(target.socket));
		if (!launched) {
			if (background) {
				fprintf(stderr, "Could not start a background chat.\n");
				return 1;
			}
			return -1;
		}
	}
	if (target.existing) {
		/* Attaching must not silently discard positional prompts. */
		for (size_t i = 0; i < prompt_count; i++) {
			if (!chats_send(target.id, prompts[i], error, sizeof(error))) {
				fprintf(stderr, "%s\n", error);
				return 1;
			}
		}
	}
	if (background) {
		printf(target.existing ? "Chat %s is already running. phi attach %s to open it.\n"
							   : "Started chat %s. phi attach %s to open it.\n",
			target.id, target.id);
		return 0;
	}

	return view(target.id, target.socket, !target.existing);
}

/* ---- for the board ----------------------------------------------------------- */

static int group_rank(const struct chat *chat) {

	const char *group = group_of(chat);

	return !strcmp(group, "waiting") ? 0
		: !strcmp(group, "approval") ? 1
		: !strcmp(group, "working")  ? 2
									 : 3;
}

static int board_order(const void *a, const void *b) {

	int x = group_rank(a), y = group_rank(b);

	return x != y ? x - y : newest_first(a, b);
}

/* Older status files predate has_messages; their session file was only
   created when a turn began. No-session chats use the explicit flag. */
static int board_has_messages(json_object *status) {

	json_object *flag = json_object_object_get(status, "has_messages");

	return flag ? json_object_get_boolean(flag) : *field(status, "session_file") != 0;
}

size_t chats_rows(struct chat_row **rows) {

	struct chat *chats;
	size_t count = load_chats(&chats);
	if (count > 1) {
		qsort(chats, count, sizeof(*chats), board_order);
	}
	/* An empty hosted chat has nothing to show on the board. Keep it in the
       registry so attach/ls still work; no-session chats can have messages. */
	size_t visible = 0;
	for (size_t i = 0; i < count; i++) {
		visible += board_has_messages(chats[i].status);
	}
	*rows = visible ? calloc(visible, sizeof(**rows)) : NULL;
	if (visible && !*rows) {
		free_chats(chats, count);
		return 0;
	}
	size_t shown = 0;
	for (size_t i = 0; i < count; i++) {
		/* Saved chats can be reopened through the same board action. */
		if (!board_has_messages(chats[i].status)) {
			continue;
		}
		struct chat_row *row = &(*rows)[shown++];
		json_object *status = chats[i].status;
		snprintf(row->id, sizeof(row->id), "%s", field(status, "id"));
		snprintf(row->title, sizeof(row->title), "%s", field(status, "title"));
		snprintf(row->detail, sizeof(row->detail), "%s", field(status, "detail"));
		snprintf(row->group, sizeof(row->group), "%s", group_of(&chats[i]));
		snprintf(row->state, sizeof(row->state), "%s", field(status, "state"));
		snprintf(row->model, sizeof(row->model), "%s", field(status, "model"));
		snprintf(row->effort, sizeof(row->effort), "%s", field(status, "thinking"));
		const char *cwd = field(status, "cwd"), *base = strrchr(cwd, '/');
		snprintf(row->place, sizeof(row->place), "%s", base && base[1] ? base + 1 : cwd);
		snprintf(row->session_file, sizeof(row->session_file), "%s", field(status, "session_file"));
		struct stat saved;
		if (*row->session_file && !stat(row->session_file, &saved)) {
			row->session_bytes = saved.st_size;
			row->session_stamp =
				(long long)saved.st_mtim.tv_sec * 1000000000LL + saved.st_mtim.tv_nsec;
		}
		row->updated = updated(&chats[i]);
		row->live = chats[i].live;
	}
	free_chats(chats, count);

	return shown;
}

int chats_live(const char *id) {

	if (!id || !*id) {
		return 0;
	}
	struct chat *chats;
	size_t count = load_chats(&chats);
	int live = 0;
	for (size_t i = 0; i < count; i++) {
		if (!strcmp(field(chats[i].status, "id"), id)) {
			live = chats[i].live;
			break;
		}
	}
	free_chats(chats, count);

	return live;
}

void chats_age(long long when, char *out, size_t size) {

	age(when, out, size);
}

/* Structured, same-UID close: never inject a prompt or signal a stored PID. */
int chats_close_force(const char *id, int force, char *error, size_t size) {

	int live = 0;
	json_object *chat = find_chat(id, &live, error, size);
	if (!chat) {
		return 0;
	}
	if (!live) {
		json_object_put(chat);
		return 1;
	}
	if (!force && strcmp(field(chat, "state"), "idle")) {
		snprintf(error, size, "chat is working; use --force");
		json_object_put(chat);
		return 0;
	}
	pid_t pid = (pid_t)json_object_get_int64(json_object_object_get(chat, "pid"));
	int zombie = 0;
	uint64_t generation = process_start(pid, &zombie);
	char canonical[32];
	snprintf(canonical, sizeof(canonical), "%s", field(chat, "id"));
	json_object_put(chat);
	json_object *reply = bridge_request_close(canonical, force);
	int accepted = json_object_get_boolean(json_object_object_get(reply, "accepted"));
	if (!accepted) {
		snprintf(error, size, "%s",
			*field(reply, "error") ? field(reply, "error") : "Close not acknowledged");
	}
	json_object_put(reply);
	if (!accepted) {
		return 0;
	}
	if (pid == getpid()) {
		return 1; /* The main loop will unwind after this callback. */
	}
	for (int attempt = 0; attempt < 600; attempt++) {
		uint64_t current = process_start(pid, &zombie);
		if (!current || zombie || (generation && current != generation)) {
			return 1;
		}
		struct timespec pause = {.tv_nsec = 25000000};
		nanosleep(&pause, NULL);
	}
	snprintf(error, size, "Close requested; chat has not exited yet");

	return 0;
}

int chats_close(const char *id, char *error, size_t size) {

	return chats_close_force(id, 0, error, size);
}

int chats_send(const char *id, const char *text, char *error, size_t size) {

	int live = 0;
	json_object *chat = find_chat(id, &live, error, size);
	if (!chat) {
		return 0;
	}
	if (!live) {
		snprintf(error, size, "That chat has ended; open it to resume.");
		json_object_put(chat);
		return 0;
	}
	if (!strcmp(field(chat, "state"), "needs_input")) {
		snprintf(error, size, "It is waiting on an approval; open it to answer.");
		json_object_put(chat);
		return 0;
	}
	char socket_path[4200];
	snprintf(socket_path, sizeof(socket_path), "%s", field(chat, "socket"));
	json_object_put(chat);
	/* Paste the text and Enter, straight to the chat's host. */
	size_t length = strlen(text);
	if (length >= 4000) {
		snprintf(error, size, "That message is too long to send this way.");
		return 0;
	}
	int fd = phi_host_connect_socket(socket_path);
	int sent = fd >= 0 && phi_host_send_frame(fd, 'p', text, length);
	if (fd >= 0) {
		close(fd);
	}
	if (!sent) {
		snprintf(error, size, "Could not reach the chat.");
		return 0;
	}

	return 1;
}

int chats_forget(const char *id, char *error, size_t size) {

	int live = 0;
	json_object *chat = find_chat(id, &live, error, size);
	if (!chat) {
		return 0;
	}
	if (live) {
		snprintf(error, size, "Chat %s is running; end it with /quit first.", id);
		json_object_put(chat);
		return 0;
	}
	char directory[4096], path[4200];
	status_directory(directory, sizeof(directory));
	snprintf(path, sizeof(path), "%.4000s/%s.json", directory, field(chat, "id"));
	json_object_put(chat);
	if (unlink(path)) {
		snprintf(error, size, "Cannot remove %s", path);
		return 0;
	}
	snprintf(path, sizeof(path), "%.4000s/%s.seen", directory, id);
	unlink(path);

	return 1;
}

int chats_rename(const char *id, const char *name, char *error, size_t size) {

	int live = 0;
	json_object *chat = find_chat(id, &live, error, size);
	if (!chat) {
		return 0;
	}
	if (!*name || strpbrk(name, "\r\n")) {
		snprintf(error, size, "Give a one-line name.");
		json_object_put(chat);
		return 0;
	}
	if (live) {
		/* The chat names its own session (and its status) with /name. */
		int busy = strcmp(field(chat, "state"), "idle") != 0;
		json_object_put(chat);
		if (busy) {
			snprintf(error, size, "Rename it when it is waiting for you.");
			return 0;
		}
		char command[300];
		snprintf(command, sizeof(command), "/name %.280s", name);
		return chats_send(id, command, error, size);
	}
	/* Ended: record the name in its session and its status file. */
	const char *file = field(chat, "session_file");
	if (*file && !transcript_name(file, name)) {
		snprintf(error, size, "Cannot rename transcript %s: %s", file, strerror(errno));
		json_object_put(chat);
		return 0;
	}
	json_object_object_add(chat, "title", json_object_new_string(name));
	char directory[4096], path[4096];
	status_directory(directory, sizeof(directory));
	snprintf(path, sizeof(path), "%.4000s/%s.json", directory, field(chat, "id"));
	const char *text = json_object_to_json_string_ext(
		chat, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
	int ok = phi_write_file_atomic(path, text, strlen(text));
	json_object_put(chat);
	if (!ok) {
		snprintf(error, size, "Cannot update %s", path);
	}

	return ok;
}

int chats_start(const char *task, char *id, size_t size) {

	char self[] = "phi", copy[4096], socket_path[4200], *argv[] = {self, copy, NULL};
	snprintf(copy, sizeof(copy), "%s", task);

	return start_chat(
		*task ? 2 : 1, argv, NULL, NULL, -1, id, size, socket_path, sizeof(socket_path));
}

int chats_launch(json_object *options, char *id, size_t size, char *error, size_t error_size) {

	int saved = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (saved < 0) {
		return 0;
	}
	const char *cwd = field(options, "cwd");
	if (*cwd && chdir(cwd)) {
		close(saved);
		snprintf(error, error_size, "Cannot enter cwd: %s", strerror(errno));
		return 0;
	}
	char self[] = "phi", *args[8] = {self};
	int count = 1;
	const char *model = field(options, "model"), *thinking = field(options, "thinking");
	if (*model) {
		args[count++] = "--model";
		args[count++] = (char *)model;
	}
	if (*thinking) {
		args[count++] = "--thinking";
		args[count++] = (char *)thinking;
	}
	args[count] = NULL;
	char socket[4200];
	const char *old = getenv("PHI_BRIDGE_TITLE");
	char *old_title = old ? strdup(old) : NULL;
	setenv("PHI_BRIDGE_TITLE", field(options, "title"), 1);
	int ok = start_chat(count, args, NULL, NULL, -1, id, size, socket, sizeof(socket));
	if (old_title) {
		setenv("PHI_BRIDGE_TITLE", old_title, 1);
		free(old_title);
	} else {
		unsetenv("PHI_BRIDGE_TITLE");
	}
	if (fchdir(saved)) {
		ok = 0;
	}
	close(saved);
	if (!ok) {
		snprintf(error, error_size, "Could not start chat.");
	}

	return ok;
}

/* ---- commands -------------------------------------------------------------- */

int chats_cli(int argc, char **argv) {

	if (argc < 2) {
		return -1;
	}
	const char *command = argv[1];
	char error[256];
	int live = 0;
	if (!strcmp(command, "close")) {
		int force = 0, idle_only = 0, ids = 0, failed = 0;
		for (int i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "--force")) {
				force = 1;
			} else if (!strcmp(argv[i], "--idle")) {
				idle_only = 1;
			} else if (argv[i][0] == '-') {
				fprintf(stderr, "Unknown close option: %s\n", argv[i]);
				return 1;
			} else {
				ids++;
			}
		}
		if ((!ids && !idle_only) || (idle_only && (ids || force))) {
			fprintf(stderr, "Usage: phi close ID... [--force] | phi close --idle\n");
			return 1;
		}
		if (idle_only) {
			struct chat *chats;
			size_t count = load_chats(&chats);
			for (size_t i = 0; i < count; i++) {
				if (!chats[i].live || strcmp(field(chats[i].status, "state"), "idle")) {
					continue;
				}
				const char *id = field(chats[i].status, "id");
				if (chats_close_force(id, 0, error, sizeof(error))) {
					printf("%s ended\n", id);
				} else if (strcmp(error, "chat is working; use --force")) {
					fprintf(stderr, "%s: %s\n", id, error);
					failed = 1;
				}
			}
			free_chats(chats, count);
			return failed;
		}
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--force")) {
				if (!chats_close_force(argv[i], force, error, sizeof(error))) {
					fprintf(stderr, "%s: %s\n", argv[i], error);
					failed = 1;
				} else {
					printf("%s ended\n", argv[i]);
				}
			}
		}
		return failed;
	}
	if (!strcmp(command, "ls") && argc == 2) {
		return list();
	}
	if (!strcmp(command, "board") && argc == 2 && board_hook) {
		return view("", "", 0);
	}
	if (!strcmp(command, "attach") && argc <= 3) {
		json_object *chat = find_chat(argc == 3 ? argv[2] : NULL, &live, error, sizeof(error));
		if (!chat) {
			fprintf(stderr, "%s\n", error);
			return 1;
		}
		char id[32], socket_path[4200];
		snprintf(id, sizeof(id), "%s", field(chat, "id"));
		json_object_put(chat);
		if (!open_chat(id, sizeof(id), socket_path, sizeof(socket_path))) {
			return 1;
		}
		return view(id, socket_path, 0);
	}
	if (!strcmp(command, "send") && argc == 4) {
		if (chats_send(argv[2], argv[3], error, sizeof(error))) {
			return 0;
		}
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	if (!strcmp(command, "rm") && argc == 3) {
		if (chats_forget(argv[2], error, sizeof(error))) {
			return 0; /* the session file itself is kept */
		}
		fprintf(stderr, "%s\n", error);
		return 1;
	}

	return -1;
}
