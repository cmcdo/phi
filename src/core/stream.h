#ifndef PHI_STREAM_H
#define PHI_STREAM_H
#include <json-c/json.h>
#include <stddef.h>
#include <stdint.h>

struct response_sink {
	void (*text)(const char *text);
	int (*cancel)(void);
	void (*observe_text)(const char *text, void *context);
	void *observer;
	void (*usage)(int64_t input_tokens, int64_t output_tokens);
	/* Display-only, borrowed partial call; never eligible for execution. */
	void (*tool_pending)(json_object *call);
	/* Only provider-visible reasoning summaries, never hidden/encrypted reasoning. */
	void (*thinking)(const char *text, int complete);
};

struct stream {
	char *line;
	size_t length, capacity;
	json_object *output, *images, *pending;
	int failed, completed;
	int delivered; /* output reached a sink: a retry would repeat it */
	int thinking_seen;
	char body[2049]; /* first bytes of a non-SSE (error) response */
	char error[512]; /* the reason from an error event in the stream */
	size_t body_used;
	long retry_after; /* seconds from a Retry-After header, or 0 */
	struct response_sink sink;
	/* Each parsed event before Phi interprets it (provider_stream_event);
       read-only. NULL when nobody is listening. */
	void (*observe_event)(json_object *event, void *context);
	void *event_context;
};

/* Zero-initialize, optionally set sink, then release with stream_clear(). */
void stream_event(struct stream *s, const char *line);
size_t stream_receive(char *data, size_t size, size_t count, void *userdata);
void stream_finish(struct stream *s);
size_t stream_header(char *data, size_t size, size_t count, void *userdata);
void stream_clear(struct stream *s);
/* A failure's message from the server's JSON (see stream.c), or 0. */
int stream_error_text(json_object *root, char *out, size_t size);
/* "input=N cached=N output=N reasoning=N" of the last completed response, then cleared ("" if none). */
const char *stream_take_usage(void);
#endif
