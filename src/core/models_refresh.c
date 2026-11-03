#define _POSIX_C_SOURCE 200809L
#include "models.h"
#include "auth.h"
#include "fsutil.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

/* The models endpoint hides models from clients older than it wants (with
   0.156.1, gpt-6.1-sol was missing until 0.159.0), and the threshold moves
   with each Codex release. Phi is not the Codex CLI, so it asks as a client
   from the future to see everything, and falls back to the newest version
   known to work if the server ever refuses that. */
#ifndef CODEX_CLIENT_VERSION
#define CODEX_CLIENT_VERSION "0.159.0"
#endif
static const char *const client_versions[] = {"999.0.0", CODEX_CLIENT_VERSION};

struct download {
	char data[4 * 1024 * 1024 + 1];
	size_t size;
};

static size_t receive(char *ptr, size_t size, size_t count, void *ctx) {

	struct download *d = ctx;
	if (count && size > (sizeof(d->data) - 1 - d->size) / count) {
		return 0;
	}
	size_t n = size * count;
	memcpy(d->data + d->size, ptr, n);
	d->size += n;
	d->data[d->size] = 0;

	return n;
}

/* The model list says what the Codex app offers, not what the API takes:
   most models accept "none" though the list leaves it out, and its "ultra"
   (Codex's task delegation) is no API level at all. The API says the truth
   when asked for a level a model lacks ("Supported values are: 'none',
   'low', ..."), so each listed model is asked once, at "minimal", which no
   model took when this was written. A model that does take it is cut off
   as soon as it answers. */
#define PROBES 32

struct probe {
	CURL *easy;
	json_object *model;
	char reply[4096];
	size_t size;
	long status;
};

static size_t probe_receive(char *ptr, size_t size, size_t count, void *ctx) {

	struct probe *p = ctx;
	size_t n = size * count, room = sizeof(p->reply) - 1 - p->size;
	curl_easy_getinfo(p->easy, CURLINFO_RESPONSE_CODE, &p->status);
	memcpy(p->reply + p->size, ptr, n < room ? n : room);
	p->size += n < room ? n : room;
	p->reply[p->size] = 0;
	/* A refusal can also come as an error event in a 200 stream. Stop at
       the answer, or as soon as the model starts on real work. */
	if (p->status == 200 &&
		(strstr(p->reply, "Supported values are:") || strstr(p->reply, "output_item") ||
			strstr(p->reply, "delta") || !room)) {
		return 0;
	}

	return n;
}

/* The quoted names after "Supported values are:", as a JSON array. */
static json_object *probe_levels(const char *reply) {

	const char *at = strstr(reply, "Supported values are:");
	if (!at) {
		return NULL;
	}
	const char *end = strchr(at, '"'); /* the end of the JSON message string */
	json_object *levels = json_object_new_array();
	for (const char *q = strchr(at, '\''); levels && q && (!end || q < end);
		q = strchr(q + 1, '\'')) {
		const char *close = strchr(q + 1, '\'');
		if (!close || (end && close > end)) {
			break;
		}
		size_t n = (size_t)(close - q - 1);
		char name[16];
		if (n && n < sizeof(name) && strspn(q + 1, "abcdefghijklmnopqrstuvwxyz0123456789-_") >= n) {
			memcpy(name, q + 1, n);
			name[n] = 0;
			json_object_array_add(levels, json_object_new_string(name));
		}
		q = close;
	}
	if (levels && !json_object_array_length(levels)) {
		json_object_put(levels);
		levels = NULL;
	}

	return levels;
}

/* Ask each listed model in parallel; record "phi_api_levels" on those that
   answered with their levels. */
