#define _GNU_SOURCE
#include "surfaces.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#define PROVIDERS 16
#define INSTANCES 32
#define CONTEXT_LIMIT 65536u
#define RESPONSE_LIMIT (PHI_SURFACE_MAX_CELLS * 12u + 4096u)

static double stamp(void) {

	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);

	return t.tv_sec + t.tv_nsec / 1e9;
}

static const char *text(json_object *o, const char *key) {

	json_object *v = json_object_object_get(o, key);

	return json_object_is_type(v, json_type_string) ? json_object_get_string(v) : "";
}

static int identifier(const char *id) {

	if (!id || !*id || strlen(id) >= 64) {
		return 0;
	}
	for (const char *p = id; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
			return 0;
		}
	}

	return 1;
}

/* Instance slots are composition IDs, not a whitelist of core coordinates. */
static int slot_valid(const char *slot) {

	return identifier(slot);
}

static struct provider {
	char id[64], owner[128], path[4096];
	void *module;
	const struct phi_surface_v1 *api;
	const struct phi_renderer_v1 *renderer;
	int attempted;
} providers[PROVIDERS];

static struct instance {
	struct provider *provider;
	void *state;
	int pid;
	int64_t serial;
	char slot[64];
	double used;
} instances[INSTANCES];

static json_object *(*discover)(void);
static double last_remote;
static unsigned registry_generation;

unsigned surfaces_generation(void) {

	return registry_generation;
}

void surfaces_set_discovery(json_object *(*snapshot)(void)) {

	discover = snapshot;
}

int surfaces_remote_active(void) {

	return last_remote > 0 && stamp() - last_remote < .3;
}

static void drop_instance(struct instance *i) {

	if (i->state) {
		i->provider->api->destroy(i->state);
	}
	memset(i, 0, sizeof(*i));
}

static void drop_provider(struct provider *p) {

	for (int i = 0; i < INSTANCES; i++) {
		if (instances[i].provider == p) {
			drop_instance(&instances[i]);
		}
	}
	if (p->module) {
		dlclose(p->module);
	}
	memset(p, 0, sizeof(*p));
}

int surfaces_register(
	const char *owner, const char *id, const char *path, char *error, size_t capacity) {

	if (!owner || !*owner || strlen(owner) >= 128 || !identifier(id)) {
		snprintf(error, capacity, "Invalid surface owner or ID");
		return 0;
	}
	struct provider *p = NULL, *empty = NULL;
	for (int i = 0; i < PROVIDERS; i++) {
		if (!*providers[i].id && !empty) {
			empty = &providers[i];
		}
		if (!strcmp(providers[i].id, id)) {
			p = &providers[i];
		}
	}
	if (p && strcmp(p->owner, owner)) {
		snprintf(error, capacity, "Surface ID belongs to another extension");
		return 0;
	}
	char resolved[4096];
	if (path && (path[0] != '/' || !realpath(path, resolved))) {
		snprintf(error, capacity, "Surface needs an existing absolute library path");
		return 0;
	}
	if (p) {
		drop_provider(p);
	}
	registry_generation++;
	if (!path) {
		return 1;
	}
	if (!p) {
		p = empty;
	}
	if (!p) {
		snprintf(error, capacity, "Too many registered surfaces");
		return 0;
	}
	snprintf(p->id, sizeof(p->id), "%s", id);
	snprintf(p->owner, sizeof(p->owner), "%s", owner);
	snprintf(p->path, sizeof(p->path), "%s", resolved);

	return 1;
}

void surfaces_forget(const char *owner) {

	for (int i = 0; i < PROVIDERS; i++) {
		if (*providers[i].id && (!owner || !strcmp(owner, providers[i].owner))) {
			drop_provider(&providers[i]);
			registry_generation++;
		}
	}
}

static struct provider *find_provider(const char *id) {

	for (int i = 0; i < PROVIDERS; i++) {
		if (*providers[i].id && !strcmp(providers[i].id, id)) {
			return &providers[i];
		}
	}

	return NULL;
}

