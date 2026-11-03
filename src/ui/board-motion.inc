/* NORMAL/visual board motions. Counts refer to visible navigation stops. */
static void board_motion_to(struct view *v, unsigned position) {

	move_cursor(v, -(long)(v->count + 4));
	if (position > 1) {
		move_cursor(v, (long)position - 1);
	}
}

static int board_motion_key(struct view *v, int special, wint_t key) {

	if (!special && key >= '0' && key <= '9' && (key != '0' || v->motion_count)) {
		unsigned digit = (unsigned)(key - '0');
		v->motion_count =
			v->motion_count > (1000000 - digit) / 10 ? 1000000 : v->motion_count * 10 + digit;
		v->pending_d = 0;
		if (!v->query[0]) {
			snprintf(v->notice, sizeof(v->notice), "Motion count: %u", v->motion_count);
		}
		return 1;
	}
	if (!special && key == 27 && (v->motion_count || v->pending_g || v->pending_d)) {
		v->motion_count = 0;
		v->pending_g = v->pending_d = 0;
		if (!v->query[0]) {
			v->notice[0] = 0;
		}
		return 1;
	}
	unsigned count = v->motion_count ? v->motion_count : 1;
	int explicit_count = v->motion_count != 0;
	if (!special && key == 'g' && !v->pending_g) {
		v->pending_g = 1;
		v->pending_d = 0;
		return 1;
	}
	int gg = !special && key == 'g' && v->pending_g;
	v->motion_count = 0;
	v->pending_g = 0;
	if (gg || (special && key == KEY_HOME)) {
		board_motion_to(v, count);
	} else if ((!special && key == 'G') || (special && key == KEY_END)) {
		if (explicit_count) {
			board_motion_to(v, count);
		} else {
			move_cursor(v, (long)(v->count + 4));
		}
	} else if ((special && key == KEY_DOWN) || (!special && key == 'j')) {
		move_cursor(v, (long)count);
	} else if ((special && key == KEY_UP) || (!special && key == 'k')) {
		move_cursor(v, -(long)count);
	} else if (!special && (key == 4 || key == 21 || key == 6 || key == 2)) {
		long page = LINES > 8 ? LINES - 8 : 1;
		if (key == 4 || key == 21) {
			page = page > 1 ? page / 2 : 1;
		}
		long amount = (long)count * page;
		move_cursor(v, (key == 21 || key == 2) ? -amount : amount);
	} else {
		return 0;
	}
	v->pending_d = 0;
	if (!v->query[0]) {
		v->notice[0] = 0;
	}

	return 1;
}
