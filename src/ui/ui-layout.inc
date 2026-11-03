/* Gather intrinsic sizes once. Providers are not polled here: their current
   rectangles are resolved before any context/frame request is sent. */
static void ui_frame_measure(struct phi_frame_measure *m, int *cy, int *cx) {

	int rows = LINES, cols = COLS;
	struct phi_rect transcript_box = {ui_chat_x, ui_chat_y, cols, rows};
	struct phi_rect editor = transcript_box;
	if (ui_editor_canvas) {
		editor = (struct phi_rect){
			ui_editor_x, ui_editor_y, getmaxx(ui_editor_canvas), getmaxy(ui_editor_canvas)};
	}
	int detached = ui_host_canvas[PHI_HOST_FOOTER] != NULL;
	struct phi_rect footer = detached ? ui_host_boxes[PHI_HOST_FOOTER] : editor;
	*m = (struct phi_frame_measure){.owners = {editor, transcript_box, footer},
		.split = ui_split_hosts,
		.footer_detached = detached,
		.margin = skin.margin,
		.footer_visible = skin.footer_visible,
		.footer_rows = skin.footer_rows,
		.footer_lines = skin.footer_line_count,
		.composer_min_rows = skin.composer_min_rows,
		.composer_max_rows = skin.composer_max_rows,
		.composer_max_percent = skin.composer_max_percent,
		.input_view = input_view,
		.above_rows = widget_rows(0),
		.below_rows = widget_rows(1),
		.header_rows = ui_host_canvas[PHI_HOST_HEADER] ? 0 : header_rows(),
		.side_rows = side_rows(editor.width, editor.height),
		.queue_rows = ui_host_canvas[PHI_HOST_QUEUE] ? 0 : queue_rows(editor.width)};
	m->input_columns = phi_frame_input_columns(editor.width, skin.border == SKIN_BORDER_ROUNDED,
		*skin.prompt != 0, skin.composer_padding, skin.compact_layout);
	m->draft_rows = draft_layout(m->input_columns.width, 0, 0, cy, cx);
	m->caret_row = *cy;
	for (int i = 0; i < PHI_FRAME_PANELS; i++) {
		struct phi_rect owner = m->owners[i];
		m->panels[i] =
			surface_rows(&skin.surfaces[i], owner.height, owner.width - 2 * skin.margin, INT_MAX);
	}
}

/* Root compositions host either a compatibility chat or independent core
   transcript and editor windows. Provider panes never own editor state. */
static struct {
	struct phi_layout tree;
	struct phi_rect boxes[PHI_LAYOUT_NODES];
	struct surface_client *clients[PHI_LAYOUT_NODES];
	int active;
} ui_composition;

static void ui_layout_windows_close(void) {

	for (int i = 0; i < PHI_HOST_COUNT; i++) {
		if (ui_host_canvas[i]) {
			delwin(ui_host_canvas[i]);
		}
		ui_host_canvas[i] = NULL;
	}
}

static void ui_layout_close(void) {

	ui_layout_windows_close();
	memset(ui_host_boxes, 0, sizeof(ui_host_boxes));
	for (int i = 0; i < PHI_LAYOUT_NODES; i++) {
		surface_client_free(ui_composition.clients[i]);
	}
	phi_layout_free(&ui_composition.tree);
	memset(&ui_composition, 0, sizeof(ui_composition));
	ui_chat_x = ui_chat_y = ui_chat_columns = 0;
	ui_split_hosts = ui_editor_x = ui_editor_y = 0;
}

static int ui_layout_wait(void) {

	if (!ui_composition.active) {
		return -1;
	}
	int visible = ui_frame_surfaces_visible(), wait = -1;
	for (int i = 0; i < ui_composition.tree.count; i++) {
		int next = visible ? surface_client_wait(ui_composition.clients[i])
						   : surface_client_pending_wait(ui_composition.clients[i]);
		if (next >= 0 && (wait < 0 || next < wait)) {
			wait = next;
		}
	}

	return wait;
}

static void ui_layout_service(int visible) {

	for (int i = 0; i < ui_composition.tree.count; i++) {
		if (surface_client_service(ui_composition.clients[i]) && visible) {
			dirty = 1;
		}
	}
}

