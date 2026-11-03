#define _GNU_SOURCE
#include "bridge.h"
#include "surfaces.h"
#include "bridge-tail.h"
#include "chats.h"
#include "events.h"
#include "fsutil.h"
#include "history.h"
#include "json.h"
#include "sessions.h"
#include "settings.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>

#define MESSAGE_LIMIT (1024 * 1024)
#define WIRE_LIMIT (8 * 1024 * 1024)
#define CLIENTS 8
#define QUEUE_SIZE 32
static int listener = -1, pumping, closing;
static int (*close_handler)(int force, char *error, size_t size);

void bridge_set_close_handler(int (*handler)(int force, char *error, size_t size)) {

	close_handler = handler;
}

static int (*cancel_wait)(void);

void bridge_set_cancel(int (*cancel)(void)) {

	cancel_wait = cancel;
}

static char endpoint[108];
static json_object *queue[QUEUE_SIZE], *current_origin;
static size_t queued;

struct connection {
	int fd;
	unsigned char header[4];
	size_t head, used, length, sent;
	char *data, *reply;
	size_t reply_size;
	double deadline;
	pid_t pid;
};
static struct connection clients[CLIENTS];

static double clock_now(void) {

	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);

	return t.tv_sec + t.tv_nsec / 1e9;
}

static json_object *failure(const char *text) {

	json_object *o = json_object_new_object();
	json_object_object_add(o, "error", json_object_new_string(text));

	return o;
}

static int plain(json_object *o, const char *key, size_t maximum, int empty) {

	json_object *v = json_object_object_get(o, key);
	if (!json_object_is_type(v, json_type_string)) {
		return 0;
	}
	const char *s = json_object_get_string(v);
	size_t n = (size_t)json_object_get_string_len(v);

	return (empty || n) && n <= maximum && strlen(s) == n;
}

