#define _POSIX_C_SOURCE 200809L
#include "codex.h"
#include "auth.h"
#include "extensions.h"
#include "http.h"
#include "models.h"
#include "events.h"
#include <strings.h>
#include "json.h"
#include "../tools.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CODEX_URL
#define CODEX_URL "https://chatgpt.com/backend-api/codex/responses"
#endif
/* A model can think for a long time before its first token, so there is no
   total deadline; only a connection that delivers nothing for this long is
   treated as stalled. */
#define STALL_SECONDS 300L
#define MAX_ATTEMPTS 4

struct codex {
	CURL *easy;
	CURLM *multi;
	struct curl_slist *headers;
	long last_status; /* HTTP status of the latest built-in attempt */
	char last_error[256]; /* why the latest request failed ("" on success) */
};

struct codex *codex_open(void) {

	if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
		return NULL;
	}
	struct codex *client = calloc(1, sizeof(*client));
	if (!client) {
		curl_global_cleanup();
		return NULL;
	}
	client->easy = curl_easy_init();
	client->multi = curl_multi_init();
	if (!client->easy || !client->multi) {
		codex_close(client);
		return NULL;
	}
	curl_easy_setopt(client->easy, CURLOPT_URL, CODEX_URL);
	curl_easy_setopt(client->easy, CURLOPT_HTTPHEADER, client->headers);
	curl_easy_setopt(client->easy, CURLOPT_WRITEFUNCTION, stream_receive);
	curl_easy_setopt(client->easy, CURLOPT_HEADERFUNCTION, stream_header);
	curl_easy_setopt(client->easy, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(client->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(client->easy, CURLOPT_LOW_SPEED_TIME, STALL_SECONDS);
	curl_easy_setopt(client->easy, CURLOPT_NOPROGRESS, 0L);

	return client;
}

void codex_close(struct codex *client) {

	if (!client) {
		return;
	}
	if (client->easy) {
		curl_easy_cleanup(client->easy);
	}
	if (client->multi) {
		curl_multi_cleanup(client->multi);
	}
	curl_slist_free_all(client->headers);
	free(client);
	curl_global_cleanup();
}

static int transfer_progress(
	void *userdata, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {

	(void)a;
	(void)b;
	(void)c;
	(void)d;
	struct stream *s = userdata;

	return s->sink.cancel && s->sink.cancel();
}

/* before_provider_headers: handlers see the request headers (the bearer
   token redacted) and may set extra or replacement headers. */
static int headers_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	(void)state;
	json_object *headers = json_object_object_get(result, "headers");
	if (!json_object_is_type(headers, json_type_object)) {
		return 0;
	}
	json_object *current = json_object_object_get(payload, "headers");
	json_object_object_foreach(headers, key, value) {
		if (json_object_is_type(value, json_type_string) && !strpbrk(key, "\r\n:") &&
			!strpbrk(json_object_get_string(value), "\r\n")) {
			json_object_object_add(current, key, json_object_get(value));
		}
	}

	return 0;
}

static struct curl_slist *extension_headers(struct curl_slist *headers) {

	if (!events_wanted("before_provider_headers")) {
		return headers;
	}
	json_object *shown = json_object_new_object();
	for (struct curl_slist *h = headers; h; h = h->next) {
		const char *colon = strchr(h->data, ':');
		if (!colon) {
			continue;
		}
		char name[128];
		snprintf(name, sizeof(name), "%.*s", (int)(colon - h->data), h->data);
		const char *value = colon + 1 + (colon[1] == ' ');
		json_object_object_add(shown, name,
			json_object_new_string(strcasecmp(name, "Authorization") ? value : "[redacted]"));
	}
	json_object *before = NULL;
	json_object_deep_copy(shown, &before, NULL);
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "headers", shown);
	events_emit("before_provider_headers", payload, headers_apply, NULL);
	/* Unchanged headers keep their original line (with the real token); a
       changed or new header is set; an empty value removes it. */
	struct curl_slist *rebuilt = NULL;
	int ok = 1;
	json_object_object_foreach(shown, key, value) {
		const char *text = json_object_get_string(value);
		const char *original = field(before, key);
		char line[8192] = "";
		if (*original && !strcmp(text, original)) {
			for (struct curl_slist *h = headers; h; h = h->next) {
				if (!strncasecmp(h->data, key, strlen(key)) && h->data[strlen(key)] == ':') {
					snprintf(line, sizeof(line), "%s", h->data);
				}
			}
		} else if (*text) {
			snprintf(line, sizeof(line), "%s: %s", key, text);
		}
		if (!*line) {
			continue;
		}
		struct curl_slist *next = curl_slist_append(rebuilt, line);
		if (!next) {
			ok = 0;
			break;
		}
		rebuilt = next;
	}
	json_object_put(payload);
	json_object_put(before);
	if (!ok || !rebuilt) {
		curl_slist_free_all(rebuilt);
		return headers;
	}
	curl_slist_free_all(headers);

	return rebuilt;
}

