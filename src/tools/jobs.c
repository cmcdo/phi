#define _GNU_SOURCE
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "internal.h"
#include <limits.h>
#include <sys/ioctl.h>

#define JOB_LIMIT 16
#define JOB_LOG_LIMIT (256 * 1024 * 1024)
#define JOB_POLL_DEFAULT (32 * 1024)
#define JOB_POLL_MAX (64 * 1024)
#define JOB_WRITE_MAX (64 * 1024)
#define JOB_COMMAND_MAX (64 * 1024)
#define JOB_CHUNK 4096

struct background_job {
	unsigned id;
	pid_t pid;
	pid_t collector;
	int input_fd;
	int input_closed;
	int log_fd;
	off_t read_offset;
	int status;
	int exited;
	int collector_exited;
	int collector_status;
	int killed;
	int pty;
	double started;
	char *command;
};

static struct background_job jobs[JOB_LIMIT];
static unsigned next_job_id = 1;

static int make_log(void) {

	char path[] = "/tmp/phi-job-XXXXXX";
	int fd = mkstemp(path);
	if (fd >= 0) {
		unlink(path);
		int flags = fcntl(fd, F_GETFD);
		if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) {
			close(fd);
			return -1;
		}
	}

	return fd;
}

/* Background output must never be allowed to back-pressure the command. The
   collector continuously drains it into an unlinked, bounded spool file. A
   marker byte at JOB_LOG_LIMIT records that later output was discarded. */
static _Noreturn void collect_output(int source, int log_fd) {

	tool_close_child_fds(source, log_fd);
	char chunk[JOB_CHUNK];
	size_t saved = 0;
	int truncated = 0;
	struct tool_ansi ansi = {.color = -1};

	for (;;) {
		ssize_t count = read(source, chunk, sizeof(chunk));
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd poller = {.fd = source, .events = POLLIN};
			if (poll(&poller, 1, 100) < 0 && errno != EINTR) {
				_exit(2);
			}
			continue;
		}
		if (count < 0 && (errno == EIO || errno == EBADF)) {
			break; /* PTY closed. */
		}
		if (count <= 0) {
			break;
		}

		size_t kept = tool_clean_output(chunk, (size_t)count, &ansi, NULL);

		size_t write_size = kept;
		if (write_size > JOB_LOG_LIMIT - saved) {
			write_size = JOB_LOG_LIMIT - saved;
		}
		if (write_size && tool_write_all(log_fd, chunk, write_size)) {
			_exit(2);
		}
		saved += write_size;
		if (write_size < kept && !truncated) {
			char marker = 1;
			if (pwrite(log_fd, &marker, 1, JOB_LOG_LIMIT) != 1) {
				_exit(2);
			}
			truncated = 1;
		}
	}
	_exit(0);
}

static void close_job_input(struct background_job *job) {

	if (job->input_fd >= 0) {
		close(job->input_fd);
		job->input_fd = -1;
		job->input_closed = 1;
	}
}

static void reap_job(struct background_job *job) {

	if (!job || !job->id) {
		return;
	}
	if (!job->exited) {
		int status = 0;
		pid_t waited = waitpid(job->pid, &status, WNOHANG);
		if (waited == job->pid) {
			job->status = status;
			job->exited = 1;
			close_job_input(job);
		} else if (waited < 0 && errno == ECHILD) {
			job->exited = 1;
			job->status = 1 << 8;
			close_job_input(job);
		}
	}
	if (!job->collector_exited) {
		int status = 0;
		pid_t waited = waitpid(job->collector, &status, WNOHANG);
		if (waited == job->collector) {
			job->collector_status = status;
			job->collector_exited = 1;
		} else if (waited < 0 && errno == ECHILD) {
			job->collector_status = 1 << 8;
			job->collector_exited = 1;
		}
	}
}

static int exit_code(const struct background_job *job) {

	return job->exited ? tool_exit_code(job->status) : -1;
}

static struct background_job *find_job(unsigned id) {

