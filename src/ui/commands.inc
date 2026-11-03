/* Inline slash suggestions. Selection never invokes the model. */
#include "../core/commands.h"
#include "../core/models.h"

static int editor_command_allowed(const struct phi_command *command) {

	if (!editor_workspace) {
		return 1;
	}
	if (editor_workspace == 2) {
		return 0;
	}
	for (size_t i = 0; i < PHI_COMMAND_COUNT; i++) {
		if (!strcmp(command->name, phi_commands[i].name)) {
			return command_workspace_allowed(i);
		}
	}

	return 0;
}

static int command_selection, command_first, command_dismissed, command_count, file_suggestions;
static wchar_t command_query_initial[INPUT_SIZE];
static wchar_t *command_query = command_query_initial;
static size_t command_query_capacity = INPUT_SIZE;

static struct {
	const char *text, *description;
	int argument;
} command_matches[512];

static int file_matches_limited;
#include "file-completion.inc"

static void command_files_reset(void) {

	if (file_suggestions) {
		dirty = 1;
	}
	project_files_clear();
	file_suggestions = file_matches_limited = 0;
	command_count = 0;
}

/* Argument suggestions made while typing (theme and extension names). Their
   text lives here until the next refresh; descriptions are constant. */
static char command_pool[128][128];
static int command_pool_used;

static void command_suggest(const char *query, const char *text, const char *description) {

	if (strncmp(text, query, strlen(query)) || command_pool_used >= 128 ||
		command_count >= (int)(sizeof(command_matches) / sizeof(*command_matches))) {
		return;
	}
	snprintf(command_pool[command_pool_used], sizeof(command_pool[0]), "%s", text);
	command_matches[command_count].text = command_pool[command_pool_used++];
	command_matches[command_count].description = description;
	command_matches[command_count++].argument = 0;
}

/* Each extension source (~/.config/phi/extensions/NAME.c) as "PREFIX NAME". */
static void command_suggest_extensions(
	const char *query, const char *prefix, const char *description) {

	char directory[4096];
	if (!phi_xdg_path(
			directory, sizeof(directory), "XDG_CONFIG_HOME", ".config", "phi/extensions")) {
		return;
	}
	DIR *dir = opendir(directory);
	struct dirent *entry;
	while (dir && (entry = readdir(dir))) {
		size_t n = strlen(entry->d_name);
		if (n < 3 || n > 60 || strcmp(entry->d_name + n - 2, ".c") || entry->d_name[0] == '.') {
			continue;
		}
		char text[128];
		snprintf(text, sizeof(text), "%s %.*s", prefix, (int)(n - 2), entry->d_name);
		command_suggest(query, text, description);
	}
	if (dir) {
		closedir(dir);
	}
}