/* Failures worth another attempt: the connection or the server hiccupped
   before anything reached the screen. Never retry after visible output. */
static int retryable(CURLcode result, long status, const struct stream *s) {

	if (s->delivered) {
		return 0;
	}
	switch (result) {
	case CURLE_OK:
		return status == 429 || status >= 500 || (status == 200 && !s->completed && !s->failed);
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_CONNECT:
	case CURLE_OPERATION_TIMEDOUT:
	case CURLE_SEND_ERROR:
	case CURLE_RECV_ERROR:
	case CURLE_GOT_NOTHING:
	case CURLE_PARTIAL_FILE:
	case CURLE_HTTP2:
	case CURLE_HTTP2_STREAM:
	case CURLE_SSL_CONNECT_ERROR:
		return 1;
	default:
		return 0;
	}
}

/* Sleep without blocking cancellation. Returns 0 if the user cancelled. */
static int wait_cancellable(long seconds, int (*cancel)(void)) {

	for (long i = 0; i < seconds * 20; i++) {
		if (cancel && cancel()) {
			return 0;
		}
		struct timespec pause = {0, 50000000};
		nanosleep(&pause, NULL);
	}

	return !(cancel && cancel());
}

/* A failed request in plain words: the server's reason when it gave one,
   and what to do about it where that is known. */
static void failure_text(
	CURLcode result, long status, const struct stream *s, char *out, size_t size) {

	char reason[512] = "";
	json_object *root = s->body_used ? json_tokener_parse(s->body) : NULL;
	int said = *s->error ? snprintf(reason, sizeof(reason), "%s", s->error) > 0
						 : root && stream_error_text(root, reason, sizeof(reason));
	if (root) {
		json_object_put(root);
	}
	if (!said && s->body_used) {
		snprintf(reason, sizeof(reason), "%.400s", s->body);
	}
	size_t n = strlen(reason);
	while (n && (reason[n - 1] == '\n' || reason[n - 1] == '\r' || reason[n - 1] == ' ')) {
		reason[--n] = 0;
	}
	const char *hint =
		strstr(reason, "reasoning.effort") || (s->body_used && strstr(s->body, "reasoning.effort"))
		? "Try another level with /effort."
		: status == 401 ? "Your sign-in has expired: run /login."
		: status == 403 ? "This account cannot use that model: try /model."
		: status == 404 ? "The model was not found: check /model."
		: status == 429 ? "Rate limited: wait a little, or check your plan's usage."
		: status >= 500 ? "The server had a problem: try again shortly."
						: "";
	if (result != CURLE_OK && !status) {
		snprintf(out, size, "Could not reach the server: %s", curl_easy_strerror(result));
	} else if (*reason && status == 200) {
		snprintf(out, size, "%s%s%s", reason, *hint ? ". " : "", hint);
	} else if (*reason) {
		snprintf(out, size, "%s (HTTP %ld)%s%s", reason, status, *hint ? ". " : "", hint);
	} else if (status == 200) {
		snprintf(out, size, "The response ended before it was complete%s",
			result != CURLE_OK ? "; the connection dropped" : "");
	} else {
		snprintf(out, size, "HTTP %ld%s%s", status, *hint ? ". " : "", hint);
	}
}