static int load_provider(struct provider *p) {

	if (p->attempted) {
		return p->api != NULL || p->renderer != NULL;
	}
	p->attempted = 1;
	p->module = dlopen(p->path, RTLD_NOW | RTLD_LOCAL);
	if (!p->module) {
		return 0;
	}
	const struct phi_surface_v1 *(*entry)(void) =
		(const struct phi_surface_v1 *(*)(void))dlsym(p->module, "phi_surface_entry");
	const struct phi_surface_v1 *api = entry ? entry() : NULL;
	if (api && api->abi == PHI_SURFACE_ABI && api->size >= sizeof(*api) && api->create &&
		api->destroy && api->render && api->delay_ms) {
		p->api = api;
	}
	const struct phi_renderer_v1 *(*renderer_entry)(void) =
		(const struct phi_renderer_v1 *(*)(void))dlsym(p->module, "phi_renderer_entry");
	const struct phi_renderer_v1 *renderer = renderer_entry ? renderer_entry() : NULL;
	if (renderer && renderer->abi == PHI_RENDERER_ABI && renderer->size >= sizeof(*renderer) &&
		renderer->render) {
		p->renderer = renderer;
	}
	if (p->api || p->renderer) {
		return 1;
	}
	dlclose(p->module);
	p->module = NULL;

	return 0;
}

int surfaces_render_semantic(const char *id, int width, int max_rows, json_object *context,
	struct phi_widget_cell *cells, size_t capacity) {

	if (!identifier(id) || width < 1 || width > 1024 || max_rows < 1 ||
		max_rows > PHI_RENDERER_MAX_ROWS ||
		(size_t)width * (size_t)max_rows > PHI_RENDERER_MAX_CELLS ||
		capacity < (size_t)width * (size_t)max_rows || !cells || !context) {
		return 0;
	}
	const char *encoded = json_object_to_json_string_ext(context, JSON_C_TO_STRING_PLAIN);
	if (!encoded || strlen(encoded) > PHI_RENDERER_MAX_JSON) {
		return 0;
	}
	struct provider *p = find_provider(id);
	if (!p || !load_provider(p) || !p->renderer) {
		return 0;
	}
	size_t count = (size_t)width * (size_t)max_rows;
	memset(cells, 0, count * sizeof(*cells));
	struct phi_renderer_frame frame = {
		.size = sizeof(frame), .width = width, .max_rows = max_rows, .context_json = encoded};
	int rows = p->renderer->render(&frame, cells, count);
	if (rows < 1 || rows > max_rows) {
		return 0;
	}
	for (size_t i = 0; i < (size_t)rows * (size_t)width; i++) {
		uint32_t g = cells[i].glyph;
		if ((g && g < 32) || g == 127 || g > 0x10ffff || (g >= 0xd800 && g <= 0xdfff) ||
			(cells[i].flags & ~3u)) {
			return 0;
		}
	}

	return rows;
}

static struct instance *get_instance(
	struct provider *p, const char *slot, int pid, int64_t serial) {

	struct instance *choice = NULL;
	for (int i = 0; i < INSTANCES; i++) {
		struct instance *v = &instances[i];
		if (v->provider == p && v->pid == pid && v->serial == serial && !strcmp(v->slot, slot)) {
			v->used = stamp();
			return v;
		}
		if (!choice || (!v->provider && choice->provider) ||
			(v->provider && choice->provider && v->used < choice->used)) {
			choice = v;
		}
	}
	if (choice->provider) {
		drop_instance(choice);
	}
	choice->provider = p;
	choice->pid = pid;
	choice->serial = serial;
	choice->used = stamp();
	snprintf(choice->slot, sizeof(choice->slot), "%s", slot);
	choice->state = p->api->create();
	if (!choice->state) {
		memset(choice, 0, sizeof(*choice));
		return NULL;
	}

	return choice;
}