	if (!id) {
		return NULL;
	}
	for (size_t i = 0; i < JOB_LIMIT; i++) {
		if (jobs[i].id == id) {
			return &jobs[i];
		}
	}

	return NULL;
}

static void forget_job(struct background_job *job) {

	if (!job || !job->id) {
		return;
	}
	close_job_input(job);
	if (job->log_fd >= 0) {
		close(job->log_fd);
	}
	free(job->command);
	*job = (struct background_job){.input_fd = -1, .log_fd = -1};
}

static struct background_job *job_slot(void) {

	struct background_job *oldest = NULL;
	for (size_t i = 0; i < JOB_LIMIT; i++) {
		if (!jobs[i].id) {
			return &jobs[i];
		}
		reap_job(&jobs[i]);
		if (jobs[i].exited && jobs[i].collector_exited && jobs[i].log_fd < 0 &&
			(!oldest || jobs[i].started < oldest->started)) {
			oldest = &jobs[i];
		}
	}
	if (oldest) {
		forget_job(oldest);
		return oldest;
	}

	return NULL;
}

static int bool_arg(json_object *args, const char *name, int fallback, int *answer) {

	json_object *value = NULL;
	*answer = fallback;
	if (!json_object_object_get_ex(args, name, &value) || !value) {
		return 1;
	}
	if (!json_object_is_type(value, json_type_boolean)) {
		return 0;
	}
	*answer = json_object_get_boolean(value);

	return 1;
}

static _Noreturn void exec_background(const char *command, int pty, int input[2], int output[2]) {

	setpgid(0, 0);
	if (pty) {
		struct winsize size = {.ws_row = 24, .ws_col = 120};
		ioctl(output[1], TIOCSWINSZ, &size);
		if (dup2(output[1], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
			dup2(output[1], STDERR_FILENO) < 0) {
			_exit(126);
		}
	} else {
		if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
			dup2(output[1], STDERR_FILENO) < 0) {
			_exit(126);
		}
	}
	char *argv[] = {"/bin/bash", "-c", (char *)command, NULL};
	tool_exec_child(NULL, argv, NULL, -1, -1);
}

