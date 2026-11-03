#define _GNU_SOURCE
#include "auth.h"
#include "http.h"
#include "json.h"
#include "fsutil.h"
#include "../tools.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

const char *phi_cache_key(void) {

	static char key[64];
	if (*key) {
		return key;
	}
	const char *chat = getenv("PHI_CHAT_ID");
	if (chat && *chat && strlen(chat) < 40) {
		snprintf(key, sizeof(key), "phi-%s", chat);
		return key;
	}
	unsigned char bytes[16] = {0};
	FILE *random = fopen("/dev/urandom", "rb");
	if (random) {
		if (fread(bytes, 1, sizeof(bytes), random) != sizeof(bytes)) {
			memset(bytes, 0, sizeof(bytes));
		}
		fclose(random);
	}
	char *p = key + sprintf(key, "phi-%d-", (int)getpid());
	for (size_t i = 0; i < sizeof(bytes); i++) {
		p += sprintf(p, "%02x", bytes[i]);
	}

	return key;
}

#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <ctype.h>
#include <poll.h>

#ifndef PHI_TOKEN_URL
#define PHI_TOKEN_URL "https://auth.openai.com/oauth/token"
#endif
/* URL-encoded, and escaped for use inside printf formats. */
#define REDIRECT_URI "http%%3A%%2F%%2Flocalhost%%3A1455%%2Fauth%%2Fcallback"
static const char *client_id = "app_EMoamEEZ73f0CkXaXp7hrann";

static double clock_now(void) {

	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);

	return t.tv_sec + t.tv_nsec / 1e9;
}

/* A nonempty single-line string without embedded NULs. */
static int string_ok(json_object *object, const char *key) {

	if (!json_object_is_type(object, json_type_object)) {
		return 0;
	}
	json_object *value = json_object_object_get(object, key);
	if (!json_object_is_type(value, json_type_string)) {
		return 0;
	}
	const char *text = json_object_get_string(value);

	return *text && strlen(text) == (size_t)json_object_get_string_len(value) &&
		!strpbrk(text, "\r\n");
}

static int valid(json_object *object) {

	json_object *expires = json_object_object_get(object, "expires");

	return string_ok(object, "access") && string_ok(object, "refresh") &&
		string_ok(object, "accountId") && json_object_is_type(expires, json_type_int) &&
		json_object_get_int64(expires) > 0;
}

/* $XDG_CONFIG_HOME/phi (or ~/.config/phi), created private and owned by us. */
static int directory(char *path, size_t size) {

	if (!phi_xdg_path(path, size, "XDG_CONFIG_HOME", ".config", "phi") ||
		!phi_make_directories(path)) {
		return 0;
	}
	struct stat st;

	return !lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !chmod(path, 0700);
}

static json_object *parse_strict(const char *text, size_t length) {

	json_tokener *parser = json_tokener_new();
	if (!parser) {
		return NULL;
	}
	json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
	json_object *object = json_tokener_parse_ex(parser, text, (int)length + 1);
	if (json_tokener_get_error(parser) != json_tokener_success) {
		if (object) {
			json_object_put(object);
		}
		object = NULL;
	}
	json_tokener_free(parser);

	return object;
}

static json_object *load(const char *path) {

	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		return NULL;
	}
	struct stat st;
	char text[65537];
	size_t used = 0;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_size > 65536) {
		close(fd);
		return NULL;
	}
	for (;;) {
		ssize_t n = read(fd, text + used, sizeof(text) - 1 - used);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0) {
			close(fd);
			return NULL;
		}
		if (!n) {
			break;
		}
		used += (size_t)n;
		if (used == sizeof(text) - 1) {
			break;
		}
	}
	close(fd);
	text[used] = 0;
	if (memchr(text, 0, used)) {
		return NULL;
	}

	return parse_strict(text, used);
}

/* Rename is the commit point. Never unlink the permanent lock file. */
static int save(const char *path, json_object *object) {

	const char *text = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);

	return phi_write_file_atomic(path, text, strlen(text));
}