static void report_failure(CURLcode result, long status, const struct stream *s) {

	char text[1024];
	failure_text(result, status, s, text, sizeof(text));
	tool_report(stderr, "Request failed: %s\n", text);
}

/* Retry progress for the JSON/RPC event streams (not an extension event). */
static void retry_event(
	const char *event, int attempt, long delay_ms, int success, const char *error) {

	if (!events_wanted(event)) {
		return;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "attempt", json_object_new_int(attempt));
	if (!strcmp(event, "auto_retry_start")) {
		json_object_object_add(payload, "maxAttempts", json_object_new_int(MAX_ATTEMPTS));
		json_object_object_add(payload, "delayMs", json_object_new_int64(delay_ms));
		json_object_object_add(payload, "errorMessage", json_object_new_string(error ? error : ""));
	} else {
		json_object_object_add(payload, "success", json_object_new_boolean(success));
		if (!success) {
			json_object_object_add(
				payload, "finalError", json_object_new_string(error ? error : ""));
		}
	}
	events_emit(event, payload, NULL, NULL);
	json_object_put(payload);
}

/* provider_stream_event: each parsed event, as it arrives, before Phi
   normalizes it. Notification only; the event is not saved. */
static void stream_event_observed(json_object *event, void *context) {

	if (!events_wanted("provider_stream_event")) {
		return;
	}
	json_object *payload = json_object_new_object();
	json_object_object_add(payload, "provider", json_object_new_string("openai-codex"));
	json_object_object_add(payload, "api", json_object_new_string("openai-responses"));
	json_object_object_add(payload, "model", json_object_new_string(context));
	json_object_object_add(payload, "data", json_object_get(event));
	events_emit("provider_stream_event", payload, NULL, NULL);
	json_object_put(payload);
}

static json_object *builtin_transport(
	json_object *body, struct response_sink sink, void *userdata) {

	struct codex *client = userdata;
	if (!client || !body) {
		return NULL;
	}
	/* Phi-private routing metadata is never sent to the built-in endpoint. */
	json_object_object_del(body, "_phi_purpose");
	json_object_object_del(body, "auth");
	const char *encoded = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);
	json_object *output = NULL;
	int retried = 0;
	char last_error[256] = "";
	for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
		/* Fresh headers each attempt: a long wait can outlive the token. */
		struct curl_slist *fresh = codex_auth_headers_cancel(sink.cancel);
		if (!fresh) {
			break;
		}
		fresh = extension_headers(fresh);
		curl_easy_setopt(client->easy, CURLOPT_HTTPHEADER, fresh);
		curl_slist_free_all(client->headers);
		client->headers = fresh;
		struct stream stream = {.sink = sink};
		if (events_wanted("provider_stream_event")) {
			stream.observe_event = stream_event_observed;
			stream.event_context = (void *)field(body, "model");
		}
		curl_easy_setopt(client->easy, CURLOPT_POSTFIELDS, encoded);
		curl_easy_setopt(client->easy, CURLOPT_WRITEDATA, &stream);
		curl_easy_setopt(client->easy, CURLOPT_HEADERDATA, &stream);
		curl_easy_setopt(client->easy, CURLOPT_XFERINFOFUNCTION, transfer_progress);
		curl_easy_setopt(client->easy, CURLOPT_XFERINFODATA, &stream);
		CURLcode result = phi_http_perform(client->multi, client->easy, sink.cancel);
		stream_finish(&stream);
		long status = 0;
		curl_easy_getinfo(client->easy, CURLINFO_RESPONSE_CODE, &status);
		client->last_status = status;
		curl_easy_setopt(client->easy, CURLOPT_POSTFIELDS, NULL);
		curl_easy_setopt(client->easy, CURLOPT_WRITEDATA, NULL);
		curl_easy_setopt(client->easy, CURLOPT_HEADERDATA, NULL);
		curl_easy_setopt(client->easy, CURLOPT_XFERINFODATA, NULL);
		int cancelled = sink.cancel && sink.cancel();
		if (result == CURLE_OK && status == 200 && !stream.failed && stream.completed &&
			stream.output) {
			client->last_error[0] = 0;
			output = json_object_get(stream.output);
			stream_clear(&stream);
			if (retried) {
				retry_event("auto_retry_end", attempt, 0, 1, NULL);
			}
			break;
		}
		failure_text(result, status, &stream, last_error, sizeof(last_error));
		snprintf(client->last_error, sizeof(client->last_error), "%s", last_error);
		int again = !cancelled && attempt < MAX_ATTEMPTS && retryable(result, status, &stream);
		long delay = stream.retry_after ? stream.retry_after : 1L << (attempt - 1);
		if (delay > 60) {
			delay = 60;
		}
		if (!cancelled) {
			report_failure(result, status, &stream);
			if (again) {
				tool_report(stderr, "Retrying in %lds (attempt %d of %d)…\n", delay, attempt + 1,
					MAX_ATTEMPTS);
			}
		}
		stream_clear(&stream);
		if (again) {
			retried = 1;
			retry_event("auto_retry_start", attempt, delay * 1000, 0, last_error);
		} else if (retried) {
			retry_event("auto_retry_end", attempt, 0, 0, last_error);
		}
		if (!again || !wait_cancellable(delay, sink.cancel)) {
			break;
		}
	}

	return output;
}

