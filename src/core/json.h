#ifndef PHI_JSON_H
#define PHI_JSON_H
#include <json-c/json.h>

static inline const char *field(json_object *object, const char *name) {

	json_object *value = NULL;
	const char *text =
		json_object_object_get_ex(object, name, &value) ? json_object_get_string(value) : NULL;

	return text ? text : "";
}
#endif
