#include "events.h"

static struct event_bus bus;
typedef void (*event_observer)(const char *event, json_object *payload);
static event_observer observer, extra_observer; /* the mode's, and chat status */
static int (*extra_wants)(const char *event);

void events_set_bus(const struct event_bus *value) {

	bus = value ? *value : (struct event_bus){0};
}

void events_set_observer(void (*value)(const char *event, json_object *payload)) {

	observer = value;
}

void events_set_status_observer(
	void (*value)(const char *event, json_object *payload), int (*wants)(const char *event)) {

	extra_observer = value;
	extra_wants = wants;
}

static int extra_wanted(const char *event) {

	return extra_observer && (!extra_wants || extra_wants(event));
}

static int bus_wants(const char *event) {

	return bus.wanted && bus.emit && bus.wanted(event);
}

int events_blocked(void) {

	return bus.blocked && bus.blocked();
}

void events_tick(void) {

	if (bus.tick) {
		bus.tick();
	}
}

int events_wanted(const char *event) {

	return observer || extra_wanted(event) || bus_wants(event);
}

int events_emit(const char *event, json_object *payload, event_apply_fn apply, void *state) {

	int failed = 0;
	if (bus_wants(event)) {
		bus.emit(event, payload, apply, state, &failed);
	}
	if (observer) {
		observer(event, payload);
	}
	if (extra_wanted(event)) {
		extra_observer(event, payload);
	}

	return failed;
}

void events_notify(const char *event, const char *key, const char *value) {

	if (!events_wanted(event)) {
		return;
	}
	json_object *payload = json_object_new_object();
	if (!payload) {
		return;
	}
	if (key) {
		json_object_object_add(payload, key, json_object_new_string(value ? value : ""));
	}
	events_emit(event, payload, NULL, NULL);
	json_object_put(payload);
}

static int cancel_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)payload;
	(void)source;
	if (!json_object_get_boolean(json_object_object_get(result, "cancel"))) {
		return 0;
	}
	*(int *)state = 1;

	return 1;
}

int events_allowed(const char *event, json_object *payload) {

	int cancel = 0;
	if (payload && events_wanted(event)) {
		events_emit(event, payload, cancel_apply, &cancel);
	}
	if (payload) {
		json_object_put(payload);
	}

	return !cancel;
}