static json_object *bash_start_run(json_object *args) {

	json_object *value = NULL;
	const char *command = NULL;
	size_t command_size = 0;
	if (!args || !json_object_object_get_ex(args, "command", &value) ||
		!(command = tool_plain_string(value, &command_size))) {
		return tool_error("Expected a command string");
	}
	if (!command_size) {
		return tool_error("Command cannot be empty");
	}
	if (command_size > JOB_COMMAND_MAX) {
		return tool_error("Command is limited to 64 KiB");
	}

	int pty = 0;
	if (!bool_arg(args, "pty", 0, &pty)) {
		return tool_error("pty must be boolean or null");
	}
	if (tool_sandbox_unavailable()) {
		return tool_error("Sandbox is enabled but bubblewrap (bwrap) is not installed.");
	}

	struct background_job *job = job_slot();
	if (!job) {
		return tool_error("Too many active background jobs (maximum 16)");
	}

	int input[2] = {-1, -1}, output[2] = {-1, -1};
	int log_fd = -1;
	if ((!pty && (pipe2(input, O_CLOEXEC) || pipe2(output, O_CLOEXEC))) ||
		(pty && tool_open_pty(output)) || (log_fd = make_log()) < 0) {
		for (int i = 0; i < 2; i++) {
			if (input[i] >= 0) {
				close(input[i]);
			}
			if (output[i] >= 0) {
				close(output[i]);
			}
		}
		if (log_fd >= 0) {
			close(log_fd);
		}
		return tool_error("Cannot create background job channels");
	}
	if (pty) {
		input[0] = input[1] = -1;
	}

	fflush(NULL);
	pid_t pid = fork();
	if (!pid) {
		exec_background(command, pty, input, output);
	}
	if (pid < 0) {
		for (int i = 0; i < 2; i++) {
			if (input[i] >= 0) {
				close(input[i]);
			}
			if (output[i] >= 0) {
				close(output[i]);
			}
		}
		close(log_fd);
		return tool_error("Cannot start background command");
	}
	setpgid(pid, pid);

	if (pty) {
		close(output[1]);
		output[1] = -1;
	} else {
		close(input[0]);
		input[0] = -1;
		close(output[1]);
		output[1] = -1;
	}

	pid_t collector = fork();
	if (!collector) {
		if (!pty && input[1] >= 0) {
			close(input[1]);
		}
		collect_output(output[0], log_fd);
	}
	if (collector < 0) {
		kill(-pid, SIGKILL);
		kill(pid, SIGKILL);
		int status;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		if (!pty && input[1] >= 0) {
			close(input[1]);
		}
		close(output[0]);
		close(log_fd);
		return tool_error("Cannot start background output collector");
	}

	char *saved_command = strdup(command);
	if (!saved_command) {
		kill(-pid, SIGKILL);
		kill(pid, SIGKILL);
		kill(collector, SIGKILL);
		int status;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		while (waitpid(collector, &status, 0) < 0 && errno == EINTR) {
		}
		if (!pty && input[1] >= 0) {
			close(input[1]);
		}
		close(output[0]);
		close(log_fd);
		return tool_error("Not enough memory");
	}

	/* The collector owns its inherited read descriptor. Phi only needs a write
       handle for stdin plus the spool descriptor for pread(). */
	int input_fd = pty ? output[0] : input[1];
	if (!pty) {
		close(output[0]);
	}
	if (tool_nonblocking(input_fd)) {
		/* Nonblocking stdin prevents a stopped/non-reading job from freezing Phi. */
		kill(-pid, SIGKILL);
		kill(pid, SIGKILL);
		kill(collector, SIGKILL);
		int status;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		while (waitpid(collector, &status, 0) < 0 && errno == EINTR) {
		}
		close(input_fd);
		close(log_fd);
		free(saved_command);
		return tool_error("Cannot configure background input");
	}

	unsigned id = next_job_id++;
	if (!id) {
		id = next_job_id++;
	}
	*job = (struct background_job){.id = id,
		.pid = pid,
		.collector = collector,
		.input_fd = input_fd,
		.log_fd = log_fd,
		.started = tool_monotonic_seconds(),
		.command = saved_command,
		.pty = pty};

	json_object *result = json_object_new_object();
	if (!result) {
		return NULL;
	}
	json_object_object_add(result, "job_id", json_object_new_int64(id));
	json_object_object_add(result, "running", json_object_new_boolean(1));
	json_object_object_add(result, "pty", json_object_new_boolean(pty));

	return result;
}

static int job_id_arg(json_object *args, unsigned *id) {

	int64_t parsed = 0;
	if (!tool_integer_arg(args, "job_id", 0, 1, UINT_MAX, &parsed)) {
		return 0;
	}
	*id = (unsigned)parsed;

	return parsed != 0;
}

static json_object *job_list(void) {

	json_object *result = json_object_new_object();
	json_object *array = json_object_new_array();
	if (!result || !array) {
		if (result) {
			json_object_put(result);
		}
		if (array) {
			json_object_put(array);
		}
		return NULL;
	}
	for (size_t i = 0; i < JOB_LIMIT; i++) {
		struct background_job *job = &jobs[i];
		if (!job->id) {
			continue;
		}
		reap_job(job);
		json_object *item = json_object_new_object();
		if (!item) {
			json_object_put(array);
			json_object_put(result);
			return NULL;
		}
		size_t command_size = strlen(job->command);
		if (command_size > 512) {
			command_size = 512;
		}
		json_object_object_add(item, "job_id", json_object_new_int64(job->id));
		json_object_object_add(item, "running", json_object_new_boolean(!job->exited));
		json_object_object_add(item, "pty", json_object_new_boolean(job->pty));
		json_object_object_add(
			item, "command", json_object_new_string_len(job->command, (int)command_size));
		int64_t unread = 0;
		if (job->log_fd >= 0) {
			struct stat st;
			if (!fstat(job->log_fd, &st)) {
				off_t available = st.st_size > JOB_LOG_LIMIT ? JOB_LOG_LIMIT : st.st_size;
				if (available > job->read_offset) {
					unread = (int64_t)(available - job->read_offset);
				}
			}
		}
		json_object_object_add(item, "unread_bytes", json_object_new_int64(unread));
		if (job->exited) {
			json_object_object_add(item, "exit_code", json_object_new_int(exit_code(job)));
		}
		json_object_array_add(array, item);
	}
	json_object_object_add(result, "jobs", array);

	return result;
}