static void probe_models(json_object *models, struct curl_slist *headers) {

	struct probe *probes = calloc(PROBES, sizeof(*probes));
	CURLM *multi = curl_multi_init();
	if (!probes || !multi) {
		free(probes);
		if (multi) {
			curl_multi_cleanup(multi);
		}
		return;
	}
	size_t used = 0;
	char bodies[PROBES][256];
	for (size_t i = 0; i < json_object_array_length(models) && used < PROBES; i++) {
		json_object *model = json_object_array_get_idx(models, i), *value;
		const char *slug =
			json_object_object_get_ex(model, "slug", &value) ? json_object_get_string(value) : NULL;
		const char *shown = json_object_object_get_ex(model, "visibility", &value)
			? json_object_get_string(value)
			: "";
		if (!slug || strcmp(shown, "list") || strlen(slug) > 96 ||
			strspn(slug, "abcdefghijklmnopqrstuvwxyz0123456789-_.") != strlen(slug)) {
			continue;
		}
		struct probe *p = &probes[used];
		snprintf(bodies[used], sizeof(bodies[used]),
			"{\"model\":\"%s\",\"instructions\":\"Reply with one word.\","
			"\"input\":[{\"role\":\"user\",\"content\":\"ok\"}],\"store\":false,\"stream\":true,"
			"\"reasoning\":{\"effort\":\"minimal\"}}",
			slug);
		p->easy = curl_easy_init();
		if (!p->easy) {
			break;
		}
		p->model = model;
		curl_easy_setopt(p->easy, CURLOPT_URL, "https://chatgpt.com/backend-api/codex/responses");
		curl_easy_setopt(p->easy, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(p->easy, CURLOPT_USERAGENT, "phi");
		curl_easy_setopt(p->easy, CURLOPT_POSTFIELDS, bodies[used]);
		curl_easy_setopt(p->easy, CURLOPT_WRITEFUNCTION, probe_receive);
		curl_easy_setopt(p->easy, CURLOPT_WRITEDATA, p);
		curl_easy_setopt(p->easy, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(p->easy, CURLOPT_TIMEOUT, 15L);
		curl_easy_setopt(p->easy, CURLOPT_NOSIGNAL, 1L);
		curl_multi_add_handle(multi, p->easy);
		used++;
	}
	int running = 1;
	while (running) {
		if (curl_multi_perform(multi, &running) != CURLM_OK) {
			break;
		}
		if (running) {
			curl_multi_poll(multi, NULL, 0, 200, NULL);
		}
	}
	for (size_t i = 0; i < used; i++) {
		struct probe *p = &probes[i];
		curl_easy_getinfo(p->easy, CURLINFO_RESPONSE_CODE, &p->status);
		json_object *levels = probe_levels(p->reply);
		if (levels) {
			json_object_object_add(p->model, "phi_api_levels", levels);
		}
		curl_multi_remove_handle(multi, p->easy);
		curl_easy_cleanup(p->easy);
	}
	curl_multi_cleanup(multi);
	free(probes);
}

int phi_catalog_refresh(char *error, size_t cap) {

	char dir[4096], path[4096];
	if (!phi_catalog_cache_path(path, sizeof(path))) {
		snprintf(error, cap, "Invalid catalogue path.");
		return 0;
	}
	snprintf(dir, sizeof(dir), "%s", path);
	*strrchr(dir, '/') = 0;
	struct curl_slist *headers = codex_auth_headers();
	if (!headers) {
		snprintf(error, cap, "Sign in with /login first.");
		return 0;
	}
	CURL *curl = curl_easy_init();
	struct download *body = calloc(1, sizeof(*body));
	long status = 0;
	if (!curl || !body) {
		snprintf(error, cap, "Out of memory.");
		goto fail;
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "phi");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
	CURLcode result = CURLE_OK;
	for (size_t attempt = 0; attempt < sizeof(client_versions) / sizeof(*client_versions);
		attempt++) {
		char url[160];
		snprintf(url, sizeof(url), "https://chatgpt.com/backend-api/codex/models?client_version=%s",
			client_versions[attempt]);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		body->size = 0;
		body->data[0] = 0;
		result = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		if (result == CURLE_OK && status == 200 && strstr(body->data, "\"slug\"")) {
			break;
		}
	}
	if (result != CURLE_OK || status != 200) {
		snprintf(error, cap, "Model refresh failed (HTTP %ld%s).", status,
			result == CURLE_WRITE_ERROR ? ", response too large" : "");
		goto fail;
	}
	if (!body->size || memchr(body->data, 0, body->size)) {
		snprintf(error, cap, "Invalid model catalogue response.");
		goto fail;
	}
	struct json_tokener *parser = json_tokener_new();
	if (!parser) {
		snprintf(error, cap, "Out of memory.");
		goto fail;
	}
	json_object *root = json_tokener_parse_ex(parser, body->data, (int)body->size + 1);
	int valid = json_tokener_get_error(parser) == json_tokener_success &&
		json_object_is_type(json_object_object_get(root, "models"), json_type_array);
	int models_found =
		valid && json_object_array_length(json_object_object_get(root, "models")) > 0;
	json_tokener_free(parser);
	if (!valid) {
		if (root) {
			json_object_put(root);
		}
		snprintf(error, cap, "Invalid model catalogue response.");
		goto fail;
	}
	/* An empty success is not a useful catalogue; preserve the previous cache. */
	if (!models_found) {
		json_object_put(root);
		snprintf(error, cap, "Codex returned no models; cache unchanged.");
		goto fail;
	}
	probe_models(json_object_object_get(root, "models"), headers);
	const char *saved = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
	int written =
		phi_make_directories(dir) && saved && phi_write_file_atomic(path, saved, strlen(saved));
	json_object_put(root);
	if (!written) {
		snprintf(error, cap, "Cannot save catalogue cache.");
		goto fail;
	}
	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	free(body);
	return 1;
fail:
	if (curl) {
		curl_easy_cleanup(curl);
	}
	curl_slist_free_all(headers);
	free(body);

	return 0;
}
