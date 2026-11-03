/* Shared welcome/board idle choreography. Deadlines wake only for pose changes;
   frames keep identical cell footprints, and delayed UI ticks never catch up in a loop. */
static uint32_t mascot_random(uint32_t *state, double stamp) {

	if (!*state) {
		*state = (uint32_t)(uint64_t)(stamp * 1000000) ^ (uint32_t)getpid() ^ 0x9e3779b9u;
	}
	if (!*state) {
		*state = 1;
	}
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;

	return *state;
}

static double mascot_delay(const struct skin *s, uint32_t *rng, double stamp) {

	uint32_t r = mascot_random(rng, stamp);

	return s->art_gesture_count ? 2.0 + (r % 3001) / 1000.0 : 3.0 + (r % 5001) / 1000.0;
}

static int mascot_gesture_valid(const struct skin *s, int index) {

	if (index < 0 || index >= s->art_gesture_count) {
		return 0;
	}
	const struct mascot_gesture *g = &s->art_gestures[index];
	for (int f = 0; f < g->count; f++) {
		if (g->rows[f] != s->art_count) {
			return 0;
		}
		for (int r = 0; r < s->art_count; r++) {
			if (skin_width(g->frames[f][r]) != skin_width(s->art[r])) {
				return 0;
			}
		}
	}

	return g->count > 0;
}

/* gesture 0 is the ordinary blink; 1..6 select a theme-authored gesture. */
static int mascot_step(const struct skin *s, double stamp, double *deadline, int *active_pose,
	uint32_t *rng, int *gesture, int *frame) {

	if (!*deadline) {
		*active_pose = *gesture = *frame = 0;
		*deadline = stamp + mascot_delay(s, rng, stamp);
		return 0;
	}
	if (stamp < *deadline) {
		return 0;
	}
	if (*active_pose && *gesture && mascot_gesture_valid(s, *gesture - 1)) {
		const struct mascot_gesture *g = &s->art_gestures[*gesture - 1];
		if (++*frame < g->count) {
			*deadline = stamp + g->interval / 1000.0;
			return 1;
		}
	}
	if (*active_pose) {
		*active_pose = *gesture = *frame = 0;
		*deadline = stamp + mascot_delay(s, rng, stamp);
	} else {
		*active_pose = 1;
		*gesture = *frame = 0;
		uint32_t choice = mascot_random(rng, stamp);
		if (s->art_gesture_count && choice % 3) {
			int index = (int)((choice / 3) % (uint32_t)s->art_gesture_count);
			if (mascot_gesture_valid(s, index)) {
				*gesture = index + 1;
			}
		}
		*deadline = stamp + (*gesture ? s->art_gestures[*gesture - 1].interval / 1000.0 : 0.12);
	}

	return 1;
}

/* Play a theme's entrance once, then hand the same clock to idle motion.
   Returning -1 means the entrance is already finished or was not requested. */
static int mascot_intro_step(const struct skin *s, double stamp, double *deadline, int *active_pose,
	uint32_t *rng, int *gesture, int *frame, int *intro_pos) {

	if (*intro_pos >= s->art_intro_count) {
		return -1;
	}
	if (*deadline && stamp < *deadline) {
		return 0;
	}
	if (*deadline && *active_pose && *gesture && mascot_gesture_valid(s, *gesture - 1)) {
		const struct mascot_gesture *g = &s->art_gestures[*gesture - 1];
		if (++*frame < g->count) {
			*deadline = stamp + g->interval / 1000.0;
			return 1;
		}
		++*intro_pos;
	}
	while (*intro_pos < s->art_intro_count) {
		int index = s->art_intro[*intro_pos] - 1;
		if (mascot_gesture_valid(s, index)) {
			*active_pose = 1;
			*gesture = index + 1;
			*frame = 0;
			*deadline = stamp + s->art_gestures[index].interval / 1000.0;
			return 1;
		}
		++*intro_pos;
	}
	*active_pose = *gesture = *frame = 0;
	*deadline = stamp + mascot_delay(s, rng, stamp);

	return 1;
}

static const char *mascot_line(
	const struct skin *s, int active_pose, int gesture, int frame, int row) {

	if (!active_pose) {
		return s->art[row];
	}
	if (gesture && mascot_gesture_valid(s, gesture - 1)) {
		const struct mascot_gesture *g = &s->art_gestures[gesture - 1];
		if (frame >= 0 && frame < g->count) {
			return g->frames[frame][row];
		}
	}

	return s->art_blink[row];
}