static void command_refresh(void) {

	int was_files = file_suggestions;
	command_count = file_suggestions = file_matches_limited = 0;
	command_pool_used = 0;
	size_t token = length;
	while (token && !iswspace(input[token - 1])) {
		token--;
	}
	if (!normal_mode && !pasting && COLS >= 30 && LINES >= 10 && cursor == length) {
		if (token < length && input[token] == L'@') {
			file_suggestions = 1;
		} else if (input[0] != L'/' || editor_workspace == 2) {
			goto hidden;
		}
	} else {
		goto hidden;
	}
	if (wcscmp(command_query, input)) {
		if (!wide_reserve(
				&command_query, &command_query_capacity, length + 1, command_query_initial)) {
			goto hidden;
		}
		wcscpy(command_query, input);
		command_selection = command_first = command_dismissed = 0;
	}
	if (!file_suggestions || !was_files) {
		project_files_clear();
	}
	if (command_dismissed) {
		return;
	}
	if (file_suggestions) {
		project_files_load();
		size_t room = SIZE_MAX / sizeof(wchar_t) - 3;
		for (size_t i = 0; i < project_file_count; i++) {
			if (!fuzzy_file(project_files[i], input + token + 1, room)) {
				continue;
			}
			if (command_count == (int)(sizeof(command_matches) / sizeof(*command_matches))) {
				file_matches_limited = 1;
				break;
			}
			command_matches[command_count].text = project_files[i];
			command_matches[command_count].description = "file";
			command_matches[command_count++].argument = 1;
		}
		if (command_selection >= command_count) {
			command_selection = 0;
		}
		return;
	}
	char query[128];
	if (length >= sizeof(query)) {
		return;
	}
	for (size_t i = 0; i <= length; i++) {
		if (input[i] > 127 || input[i] == L'\n') {
			return;
		}
		query[i] = (char)input[i];
	}
	if (editor_workspace) {
		size_t command_token = strcspn(query, " \t");
		for (size_t i = 0; i < PHI_COMMAND_COUNT; i++) {
			if (strlen(phi_commands[i].name) == command_token &&
				!strncmp(query, phi_commands[i].name, command_token) &&
				!command_workspace_allowed(i)) {
				return;
			}
		}
	}
	if (!strncmp(query, "/model ", 7)) {
		for (size_t i = 0; i < phi_catalog_count(); i++) {
			const struct phi_catalog_model *model = phi_catalog_at(i);
			if (!model || strncmp(model->command, query, strlen(query))) {
				continue;
			}
			command_matches[command_count].text = model->command;
			command_matches[command_count].description = model->label;
			command_matches[command_count++].argument = 0;
		}
		size_t extra = extra_model_count && extra_model_at ? extra_model_count() : 0;
		for (size_t i = 0; i < extra; i++) {
			const char *id, *name;
			if (!extra_model_at(i, &id, &name, NULL, NULL) || !id) {
				continue;
			}
			char text[128];
			snprintf(text, sizeof(text), "/model %s", id);
			command_suggest(query, text, name && *name ? name : "Extension model");
		}
	} else if (!strncmp(query, "/effort ", 8)) {
		/* The current model's levels, in the server's order and words. */
		static char texts[PHI_LEVELS][32], descriptions[PHI_LEVELS][128];
		const char *name = NULL, *description = NULL;
		int preferred = 0;
		size_t count = model_level(0, &name, &description, &preferred);
		for (size_t i = 0; i < count && i < PHI_LEVELS; i++) {
			model_level(i, &name, &description, &preferred);
			snprintf(
				texts[i], sizeof(texts[i]), "/effort %s", !strcmp(name, "none") ? "off" : name);
			if (strncmp(texts[i], query, strlen(query))) {
				continue;
			}
			snprintf(descriptions[i], sizeof(descriptions[i]), "%s%s", description,
				preferred ? " (default)" : "");
			command_matches[command_count].text = texts[i];
			command_matches[command_count].description = descriptions[i];
			command_matches[command_count++].argument = 0;
		}
	} else if (!strncmp(query, "/approval ", 10)) {
		static const char *values[] = {"/approval off", "/approval dangerous", "/approval all"};
		static const char *descriptions[] = {
			"No prompts", "Approve bash/write/edit", "Approve every model tool"};
		for (int i = 0; i < 3; i++) {
			if (!strncmp(values[i], query, strlen(query))) {
				command_matches[command_count].text = values[i];
				command_matches[command_count].description = descriptions[i];
				command_matches[command_count++].argument = 0;
			}
		}
	} else if (!strncmp(query, "/sandbox ", 9)) {
		static const char *values[] = {"/sandbox off", "/sandbox workspace", "/sandbox strict"};
		static const char *descriptions[] = {
			"Unrestricted", "Project filesystem, network on", "Project filesystem, network off"};
		for (int i = 0; i < 3; i++) {
			if (!strncmp(values[i], query, strlen(query))) {
				command_matches[command_count].text = values[i];
				command_matches[command_count].description = descriptions[i];
				command_matches[command_count++].argument = 0;
			}
		}
	} else if (!strncmp(query, "/theme ", 7)) {
		command_pool_used = 0;
		json_object *names = ui_theme_list();
		for (size_t i = 0; i < json_object_array_length(names); i++) {
			const char *name = json_object_get_string(json_object_array_get_idx(names, i));
			char text[128];
			snprintf(text, sizeof(text), "/theme %.100s", name);
			command_suggest(query, text,
				!strcmp(name, "default")                ? "Phi's own look"
					: !strcmp(name, theme_current_name) ? "Current theme"
														: "Theme");
		}
		json_object_put(names);
	} else if (!strncmp(query, "/reload-extensions ", 19)) {
		command_pool_used = 0;
		command_suggest_extensions(query, "/reload-extensions", "Reload this extension");
	} else if (!strncmp(query, "/extensions ", 12)) {
		command_pool_used = 0;
		command_suggest(query, "/extensions api", "Show the extension API and guide");
		command_suggest_extensions(query, "/extensions disable", "Disable this extension");
	} else {
		for (size_t i = 0; i < ui_command_count(); i++) {
			const struct phi_command *command = ui_command_at(i);
			if (!command || !editor_command_allowed(command) ||
				strncmp(command->name, query, strlen(query))) {
				continue;
			}
			command_matches[command_count].text = command->name;
			command_matches[command_count].description =
				command_description(command, editor_workspace);
			command_matches[command_count++].argument = command->argument;
		}
	}
	if (command_selection >= command_count) {
		command_selection = 0;
	}
	return;
hidden:
	command_files_reset();
	command_query[0] = 0;
	command_dismissed = 0;
}