static int private_state(char *out, size_t size) {

	if (!phi_xdg_path(out, size, "XDG_STATE_HOME", ".local/state", "phi") ||
		!phi_make_directories(out)) {
		return 0;
	}
	struct stat st;

	return !lstat(out, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !chmod(out, 0700);
}

static json_object *parse(const char *text, size_t size) {

	json_tokener *p = json_tokener_new();
	if (!p) {
		return NULL;
	}
	json_tokener_set_flags(p, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	json_object *o = json_tokener_parse_ex(p, text, (int)size + 1);
	if (json_tokener_get_error(p) != json_tokener_success) {
		json_object_put(o);
		o = NULL;
	}
	json_tokener_free(p);

	return o;
}

static int write_all(int fd, const char *text, size_t size) {

	while (size) {
		ssize_t n = write(fd, text, size);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return 0;
		}
		text += n;
		size -= (size_t)n;
	}

	return 1;
}

/* A permanent per-chat lock serializes a sliding-hour rate window across
   host restarts. Failed auditing consumes a slot conservatively, not a message. */
static int audit_admit(json_object *message, char *error, size_t size) {

	char dir[4096], lockpath[4200], ratepath[4200], logpath[4200];
	const char *id = getenv("PHI_CHAT_ID");
	if (!id || strlen(id) != 8 || strspn(id, "0123456789abcdef") != 8 ||
		!private_state(dir, sizeof(dir))) {
		snprintf(error, size, "Cannot open private bridge state.");
		return 0;
	}
	snprintf(lockpath, sizeof(lockpath), "%s/bridge-%s.lock", dir, id);
	snprintf(ratepath, sizeof(ratepath), "%s/bridge-%s.rate", dir, id);
	snprintf(logpath, sizeof(logpath), "%s/bridge.log", dir);
	int fd = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600), ok = 0;
	struct stat st;
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		st.st_nlink != 1) {
		goto done;
	}
	int locked;
	do {
		locked = flock(fd, LOCK_EX);
	} while (locked && errno == EINTR);
	if (locked) {
		goto done;
	}
	int limit = 20;
	const char *setting = getenv("PHI_BRIDGE_RATE_LIMIT");
	if (setting && *setting) {
		char *end;
		long n = strtol(setting, &end, 10);
		if (*end || n < 1 || n > 1000) {
			snprintf(error, size, "Invalid PHI_BRIDGE_RATE_LIMIT (1..1000).");
			goto done;
		}
		limit = (int)n;
	}
	char *text = phi_read_file(ratepath, 65536, PHI_READ_OWNED, NULL);
	if (!text && errno != ENOENT) {
		goto done;
	}
	json_object *old = text ? parse(text, strlen(text)) : json_object_new_array();
	free(text);
	if (!json_object_is_type(old, json_type_array)) {
		json_object_put(old);
		snprintf(error, size, "Invalid bridge rate state.");
		goto done;
	}
	json_object *recent = json_object_new_array();
	time_t now = time(NULL);
	for (size_t i = 0; i < json_object_array_length(old); i++) {
		json_object *v = json_object_array_get_idx(old, i);
		if (!json_object_is_type(v, json_type_int)) {
			json_object_put(old);
			json_object_put(recent);
			snprintf(error, size, "Invalid bridge rate state.");
			goto done;
		}
		if (json_object_get_int64(v) > (int64_t)now - 3600) {
			json_object_array_add(recent, json_object_get(v));
		}
	}
	json_object_put(old);
	if (json_object_array_length(recent) >= (size_t)limit) {
		json_object_put(recent);
		snprintf(error, size, "Bridge rate limit reached (%d messages/chat/hour).", limit);
		goto done;
	}
	json_object_array_add(recent, json_object_new_int64(now));
	const char *encoded = json_object_to_json_string_ext(recent, JSON_C_TO_STRING_PLAIN);
	int saved = phi_write_file_atomic(ratepath, encoded, strlen(encoded));
	json_object_put(recent);
	if (!saved) {
		goto done;
	}
	int log =
		open(logpath, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
	if (log < 0) {
		goto done;
	}
	if (fstat(log, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
		fchmod(log, 0600)) {
		close(log);
		goto done;
	}
	json_object *entry = json_object_new_object();
	json_object_object_add(entry, "time", json_object_new_int64(now));
	json_object_object_add(entry, "chat_id", json_object_new_string(id));
	json_object_object_add(
		entry, "origin", json_object_get(json_object_object_get(message, "origin")));
	const char *body = field(message, "text");
	size_t bytes = 0, chars = 0;
	while (body[bytes] && chars < 200) {
		unsigned char c = (unsigned char)body[bytes++];
		if ((c & 0xc0) != 0x80) {
			chars++;
		}
	}
	while (body[bytes] && ((unsigned char)body[bytes] & 0xc0) == 0x80) {
		bytes++;
	}
	json_object_object_add(entry, "preview", json_object_new_string_len(body, (int)bytes));
	encoded = json_object_to_json_string_ext(entry, JSON_C_TO_STRING_PLAIN);
	char *line = NULL;
	int n = asprintf(&line, "%s\n", encoded);
	do {
		locked = flock(log, LOCK_EX);
	} while (locked && errno == EINTR);
	ok = !locked && n > 0 && write_all(log, line, (size_t)n) && !fsync(log);
	flock(log, LOCK_UN);
	close(log);
	free(line);
	json_object_put(entry);
done:
	if (fd >= 0) {
		flock(fd, LOCK_UN);
		close(fd);
	}
	if (!ok && !*error) {
		snprintf(error, size, "Cannot persist bridge admission/audit: %s", strerror(errno));
	}

	return ok;
}

struct admission {
	int denied;
	char reason[256];
};

static int admit_result(
	json_object *payload, json_object *answer, const char *extension, void *state) {

	(void)payload;
	(void)extension;
	struct admission *gate = state;
	json_object *accept = json_object_object_get(answer, "accept");
	if (accept &&
		(!json_object_is_type(accept, json_type_boolean) || !json_object_get_boolean(accept))) {
		gate->denied = 1;
		snprintf(gate->reason, sizeof(gate->reason), "%s",
			*field(answer, "reason") ? field(answer, "reason") : "External message rejected.");
	}

	return 0;
}

static json_object *deliver(json_object *request, pid_t peer) {

	if (!strcmp(field(request, "method"), "render_surface")) {
		if (closing || events_blocked()) {
			return failure("Surface host unavailable during shutdown or native callbacks");
		}
		return surfaces_serve(request, (int)peer);
	}
	if (!strcmp(field(request, "method"), "close_chat")) {
		char error[256] = "Chat close control unavailable; restart it with this build.";
		json_object *force = json_object_object_get(request, "force");
		if (force && !json_object_is_type(force, json_type_boolean)) {
			return failure("force must be boolean");
		}
		if (!closing &&
			(!close_handler ||
				!close_handler(json_object_get_boolean(force), error, sizeof(error)))) {
			return failure(error);
		}
		closing = 1;
		json_object *answer = json_object_new_object();
		json_object_object_add(answer, "accepted", json_object_new_boolean(1));
		return answer;
	}
	if (strcmp(field(request, "method"), "send_chat_message")) {
		return failure(
			"This socket accepts user-message delivery and chat close, never approvals or terminal commands.");
	}
	if (closing) {
		return failure("Chat is closing.");
	}
	if (!plain(request, "text", MESSAGE_LIMIT, 0)) {
		return failure("text must be nonempty UTF-8 without NUL, at most 1 MiB.");
	}
	if (queued == QUEUE_SIZE) {
		return failure("External-message queue is full.");
	}
	json_object *metadata = json_object_object_get(request, "origin");
	if (metadata &&
		(!json_object_is_type(metadata, json_type_object) ||
			strlen(json_object_to_json_string(metadata)) > 4096)) {
		return failure("origin must be an object of at most 4096 encoded bytes.");
	}
	if (events_blocked()) {
		return failure("Admission unavailable during a native callback; retry later.");
	}
	const char *actor = field(request, "actor");
	if (strlen(actor) > 128 ||
		strspn(actor, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:/ -") !=
			strlen(actor)) {
		return failure("Invalid origin actor.");
	}
	json_object *message = json_object_new_object(), *origin = json_object_new_object();
	unsigned char random[16];
	if (getrandom(random, sizeof(random), 0) != sizeof(random)) {
		json_object_put(message);
		json_object_put(origin);
		return failure("Cannot create delivery ID.");
	}
	char id[33];
	for (int i = 0; i < 16; i++) {
		snprintf(id + 2 * i, 3, "%02x", random[i]);
	}
	const char *via = !strcmp(field(request, "via"), "phi new") ? "phi new" : "phi send";
	json_object_object_add(origin, "via", json_object_new_string(via));
	json_object_object_add(origin, "actor", json_object_new_string(actor));
	json_object_object_add(origin, "pid", json_object_new_int64(peer));
	json_object_object_add(origin, "uid", json_object_new_int64(getuid()));
	json_object_object_add(origin, "delivery_id", json_object_new_string(id));
	if (metadata) {
		json_object_object_add(origin, "metadata", json_object_get(metadata));
	}
	json_object_object_add(message, "origin", origin);
	json_object_object_add(message, "text", json_object_new_string(field(request, "text")));
	struct admission gate = {0};
	if (events_emit("external_message_admission", message, admit_result, &gate) || gate.denied) {
		json_object_put(message);
		return failure(*gate.reason ? gate.reason : "External-message admission handler failed.");
	}
	char error[256] = "";
	if (!audit_admit(message, error, sizeof(error))) {
		json_object_put(message);
		return failure(error);
	}
	if (!chats_record_delivery(message)) {
		json_object_put(message);
		return failure(
			"Cannot persist full delivery; session saving must be enabled and writable.");
	}
	queue[queued++] = message;
	chats_bridge_status(endpoint, (int)queued);
	events_emit("external_message_delivered", message, NULL, NULL);
	json_object *answer = json_object_new_object();
	json_object_object_add(answer, "accepted", json_object_new_boolean(1));
	json_object_object_add(answer, "delivery_id", json_object_new_string(id));
	json_object_object_add(answer, "queued", json_object_new_int((int)queued));

	return answer;
}

int bridge_pending(void) {

	return queued != 0 || closing;
}

const char *bridge_socket(void) {

	return endpoint;
}

const char *bridge_origin_label(void) {

	return current_origin ? field(current_origin, "via") : "";
}

json_object *bridge_take_origin(void) {

	json_object *o = current_origin;
	current_origin = NULL;

	return o;
}

char *bridge_take(void) {

	if (!queued) {
		return NULL;
	}
	json_object *item = queue[0];
	char *text = strdup(field(item, "text"));
	if (!text) {
		return NULL;
	}
	json_object_put(current_origin);
	current_origin = json_object_get(json_object_object_get(item, "origin"));
	memmove(queue, queue + 1, (--queued) * sizeof(*queue));
	json_object_put(item);
	chats_bridge_status(endpoint, (int)queued);

	return text;
}

static void disconnect(struct connection *c) {

	if (c->fd >= 0) {
		close(c->fd);
	}
	free(c->data);
	free(c->reply);
	*c = (struct connection){.fd = -1};
}

int bridge_open(void) {

	closing = 0;
	const char *base = getenv("PHI_HOST_SOCKET");
	if (!base) {
		return 1; /* not hosted */
	}
	if (snprintf(endpoint, sizeof(endpoint), "%s.bridge", base) >= (int)sizeof(endpoint)) {
		endpoint[0] = 0;
		return 0;
	}
	for (int i = 0; i < CLIENTS; i++) {
		clients[i].fd = -1;
	}
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", endpoint);
	mode_t mask = umask(0077);
	int bound = listener >= 0 && !bind(listener, (struct sockaddr *)&address, sizeof(address));
	umask(mask);
	if (!bound || listen(listener, CLIENTS)) {
		if (bound) {
			unlink(endpoint);
		}
		if (listener >= 0) {
			close(listener);
		}
		listener = -1;
		endpoint[0] = 0;
		return 0;
	}
	chats_bridge_status(endpoint, 0);

	return 1;
}

void bridge_close(void) {

	if (listener >= 0) {
		close(listener);
		listener = -1;
		unlink(endpoint);
		for (int i = 0; i < CLIENTS; i++) {
			disconnect(&clients[i]);
		}
	}
	endpoint[0] = 0;
	while (queued) {
		json_object_put(queue[--queued]);
	}
	json_object_put(current_origin);
	current_origin = NULL;
}

void bridge_pump(void) {

	if (listener < 0 || pumping || events_blocked()) {
		return;
	}
	pumping = 1;
	for (int i = 0; i < CLIENTS; i++) {
		if (clients[i].fd < 0) {
			int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
			if (fd < 0) {
				break;
			}
			struct ucred peer;
			socklen_t size = sizeof(peer);
			if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != getuid()) {
				close(fd);
				continue;
			}
			clients[i] =
				(struct connection){.fd = fd, .pid = peer.pid, .deadline = clock_now() + 15};
		}
	}
	for (int i = 0; i < CLIENTS; i++) {
		struct connection *c = &clients[i];
		if (c->fd < 0) {
			continue;
		}
		if (clock_now() >= c->deadline) {
			disconnect(c);
			continue;
		}
		size_t budget = 256 * 1024;
		while (!c->reply && budget && c->fd >= 0) {
			void *where = c->head < 4 ? (void *)(c->header + c->head) : (void *)(c->data + c->used);
			size_t wanted = c->head < 4 ? 4 - c->head : c->length - c->used;
			if (wanted > budget) {
				wanted = budget;
			}
			ssize_t n = read(c->fd, where, wanted);
			if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
				break;
			}
			if (n <= 0) {
				disconnect(c);
				break;
			}
			budget -= (size_t)n;
			if (c->head < 4) {
				c->head += (size_t)n;
				if (c->head == 4) {
					c->length = ((size_t)c->header[0] << 24) | ((size_t)c->header[1] << 16) |
						((size_t)c->header[2] << 8) | c->header[3];
					if (!c->length || c->length > WIRE_LIMIT ||
						!(c->data = malloc(c->length + 1))) {
						disconnect(c);
						break;
					}
				}
			} else {
				c->used += (size_t)n;
			}
			if (c->data && c->used == c->length) {
				c->data[c->used] = 0;
				json_object *request = parse(c->data, c->used);
				json_object *reply = json_object_is_type(request, json_type_object)
					? deliver(request, c->pid)
					: failure("Invalid UTF-8 JSON request.");
				const char *text = json_object_to_json_string_ext(reply, JSON_C_TO_STRING_PLAIN);
				size_t length = strlen(text);
				c->reply = malloc(length + 4);
				if (c->reply) {
					c->reply[0] = (char)(length >> 24);
					c->reply[1] = (char)(length >> 16);
					c->reply[2] = (char)(length >> 8);
					c->reply[3] = (char)length;
					memcpy(c->reply + 4, text, length);
					c->reply_size = length + 4;
				}
				json_object_put(request);
				json_object_put(reply);
				if (!c->reply) {
					disconnect(c);
				}
				break;
			}
		}
		if (c->fd >= 0 && c->reply) {
			ssize_t n = send(c->fd, c->reply + c->sent, c->reply_size - c->sent, MSG_NOSIGNAL);
			if (n > 0) {
				c->sent += (size_t)n;
			}
			if (c->sent == c->reply_size || (!n) || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				disconnect(c);
			}
		}
	}
	pumping = 0;
}