static int messages_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	(void)state;
	json_object *messages = json_object_object_get(result, "messages");
	if (json_object_is_type(messages, json_type_array)) {
		json_object_object_add(payload, "messages", json_object_get(messages));
	}

	return 0;
}

/* context / context_with_system: request-local edits of the conversation
   (and, with_system, the instructions). History itself is never changed. */
static void context_events(json_object *body) {

	if (events_wanted("context")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(
			payload, "messages", json_object_get(json_object_object_get(body, "input")));
		events_emit("context", payload, messages_apply, NULL);
		json_object_object_add(
			body, "input", json_object_get(json_object_object_get(payload, "messages")));
		json_object_put(payload);
	}
	if (events_wanted("context_with_system")) {
		json_object *messages = json_object_new_array(), *system = json_object_new_object();
		json_object_object_add(system, "role", json_object_new_string("system"));
		json_object_object_add(
			system, "content", json_object_get(json_object_object_get(body, "instructions")));
		json_object_array_add(messages, system);
		json_object *input = json_object_object_get(body, "input");
		for (size_t i = 0; i < json_object_array_length(input); i++) {
			json_object_array_add(messages, json_object_get(json_object_array_get_idx(input, i)));
		}
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "messages", messages);
		events_emit("context_with_system", payload, messages_apply, NULL);
		json_object *edited = json_object_object_get(payload, "messages");
		json_object *first = json_object_array_get_idx(edited, 0);
		if (json_object_array_length(edited) && !strcmp(field(first, "role"), "system")) {
			json_object_object_add(
				body, "instructions", json_object_get(json_object_object_get(first, "content")));
			json_object *rest = json_object_new_array();
			for (size_t i = 1; i < json_object_array_length(edited); i++) {
				json_object_array_add(rest, json_object_get(json_object_array_get_idx(edited, i)));
			}
			json_object_object_add(body, "input", rest);
		}
		json_object_put(payload);
	}
}

static int request_apply(
	json_object *payload, json_object *result, const char *source, void *state) {

	(void)source;
	(void)state;
	json_object *request = json_object_object_get(result, "request");
	if (json_object_is_type(request, json_type_object)) {
		json_object_object_add(payload, "request", json_object_get(request));
	}

	return 0;
}