static int command_rows(int top) {

	command_refresh();
	int n = command_count < 4 ? command_count : 4;
	int notice =
		file_suggestions && !command_dismissed && (project_files_limited || file_matches_limited);
	int available = top - 2;
	if (available <= 0) {
		return 0;
	}
	int capacity = available - notice;
	if (n > capacity) {
		n = capacity;
	}

	return (n > 0 ? n : 0) + notice;
}

static int command_scroll(int first, int selected, int visible, int count) {

	if (visible <= 0 || count <= visible) {
		return 0;
	}
	if (selected < first) {
		first = selected;
	} else if (selected >= first + visible) {
		first = selected - visible + 1;
	}
	int maximum = count - visible;

	return first > maximum ? maximum : first;
}

static void command_draw(int y, int rows, int cols) {

	if (rows < 1) {
		return;
	}
	/* Opaque overlay: erase covered transcript cells before painting labels. */
	attrset(COLOR_PAIR(27));
	for (int i = 0; i < rows; i++) {
		mvhline(y + i, 0, ' ', cols);
	}
	attrset(A_NORMAL);
	const char *notice = file_suggestions && !command_dismissed
		? (project_files_limited         ? "File scan incomplete (limit or I/O error)"
				  : file_matches_limited ? "More matches: type to narrow"
										 : NULL)
		: NULL;
	if (notice) {
		welcome_text(y++, skin.margin, cols - 2 * skin.margin, notice, 3, 0);
	}
	int visible = rows - (notice != NULL);
	int description_x = 24;
	command_first = command_scroll(command_first, command_selection, visible, command_count);
	for (int i = 0; i < visible && command_first + i < command_count; i++) {
		int at = command_first + i, selected = at == command_selection;
		const char *label = command_matches[at].text;
		if (!strncmp(label, "/model ", 7)) {
			label += 7;
		} else if (!strncmp(label, "/theme ", 7)) {
			label += 7;
		} else if (!strncmp(label, "/reload-extensions ", 19)) {
			label += 19;
		} else if (!strncmp(label, "/extensions ", 12)) {
			label += 12;
		}
		welcome_text(y + i, skin.margin, description_x - skin.margin - 2, label, selected ? 28 : 3,
			selected);
		welcome_text(y + i, description_x, cols - description_x - 2,
			command_matches[at].description, selected ? 27 : 3, 0);
	}
}

/* -1 unhandled, 0 consumed, 1 submit. */
static int command_key(int kind, wint_t key) {

	command_refresh();
	if (!command_count) {
		return -1;
	}
	if (kind == KEY_CODE_YES && (key == KEY_UP || key == KEY_DOWN)) {
		command_selection =
			(command_selection + (key == KEY_UP ? command_count - 1 : 1)) % command_count;
		return 0;
	}
	if (kind == OK && key == 27) {
		command_dismissed = 1;
		command_files_reset();
		return 0;
	}
	int enter = (kind == OK && key == '\r') || (kind == KEY_CODE_YES && key == KEY_ENTER);
	if (!enter && !(kind == OK && key == '\t')) {
		return -1;
	}
	const char *text = command_matches[command_selection].text;
	int argument = command_matches[command_selection].argument;
	if (file_suggestions) {
		size_t begin = length;
		while (begin && !iswspace(input[begin - 1])) {
			begin--;
		}
		wchar_t selected[INPUT_SIZE];
		size_t n = mbstowcs(selected, text, INPUT_SIZE);
		struct stat info;
		/* An external build/editor may remove a file while the menu is open. */
		if (n == (size_t)-1 || n >= INPUT_SIZE || !input_reserve(begin + n + 2) ||
			lstat(text, &info) || !S_ISREG(info.st_mode)) {
			command_files_reset();
			beep();
			return 0;
		}
		length = begin;
		input[length++] = L'@';
		memcpy(input + length, selected, n * sizeof(*input));
		length += n;
		input[length++] = L' ';
		input[length] = 0;
		cursor = length;
		input_view = 0;
		command_files_reset();
		return 0;
	}
	if (!input_reserve(strlen(text) + (argument != 0))) {
		return 0;
	}
	length = strlen(text);
	for (size_t i = 0; i <= length; i++) {
		input[i] = (unsigned char)text[i];
	}
	if (argument) {
		input[length++] = L' ';
		input[length] = 0;
	}
	cursor = length;
	input_view = 0;

	return enter && !argument ? 1 : 0;
}