static int render_local(const char *id, const char *slot, int width, int height,
	json_object *context, int pid, int64_t serial, const struct phi_surface_event *event,
	struct phi_widget_cell *cells, int *delay) {

	struct provider *p = find_provider(id);
	if (!p || !load_provider(p) || !p->api) {
		return 0;
	}
	const char *encoded =
		context ? json_object_to_json_string_ext(context, JSON_C_TO_STRING_PLAIN) : "{}";
	if (strlen(encoded) > CONTEXT_LIMIT) {
		return 0;
	}
	struct instance *i = get_instance(p, slot, pid, serial);
	if (!i) {
		return 0;
	}
	struct phi_surface_frame frame = {.size = sizeof(frame),
		.slot = slot,
		.width = width,
		.height = height,
		.time = stamp(),
		.context_json = encoded};
	if (event && p->api->input) {
		p->api->input(i->state, &frame, event);
	}
	size_t count = (size_t)width * (size_t)height;
	memset(cells, 0, count * sizeof(*cells));
	if (!p->api->render(i->state, &frame, cells, count)) {
		return 0;
	}
	int ms = p->api->delay_ms(i->state);
	*delay = ms <= 0 ? 0 : ms < 50 ? 50 : ms > 2000 ? 2000 : ms;

	return 1;
}

/* Explicit little-endian wire cells; no pointers, paths, padding bytes or code. */
static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *pack(const struct phi_widget_cell *cells, size_t count) {

	size_t bytes = count * 8, length = ((bytes + 2) / 3) * 4;
	unsigned char *raw = malloc(bytes);
	char *out = malloc(length + 1);
	if (!raw || !out) {
		free(raw);
		free(out);
		return NULL;
	}
	for (size_t i = 0; i < count; i++) {
		uint32_t g = cells[i].glyph;
		for (int b = 0; b < 4; b++) {
			raw[i * 8 + b] = (unsigned char)(g >> (8 * b));
		}
		raw[i * 8 + 4] = cells[i].foreground;
		raw[i * 8 + 5] = cells[i].background;
		raw[i * 8 + 6] = (unsigned char)cells[i].flags;
		raw[i * 8 + 7] = (unsigned char)(cells[i].flags >> 8);
	}
	size_t at = 0;
	for (size_t i = 0; i < bytes; i += 3) {
		unsigned value = (unsigned)raw[i] << 16;
		if (i + 1 < bytes) {
			value |= (unsigned)raw[i + 1] << 8;
		}
		if (i + 2 < bytes) {
			value |= raw[i + 2];
		}
		out[at++] = alphabet[(value >> 18) & 63];
		out[at++] = alphabet[(value >> 12) & 63];
		out[at++] = i + 1 < bytes ? alphabet[(value >> 6) & 63] : '=';
		out[at++] = i + 2 < bytes ? alphabet[value & 63] : '=';
	}
	out[at] = 0;
	free(raw);

	return out;
}

static int unbase(unsigned char c) {

	const char *p = c ? strchr(alphabet, c) : NULL;

	return p ? (int)(p - alphabet) : -1;
}

static int cell_equal(struct phi_widget_cell a, struct phi_widget_cell b) {

	return a.glyph == b.glyph && a.foreground == b.foreground && a.background == b.background &&
		a.flags == b.flags;
}

static int unpack(const char *encoded, struct phi_widget_cell *cells, size_t count, int *changed) {

	size_t bytes = count * 8, length = strlen(encoded), at = 0;
	if (length != ((bytes + 2) / 3) * 4) {
		return 0;
	}
	unsigned char *raw = malloc(bytes);
	if (!raw) {
		return 0;
	}
	for (size_t i = 0; i < length; i += 4) {
		unsigned value = 0;
		for (int j = 0; j < 4; j++) {
			int n = unbase((unsigned char)encoded[i + j]);
			if (n < 0 && !(encoded[i + j] == '=' && i + 4 == length && j >= 2)) {
				free(raw);
				return 0;
			}
			value = (value << 6) | (unsigned)(n < 0 ? 0 : n);
		}
		for (int b = 2; b >= 0 && at < bytes; b--) {
			raw[at++] = (unsigned char)(value >> (b * 8));
		}
	}
	for (size_t i = 0; i < count; i++) {
		uint32_t g = 0;
		for (int b = 0; b < 4; b++) {
			g |= (uint32_t)raw[i * 8 + b] << (8 * b);
		}
		struct phi_widget_cell cell = {.glyph = g,
			.foreground = raw[i * 8 + 4],
			.background = raw[i * 8 + 5],
			.flags = (uint16_t)(raw[i * 8 + 6] | raw[i * 8 + 7] << 8)};
		if (!cell_equal(cells[i], cell)) {
			*changed = 1;
		}
		cells[i] = cell;
	}
	free(raw);

	return 1;
}