/* Send body through extension providers/wrappers or the built-in transport,
   with the provider request/response events around it. Consumes body. */
static json_object *send_body(
	struct codex *client, json_object *body, const char *purpose, struct response_sink sink) {

	if (events_wanted("before_provider_request")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "purpose", json_object_new_string(purpose));
		json_object_object_add(payload, "request", body);
		events_emit("before_provider_request", payload, request_apply, NULL);
		body = json_object_get(json_object_object_get(payload, "request"));
		json_object_put(payload);
	}
	json_object_object_add(body, "_phi_purpose", json_object_new_string(purpose));
	client->last_status = 0;
	client->last_error[0] = 0;
	/* This is the single model-dispatch boundary. Built-in Codex, extension
       providers, and provider middleware all pass through it, including
       compaction requests. */
	json_object *output = extensions_provider_dispatch(body, sink, builtin_transport, client);
	json_object_put(body);
	if (events_wanted("after_provider_response")) {
		json_object *payload = json_object_new_object();
		json_object_object_add(payload, "purpose", json_object_new_string(purpose));
		json_object_object_add(payload, "status", json_object_new_int64(client->last_status));
		json_object_object_add(payload, "ok", json_object_new_boolean(output != NULL));
		events_emit("after_provider_response", payload, NULL, NULL);
		json_object_put(payload);
	}

	return output;
}

static json_object *respond(struct codex *client, json_object *history, const char *model,
	const char *effort, struct response_sink sink, int summary) {

	json_object *body = summary ? codex_summary_body(history, model, effort)
								: codex_request_body(history, model, effort);
	if (!body) {
		return NULL;
	}
	if (!summary) {
		context_events(body);
	}

	return send_body(client, body, summary ? "compaction" : "agent", sink);
}

json_object *codex_respond(struct codex *client, json_object *history, const char *model,
	const char *effort, struct response_sink sink) {

	return respond(client, history, model, effort, sink, 0);
}

json_object *codex_respond_summary(struct codex *client, json_object *history, const char *model,
	const char *effort, struct response_sink sink) {

	return respond(client, history, model, effort, sink, 1);
}

static int64_t complete_input_tokens, complete_output_tokens;

static void complete_usage(int64_t input_tokens, int64_t output_tokens) {

	complete_input_tokens = input_tokens;
	complete_output_tokens = output_tokens;
}

static json_object *complete_error(const char *message) {

	json_object *answer = json_object_new_object();
	if (answer) {
		json_object_object_add(answer, "error", json_object_new_string(message));
	}

	return answer;
}

/* Text as it streams, for codex_complete_stream's observer. */
struct complete_observer {
	void (*on_text)(const char *text, void *state);
	void *state;
};

static void complete_text(const char *text, void *context) {

	struct complete_observer *observer = context;
	if (observer->on_text && text && *text) {
		observer->on_text(text, observer->state);
	}
}

json_object *codex_complete_request(struct codex *client, json_object *request,
	const char *default_model, const char *default_effort, int (*cancel)(void)) {

	return codex_complete_stream(
		client, request, default_model, default_effort, cancel, NULL, NULL);
}

/* The request body for {"messages", "system", "model", "effort"}, or NULL
   with *problem set to an {"error"} answer. */