static int transfer(int fd, void *data, size_t size, int sending, double deadline) {

	char *at = data;
	while (size) {
		int ms = (int)((deadline - clock_now()) * 1000);
		if (ms <= 0) {
			errno = ETIMEDOUT;
			return 0;
		}
		struct pollfd p = {fd, sending ? POLLOUT : POLLIN, 0};
		int ready = poll(&p, 1, ms);
		if (ready < 0 && errno == EINTR) {
			return 0;
		}
		if (ready <= 0) {
			if (!ready) {
				errno = ETIMEDOUT;
			}
			return 0;
		}
		ssize_t n = sending ? send(fd, at, size, MSG_NOSIGNAL) : read(fd, at, size);
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			continue;
		}
		if (n <= 0) {
			if (!n) {
				errno = EPIPE;
			}
			return 0;
		}
		at += n;
		size -= (size_t)n;
	}

	return 1;
}

/* Consumes both objects; authenticated transport shared by messages and close. */
static json_object *exchange(json_object *chat, json_object *wire) {

	const char *path = field(chat, "bridge_socket");
	if (!*path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
		json_object_put(chat);
		json_object_put(wire);
		return failure("Chat lacks the bridge service; restart it with this build.");
	}
	if ((pid_t)json_object_get_int64(json_object_object_get(chat, "pid")) == getpid()) {
		json_object *reply = deliver(wire, getpid());
		json_object_put(wire);
		json_object_put(chat);
		return reply;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	json_object *reply = NULL;
	double deadline = clock_now() + 15;
	if (fd >= 0 && !connect(fd, (struct sockaddr *)&address, sizeof(address))) {
		struct ucred peer;
		socklen_t size = sizeof(peer);
		if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) && peer.uid == getuid() &&
			peer.pid == json_object_get_int64(json_object_object_get(chat, "pid"))) {
			const char *text = json_object_to_json_string_ext(wire, JSON_C_TO_STRING_PLAIN);
			size_t length = strlen(text);
			unsigned char header[4] = {(unsigned char)(length >> 24), (unsigned char)(length >> 16),
				(unsigned char)(length >> 8), (unsigned char)length};
			if (length <= WIRE_LIMIT && transfer(fd, header, 4, 1, deadline) &&
				transfer(fd, (void *)text, length, 1, deadline) &&
				transfer(fd, header, 4, 0, deadline)) {
				size_t count = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
					((size_t)header[2] << 8) | header[3];
				if (count && count <= 65536) {
					char *body = malloc(count + 1);
					if (body && transfer(fd, body, count, 0, deadline)) {
						body[count] = 0;
						reply = parse(body, count);
					}
					free(body);
				}
			}
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	json_object_put(chat);
	json_object_put(wire);
	if (json_object_is_type(reply, json_type_object) &&
		(*field(reply, "error") ||
			json_object_get_boolean(json_object_object_get(reply, "accepted")))) {
		return reply;
	}
	json_object_put(reply);

	return failure(
		"Request not acknowledged (timeout/disconnect). Outcome may be unknown; do not blindly retry.");
}

static json_object *send_message(const char *caller, json_object *request) {

	char error[256];
	if (!plain(request, "text", MESSAGE_LIMIT, 0)) {
		return failure("text must be nonempty and at most 1 MiB without NUL.");
	}
	json_object *chat = chats_lookup(field(request, "id"), error, sizeof(error));
	if (!chat) {
		return failure(error);
	}
	if (!json_object_get_boolean(json_object_object_get(chat, "live"))) {
		json_object_put(chat);
		return failure("Chat ended; delivery refused.");
	}
	json_object *wire = json_object_new_object();
	json_object_object_add(wire, "method", json_object_new_string("send_chat_message"));
	json_object_object_add(wire, "text", json_object_get(json_object_object_get(request, "text")));
	json_object_object_add(wire, "actor", json_object_new_string(caller ? caller : "local"));
	json_object_object_add(wire, "via", json_object_new_string(field(request, "via")));
	json_object *metadata = json_object_object_get(request, "origin");
	if (metadata) {
		json_object_object_add(wire, "origin", json_object_get(metadata));
	}

	return exchange(chat, wire);
}

json_object *bridge_request_close(const char *id, int force) {

	char error[256];
	json_object *chat = chats_lookup(id, error, sizeof(error));
	if (!chat) {
		return failure(error);
	}
	json_object *wire = json_object_new_object();
	json_object_object_add(wire, "method", json_object_new_string("close_chat"));
	json_object_object_add(wire, "force", json_object_new_boolean(force));

	return exchange(chat, wire);
}

static json_object *item_text(json_object *item) {

	json_object *content = json_object_object_get(item, "content");
	if (json_object_is_type(content, json_type_string)) {
		return json_object_get(content);
	}
	char *text = strdup("");
	size_t used = 0;
	if (!text) {
		return NULL;
	}
	if (json_object_is_type(content, json_type_array)) {
		for (size_t i = 0; i < json_object_array_length(content); i++) {
			json_object *part = json_object_array_get_idx(content, i);
			const char *type = field(part, "type");
			if (strcmp(type, "output_text") && strcmp(type, "input_text")) {
				continue;
			}
			const char *piece = field(part, "text");
			size_t n = strlen(piece);
			if (n > INT_MAX - used) {
				free(text);
				return NULL;
			}
			char *grown = realloc(text, used + n + 1);
			if (!grown) {
				free(text);
				return NULL;
			}
			text = grown;
			memcpy(text + used, piece, n + 1);
			used += n;
		}
	}
	json_object *out = json_object_new_string_len(text, (int)used);
	free(text);

	return out;
}

static json_object *branch(json_object *request) {

	char error[256];
	json_object *status = chats_lookup(field(request, "id"), error, sizeof(error));
	if (!status) {
		return failure(error);
	}
	json_object *history = NULL, *entries = json_object_new_array();
	const char *path = field(status, "session_file");
	if (*path) {
		struct session_entry entry;
		struct session_tree tree;
		if (!session_from_path(path, &entry, error, sizeof(error))) {
			json_object_put(status);
			json_object_put(entries);
			return failure(error);
		}
		if (entry.version == 2) {
			if (session_tree_index(&entry, &tree, error, sizeof(error))) {
				json_object *head = json_object_object_get(status, "session_head");
				uint64_t selected =
					head && json_object_get_boolean(json_object_object_get(status, "live"))
					? (uint64_t)json_object_get_int64(head)
					: tree.head;
				history = session_tree_history(&tree, selected, error, sizeof(error));
				session_tree_free(&tree);
			}
		} else {
			history = session_load(&entry, error, sizeof(error));
		}
		if (!history) {
			json_object_put(status);
			json_object_put(entries);
			return failure(error);
		}
	}
	json_object *last = json_object_new_string("");
	for (size_t i = 0; history && i < json_object_array_length(history); i++) {
		json_object *item = json_object_array_get_idx(history, i);
		if (history_is_hidden(item)) {
			continue;
		}
		const char *role = field(item, "role"), *type = field(item, "type");
		if (!strcmp(role, "user") || !strcmp(role, "assistant")) {
			json_object *entry = json_object_new_object(), *text = item_text(item);
			if (!text) {
				json_object_put(entry);
				json_object_put(history);
				json_object_put(entries);
				json_object_put(status);
				json_object_put(last);
				return failure("Cannot allocate full message text.");
			}
			json_object_object_add(entry, "role", json_object_new_string(role));
			json_object_object_add(entry, "text", text);
			json_object *origin = json_object_object_get(item, "_phi_origin");
			if (origin) {
				json_object_object_add(entry, "origin", json_object_get(origin));
			}
			json_object_array_add(entries, entry);
			if (!strcmp(role, "assistant")) {
				json_object_put(last);
				last = json_object_get(text);
			}
		} else if (!strcmp(type, "function_call")) {
			json_object *entry = json_object_new_object(),
						*args = json_tokener_parse(field(item, "arguments"));
			const char *detail = field(args, "command");
			if (!*detail) {
				detail = field(args, "path");
			}
			json_object_object_add(entry, "role", json_object_new_string("tool"));
			json_object_object_add(entry, "name", json_object_new_string(field(item, "name")));
			json_object_object_add(
				entry, "call_id", json_object_new_string(field(item, "call_id")));
			size_t n = strcspn(detail, "\n");
			if (n > 1024) {
				n = 1024;
			}
			while (n && ((unsigned char)detail[n] & 0xc0) == 0x80) {
				n--;
			}
			json_object_object_add(entry, "summary", json_object_new_string_len(detail, (int)n));
			for (size_t j = i + 1; j < json_object_array_length(history); j++) {
				json_object *answer = json_object_array_get_idx(history, j);
				if (strcmp(field(answer, "type"), "function_call_output") ||
					strcmp(field(answer, "call_id"), field(item, "call_id"))) {
					continue;
				}
				json_object *result = json_tokener_parse(field(answer, "output"));
				if (result) {
					json_object_object_add(entry, "result", result);
				} else {
					json_object_object_add(
						entry, "result", json_object_new_string(field(answer, "output")));
				}
				break;
			}
			json_object_put(args);
			json_object_array_add(entries, entry);
		}
	}
	json_object_put(history);
	json_object *out = json_object_new_object();
	json_object_object_add(out, "chat", status);
	json_object_object_add(out, "entries", entries);
	json_object_object_add(out, "last_assistant", last);

	return out;
}

static int watch_poll(struct pollfd *fds, nfds_t count, double deadline) {

	for (;;) {
		if (cancel_wait && cancel_wait()) {
			errno = EINTR;
			return -1;
		}
		double remaining = deadline - clock_now();
		if (remaining <= 0) {
			return 0;
		}
		int ms = remaining * 1000 > INT_MAX ? INT_MAX : (int)(remaining * 1000) + 1;
		if (cancel_wait && ms > 50) {
			ms = 50; /* UI cancellation only, never status polling */
		}
		int ready = poll(fds, count, ms);
		if (ready || clock_now() >= deadline) {
			return ready;
		}
	}
}

/* Watch directories, not replaced status inodes. Subscribe before the first
   snapshot, and include pidfds so crashes are noticed without status writes. */
static json_object *watch(json_object *request, int ready_only) {

	json_object *ids = json_object_object_get(request, "ids");
	if (!json_object_is_type(ids, json_type_array) || !json_object_array_length(ids) ||
		json_object_array_length(ids) > 64) {
		return failure("ids must contain 1..64 chat IDs.");
	}
	int timeout = ready_only ? 20 : 7200;
	json_object *t = json_object_object_get(request, "timeout");
	if (t) {
		int64_t n = json_object_get_int64(t);
		if (!json_object_is_type(t, json_type_int) || n < 0 || n > 604800) {
			return failure("timeout must be 0..604800 seconds.");
		}
		timeout = (int)n;
	}
	int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	char durable[4096], runtime[4096];
	if (fd < 0 || !private_state(durable, sizeof(durable))) {
		if (fd >= 0) {
			close(fd);
		}
		return failure("Cannot create status watcher.");
	}
	size_t n = strlen(durable);
	snprintf(durable + n, sizeof(durable) - n, "/chats");
	const char *run = getenv("XDG_RUNTIME_DIR");
	if (run && *run == '/') {
		snprintf(runtime, sizeof(runtime), "%s/phi", run);
	} else {
		snprintf(runtime, sizeof(runtime), "/tmp/phi-%u", (unsigned)getuid());
	}
	if (!phi_make_directories(durable) || !phi_make_directories(runtime) ||
		inotify_add_watch(fd, durable, IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE | IN_CREATE) < 0 ||
		inotify_add_watch(fd, runtime, IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE | IN_CREATE) < 0) {
		close(fd);
		return failure("Cannot watch chat status directories.");
	}
	double deadline = clock_now() + timeout;
	json_object *out = NULL;
	for (;;) {
		struct pollfd fds[65] = {{fd, POLLIN, 0}};
		size_t count = 1;
		int changed = 0;
		for (size_t i = 0; i < json_object_array_length(ids); i++) {
			const char *id = json_object_get_string(json_object_array_get_idx(ids, i));
			char error[256];
			json_object *status = id ? chats_lookup(id, error, sizeof(error)) : NULL;
			if (!status) {
				if (!ready_only) {
					out = failure(id ? error : "Invalid ID.");
					break;
				}
				continue;
			}
			int live = json_object_get_boolean(json_object_object_get(status, "live"));
			int finished = ready_only ? live && *field(status, "bridge_socket")
									  : strcmp(field(status, "state"), "working") != 0;
			if (finished) {
				json_object *one = json_object_new_object();
				json_object_object_add(one, "id", json_object_new_string(field(status, "id")));
				out = ready_only ? json_object_get(status) : branch(one);
				json_object_put(one);
				json_object_put(status);
				break;
			}
#ifdef SYS_pidfd_open
			int pidfd = live
				? (int)syscall(SYS_pidfd_open,
					  (pid_t)json_object_get_int64(json_object_object_get(status, "pid")), 0)
				: -1;
			if (pidfd >= 0) {
				fds[count++] = (struct pollfd){pidfd, POLLIN, 0};
			} else if (live && errno == ESRCH) {
				changed = 1;
			} else if (live) {
				snprintf(
					error, sizeof(error), "Cannot watch chat process exit: %s", strerror(errno));
				out = failure(error);
				json_object_put(status);
				break;
			}
#endif
			json_object_put(status);
		}
		if (out || changed) {
			for (size_t i = 1; i < count; i++) {
				close(fds[i].fd);
			}
			if (out) {
				break;
			}
			continue;
		}
		int event = watch_poll(fds, (nfds_t)count, deadline);
		for (size_t i = 1; i < count; i++) {
			close(fds[i].fd);
		}
		if (event <= 0) {
			out = failure(event < 0 ? "Wait interrupted." : "Wait timed out.");
			json_object_object_add(out, "exit_code", json_object_new_int(event < 0 ? 130 : 124));
			break;
		}
		char events[8192];
		for (int drains = 0; drains < 16 && read(fd, events, sizeof(events)) > 0; drains++) {
		}
	}
	close(fd);

	return out;
}

json_object *bridge_call(const char *caller, const char *method, json_object *request) {

	if (!method || !json_object_is_type(request, json_type_object)) {
		return failure("Bridge request must be an object with a method.");
	}
	if (!strcmp(method, "list_chats")) {
		return chats_snapshot();
	}
	if (!strcmp(method, "get_chat")) {
		char error[256];
		json_object *out = chats_lookup(field(request, "id"), error, sizeof(error));
		return out ? out : failure(error);
	}
	if (!strcmp(method, "get_chat_branch")) {
		return branch(request);
	}
	if (!strcmp(method, "watch_chats")) {
		return watch(request, 0);
	}
	if (!strcmp(method, "send_chat_message")) {
		return send_message(caller, request);
	}
	if (!strcmp(method, "create_chat")) {
		const char *encoded = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
		json_object *valid = parse(encoded, strlen(encoded));
		if (!valid) {
			return failure("Creation options must contain valid UTF-8 JSON.");
		}
		json_object_put(valid);
		json_object *metadata = json_object_object_get(request, "origin");
		if (metadata &&
			(!json_object_is_type(metadata, json_type_object) ||
				strlen(json_object_to_json_string(metadata)) > 4096)) {
			return failure("Invalid origin metadata.");
		}
		for (const unsigned char *p = (const unsigned char *)field(request, "title"); *p; p++) {
			if (*p < 32 || *p == 127) {
				return failure("title must be a single line without control characters.");
			}
		}
		const char *keys[] = {"cwd", "model", "thinking", "title"};
		const size_t limits[] = {4095, 95, 15, 127};
		for (int i = 0; i < 4; i++) {
			if (json_object_object_get(request, keys[i]) &&
				!plain(request, keys[i], limits[i], 1)) {
				return failure("Invalid creation option type or length.");
			}
		}
		const char *model = field(request, "model"), *effort = field(request, "thinking");
		if ((*model && !settings_model_valid(model)) || (*effort && !settings_effort(effort))) {
			return failure("Invalid model or thinking level.");
		}
		if (json_object_object_get(request, "text") && !plain(request, "text", MESSAGE_LIMIT, 1)) {
			return failure("Invalid initial message (maximum 1 MiB).");
		}
		char id[32], error[256] = "";
		if (!chats_launch(request, id, sizeof(id), error, sizeof(error))) {
			return failure(*error ? error : "Cannot launch chat.");
		}
		json_object *query = json_object_new_object(), *ids = json_object_new_array();
		json_object_array_add(ids, json_object_new_string(id));
		json_object_object_add(query, "ids", ids);
		json_object *ready = watch(query, 1);
		json_object_put(query);
		if (*field(ready, "error")) {
			json_object_object_add(ready, "id", json_object_new_string(id));
			return ready;
		}
		json_object_put(ready);
		json_object *out = json_object_new_object();
		json_object_object_add(out, "id", json_object_new_string(id));
		if (*field(request, "text")) {
			json_object *send = json_object_new_object();
			json_object_object_add(send, "id", json_object_new_string(id));
			json_object_object_add(
				send, "text", json_object_get(json_object_object_get(request, "text")));
			json_object_object_add(send, "via", json_object_new_string("phi new"));
			if (metadata) {
				json_object_object_add(send, "origin", json_object_get(metadata));
			}
			json_object *ack = send_message(caller, send);
			json_object_put(send);
			if (*field(ack, "error")) {
				json_object_object_add(ack, "id", json_object_new_string(id));
				json_object_put(out);
				return ack;
			}
			json_object_object_add(out, "delivery", ack);
		}
		return out;
	}

	return NULL; /* not a bridge method */
}

static char *read_input(const char *path) {

	if (strcmp(path, "-")) {
		return phi_read_file(path, MESSAGE_LIMIT, PHI_READ_FOLLOW, NULL);
	}
	char *out = malloc(MESSAGE_LIMIT + 1);
	if (!out) {
		return NULL;
	}
	size_t used = 0;
	for (;;) {
		ssize_t n = read(STDIN_FILENO, out + used, MESSAGE_LIMIT + 1 - used);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 || (n > 0 && used + (size_t)n > MESSAGE_LIMIT)) {
			free(out);
			errno = EFBIG;
			return NULL;
		}
		if (!n) {
			break;
		}
		used += (size_t)n;
	}
	if (memchr(out, 0, used)) {
		free(out);
		errno = EINVAL;
		return NULL;
	}
	out[used] = 0;

	return out;
}

