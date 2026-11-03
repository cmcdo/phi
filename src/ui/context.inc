/* Last completed request, not cumulative session billing. */
#include "../core/context.h"
#include <inttypes.h>
static int64_t context_used = -1, context_limit;
static int context_stale;

static void context_model(const char *model) {

	context_limit = context_window(model);
	context_used = -1;
	context_stale = 0;
}

static void context_tokens(char *text, size_t size, int64_t value) {

	if (value >= 1000000) {
		snprintf(text, size, "%.1fM", (double)value / 1000000);
	} else if (value >= 1000 && value % 1000) {
		snprintf(text, size, "%.1fk", (double)value / 1000);
	} else if (value >= 1000) {
		snprintf(text, size, "%" PRId64 "k", value / 1000);
	} else {
		snprintf(text, size, "%" PRId64, value);
	}
}

static inline void context_summary(char *text, size_t size) {

	char used_text[32] = "--", limit[32] = "?";
	*text = 0;
	if (context_used < 0 && !context_limit) {
		return;
	}
	if (context_used >= 0) {
		context_tokens(used_text, sizeof(used_text), context_used);
	}
	if (context_limit) {
		context_tokens(limit, sizeof(limit), context_limit);
	}
	if (context_used >= 0 && context_limit) {
		int64_t remaining = context_used < context_limit ? context_limit - context_used : 0;
		int percent = (int)(remaining * 100 / context_limit);
		snprintf(text, size, "%s / %s · %s%d%% left", used_text, limit, context_stale ? "~" : "",
			percent);
	} else {
		snprintf(text, size, "%s / %s", used_text, limit);
	}
}

void ui_context_usage(int64_t input_tokens, int64_t output_tokens) {

	if (input_tokens < 0 || output_tokens < 0 || input_tokens > INT64_MAX - output_tokens) {
		return;
	}
	context_used = input_tokens + output_tokens;
	context_stale = 0;
	dirty = 1;
}