static json_object *complete_body(json_object *request, const char *default_model,
	const char *default_effort, const char **model_out, json_object **problem) {

	*problem = NULL;
	json_object *messages = json_object_object_get(request, "messages");
	if (!json_object_is_type(messages, json_type_array) || !json_object_array_length(messages)) {
		*problem = complete_error("messages must be a nonempty array of {role, content}");
		return NULL;
	}
	const char *model = *field(request, "model") ? field(request, "model") : default_model;
	const char *effort = *field(request, "effort") ? field(request, "effort") : default_effort;
	if (phi_effort_rank(effort) < 0) {
		*problem =
			complete_error("effort must be none, minimal, low, medium, high, xhigh, max or ultra");
		return NULL;
	}
	const char *system =
		*field(request, "system") ? field(request, "system") : "You are a helpful assistant.";
	json_object *history = json_object_new_array();
	if (!history) {
		return NULL;
	}
	for (size_t i = 0; i < json_object_array_length(messages); i++) {
		json_object *message = json_object_array_get_idx(messages, i);
		const char *role = field(message, "role");
		json_object *content = json_object_object_get(message, "content");
		if ((strcmp(role, "user") && strcmp(role, "assistant")) ||
			!json_object_is_type(content, json_type_string)) {
			json_object_put(history);
			*problem = complete_error("each message needs role user|assistant and string content");
			return NULL;
		}
		json_object *item = json_object_new_object();
		json_object_object_add(item, "role", json_object_new_string(role));
		json_object_object_add(item, "content", json_object_get(content));
		json_object_array_add(history, item);
	}
	json_object *body = codex_custom_body(history, model, effort, system);
	json_object_put(history);
	if (!body) {
		*problem = complete_error("Cannot build model request");
	}
	if (model_out) {
		*model_out = model;
	}

	return body;
}

/* {"text", "model", "usage"} from a response's output items (borrowed). */
static json_object *complete_answer(
	json_object *output, const char *model, int64_t input_tokens, int64_t output_tokens) {

	size_t length = 0;
	for (size_t i = 0; i < json_object_array_length(output); i++) {
		json_object *content =
			json_object_object_get(json_object_array_get_idx(output, i), "content");
		for (size_t j = 0;
			json_object_is_type(content, json_type_array) && j < json_object_array_length(content);
			j++) {
			if (!strcmp(field(json_object_array_get_idx(content, j), "type"), "output_text")) {
				length += strlen(field(json_object_array_get_idx(content, j), "text"));
			}
		}
	}
	char *text = malloc(length + 1);
	if (!text) {
		return NULL;
	}
	size_t used = 0;
	for (size_t i = 0; i < json_object_array_length(output); i++) {
		json_object *content =
			json_object_object_get(json_object_array_get_idx(output, i), "content");
		for (size_t j = 0;
			json_object_is_type(content, json_type_array) && j < json_object_array_length(content);
			j++) {
			json_object *part = json_object_array_get_idx(content, j);
			if (strcmp(field(part, "type"), "output_text")) {
				continue;
			}
			size_t n = strlen(field(part, "text"));
			memcpy(text + used, field(part, "text"), n);
			used += n;
		}
	}
	text[used] = 0;
	json_object *answer = json_object_new_object();
	if (answer) {
		json_object_object_add(answer, "text", json_object_new_string(text));
		json_object_object_add(answer, "model", json_object_new_string(model));
		if (input_tokens >= 0) {
			json_object *usage = json_object_new_object();
			json_object_object_add(usage, "input_tokens", json_object_new_int64(input_tokens));
			json_object_object_add(usage, "output_tokens", json_object_new_int64(output_tokens));
			json_object_object_add(answer, "usage", usage);
		}
	}
	free(text);

	return answer;
}

json_object *codex_complete_stream(struct codex *client, json_object *request,
	const char *default_model, const char *default_effort, int (*cancel)(void),
	void (*on_text)(const char *text, void *state), void *state) {

	const char *model = default_model;
	json_object *problem = NULL;
	json_object *body = complete_body(request, default_model, default_effort, &model, &problem);
	if (!body) {
		return problem;
	}
	complete_input_tokens = complete_output_tokens = -1;
	struct complete_observer observer = {on_text, state};
	struct response_sink sink = {.cancel = cancel,
		.usage = complete_usage,
		.observe_text = on_text ? complete_text : NULL,
		.observer = &observer};
	json_object *output = send_body(client, body, "extension", sink);
	if (!output) {
		return complete_error(cancel && cancel() ? "Cancelled" : "Model request failed");
	}
	json_object *answer =
		complete_answer(output, model, complete_input_tokens, complete_output_tokens);
	json_object_put(output);

	return answer;
}