static void ui_layout_poll(void) {

	if (!ui_frame_surfaces_visible()) {
		return;
	}
	for (int i = 0; i < ui_composition.tree.count; i++) {
		struct phi_layout_node *n = &ui_composition.tree.nodes[i];
		struct phi_rect r = ui_composition.boxes[i];
		if (n->kind != PHI_LAYOUT_PROGRAM || r.width <= 0 || r.height <= 0) {
			surface_client_free(ui_composition.clients[i]);
			ui_composition.clients[i] = NULL;
			continue;
		}
		if (!ui_composition.clients[i]) {
			ui_composition.clients[i] = surface_client_new();
		}
		json_object *context = ui_surface_context(1);
		json_object_object_add(context, "layout_id", json_object_new_string(n->id));
		if (surface_client_poll(
				ui_composition.clients[i], n->provider, n->id, r.width, r.height, context)) {
			dirty = 1;
		}
		json_object_put(context);
	}
}

static void ui_layout_paint(void) {

	for (int i = 0; i < ui_composition.tree.count; i++) {
		struct phi_layout_node *n = &ui_composition.tree.nodes[i];
		struct phi_rect r = ui_composition.boxes[i];
		if (r.width <= 0 || r.height <= 0) {
			continue;
		}
		if (n->kind == PHI_LAYOUT_PROGRAM) {
			surface_paint(ui_composition.clients[i], r.y, r.x, r.width, r.height, 27);
		}
		if (n->kind == PHI_LAYOUT_TEXT) {
			char text[2048];
			welcome_expand(n->text, text, sizeof(text));
			char *line = text;
			int row = 0;
			while (line && row < r.height) {
				char *end = strchr(line, '\n');
				if (end) {
					*end++ = 0;
				}
				welcome_text(r.y + row++, r.x, r.width, line, 3, 0);
				line = end;
			}
		}
	}
}

static int ui_layout_mouse(const MEVENT *mouse) {

	if (!ui_composition.active || !ui_frame_surfaces_visible() || transcript_selection.dragging ||
		cinema.painting) {
		return 0;
	}
	int kind = mouse->bstate & BUTTON1_PRESSED ? PHI_WIDGET_PRESS
		: mouse->bstate & BUTTON1_RELEASED     ? PHI_WIDGET_RELEASE
		: mouse->bstate & BUTTON1_CLICKED      ? PHI_WIDGET_CLICK
											   : -1;
	for (int i = 0; i < ui_composition.tree.count; i++) {
		struct phi_rect r = ui_composition.boxes[i];
		if (!phi_rect_contains(r, mouse->x, mouse->y)) {
			continue;
		}
		if (ui_composition.tree.nodes[i].kind == PHI_LAYOUT_PROGRAM && kind >= 0) {
			struct phi_surface_event event = {
				.kind = kind, .x = mouse->x - r.x, .y = mouse->y - r.y, .button = 1};
			if (surface_client_event(ui_composition.clients[i], &event)) {
				dirty = 1;
			}
		}
	}
	const struct phi_layout *tree = &ui_composition.tree;
	struct phi_rect r = ui_composition.boxes[tree->chat >= 0 ? tree->chat : tree->transcript];
	int inside = phi_rect_contains(r, mouse->x, mouse->y);
	if (tree->composer >= 0) {
		r = ui_composition.boxes[tree->composer];
		inside |= phi_rect_contains(r, mouse->x, mouse->y);
	}
	r = ui_host_boxes[PHI_HOST_FOOTER];
	inside |= phi_rect_contains(r, mouse->x, mouse->y);

	return !inside;
}

