#define _POSIX_C_SOURCE 200809L
#include "usage.h"
#include "auth.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#ifndef CODEX_USAGE_URL
#define CODEX_USAGE_URL "https://chatgpt.com/backend-api/wham/usage"
#endif

struct usage_client {
	CURL *easy;
	CURLM *multi;
	struct curl_slist *headers;
	char body[65537];
	size_t used;
	int running, attempted, refresh_auth;
	long http;
	CURLcode transfer_error;
	json_object *snapshot;
	time_t next;
	struct weekly_usage value;
};

struct weekly_usage usage_parse(json_object *object, time_t now) {

	struct weekly_usage result = {0};
	json_object *limits = json_object_object_get(object, "rate_limit");
	const char *names[] = {"primary_window", "secondary_window"};
	for (int i = 0; limits && i < 2; i++) {
		json_object *window = json_object_object_get(limits, names[i]);
		if (!json_object_is_type(window, json_type_object)) {
			continue;
		}
		json_object *duration = json_object_object_get(window, "limit_window_seconds");
		/* Require an explicitly weekly window: never label the 5h quota weekly. */
		int64_t seconds = json_object_get_int64(duration);
		if (!json_object_is_type(duration, json_type_int) || seconds < 6 * 86400 ||
			seconds > 8 * 86400) {
			continue;
		}
		json_object *used = json_object_object_get(window, "used_percent");
		if (!json_object_is_type(used, json_type_int) &&
			!json_object_is_type(used, json_type_double)) {
			continue;
		}
		double percent = json_object_get_double(used);
		if (!isfinite(percent)) {
			continue;
		}
		result.available = 1;
		result.left = percent < 0 ? 100 : percent > 100 ? 0 : 100 - percent;
		json_object *reset = json_object_object_get(window, "reset_at");
		int64_t at = json_object_get_int64(reset);
		if (json_object_is_type(reset, json_type_int) && at > 0 && at <= 4102444800LL) {
			result.reset = (time_t)at;
		} else {
			reset = json_object_object_get(window, "reset_after_seconds");
			int64_t after = json_object_get_int64(reset);
			if (json_object_is_type(reset, json_type_int) && after >= 0 && after <= 8 * 86400) {
				result.reset = now + (time_t)after;
			}
		}
		break;
	}

	return result;
}

void usage_format(struct weekly_usage value, time_t now, char *text, size_t size) {

	if (!value.available) {
		snprintf(text, size, "wk unavailable");
		return;
	}
	char reset[48] = "";
	if (value.reset) {
		long remaining = value.reset > now ? (long)(value.reset - now) : 0;
		if (remaining >= 86400) {
			snprintf(reset, sizeof(reset), " · resets %ldd%ldh", remaining / 86400,
				remaining % 86400 / 3600);
		} else if (remaining >= 3600) {
			snprintf(reset, sizeof(reset), " · resets %ldh%ldm", remaining / 3600,
				remaining % 3600 / 60);
		} else {
			snprintf(reset, sizeof(reset), " · resets %ldm", remaining / 60);
		}
	}
	snprintf(text, size, "wk %d%% left%s", (int)value.left, reset);
}

static size_t receive(char *data, size_t size, size_t count, void *context) {

	struct usage_client *client = context;
	if (count && size > (sizeof(client->body) - 1 - client->used) / count) {
		return 0;
	}
	size_t bytes = size * count;
	memcpy(client->body + client->used, data, bytes);
	client->used += bytes;
	client->body[client->used] = 0;

	return bytes;
}

static int header(struct usage_client *client, const char *text) {

	struct curl_slist *next = curl_slist_append(client->headers, text);
	if (!next) {
		return 0;
	}
	client->headers = next;

	return 1;
}

static int start(struct usage_client *client) {

	curl_easy_setopt(client->easy, CURLOPT_HTTPHEADER, NULL);
	curl_slist_free_all(client->headers);
	client->headers = NULL;
	struct curl_slist *auth =
		client->refresh_auth ? codex_auth_headers() : codex_auth_headers_quiet();
	if (!auth) {
		return 0;
	}
	int ok = 1;
	for (struct curl_slist *p = auth; p && ok; p = p->next) {
		if (!strncasecmp(p->data, "Authorization:", 14) ||
			!strncasecmp(p->data, "chatgpt-account-id:", 19)) {
			ok = header(client, p->data);
		}
	}
	curl_slist_free_all(auth);
	if (!ok || !header(client, "Accept: application/json") ||
		!header(client, "Origin: https://chatgpt.com") ||
		!header(client, "Referer: https://chatgpt.com/")) {
		return 0;
	}
	client->used = 0;
	client->body[0] = 0;
	curl_easy_setopt(client->easy, CURLOPT_HTTPHEADER, client->headers);

	return curl_multi_add_handle(client->multi, client->easy) == CURLM_OK;
}

struct usage_client *usage_open(void) {