/* Side requests: model calls that run alongside everything else, on their
   own connections, advanced by codex_side_pump from the loop that keeps the
   keyboard responsive. No thread: callbacks run on Phi's thread, from the
   pump, never inside codex_side_start. The built-in endpoint only; the
   caller runs other providers itself. */
#define SIDE_MAX 8

static struct side {
	int id;
	CURL *easy;
	struct curl_slist *headers;
	char *body, model[96];
	struct stream stream;
	int (*on_text)(const char *text, void *state); /* nonzero stops the request */
	void (*on_done)(json_object *answer, void *state);
	void *state, *owner;
	int64_t input_tokens, output_tokens;
	int stopped;
	json_object *ready; /* an answer found another way, delivered at the next pump */
} sides[SIDE_MAX];

static CURLM *side_multi;
static int side_next_id;

static void side_text(const char *text, void *context) {

	struct side *side = context;
	if (!side->stopped && side->on_text && text && *text && side->on_text(text, side->state)) {
		side->stopped = 1;
	}
}

static void side_free(struct side *side) {

	if (side->easy) {
		if (side_multi) {
			curl_multi_remove_handle(side_multi, side->easy);
		}
		curl_easy_cleanup(side->easy);
	}
	curl_slist_free_all(side->headers);
	free(side->body);
	stream_clear(&side->stream);
	if (side->ready) {
		json_object_put(side->ready);
	}
	*side = (struct side){0};
}