json_object *surfaces_serve(json_object *request, int peer_pid) {

	json_object *out = json_object_new_object();
	json_object_object_add(out, "accepted", json_object_new_boolean(1));
	json_object_object_add(out, "available", json_object_new_boolean(0));
	const char *id = text(request, "provider"), *slot = text(request, "slot");
	int w = json_object_get_int(json_object_object_get(request, "width")),
		h = json_object_get_int(json_object_object_get(request, "height"));
	if (!identifier(id) || !slot_valid(slot) || w < 1 || w > 1024 || h < 1 || h > 512 ||
		(size_t)w * (size_t)h > PHI_SURFACE_MAX_CELLS) {
		return out;
	}
	json_object *context = json_object_object_get(request, "context");
	if (context &&
		(!json_object_is_type(context, json_type_object) ||
			strlen(json_object_to_json_string(context)) > CONTEXT_LIMIT)) {
		return out;
	}
	struct phi_surface_event event, *ep = NULL;
	json_object *e = json_object_object_get(request, "event");
	if (e) {
		event = (struct phi_surface_event){
			.kind = json_object_get_int(json_object_object_get(e, "kind")),
			.x = json_object_get_int(json_object_object_get(e, "x")),
			.y = json_object_get_int(json_object_object_get(e, "y")),
			.button = json_object_get_int(json_object_object_get(e, "button"))};
		if (event.kind < PHI_WIDGET_PRESS || event.kind > PHI_WIDGET_CLICK || event.x < 0 ||
			event.x >= w || event.y < 0 || event.y >= h || event.button < 0 || event.button > 5) {
			return out;
		}
		ep = &event;
	}
	struct phi_widget_cell *cells = calloc((size_t)w * (size_t)h, sizeof(*cells));
	int delay = 0;
	int ok = cells &&
		render_local(id, slot, w, h, context, peer_pid,
			json_object_get_int64(json_object_object_get(request, "serial")), ep, cells, &delay);
	char *encoded = ok ? pack(cells, (size_t)w * (size_t)h) : NULL;
	free(cells);
	if (encoded) {
		json_object_object_add(out, "available", json_object_new_boolean(1));
		json_object_object_add(out, "width", json_object_new_int(w));
		json_object_object_add(out, "height", json_object_new_int(h));
		json_object_object_add(out, "delay_ms", json_object_new_int(delay));
		json_object_object_add(out, "cells", json_object_new_string(encoded));
		free(encoded);
		if (peer_pid != (int)getpid()) {
			last_remote = stamp();
		}
	}

	return out;
}

struct surface_client {
	char id[64], slot[64];
	int width, height, present, delay, fd, pid;
	int64_t serial;
	double next, deadline, discovered, last_request;
	uint64_t context_hash;
	struct phi_widget_cell *cells, *scratch; /* reusable local-render comparison buffer */
	json_object *candidates;
	size_t candidate;
	char *wire, *reply;
	size_t sent, wire_size, read_head, used, reply_size;
	unsigned char head[4];
	unsigned generation;
	uint64_t event_revision, sent_event_revision;
	int has_event;
	struct phi_surface_event event;
};

static int64_t next_serial;

struct surface_client *surface_client_new(void) {

	struct surface_client *c = calloc(1, sizeof(*c));
	if (c) {
		uint64_t serial = 0;
		if (getrandom(&serial, sizeof(serial), GRND_NONBLOCK) != (ssize_t)sizeof(serial)) {
			serial = ((uint64_t)getpid() << 32) ^ (uint64_t)(stamp() * 1000000) ^
				(uint64_t)++next_serial;
		}
		c->fd = -1;
		c->serial = (int64_t)(serial & INT64_MAX);
		if (!c->serial) {
			c->serial = ++next_serial;
		}
	}

	return c;
}

static void connection_drop(struct surface_client *c) {

	if (c->fd >= 0) {
		close(c->fd);
	}
	c->fd = -1;
	free(c->wire);
	free(c->reply);
	c->wire = c->reply = NULL;
	c->sent = c->wire_size = c->read_head = c->used = c->reply_size = 0;
}