	if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
		return NULL;
	}
	struct usage_client *client = calloc(1, sizeof(*client));
	if (!client) {
		curl_global_cleanup();
		return NULL;
	}
	client->easy = curl_easy_init();
	client->multi = curl_multi_init();
	if (!client->easy || !client->multi) {
		usage_close(client);
		return NULL;
	}
	curl_easy_setopt(client->easy, CURLOPT_URL, CODEX_USAGE_URL);
	curl_easy_setopt(client->easy, CURLOPT_USERAGENT, "phi-codex-usage");
	curl_easy_setopt(client->easy, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(client->easy, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(client->easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(client->easy, CURLOPT_WRITEFUNCTION, receive);
	curl_easy_setopt(client->easy, CURLOPT_WRITEDATA, client);

	return client;
}

void usage_poll(struct usage_client *client, char *text, size_t size) {

	if (!client) {
		snprintf(text, size, "wk unavailable");
		return;
	}
	time_t now = time(NULL);
	if (!client->running && now >= client->next) {
		client->next = now + 60;
		client->running = start(client);
		if (!client->running) {
			client->attempted = 1;
			client->value.available = 0;
		}
	}
	if (client->running) {
		int running;
		CURLMcode status = curl_multi_perform(client->multi, &running);
		int remaining;
		CURLMsg *message;
		while ((message = curl_multi_info_read(client->multi, &remaining))) {
			if (message->msg != CURLMSG_DONE) {
				continue;
			}
			long http;
			curl_easy_getinfo(client->easy, CURLINFO_RESPONSE_CODE, &http);
			client->http = http;
			client->transfer_error = message->data.result;
			json_object_put(client->snapshot);
			client->snapshot = NULL;
			client->value.available = 0;
			if (message->data.result == CURLE_OK && http == 200) {
				json_object *object = json_tokener_parse(client->body);
				if (json_object_is_type(object, json_type_object)) {
					client->value = usage_parse(object, now);
					client->snapshot = object;
				} else {
					json_object_put(object);
				}
			}
			curl_multi_remove_handle(client->multi, client->easy);
			client->running = 0;
			client->attempted = 1;
		}
		if (status != CURLM_OK) {
			curl_multi_remove_handle(client->multi, client->easy);
			client->running = 0;
			client->attempted = 1;
			client->value.available = 0;
		}
	}
	if (!client->attempted) {
		snprintf(text, size, "wk --");
	} else {
		usage_format(client->value, now, text, size);
	}
}

json_object *usage_fetch(char *error, size_t size) {

	struct usage_client *client = usage_open();
	if (!client) {
		snprintf(error, size, "Cannot initialize Codex usage request");
		return NULL;
	}
	client->refresh_auth = 1;
	char status[128];
	while (!client->attempted) {
		usage_poll(client, status, sizeof(status));
		if (client->running && curl_multi_poll(client->multi, NULL, 0, 1000, NULL) != CURLM_OK) {
			snprintf(error, size, "Cannot wait for Codex usage response");
			usage_close(client);
			return NULL;
		}
	}
	json_object *answer = NULL;
	if (client->snapshot) {
		answer = json_object_new_object();
		const char *keys[] = {"plan_type", "rate_limit", "code_review_rate_limit",
			"additional_rate_limits", "credits"};
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			json_object *value = json_object_object_get(client->snapshot, keys[i]);
			if (value) {
				json_object_object_add(answer, keys[i], json_object_get(value));
			}
		}
		json_object *weekly = json_object_new_object();
		json_object_object_add(
			weekly, "available", json_object_new_boolean(client->value.available));
		if (client->value.available) {
			json_object_object_add(
				weekly, "remaining_percent", json_object_new_double(client->value.left));
			json_object_object_add(
				weekly, "used_percent", json_object_new_double(100 - client->value.left));
			if (client->value.reset) {
				json_object_object_add(
					weekly, "reset_at", json_object_new_int64(client->value.reset));
			}
		}
		json_object_object_add(answer, "weekly", weekly);
		json_object_object_add(answer, "fetched_at", json_object_new_int64(time(NULL)));
	} else if (client->http == 200) {
		snprintf(error, size, "Codex usage returned invalid JSON");
	} else if (client->http) {
		snprintf(error, size, "Codex usage request failed (HTTP %ld)", client->http);
	} else if (client->transfer_error) {
		snprintf(error, size, "Codex usage request failed: %s",
			curl_easy_strerror(client->transfer_error));
	} else {
		snprintf(error, size, "Codex usage unavailable; check your Phi login");
	}
	usage_close(client);

	return answer;
}

int usage_cli(int argc, char **argv) {

	if (argc < 2 || strcmp(argv[1], "usage")) {
		return -1;
	}
	int json = argc == 3 && !strcmp(argv[2], "--json");
	if (argc != 2 && !json) {
		fprintf(stderr, "Usage: phi usage [--json]\n");
		return 2;
	}
	char error[256];
	json_object *answer = usage_fetch(error, sizeof(error));
	if (!answer) {
		if (json) {
			answer = json_object_new_object();
			json_object_object_add(answer, "error", json_object_new_string(error));
			puts(json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN));
			json_object_put(answer);
		} else {
			fprintf(stderr, "%s\n", error);
		}
		return 1;
	}
	if (json) {
		puts(json_object_to_json_string_ext(answer, JSON_C_TO_STRING_PLAIN));
	} else {
		json_object *weekly = json_object_object_get(answer, "weekly");
		struct weekly_usage value = {
			.available = json_object_get_boolean(json_object_object_get(weekly, "available")),
			.left = json_object_get_double(json_object_object_get(weekly, "remaining_percent")),
			.reset = (time_t)json_object_get_int64(json_object_object_get(weekly, "reset_at"))};
		char text[128];
		usage_format(value, time(NULL), text, sizeof(text));
		puts(text);
	}
	json_object_put(answer);

	return 0;
}

void usage_close(struct usage_client *client) {

	if (!client) {
		return;
	}
	if (client->running) {
		curl_multi_remove_handle(client->multi, client->easy);
	}
	if (client->easy) {
		curl_easy_cleanup(client->easy);
	}
	if (client->multi) {
		curl_multi_cleanup(client->multi);
	}
	json_object_put(client->snapshot);
	curl_slist_free_all(client->headers);
	free(client);
	curl_global_cleanup();
}