int codex_side_start(json_object *request, const char *default_model, const char *default_effort,
	int (*on_text)(const char *text, void *state),
	void (*on_done)(json_object *answer, void *state), void *state, void *owner, char *error,
	size_t size) {

	struct side *side = NULL;
	for (int i = 0; i < SIDE_MAX && !side; i++) {
		if (!sides[i].id) {
			side = &sides[i];
		}
	}
	if (!side) {
		snprintf(error, size, "too many side requests at once (at most %d)", SIDE_MAX);
		return 0;
	}
	if (!side_multi && !(side_multi = curl_multi_init())) {
		snprintf(error, size, "cannot start a connection");
		return 0;
	}
	const char *model = default_model;
	json_object *problem = NULL;
	json_object *body = complete_body(request, default_model, default_effort, &model, &problem);
	if (!body) {
		snprintf(error, size, "%s", problem ? field(problem, "error") : "cannot build the request");
		if (problem) {
			json_object_put(problem);
		}
		return 0;
	}
	json_object_object_del(body, "_phi_purpose");
	json_object_object_del(body, "auth");
	side->body = strdup(json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN));
	json_object_put(body);
	side->headers = codex_auth_headers();
	if (side->headers) {
		side->headers = extension_headers(side->headers);
	}
	side->easy = curl_easy_init();
	if (!side->body || !side->headers || !side->easy) {
		snprintf(error, size, side->headers ? "out of memory" : "not logged in");
		side_free(side);
		return 0;
	}
	snprintf(side->model, sizeof(side->model), "%s", model);
	side->on_text = on_text;
	side->on_done = on_done;
	side->state = state;
	side->owner = owner;
	side->input_tokens = side->output_tokens = -1;
	side->stream.sink = (struct response_sink){.observe_text = side_text, .observer = side};
	curl_easy_setopt(side->easy, CURLOPT_URL, CODEX_URL);
	curl_easy_setopt(side->easy, CURLOPT_HTTPHEADER, side->headers);
	curl_easy_setopt(side->easy, CURLOPT_POSTFIELDS, side->body);
	curl_easy_setopt(side->easy, CURLOPT_WRITEFUNCTION, stream_receive);
	curl_easy_setopt(side->easy, CURLOPT_WRITEDATA, &side->stream);
	curl_easy_setopt(side->easy, CURLOPT_HEADERFUNCTION, stream_header);
	curl_easy_setopt(side->easy, CURLOPT_HEADERDATA, &side->stream);
	curl_easy_setopt(side->easy, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(side->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(side->easy, CURLOPT_LOW_SPEED_TIME, STALL_SECONDS);
	curl_easy_setopt(side->easy, CURLOPT_PRIVATE, side);
	if (curl_multi_add_handle(side_multi, side->easy) != CURLM_OK) {
		snprintf(error, size, "cannot start the request");
		side_free(side);
		return 0;
	}
	if (++side_next_id <= 0) {
		side_next_id = 1;
	}
	side->id = side_next_id;

	return side->id;
}

int codex_side_later(json_object *answer, void (*on_done)(json_object *answer, void *state),
	void *state, void *owner) {

	struct side *side = NULL;
	for (int i = 0; i < SIDE_MAX && !side; i++) {
		if (!sides[i].id) {
			side = &sides[i];
		}
	}
	if (!side || !answer) {
		return 0;
	}
	side->ready = json_object_get(answer);
	side->on_done = on_done;
	side->state = state;
	side->owner = owner;
	if (++side_next_id <= 0) {
		side_next_id = 1;
	}
	side->id = side_next_id;

	return side->id;
}

int codex_side_active(void) {

	int count = 0;
	for (int i = 0; i < SIDE_MAX; i++) {
		count += sides[i].id != 0;
	}

	return count;
}

/* Finish a side: its answer (or error) goes to on_done, once. */
static void side_finish(struct side *side, CURLcode result) {

	long status = 0;
	json_object *answer = NULL;
	if (side->ready) {
		answer = json_object_get(side->ready);
	} else if (side->stopped) {
		answer = complete_error("Stopped");
	} else {
		stream_finish(&side->stream);
	}
	if (side->easy) {
		curl_easy_getinfo(side->easy, CURLINFO_RESPONSE_CODE, &status);
	}
	if (answer) {
	} else if (result == CURLE_OK && status == 200 && !side->stream.failed &&
		side->stream.completed && side->stream.output) {
		answer = complete_answer(
			side->stream.output, side->model, side->input_tokens, side->output_tokens);
	} else {
		char message[1024];
		failure_text(result, status, &side->stream, message, sizeof(message));
		answer = complete_error(message);
	}
	void (*on_done)(json_object *, void *) = side->on_done;
	void *state = side->state;
	side_free(side);
	if (on_done) {
		on_done(answer, state);
	}
	if (answer) {
		json_object_put(answer);
	}
}

void codex_side_pump(void) {

	if (!codex_side_active()) {
		return;
	}
	/* Answers already known, and requests their callback stopped. */
	for (int i = 0; i < SIDE_MAX; i++) {
		if (sides[i].id && (sides[i].ready || sides[i].stopped)) {
			side_finish(&sides[i], CURLE_OK);
		}
	}
	if (!side_multi) {
		return;
	}
	int running = 0;
	curl_multi_perform(side_multi, &running);
	CURLMsg *message;
	int remaining;
	while ((message = curl_multi_info_read(side_multi, &remaining))) {
		if (message->msg != CURLMSG_DONE) {
			continue;
		}
		struct side *side = NULL;
		curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, (char **)&side);
		if (side && side->id) {
			side_finish(side, message->data.result);
		}
	}
}

/* Stop one request (id) or every request of an owner (id 0): no callbacks. */
void codex_side_cancel(int id, void *owner) {

	for (int i = 0; i < SIDE_MAX; i++) {
		if (sides[i].id && (id ? sides[i].id == id : sides[i].owner == owner)) {
			side_free(&sides[i]);
		}
	}
}

const char *codex_last_error(struct codex *client) {

	return client && client->last_error[0] ? client->last_error : NULL;
}

long codex_last_status(struct codex *client) {

	return client ? client->last_status : 0;
}