static json_object *bash_poll_run(json_object *args) {

	json_object *id_value = NULL;
	if (!json_object_object_get_ex(args, "job_id", &id_value) || !id_value) {
		return job_list();
	}
	unsigned id;
	if (!job_id_arg(args, &id)) {
		return tool_error("job_id must be a positive integer or null");
	}
	struct background_job *job = find_job(id);
	if (!job) {
		return tool_error("Unknown background job");
	}
	reap_job(job);

	int64_t maximum = JOB_POLL_DEFAULT;
	if (!tool_integer_arg(args, "max_bytes", JOB_POLL_DEFAULT, 1, JOB_POLL_MAX, &maximum)) {
		return tool_error("max_bytes must be 1..65536 or null");
	}

	/* Give the collector a brief chance to publish final bytes after the shell
       exits. If it finishes while we read, refresh the size so `more` cannot
       falsely report that all output has been consumed. */
	if (job->exited && !job->collector_exited) {
		struct timespec pause = {0, 5000000};
		nanosleep(&pause, NULL);
		reap_job(job);
	}
	struct stat st = {0};
	off_t available = job->read_offset;
	int truncated = 0;
	if (job->log_fd >= 0) {
		if (fstat(job->log_fd, &st)) {
			return tool_error("Cannot inspect background output");
		}
		truncated = st.st_size > JOB_LOG_LIMIT;
		available = st.st_size > JOB_LOG_LIMIT ? JOB_LOG_LIMIT : st.st_size;
	}
	off_t remaining = available > job->read_offset ? available - job->read_offset : 0;
	size_t size = remaining > maximum ? (size_t)maximum : (size_t)remaining;
	char *buffer = malloc(size + 1);
	if (!buffer) {
		return tool_error("Not enough memory");
	}
	ssize_t got = size && job->log_fd >= 0 ? pread(job->log_fd, buffer, size, job->read_offset) : 0;
	if (got < 0) {
		free(buffer);
		return tool_error("Cannot read background output");
	}
	buffer[got] = '\0';
	job->read_offset += got;
	reap_job(job);
	if (job->collector_exited && job->log_fd >= 0 && !fstat(job->log_fd, &st)) {
		truncated = st.st_size > JOB_LOG_LIMIT;
		available = st.st_size > JOB_LOG_LIMIT ? JOB_LOG_LIMIT : st.st_size;
	}
	int output_error = job->collector_exited &&
		(!WIFEXITED(job->collector_status) || WEXITSTATUS(job->collector_status) != 0);
	int more = job->read_offset < available;
	if (job->collector_exited && !more && job->log_fd >= 0) {
		close(job->log_fd);
		job->log_fd = -1; /* consumed incremental output can release disk space */
	}

	json_object *result = json_object_new_object();
	if (!result) {
		free(buffer);
		return NULL;
	}
	json_object_object_add(result, "job_id", json_object_new_int64(id));
	json_object_object_add(result, "output", json_object_new_string_len(buffer, (int)got));
	free(buffer);
	json_object_object_add(result, "running", json_object_new_boolean(!job->exited));
	json_object_object_add(result, "output_open", json_object_new_boolean(!job->collector_exited));
	json_object_object_add(
		result, "exit_code", job->exited ? json_object_new_int(exit_code(job)) : NULL);
	json_object_object_add(result, "killed", json_object_new_boolean(job->killed));
	json_object_object_add(result, "output_truncated", json_object_new_boolean(truncated));
	json_object_object_add(result, "output_error", json_object_new_boolean(output_error));
	json_object_object_add(result, "more", json_object_new_boolean(more));

	return result;
}