void surface_client_free(struct surface_client *c) {

	if (!c) {
		return;
	}
	/* Local instances have the same ownership lifetime as their client. Remote
       broker instances remain bounded by the server's instance cache. */
	for (int i = 0; i < INSTANCES; i++) {
		if (instances[i].provider && instances[i].pid == (int)getpid() &&
			instances[i].serial == c->serial) {
			drop_instance(&instances[i]);
		}
	}
	connection_drop(c);
	free(c->cells);
	free(c->scratch);
	json_object_put(c->candidates);
	free(c);
}

const struct phi_widget_cell *surface_client_cells(const struct surface_client *c) {

	return c && c->present ? c->cells : NULL;
}

int surface_client_event(struct surface_client *c, const struct phi_surface_event *event) {

	if (!c || !c->present || event->kind < PHI_WIDGET_PRESS || event->kind > PHI_WIDGET_CLICK ||
		event->x < 0 || event->x >= c->width || event->y < 0 || event->y >= c->height) {
		return 0;
	}
	c->event = *event;
	c->event_revision++;
	c->has_event = 1;
	c->next = 0;

	return 1;
}

static uint64_t hash_context(json_object *context) {

	const unsigned char *p = (const unsigned char *)(context
			? json_object_to_json_string_ext(context, JSON_C_TO_STRING_PLAIN)
			: "{}");
	uint64_t h = 1469598103934665603ULL;
	while (*p) {
		h ^= *p++;
		h *= 1099511628211ULL;
	}

	return h;
}

static void request_start(struct surface_client *c, json_object *context) {

	if (!discover) {
		c->present = 0;
		c->next = stamp() + 1;
		return;
	}
	if (!c->candidates) {
		json_object_put(c->candidates);
		c->candidates = discover();
		c->candidate = 0;
		c->discovered = stamp();
		if (!json_object_is_type(c->candidates, json_type_array)) {
			json_object_put(c->candidates);
			c->candidates = NULL;
			c->present = 0;
			c->next = stamp() + 1;
			return;
		}
	}
	size_t count = json_object_array_length(c->candidates);
	while (c->candidate < count) {
		json_object *owner = json_object_array_get_idx(c->candidates, c->candidate++);
		int pid = json_object_get_int(json_object_object_get(owner, "pid"));
		const char *path = text(owner, "bridge_socket");
		if (!json_object_get_boolean(json_object_object_get(owner, "live")) ||
			pid == (int)getpid() || !*path ||
			strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
			continue;
		}
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (fd < 0) {
			continue;
		}
		struct sockaddr_un address = {.sun_family = AF_UNIX};
		snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
		if (connect(fd, (struct sockaddr *)&address, sizeof(address)) && errno != EINPROGRESS) {
			close(fd);
			continue;
		}
		struct ucred peer;
		socklen_t length = sizeof(peer);
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) || peer.uid != geteuid() ||
			peer.pid != pid) {
			close(fd);
			continue;
		}
		json_object *request = json_object_new_object();
		json_object_object_add(request, "method", json_object_new_string("render_surface"));
		json_object_object_add(request, "provider", json_object_new_string(c->id));
		json_object_object_add(request, "slot", json_object_new_string(c->slot));
		json_object_object_add(request, "serial", json_object_new_int64(c->serial));
		json_object_object_add(request, "width", json_object_new_int(c->width));
		json_object_object_add(request, "height", json_object_new_int(c->height));
		if (context) {
			json_object_object_add(request, "context", json_object_get(context));
		}
		c->sent_event_revision = c->has_event ? c->event_revision : 0;
		if (c->has_event) {
			json_object *event = json_object_new_object();
			json_object_object_add(event, "kind", json_object_new_int(c->event.kind));
			json_object_object_add(event, "x", json_object_new_int(c->event.x));
			json_object_object_add(event, "y", json_object_new_int(c->event.y));
			json_object_object_add(event, "button", json_object_new_int(c->event.button));
			json_object_object_add(request, "event", event);
		}
		const char *encoded = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
		size_t bytes = strlen(encoded);
		if (bytes > CONTEXT_LIMIT) {
			json_object_put(request);
			close(fd);
			c->next = stamp() + 1;
			return;
		}
		c->wire = malloc(bytes + 4);
		if (!c->wire) {
			json_object_put(request);
			close(fd);
			c->next = stamp() + 1;
			return;
		}
		c->wire[0] = (char)(bytes >> 24);
		c->wire[1] = (char)(bytes >> 16);
		c->wire[2] = (char)(bytes >> 8);
		c->wire[3] = (char)bytes;
		memcpy(c->wire + 4, encoded, bytes);
		c->wire_size = bytes + 4;
		json_object_put(request);
		c->fd = fd;
		c->pid = pid;
		c->deadline = stamp() + 1;
		return;
	}
	c->present = 0;
	c->candidate = 0;
	json_object_put(c->candidates);
	c->candidates = NULL;
	c->next = stamp() + 1;
}

