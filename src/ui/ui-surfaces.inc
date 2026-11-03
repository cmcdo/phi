#include "surface-cells.inc"
#include "elements.inc"

static void ui_chrome_expand(const char *text, char *out, size_t size, void *context) {

	(void)context;
	welcome_expand(text, out, size);
}

static struct ui_surface_panel {
	struct surface_client *client;
	int x, y, width, height; /* owner-local paint rectangle */
	struct phi_rect screen; /* same resolved rectangle for pointer input */
} ui_panels[PHI_FRAME_PANELS];

static void ui_surfaces_close(void) {

	for (int i = 0; i < PHI_FRAME_PANELS; i++) {
		surface_client_free(ui_panels[i].client);
		memset(&ui_panels[i], 0, sizeof(ui_panels[i]));
	}
	surface_pair_count = 0;
}

static json_object *ui_surface_context(int slot) {

	json_object *context = json_object_new_object();
	json_object_object_add(context, "model", json_object_new_string(footer_model));
	json_object_object_add(context, "effort", json_object_new_string(ui_effort()));
	json_object_object_add(context, "usage", json_object_new_string(account_usage));
	json_object_object_add(
		context, "mode", json_object_new_string(normal_mode ? "normal" : "insert"));
	char path[4096];
	welcome_expand("{path}", path, sizeof(path));
	json_object_object_add(context, "path", json_object_new_string(path));
	json_object_object_add(
		context, "state", json_object_new_string(working ? "working" : "waiting"));
	json_object_object_add(context, "scroll_offset", json_object_new_int(scroll_offset));
	json_object_object_add(context, "queued", json_object_new_int((int)queued_count));
	if (!slot) {
		char *draft = ui_editor_text();
		if (draft) {
			size_t bytes = strlen(draft), total = bytes;
			if (bytes > 8192) {
				bytes = 8192;
				while (bytes && ((unsigned char)draft[bytes] & 0xc0) == 0x80) {
					bytes--;
				}
			}
			json_object_object_add(context, "draft", json_object_new_string_len(draft, (int)bytes));
			json_object_object_add(context, "draft_bytes", json_object_new_int64((int64_t)total));
			free(draft);
		}
	}

	return context;
}

static int ui_surface_poll(int slot) {

	struct ui_surface_panel *p = &ui_panels[slot];
	if (!p->client || modal_active || pending_approval) {
		return 0;
	}
	static const char *names[] = {"composer", "transcript", "footer"};
	json_object *context = ui_surface_context(slot);
	int changed = surface_client_poll(
		p->client, skin.surfaces[slot].id, names[slot], p->width, p->height, context);
	json_object_put(context);

	return changed;
}

static int ui_surfaces_service(int visible) {

	int changed = 0;
	for (int i = 0; i < PHI_FRAME_PANELS; i++) {
		changed |= surface_client_service(ui_panels[i].client);
	}

	return visible && changed;
}

static int ui_surfaces_poll(void) {

	int changed = 0;
	for (int i = 0; i < PHI_FRAME_PANELS; i++) {
		if (surface_client_due(ui_panels[i].client)) {
			changed |= ui_surface_poll(i);
		}
	}

	return changed;
}

static void ui_surface_configure(int slot, struct phi_rect box, struct phi_rect owner) {

	struct ui_surface_panel *p = &ui_panels[slot];
	if (!*skin.surfaces[slot].id || box.height <= 0 || box.width <= 0) {
		surface_client_free(p->client);
		memset(p, 0, sizeof(*p));
		return;
	}
	if (!p->client) {
		p->client = surface_client_new();
	}
	p->x = box.x;
	p->y = box.y;
	p->width = box.width;
	p->height = box.height;
	p->screen = phi_frame_screen_rect(box, owner);
	ui_surface_poll(slot);
}

static void ui_surface_paint(int slot) {

	struct ui_surface_panel *p = &ui_panels[slot];
	if (!modal_active && !pending_approval) {
		surface_paint(p->client, p->y, p->x, p->width, p->height, 27);
	}
}

static int ui_surfaces_wait(void) {

	int visible = ui_frame_surfaces_visible(), wait = -1;
	for (int i = 0; i < PHI_FRAME_PANELS; i++) {
		int next = visible ? surface_client_wait(ui_panels[i].client)
						   : surface_client_pending_wait(ui_panels[i].client);
		if (next >= 0 && (wait < 0 || next < wait)) {
			wait = next;
		}
	}

	return wait;
}

static int ui_surface_mouse(const MEVENT *mouse) {

	if (!ui_frame_surfaces_visible()) {
		return 0;
	}
	int kind = -1;
	if (mouse->bstate & BUTTON1_PRESSED) {
		kind = PHI_WIDGET_PRESS;
	} else if (mouse->bstate & BUTTON1_RELEASED) {
		kind = PHI_WIDGET_RELEASE;
	} else if (mouse->bstate & BUTTON1_CLICKED) {
		kind = PHI_WIDGET_CLICK;
	}
	if (kind < 0) {
		return 0;
	}
	for (int i = 0; i < PHI_FRAME_PANELS; i++) {
		struct ui_surface_panel *p = &ui_panels[i];
		int x = mouse->x, y = mouse->y;
		if (!phi_frame_hit(p->screen, &x, &y)) {
			continue;
		}
		struct phi_surface_event event = {.kind = kind, .x = x, .y = y, .button = 1};
		if (surface_client_event(p->client, &event)) {
			dirty = 1;
			return 1;
		}
	}

	return 0;
}
