#ifndef PHI_AUTH_H
#define PHI_AUTH_H
#include <curl/curl.h>
/* Caller owns headers. Normal calls refresh under a process lock; quiet calls only read. */
struct curl_slist *codex_auth_headers_cancel(int (*cancel)(void));
/* link borrows the login URL while waiting; NULL clears it before exchange/return. */
int codex_auth_login(int (*cancel)(void), void (*link)(const char *url));
int codex_auth_logout(void);
struct curl_slist *codex_auth_headers(void);
struct curl_slist *codex_auth_headers_quiet(void);
/* One key per chat (PHI_CHAT_ID, else random per process): sent as prompt_cache_key and the
   session_id header so the backend routes every request of a chat to the same prompt cache. */
const char *phi_cache_key(void);
#endif