static json_object *bash_write_run(json_object *args) {

	unsigned id;
	if (!job_id_arg(args, &id)) {
		return tool_error("job_id must be a positive integer");
	}
	struct background_job *job = find_job(id);
	if (!job) {
		return tool_error("Unknown background job");
	}
	reap_job(job);
	if (job->exited || job->input_fd < 0 || job->input_closed) {
		return tool_error("Background job is no longer accepting input");
	}

	json_object *value = NULL;
	size_t size = 0;
	const char *data = NULL;
	if (!json_object_object_get_ex(args, "data", &value) ||
		!(data = tool_plain_string(value, &size))) {
		return tool_error("Expected a data string");
	}
	if (size > JOB_WRITE_MAX) {
		return tool_error("data is limited to 64 KiB per call");
	}
	int close_stdin = 0;
	if (!bool_arg(args, "close_stdin", 0, &close_stdin)) {
		return tool_error("close_stdin must be boolean or null");
	}

	size_t written = 0;
	double deadline = tool_monotonic_seconds() + 1.0;
	while (written < size) {
		ssize_t count = write(job->input_fd, data + written, size - written);
		if (count > 0) {
			written += (size_t)count;
			continue;
		}
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			int milliseconds = (int)((deadline - tool_monotonic_seconds()) * 1000.0);
			if (milliseconds <= 0) {
				break;
			}
			struct pollfd poller = {.fd = job->input_fd, .events = POLLOUT};
			if (poll(&poller, 1, milliseconds > 50 ? 50 : milliseconds) < 0 && errno != EINTR) {
				break;
			}
			continue;
		}
		if (count < 0 && (errno == EPIPE || errno == EIO)) {
			reap_job(job);
			break;
		}
		break;
	}
	if (close_stdin) {
		if (job->pty) {
			const char eof = 4; /* terminal EOF in canonical mode */
			(void)write(job->input_fd, &eof, 1);
			job->input_closed = 1;
		} else {
			close_job_input(job);
		}
	}

	json_object *result = json_object_new_object();
	if (!result) {
		return NULL;
	}
	json_object_object_add(result, "job_id", json_object_new_int64(id));
	json_object_object_add(result, "bytes_written", json_object_new_int64((int64_t)written));
	json_object_object_add(result, "complete", json_object_new_boolean(written == size));
	json_object_object_add(result, "stdin_closed", json_object_new_boolean(close_stdin));

	return result;
}

static json_object *bash_kill_run(json_object *args) {

	unsigned id;
	if (!job_id_arg(args, &id)) {
		return tool_error("job_id must be a positive integer");
	}
	struct background_job *job = find_job(id);
	if (!job) {
		return tool_error("Unknown background job");
	}
	reap_job(job);
	if (!job->exited) {
		kill(-job->pid, SIGTERM);
		kill(job->pid, SIGTERM);
		double deadline = tool_monotonic_seconds() + 0.5;
		while (!job->exited && tool_monotonic_seconds() < deadline) {
			struct timespec pause = {0, 10000000};
			nanosleep(&pause, NULL);
			reap_job(job);
		}
		if (!job->exited) {
			kill(-job->pid, SIGKILL);
			kill(job->pid, SIGKILL);
			int status;
			while (waitpid(job->pid, &status, 0) < 0 && errno == EINTR) {
			}
			job->status = status;
			job->exited = 1;
			close_job_input(job);
		}
		job->killed = 1;
	}
	reap_job(job);

	json_object *result = json_object_new_object();
	if (!result) {
		return NULL;
	}
	json_object_object_add(result, "job_id", json_object_new_int64(id));
	json_object_object_add(result, "running", json_object_new_boolean(!job->exited));
	json_object_object_add(
		result, "exit_code", job->exited ? json_object_new_int(exit_code(job)) : NULL);
	json_object_object_add(result, "killed", json_object_new_boolean(job->killed));

	return result;
}

void tools_reap_jobs(void) {

	for (size_t i = 0; i < JOB_LIMIT; i++) {
		if (jobs[i].id) {
			reap_job(&jobs[i]);
		}
	}
}