/* Serializes credential use across Phi processes; waits up to 10 s. */
static int lock_file(const char *dir, int quiet, int (*cancel)(void)) {

	char path[4200];
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
	if (fd < 0) {
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || fchmod(fd, 0600)) {
		close(fd);
		return -1;
	}
	double deadline = clock_now() + 10;
	for (;;) {
		if (cancel && cancel()) {
			break;
		}
		if (!flock(fd, LOCK_EX | LOCK_NB)) {
			return fd;
		}
		if (quiet || (errno != EAGAIN && errno != EWOULDBLOCK) || clock_now() >= deadline) {
			break;
		}
		struct timespec pause = {0, 50000000};
		nanosleep(&pause, NULL);
	}
	close(fd);

	return -1;
}

struct buffer {
	char text[65537];
	size_t used;
};

static size_t receive(char *data, size_t size, size_t count, void *context) {

	struct buffer *b = context;
	if (size && count > (sizeof(b->text) - 1 - b->used) / size) {
		return 0;
	}
	size_t n = size * count;
	memcpy(b->text + b->used, data, n);
	b->used += n;
	b->text[b->used] = 0;

	return n;
}

/* POST a form to the token endpoint. *reply is a parsed JSON object, or NULL
   if the body was not one. Returns the transfer result; *status is the HTTP
   status. CURLE_ABORTED_BY_CALLBACK means cancel() fired. */
static CURLcode post_form(
	const char *form, int (*cancel)(void), long *status, json_object **reply) {

	*status = 0;
	*reply = NULL;
	CURL *easy = curl_easy_init();
	CURLM *multi = curl_multi_init();
	struct curl_slist *headers =
		curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
	struct buffer *body = calloc(1, sizeof(*body));
	CURLcode result = CURLE_FAILED_INIT;
	if (easy && multi && headers && body) {
		curl_easy_setopt(easy, CURLOPT_URL, PHI_TOKEN_URL);
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDS, form);
		curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receive);
		curl_easy_setopt(easy, CURLOPT_WRITEDATA, body);
		curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(easy, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
		result = phi_http_perform(multi, easy, cancel);
		curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, status);
		if (result == CURLE_OK) {
			json_object *parsed = parse_strict(body->text, body->used);
			if (json_object_is_type(parsed, json_type_object)) {
				*reply = parsed;
			} else if (parsed) {
				json_object_put(parsed);
			}
		}
	}
	free(body);
	curl_slist_free_all(headers);
	if (easy) {
		curl_easy_cleanup(easy);
	}
	if (multi) {
		curl_multi_cleanup(multi);
	}

	return result;
}

/* No automatic retry: a lost response may already have rotated the token. */
static json_object *refresh(json_object *old, int (*cancel)(void), const char **error) {

	json_object *result = NULL, *reply = NULL;
	char *escaped = curl_easy_escape(NULL, field(old, "refresh"), 0), *form = NULL;
	if (!escaped) {
		return NULL;
	}
	size_t size = strlen(escaped) + strlen(client_id) + 100;
	form = malloc(size);
	if (!form) {
		goto done;
	}
	snprintf(
		form, size, "grant_type=refresh_token&refresh_token=%s&client_id=%s", escaped, client_id);
	long status;
	CURLcode code = post_form(form, cancel, &status, &reply);
	if (code == CURLE_ABORTED_BY_CALLBACK) {
		*error = "Authentication cancelled; credentials unchanged.";
		goto done;
	}
	if (code != CURLE_OK) {
		goto done;
	}
	int rejected = status >= 400 && status < 500 && status != 429;
	if (!reply) {
		if (status == 200) {
			*error = "Malformed OAuth refresh response; credentials unchanged.";
		} else if (rejected) {
			*error =
				"OAuth refresh rejected; credentials unchanged. Check login or authentication configuration.";
		}
		goto done;
	}
	if (status != 200) {
		json_object *provider_error = json_object_object_get(reply, "error");
		const char *provider_code = json_object_is_type(provider_error, json_type_object)
			? field(provider_error, "code")
			: "";
		if (rejected) {
			*error =
				"OAuth refresh rejected; credentials unchanged. Check login or authentication configuration.";
		}
		if ((status == 400 || status == 401) &&
			(!strcmp(field(reply, "error"), "invalid_grant") ||
				!strcmp(field(reply, "error"), "invalid_token") ||
				!strcmp(provider_code, "refresh_token_reused") ||
				!strcmp(provider_code, "refresh_token_expired") ||
				!strcmp(provider_code, "refresh_token_invalidated"))) {
			*error = "Codex credentials expired or revoked; a new login is required.";
		}
		goto done;
	}
	json_object *expiry = json_object_object_get(reply, "expires_in");
	int64_t seconds = json_object_get_int64(expiry);
	if (string_ok(reply, "access_token") && string_ok(reply, "refresh_token") &&
		json_object_is_type(expiry, json_type_int) && seconds > 0 && seconds <= 31536000) {
		result = json_object_new_object();
		json_object_object_add(
			result, "access", json_object_new_string(field(reply, "access_token")));
		json_object_object_add(
			result, "refresh", json_object_new_string(field(reply, "refresh_token")));
		json_object_object_add(
			result, "accountId", json_object_new_string(field(old, "accountId")));
		json_object_object_add(
			result, "expires", json_object_new_int64((int64_t)time(NULL) * 1000 + seconds * 1000));
	} else {
		*error = "Malformed OAuth refresh response; credentials unchanged.";
	}
done:
	if (reply) {
		json_object_put(reply);
	}
	free(form);
	curl_free(escaped);

	return result;
}