static int response_poll(struct surface_client *c) {

	if (stamp() > c->deadline) {
		goto failed;
	}
	for (int operation = 0; operation < 16; operation++) {
		if (c->sent < c->wire_size) {
			ssize_t n = send(c->fd, c->wire + c->sent, c->wire_size - c->sent, MSG_NOSIGNAL);
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
				return 0;
			}
			if (n <= 0) {
				goto failed;
			}
			c->sent += (size_t)n;
			continue;
		}
		if (c->read_head < 4) {
			ssize_t n = read(c->fd, c->head + c->read_head, 4 - c->read_head);
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
				return 0;
			}
			if (n <= 0) {
				goto failed;
			}
			c->read_head += (size_t)n;
			if (c->read_head < 4) {
				continue;
			}
			c->reply_size = (size_t)c->head[0] << 24 | (size_t)c->head[1] << 16 |
				(size_t)c->head[2] << 8 | c->head[3];
			if (!c->reply_size || c->reply_size > RESPONSE_LIMIT ||
				!(c->reply = malloc(c->reply_size + 1))) {
				goto failed;
			}
		}
		ssize_t n = read(c->fd, c->reply + c->used, c->reply_size - c->used);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			return 0;
		}
		if (n <= 0) {
			goto failed;
		}
		c->used += (size_t)n;
		if (c->used < c->reply_size) {
			continue;
		}
		c->reply[c->used] = 0;
		json_tokener *parser = json_tokener_new();
		if (!parser) {
			goto failed;
		}
		json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
		json_object *out = json_tokener_parse_ex(parser, c->reply, (int)c->used + 1);
		int valid = json_tokener_get_error(parser) == json_tokener_success &&
			json_object_is_type(out, json_type_object);
		json_tokener_free(parser);
		int changed = !c->present;
		if (valid && json_object_get_boolean(json_object_object_get(out, "available")) &&
			json_object_get_int(json_object_object_get(out, "width")) == c->width &&
			json_object_get_int(json_object_object_get(out, "height")) == c->height &&
			unpack(text(out, "cells"), c->cells, (size_t)c->width * (size_t)c->height, &changed)) {
			int delay = json_object_get_int(json_object_object_get(out, "delay_ms"));
			c->delay = delay <= 0 ? 0 : delay < 50 ? 50 : delay > 2000 ? 2000 : delay;
			c->present = 1;
			if (c->event_revision == c->sent_event_revision) {
				c->has_event = 0;
			}
			if (c->candidate) {
				c->candidate--; /* Prefer this owner next time. */
			}
			c->next = c->has_event ? 0 : stamp() + (c->delay ? c->delay / 1000.0 : 1);
			json_object_put(out);
			connection_drop(c);
			return changed;
		}
		json_object_put(out);
		goto failed;
	}
	return 0;
failed:
	/* Do not replay a possibly executed click after an unacknowledged request. */
	if (c->event_revision == c->sent_event_revision) {
		c->has_event = 0;
	}
	connection_drop(c);
	c->next = stamp() + .05;
	if (c->present) {
		c->present = 0;
		return 1;
	}

	return 0;
}

