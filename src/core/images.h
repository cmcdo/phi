#ifndef PHI_IMAGES_H
#define PHI_IMAGES_H
#include <json-c/json.h>
#include <stddef.h>
struct stream;
int save_generated_png(
	const char *encoded, size_t size, char *path, size_t path_size, int (*cancel)(void));
/* Returns an owned item reference or a saved-path note for generated images. */
json_object *retain_output_item(struct stream *s, json_object *item);
#endif
