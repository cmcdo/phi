#define _GNU_SOURCE
#include "proctitle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

static char *area; /* the original argument strings, end to end */
/* Their copies, used for the rest of the run. Nothing reads this pointer,
   so it is volatile: an optimiser would drop the store, and the copies
   would look leaked to LeakSanitizer when they are kept on purpose. */
static char **volatile arguments;
static size_t area_size;

char **proctitle_init(int argc, char **argv) {

	char **copy = calloc((size_t)argc + 1, sizeof(*copy));
	if (!copy) {
		return argv;
	}
	for (int i = 0; i < argc; i++) {
		copy[i] = strdup(argv[i]);
		if (!copy[i]) {
			for (int j = 0; j < i; j++) {
				free(copy[j]);
			}
			free(copy);
			return argv;
		}
	}
	/* Only a contiguous block can be reused safely. */
	char *end = argv[0];
	for (int i = 0; i < argc; i++) {
		if (argv[i] != end) {
			break;
		}
		end = argv[i] + strlen(argv[i]) + 1;
	}
	area = argv[0];
	area_size = (size_t)(end - argv[0]);

	return arguments = copy;
}

void proctitle_set(const char *name, const char *title) {

	if (name) {
		prctl(PR_SET_NAME, name, 0, 0, 0);
	}
	if (!area || area_size < 2 || !title) {
		return;
	}
	memset(area, 0, area_size);
	snprintf(area, area_size, "%s", title);
}