static struct curl_slist *get_headers(int quiet, int (*cancel)(void)) {

	const char *error =
		"Authentication temporarily unavailable; credentials unchanged. Retry later.";
	char dir[4096], path[4200];
	struct curl_slist *headers = NULL;
	json_object *credentials = NULL;
	int lock = -1;
	if (!directory(dir, sizeof(dir))) {
		goto done;
	}
	lock = lock_file(dir, quiet, cancel);
	if (lock < 0) {
		goto done;
	}
	snprintf(path, sizeof(path), "%s/auth.json", dir);
	credentials = load(path);
	if (!valid(credentials)) {
		error = "Phi credentials missing or invalid. Run /login or phi --login.";
		goto done;
	}
	int64_t expiry = json_object_get_int64(json_object_object_get(credentials, "expires"));
	if (expiry <= (int64_t)time(NULL) * 1000 + 60000) {
		if (quiet) {
			goto done; /* usage polling must never recursively refresh */
		}
		json_object *next = refresh(credentials, cancel, &error);
		if (!next) {
			goto done;
		}
		if (!save(path, next)) {
			json_object_put(next);
			error =
				"Cannot save rotated credentials; old file preserved. A new login may be required.";
			goto done;
		}
		json_object_put(credentials);
		credentials = next;
	}
	size_t size =
		strlen(field(credentials, "access")) + strlen(field(credentials, "accountId")) + 64;
	char *line = malloc(size);
	if (!line) {
		goto done;
	}
	snprintf(line, size, "Authorization: Bearer %s", field(credentials, "access"));
	headers = curl_slist_append(NULL, line);
	snprintf(line, size, "chatgpt-account-id: %s", field(credentials, "accountId"));
	char session[96];
	snprintf(session, sizeof(session), "session_id: %s", phi_cache_key());
	const char *fixed[] = {line, "Content-Type: application/json", "Accept: text/event-stream",
		"OpenAI-Beta: responses=experimental", "originator: phi", session};
	for (size_t i = 0; headers && i < sizeof(fixed) / sizeof(*fixed); i++) {
		struct curl_slist *next = curl_slist_append(headers, fixed[i]);
		if (!next) {
			curl_slist_free_all(headers);
			headers = NULL;
			break;
		}
		headers = next;
	}
	free(line);
done:
	if (credentials) {
		json_object_put(credentials);
	}
	if (lock >= 0) {
		close(lock);
	}
	if (!headers && !quiet) {
		tool_report(stderr, "%s\n", error);
	}

	return headers;
}

struct curl_slist *codex_auth_headers(void) {

	return get_headers(0, NULL);
}

struct curl_slist *codex_auth_headers_quiet(void) {

	return get_headers(1, NULL);
}

