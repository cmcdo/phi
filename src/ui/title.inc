/* Legacy compatibility hooks. The theme-specific FreeType/Sixel title was
   retired; reusable native cell providers own artwork and animation instead.
   These internal call sites will disappear with the shared frame/layout pass. */
static void title_init(void) {

}

static void title_resized(void) {

}

static int title_wait_timeout(void) {

	return -1;
}

static int title_redraw_due(void) {

	return 0;
}

static void title_begin_frame(void) {

}

static void title_update_begin(void) {

}

static void title_update_end(void) {

}

static void title_close(void) {

}
