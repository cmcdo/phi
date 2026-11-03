#ifndef PHI_USAGE_H
#define PHI_USAGE_H
#include <stddef.h>
#include <time.h>
#include <json-c/json.h>

struct weekly_usage {
	int available;
	double left;
	time_t reset;
};
struct weekly_usage usage_parse(json_object *object, time_t now);
void usage_format(struct weekly_usage value, time_t now, char *text, size_t size);
struct usage_client;
struct usage_client *usage_open(void);
/* Advances a separate non-blocking curl transfer; copies the current status. */
void usage_poll(struct usage_client *client, char *text, size_t size);
void usage_close(struct usage_client *client);
/* One authenticated, bounded fetch; no model request. Caller owns result. */
json_object *usage_fetch(char *error, size_t size);
int usage_cli(int argc, char **argv); /* -1 unless argv[1] is usage */
#endif