int surface_client_poll(struct surface_client *c, const char *id, const char *slot, int w, int h,
	json_object *context) {

	if (!c) {
		return 0;
	}
	int had_frame = c->present;
	if (c->generation != registry_generation) {
		c->generation = registry_generation;
		c->next = 0;
		c->last_request = 0;
	}
	if (!id || !identifier(id) || !slot_valid(slot) || w < 1 || w > 1024 || h < 1 || h > 512 ||
		(size_t)w * (size_t)h > PHI_SURFACE_MAX_CELLS) {
		int changed = c->present;
		c->present = 0;
		c->id[0] = 0;
		c->has_event = 0;
		connection_drop(c);
		free(c->cells);
		free(c->scratch);
		c->cells = c->scratch = NULL;
		return changed; /* Disabled clients have neither deadlines nor readiness. */
	}
	if (strcmp(c->id, id) || strcmp(c->slot, slot) || c->width != w || c->height != h) {
		connection_drop(c);
		free(c->cells);
		free(c->scratch);
		c->scratch = NULL;
		c->cells = calloc((size_t)w * (size_t)h, sizeof(*c->cells));
		snprintf(c->id, sizeof(c->id), "%s", id);
		snprintf(c->slot, sizeof(c->slot), "%s", slot);
		c->width = w;
		c->height = h;
		c->next = 0;
		c->present = 0;
		c->has_event = 0;
		c->context_hash = 0;
	}
	if (!c->cells) {
		if (stamp() < c->next) {
			return had_frame != c->present;
		}
		c->cells = calloc((size_t)w * (size_t)h, sizeof(*c->cells));
		if (!c->cells) {
			c->next = stamp() + 1;
			return had_frame != c->present;
		}
	}
	if (c->fd >= 0) {
		return response_poll(c);
	}
	uint64_t hash = hash_context(context);
	int changed_context = hash != c->context_hash;
	if (stamp() < c->next && !changed_context && !c->has_event) {
		return had_frame != c->present;
	}
	if (stamp() - c->last_request < .05) {
		c->next = c->last_request + .05;
		return had_frame != c->present;
	}
	c->last_request = stamp();
	if (find_provider(id)) {
		size_t count = (size_t)w * (size_t)h;
		if (!c->scratch) {
			c->scratch = calloc(count, sizeof(*c->scratch));
		}
		if (!c->scratch) {
			c->next = stamp() + 1;
			return had_frame != c->present;
		}
		int delay = 0;
		int present = render_local(id, slot, w, h, context, (int)getpid(), c->serial,
			c->has_event ? &c->event : NULL, c->scratch, &delay);
		int changed = c->present != present;
		if (present) {
			for (size_t i = 0; !changed && i < count; i++) {
				changed = !cell_equal(c->cells[i], c->scratch[i]);
			}
			struct phi_widget_cell *old = c->cells;
			c->cells = c->scratch;
			c->scratch = old;
		}
		c->present = present;
		c->delay = delay;
		c->has_event = 0;
		c->context_hash = hash;
		c->next = stamp() + (delay ? delay / 1000.0 : 1);
		return changed || had_frame != c->present;
	}
	c->context_hash = hash;
	request_start(c, context);

	return had_frame != c->present;
}

/* Advance only an existing exchange: safe while its view is hidden. */
int surface_client_service(struct surface_client *c) {

	return c && c->fd >= 0 ? response_poll(c) : 0;
}

int surface_client_pending_wait(const struct surface_client *c) {

	if (!c || c->fd < 0) {
		return -1;
	}
	double left = c->deadline - stamp();

	return left <= 0 ? 0 : left < .01 ? (int)(left * 1000) + 1 : 10;
}

int surface_client_due(const struct surface_client *c) {

	if (!c || !*c->id) {
		return 0;
	}
	if (c->generation != registry_generation) {
		return 1;
	}
	if (c->fd < 0) {
		return stamp() >= c->next;
	}
	if (stamp() >= c->deadline) {
		return 1;
	}
	struct pollfd fd = {.fd = c->fd, .events = c->sent < c->wire_size ? POLLOUT : POLLIN};

	return poll(&fd, 1, 0) > 0;
}

int surface_client_wait(const struct surface_client *c) {

	if (!c || !*c->id) {
		return -1;
	}
	if (c->fd >= 0) {
		return surface_client_pending_wait(c);
	}
	double left = c->next - stamp();

	return left <= 0 ? 0 : (int)(left * 1000) + 1;
}