struct curl_slist *codex_auth_headers_cancel(int (*cancel)(void)) {

	return get_headers(0, cancel);
}

static void base64url(const unsigned char *input, size_t length, char *output) {

	int n = EVP_EncodeBlock((unsigned char *)output, input, (int)length);
	while (n && output[n - 1] == '=') {
		n--;
	}
	for (int i = 0; i < n; i++) {
		if (output[i] == '+') {
			output[i] = '-';
		} else if (output[i] == '/') {
			output[i] = '_';
		}
	}
	output[n] = 0;
}

static int decode64url(const char *input, unsigned char *output, size_t size) {

	size_t n = strlen(input), original = n;
	if (n > size * 4 / 3 - 4) {
		return -1;
	}
	char *copy = malloc(n + 5);
	if (!copy) {
		return -1;
	}
	memcpy(copy, input, n);
	for (size_t i = 0; i < n; i++) {
		if (copy[i] == '-') {
			copy[i] = '+';
		} else if (copy[i] == '_') {
			copy[i] = '/';
		}
	}
	while (n % 4) {
		copy[n++] = '=';
	}
	copy[n] = 0;
	int got = EVP_DecodeBlock(output, (unsigned char *)copy, (int)n);
	free(copy);
	if (got < 0) {
		return -1;
	}

	return got - (int)(n - original);
}

/* Return an owned decoded value without altering other query parameters. */
static char *url_value(const char *query, const char *name) {

	char *result = NULL;
	size_t keylen = strlen(name);
	for (const char *p = query; *p;) {
		const char *end = strchr(p, '&');
		if (!end) {
			end = p + strlen(p);
		}
		const char *equal = memchr(p, '=', (size_t)(end - p));
		if (equal && (size_t)(equal - p) == keylen && !memcmp(p, name, keylen)) {
			if (result) {
				free(result);
				return NULL;
			} /* reject duplicate keys */
			result = malloc((size_t)(end - equal));
			if (!result) {
				return NULL;
			}
			char *out = result;
			for (const char *q = equal + 1; q < end; q++) {
				unsigned char c = (unsigned char)*q;
				if (c == '%') {
					if (end - q < 3 || !isxdigit((unsigned char)q[1]) ||
						!isxdigit((unsigned char)q[2])) {
						free(result);
						return NULL;
					}
					char hex[3] = {q[1], q[2], 0};
					c = (unsigned char)strtol(hex, NULL, 16);
					q += 2;
				} else if (c == '+') {
					c = ' ';
				}
				if (c < 32 || c == 127) {
					free(result);
					return NULL;
				}
				*out++ = (char)c;
			}
			*out = 0;
		}
		p = *end ? end + 1 : end;
	}

	return result;
}

/* Each peer gets at most two seconds, within the overall login deadline. */
static int callback_read(int fd, char *request, size_t size, double deadline, int (*cancel)(void)) {

	double peer_deadline = clock_now() + 2;
	if (peer_deadline < deadline) {
		deadline = peer_deadline;
	}
	size_t used = 0;
	while (clock_now() < deadline && used < size - 1) {
		if (cancel && cancel()) {
			return 0;
		}
		struct pollfd p = {fd, POLLIN, 0};
		int ready = poll(&p, 1, 50);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			return 0;
		}
		if (!ready) {
			continue;
		}
		ssize_t n = recv(fd, request + used, size - 1 - used, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				continue;
			}
			return 0;
		}
		if (!n || memchr(request + used, 0, (size_t)n)) {
			return 0;
		}
		used += (size_t)n;
		request[used] = 0;
		if (strstr(request, "\r\n\r\n")) {
			return 1;
		}
	}

	return 0;
}