int bridge_cli(int argc, char **argv) {

	if (argc < 2) {
		return -1;
	}
	const char *command = argv[1], *method = NULL;
	if (!strcmp(command, "ls") && argc > 2) {
		method = "list_chats";
	} else if (!strcmp(command, "tail")) {
		method = "get_chat_branch";
	} else if (!strcmp(command, "wait")) {
		method = "watch_chats";
	} else if (!strcmp(command, "send")) {
		method = "send_chat_message";
	} else if (!strcmp(command, "new")) {
		method = "create_chat";
	} else {
		return -1;
	}
	json_object *request = json_object_new_object(), *ids = json_object_new_array();
	int json = !strcmp(method, "list_chats"), any = 0, code = 0;
	long count = 1;
	const char *input = NULL;
	for (int i = 2; i < argc; i++) {
		const char *arg = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(arg, "--json")) {
			json = 1;
		} else if (!strcmp(arg, "--any") && !strcmp(command, "wait")) {
			any = 1;
		} else if ((!strcmp(arg, "--timeout") || !strcmp(arg, "--cwd") || !strcmp(arg, "--model") ||
					   !strcmp(arg, "--thinking") || !strcmp(arg, "--title") ||
					   !strcmp(arg, "--file")) &&
			next) {
			i++;
			int file_option = !strcmp(arg, "--file"), timeout_option = !strcmp(arg, "--timeout");
			if ((file_option && strcmp(command, "new") && strcmp(command, "send")) ||
				(timeout_option && strcmp(command, "wait")) ||
				(!file_option && !timeout_option && strcmp(command, "new"))) {
				code = 2;
				break;
			}
			if (file_option) {
				if (input) {
					code = 2;
					break;
				}
				input = next;
			} else if (!strcmp(arg, "--timeout")) {
				char *end;
				long n = strtol(next, &end, 10);
				if (*end || n < 0 || n > 604800) {
					code = 2;
					break;
				}
				json_object_object_add(request, "timeout", json_object_new_int64(n));
			} else {
				json_object_object_add(request, arg + 2, json_object_new_string(next));
			}
		} else if (arg[0] == '-' && strcmp(arg, "-")) {
			code = 2;
			break;
		} else if (!strcmp(command, "new") && !json_object_object_get(request, "text")) {
			json_object_object_add(request, "text", json_object_new_string(arg));
		} else if (!strcmp(command, "wait") && arg[0] != '-') {
			json_object_array_add(ids, json_object_new_string(arg));
		} else if ((!strcmp(command, "tail") || !strcmp(command, "send")) &&
			!json_object_object_get(request, "id")) {
			json_object_object_add(request, "id", json_object_new_string(arg));
		} else if (!strcmp(command, "send") && !json_object_object_get(request, "text")) {
			if (!strcmp(arg, "-")) {
				input = arg;
			} else {
				json_object_object_add(request, "text", json_object_new_string(arg));
			}
		} else if (!strcmp(command, "tail")) {
			char *end;
			count = strtol(arg, &end, 10);
			if (*end || count < 1 || count > 100000) {
				code = 2;
				break;
			}
		} else {
			code = 2;
			break;
		}
	}
	if (!strcmp(command, "wait") &&
		((!any && json_object_array_length(ids) != 1) || !json_object_array_length(ids))) {
		code = 2;
	}
	json_object_object_add(request, "ids", ids);
	if (input) {
		if (json_object_object_get(request, "text")) {
			code = 2;
		}
		char *text = code ? NULL : read_input(input);
		if (!text) {
			code = 2;
		} else {
			json_object_object_add(request, "text", json_object_new_string(text));
			free(text);
		}
	}
	if (code) {
		fprintf(stderr,
			"Invalid bridge arguments or unreadable input. See docs/agent-bridge-spec.md.\n");
		json_object_put(request);
		return code;
	}
	json_object *answer = bridge_call("cli", method, request);
	json_object_put(request);
	if (!answer) {
		return 1;
	}
	if (*field(answer, "error")) {
		code = json_object_object_get(answer, "exit_code")
			? json_object_get_int(json_object_object_get(answer, "exit_code"))
			: 1;
		if (json) {
			puts(json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN));
		} else {
			fprintf(stderr, "%s\n", field(answer, "error"));
			if (!strcmp(command, "new") && *field(answer, "id")) {
				fprintf(stderr, "Chat ID: %s (creation/delivery did not complete).\n",
					field(answer, "id"));
			}
		}
	} else if (!strcmp(command, "new")) {
		puts(json ? json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN)
				  : field(answer, "id"));
	} else if (!strcmp(command, "wait")) {
		json_object *chat = json_object_object_get(answer, "chat");
		const char *state = field(chat, "state");
		code = !strcmp(state, "needs_input") ? 2 : !strcmp(state, "ended") ? 3 : 0;
		if (json) {
			puts(json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN));
		} else {
			printf("%s\n%s\n", state, field(answer, "last_assistant"));
			if (code == 2) {
				puts(json_object_to_json_string(json_object_object_get(chat, "approval")));
			}
		}
	} else if (!strcmp(command, "tail")) {
		json_object *entries = json_object_object_get(answer, "entries");
		size_t length = json_object_array_length(entries);
		if (count == 1 && !json) {
			puts(field(answer, "last_assistant"));
		} else {
			json_object *tail = json_object_new_array();
			for (size_t i = length > (size_t)count ? length - (size_t)count : 0; i < length; i++) {
				json_object_array_add(tail, json_object_get(json_object_array_get_idx(entries, i)));
			}
			if (json) {
				json_object_object_add(answer, "entries", tail);
				puts(json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN));
			} else {
				for (size_t i = 0; i < json_object_array_length(tail); i++) {
					json_object *e = json_object_array_get_idx(tail, i);
					if (!strcmp(field(e, "role"), "tool")) {
						bridge_print_tool(stdout, e);
					} else {
						printf("%s: %s\n", field(e, "role"), field(e, "text"));
					}
				}
				json_object_put(tail);
			}
		}
		json_object *chat = json_object_object_get(answer, "chat"),
					*approval = json_object_object_get(chat, "approval");
		if (!json && approval) {
			printf("Pending approval: %s\n", json_object_to_json_string(approval));
		}
	} else if (json) {
		puts(json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN));
	}
	json_object_put(answer);

	return code;
}