void tools_abort_jobs(void) {

	/* Best-effort emergency path for Phi's signal handler: kill() is sufficient
       here; process exit closes descriptors and the normal reaper is unavailable. */
	for (size_t i = 0; i < JOB_LIMIT; i++) {
		if (!jobs[i].id) {
			continue;
		}
		if (!jobs[i].exited) {
			kill(-jobs[i].pid, SIGKILL);
			kill(jobs[i].pid, SIGKILL);
		}
		if (!jobs[i].collector_exited) {
			kill(jobs[i].collector, SIGKILL);
		}
	}
}

void tools_shutdown(void) {

	for (size_t i = 0; i < JOB_LIMIT; i++) {
		struct background_job *job = &jobs[i];
		if (!job->id) {
			continue;
		}
		reap_job(job);
		if (!job->exited) {
			kill(-job->pid, SIGKILL);
			kill(job->pid, SIGKILL);
			int status;
			while (waitpid(job->pid, &status, 0) < 0 && errno == EINTR) {
			}
			job->status = status;
			job->exited = 1;
		}
		close_job_input(job);
		if (!job->collector_exited) {
			kill(job->collector, SIGKILL);
			int status;
			while (waitpid(job->collector, &status, 0) < 0 && errno == EINTR) {
			}
			job->collector_exited = 1;
		}
		forget_job(job);
	}
}

const struct phi_tool phi_tool_bash_start = {.name = "bash_start",
	.schema =
		"{\"type\":\"function\",\"name\":\"bash_start\","
		"\"description\":\"Start a Bash command in the background and return immediately with a job_id. Use bash_poll to read output while continuing other work. Set pty=true only for commands that need terminal semantics or interactive stdin; do not append & to the command. Background output is continuously drained and bounded.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\"},"
		"\"pty\":{\"type\":[\"boolean\",\"null\"]}},"
		"\"required\":[\"command\",\"pty\"],\"additionalProperties\":false},\"strict\":true}",
	.detail_key = "command",
	.run = bash_start_run};

const struct phi_tool phi_tool_bash_poll = {.name = "bash_poll",
	.schema =
		"{\"type\":\"function\",\"name\":\"bash_poll\","
		"\"description\":\"Read new output and status from a background Bash job without blocking. Repeated calls continue from the previous output offset. Pass job_id=null to list known jobs. max_bytes defaults to 32768 and is capped at 65536.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"job_id\":{\"type\":[\"integer\",\"null\"],\"minimum\":1},"
		"\"max_bytes\":{\"type\":[\"integer\",\"null\"],\"minimum\":1,\"maximum\":65536}},"
		"\"required\":[\"job_id\",\"max_bytes\"],\"additionalProperties\":false},\"strict\":true}",
	.detail_key = NULL,
	.run = bash_poll_run};

const struct phi_tool phi_tool_bash_write = {.name = "bash_write",
	.schema =
		"{\"type\":\"function\",\"name\":\"bash_write\","
		"\"description\":\"Write stdin to a running background Bash job. close_stdin=true sends EOF (terminal EOT for PTY jobs) after the data. Data is limited to 64 KiB per call.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"job_id\":{\"type\":\"integer\",\"minimum\":1},"
		"\"data\":{\"type\":\"string\"},"
		"\"close_stdin\":{\"type\":[\"boolean\",\"null\"]}},"
		"\"required\":[\"job_id\",\"data\",\"close_stdin\"],\"additionalProperties\":false},\"strict\":true}",
	.detail_key = "data",
	.run = bash_write_run};

const struct phi_tool phi_tool_bash_kill = {.name = "bash_kill",
	.schema =
		"{\"type\":\"function\",\"name\":\"bash_kill\","
		"\"description\":\"Terminate a background Bash job and its process group. Sends SIGTERM first, then SIGKILL after 500 ms if needed.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{"
		"\"job_id\":{\"type\":\"integer\",\"minimum\":1}},"
		"\"required\":[\"job_id\"],\"additionalProperties\":false},\"strict\":true}",
	.detail_key = NULL,
	.run = bash_kill_run};