/* The ChatGPT account id is a claim inside the access token (a JWT). */
static void account_from_token(const char *token, char *account, size_t size) {

	*account = 0;
	const char *first = strchr(token, '.'), *second = first ? strchr(first + 1, '.') : NULL;
	if (!first || !second || (size_t)(second - first - 1) >= 32700) {
		return;
	}
	char part[32768];
	unsigned char decoded[32768];
	memcpy(part, first + 1, (size_t)(second - first - 1));
	part[second - first - 1] = 0;
	int got = decode64url(part, decoded, sizeof(decoded));
	if (got <= 0) {
		return;
	}
	decoded[got] = 0;
	json_object *jwt = json_tokener_parse((char *)decoded);
	json_object *claim = jwt ? json_object_object_get(jwt, "https://api.openai.com/auth") : NULL;
	if (string_ok(claim, "chatgpt_account_id")) {
		snprintf(account, size, "%s", field(claim, "chatgpt_account_id"));
	}
	if (jwt) {
		json_object_put(jwt);
	}
}

static json_object *exchange_code(
	const char *code, const char *verifier, int (*cancel)(void), const char **error) {

	json_object *result = NULL, *reply = NULL;
	char *escaped_code = curl_easy_escape(NULL, code, 0),
		 *escaped_verifier = curl_easy_escape(NULL, verifier, 0);
	char *form = NULL;
	if (!escaped_code || !escaped_verifier) {
		goto done;
	}
	size_t size = strlen(escaped_code) + strlen(escaped_verifier) + strlen(client_id) + 200;
	form = malloc(size);
	if (!form) {
		goto done;
	}
	snprintf(form, size,
		"grant_type=authorization_code&client_id=%s&code=%s&code_verifier=%s&redirect_uri=" REDIRECT_URI,
		client_id, escaped_code, escaped_verifier);
	long status;
	CURLcode transfer = post_form(form, cancel, &status, &reply);
	if (transfer == CURLE_ABORTED_BY_CALLBACK) {
		*error = "Login cancelled.";
		goto done;
	}
	if (transfer != CURLE_OK || status != 200) {
		goto done;
	}
	json_object *expiry = reply ? json_object_object_get(reply, "expires_in") : NULL;
	int64_t seconds = json_object_get_int64(expiry);
	if (reply && string_ok(reply, "access_token") && string_ok(reply, "refresh_token") &&
		json_object_is_type(expiry, json_type_int) && seconds > 0) {
		const char *token = field(reply, "access_token");
		char account[512];
		account_from_token(token, account, sizeof(account));
		if (*account) {
			result = json_object_new_object();
			json_object_object_add(result, "access", json_object_new_string(token));
			json_object_object_add(
				result, "refresh", json_object_new_string(field(reply, "refresh_token")));
			json_object_object_add(result, "expires",
				json_object_new_int64((int64_t)time(NULL) * 1000 + seconds * 1000));
			json_object_object_add(result, "accountId", json_object_new_string(account));
		}
	}
	if (!result) {
		*error = "Login returned invalid credentials; nothing was saved.";
	}
done:
	if (reply) {
		json_object_put(reply);
	}
	free(form);
	curl_free(escaped_code);
	curl_free(escaped_verifier);

	return result;
}

int codex_auth_logout(void) {

	char dir[4096], path[4200];
	if (!directory(dir, sizeof(dir))) {
		return 0;
	}
	int lock = lock_file(dir, 0, NULL);
	if (lock < 0) {
		return 0;
	}
	snprintf(path, sizeof(path), "%s/auth.json", dir);
	struct stat st;
	int ok = 0;
	if (lstat(path, &st)) {
		ok = errno == ENOENT;
	} else if (S_ISREG(st.st_mode) && st.st_uid == geteuid()) {
		ok = !unlink(path);
	}
	close(lock);
	tool_report(stderr, ok ? "Phi credentials removed.\n" : "Could not remove Phi credentials.\n");

	return ok;
}

/* Browser OAuth with PKCE and a one-shot localhost:1455 callback listener.
   The credential lock is taken only to save: every request takes it too,
   so holding it through the browser wait would stall Phi's other chats
   (and their logins) for minutes. The port keeps logins one at a time. */