static void draw(int force) {

	if (!active || presentation_batch) {
		return;
	}
	if (editor_host_redraw) {
		if (!modal_active && (force || (dirty && now() - last_draw >= FRAME_INTERVAL))) {
			dirty = 0;
			last_draw = now();
			editor_host_redraw(editor_host_context);
		}
		return;
	}
	ui_layout_service(ui_frame_surfaces_visible());
	if (modal_active) {
		ui_surfaces_service(0);
		if (ui_composition.active) {
			ui_layout_close();
			dirty = 1;
		}
		return;
	}
	if (history_view_active()) {
		ui_surfaces_service(0);
	}
	if (active && history_view_active() && !modal_active && !pending_approval) {
		if (!force && dirty && now() - last_draw < FRAME_INTERVAL) {
			return;
		}
		int painted = history_view_paint(force || dirty);
		if (history_view_active()) {
			dirty = 0;
			wheel_pending = 0;
			if (painted) {
				last_draw = now();
			}
			return;
		}
		force = 1;
	}
	const struct phi_layout *tree =
		!used && !working && skin.empty_layout.count ? &skin.empty_layout : &skin.layout;
	struct phi_rect boxes[PHI_LAYOUT_NODES];
	int enabled = active && !modal_active && !pending_approval &&
		phi_layout_arrange(tree, COLS, LINES, boxes);
	if (!enabled) {
		if (ui_composition.active) {
			ui_layout_close();
			dirty = 1;
		}
		draw_chat(force);
		return;
	}
	if (!ui_composition.active || !phi_layout_equal(tree, &ui_composition.tree)) {
		ui_layout_close();
		if (!phi_layout_clone(&ui_composition.tree, tree)) {
			dirty = 1;
			draw_chat(force);
			return;
		}
		ui_composition.active = 1;
		dirty = 1;
	}
	if (memcmp(boxes, ui_composition.boxes, sizeof(boxes))) {
		memcpy(ui_composition.boxes, boxes, sizeof(boxes));
		dirty = 1;
	}
	struct phi_rect r = boxes[tree->chat >= 0 ? tree->chat : tree->transcript];
	struct phi_rect e = tree->composer >= 0 ? boxes[tree->composer] : r;
	if (ui_chat_x != r.x || ui_chat_y != r.y || ui_chat_columns != r.width) {
		transcript_selection_clear();
	}
	ui_chat_x = r.x;
	ui_chat_y = r.y;
	ui_chat_columns = r.width;
	ui_split_hosts = tree->composer >= 0;
	ui_editor_x = e.x;
	ui_editor_y = e.y;
	ui_layout_poll();
	WINDOW *canvas = derwin(stdscr, r.height, r.width, r.y, r.x);
	WINDOW *editor = ui_split_hosts ? derwin(stdscr, e.height, e.width, e.y, e.x) : NULL;
	int hosts_ok = 1;
	for (int host = 0; host < PHI_HOST_COUNT; host++) {
		int index = tree->hosts[host];
		ui_host_boxes[host] = index >= 0 ? boxes[index] : (struct phi_rect){0};
		if (index < 0) {
			continue;
		}
		struct phi_rect h = boxes[index];
		ui_host_canvas[host] = derwin(stdscr, h.height, h.width, h.y, h.x);
		if (!ui_host_canvas[host]) {
			hosts_ok = 0;
		}
	}
	if (!canvas || (ui_split_hosts && !editor) || !hosts_ok) {
		if (canvas) {
			delwin(canvas);
		}
		if (editor) {
			delwin(editor);
		}
		ui_layout_close();
		dirty = 1;
		draw_chat(force);
		return;
	}
	int suspended = ui_frame_suspended;
	ui_editor_canvas = editor;
	ui_canvas = canvas;
	int painted = draw_chat(force);
	ui_canvas = NULL;
	ui_editor_canvas = NULL;
	if (suspended && !ui_frame_suspended) {
		ui_layout_poll();
	}
	if (painted) {
		int cy, cx;
		getyx(editor ? editor : canvas, cy, cx);
		attrset(COLOR_PAIR(27));
		/* Clear only gaps around core hosts, never their shared backing cells. */
		for (int y = 0; y < LINES; y++) {
			int start = 0;
			for (int x = 0; x <= COLS; x++) {
				int owned = x < COLS &&
					(phi_rect_contains(r, x, y) || (editor && phi_rect_contains(e, x, y)));
				for (int host = 0; !owned && x < COLS && host < PHI_HOST_COUNT; host++) {
					struct phi_rect h = ui_host_boxes[host];
					owned = phi_rect_contains(h, x, y);
				}
				if (owned || x == COLS) {
					if (x > start) {
						mvhline(y, start, ' ', x - start);
					}
					start = x + 1;
				}
			}
		}
		ui_layout_paint();
		move(e.y + cy, e.x + cx);
		touchwin(stdscr);
		frame_begin();
		wnoutrefresh(stdscr);
		doupdate();
		frame_end();
	}
	ui_layout_windows_close();
	if (editor) {
		delwin(editor);
	}
	delwin(canvas);
}