int codex_auth_login(int (*cancel)(void)) {

	const char *error = "Login failed; existing credentials unchanged.";
	char dir[4096], path[4200];
	int lock = -1, server = -1, ok = 0;
	pid_t child = -1;
	if (!directory(dir, sizeof(dir))) {
		goto done;
	}

	unsigned char random[64], digest[SHA256_DIGEST_LENGTH];
	char verifier[96], challenge[64], state[40];
	if (RAND_bytes(random, sizeof(random)) != 1) {
		goto done;
	}
	base64url(random, sizeof(random), verifier);
	SHA256((unsigned char *)verifier, strlen(verifier), digest);
	base64url(digest, sizeof(digest), challenge);
	if (RAND_bytes(random, 16) != 1) {
		goto done;
	}
	for (int i = 0; i < 16; i++) {
		sprintf(state + i * 2, "%02x", random[i]);
	}

	server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	int one = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in address = {
		.sin_family = AF_INET, .sin_port = htons(1455), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address)) ||
		listen(server, 4)) {
		error =
			"Cannot listen on localhost:1455 for OAuth callback (is a login already waiting in another Phi?).";
		goto done;
	}
	char url[4096];
	snprintf(url, sizeof(url),
		"https://auth.openai.com/oauth/authorize?response_type=code&client_id=%s&redirect_uri=" REDIRECT_URI
		"&scope=openid%%20profile%%20email%%20offline_access&code_challenge=%s&code_challenge_method=S256"
		"&state=%s&id_token_add_organizations=true&codex_cli_simplified_flow=true&originator=phi",
		client_id, challenge, state);
	child = fork();
	if (!child) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		execlp("xdg-open", "xdg-open", url, (char *)NULL);
		_exit(127);
	}
	tool_report(stderr,
		"Opening browser for Phi login. Waiting up to 5 minutes; press Esc/Ctrl-C to cancel.\n"
		"If it does not open, visit:\n%s\n",
		url);

	double deadline = clock_now() + 300;
	char request[16385], *code = NULL;
	while (clock_now() < deadline && !code) {
		if (cancel && cancel()) {
			error = "Login cancelled.";
			break;
		}
		struct pollfd p = {server, POLLIN, 0};
		if (poll(&p, 1, 50) <= 0) {
			continue;
		}
		int client = accept(server, NULL, NULL);
		if (client < 0) {
			continue;
		}
		int flags = fcntl(client, F_GETFL);
		if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0 ||
			fcntl(client, F_SETFD, FD_CLOEXEC) < 0) {
			close(client);
			continue;
		}
		if (callback_read(client, request, sizeof(request), deadline, cancel)) {
			char *line_end = strstr(request, "\r\n"), *version = strstr(request, " HTTP/");
			const char *prefix = "GET /auth/callback?";
			if (version && line_end && version < line_end &&
				!strncmp(request, prefix, strlen(prefix))) {
				*version = 0;
				const char *query = request + strlen(prefix);
				char *got_state = url_value(query, "state");
				code = url_value(query, "code");
				if (!got_state || strcmp(got_state, state) || !code || !*code) {
					free(code);
					code = NULL;
				}
				free(got_state);
			}
			const char *response = code
				? "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
				  "<h1>Callback received</h1><p>You can return to Phi.</p>"
				: "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid OAuth callback.";
			(void)send(client, response, strlen(response), MSG_NOSIGNAL);
		}
		close(client);
	}
	if (!code) {
		if (clock_now() >= deadline) {
			error = "Login timed out.";
		}
		goto done;
	}
	json_object *credentials = exchange_code(code, verifier, cancel, &error);
	free(code);
	if (!credentials) {
		goto done;
	}
	lock = lock_file(dir, 0, cancel);
	if (lock < 0) {
		json_object_put(credentials);
		error = cancel && cancel()
			? "Login cancelled; existing credentials unchanged."
			: "Credentials are busy in another Phi; login not saved. Try /login again.";
		goto done;
	}
	snprintf(path, sizeof(path), "%s/auth.json", dir);
	ok = save(path, credentials);
	json_object_put(credentials);
	if (!ok) {
		error = "Login succeeded but credentials could not be saved; existing file preserved.";
	}
done:
	if (server >= 0) {
		close(server);
	}
	if (child > 0) {
		waitpid(child, NULL, WNOHANG);
	}
	if (lock >= 0) {
		close(lock);
	}
	if (ok) {
		tool_report(stderr, "Phi login complete.\n");
	} else {
		tool_report(stderr, "%s\n", error);
	}

	return ok;
}
