/* Host-owned layout motion. Profiles are independent of widget callbacks;
   welcome adapters map progress to a slide or a hidden-position dissolve.
   No new thread/render loop; only temporary deadlines on Phi's event loop. */
static struct {
	int running, from, offset, relocated, mask_width, mask_height;
	double started, next, visibility;
	struct motion_profile profile;
	void *module;
	const struct phi_transition_v1 *plugin;
	char plugin_id[64];
	struct phi_transition_frame frame;
	struct phi_transition_result result;
} header_motion;

static double motion_ease(int easing, double t) {

	if (easing == 1) {
		return t;
	}
	if (easing == 2) {
		double u = 1 - t;
		return 1 - u * u * u;
	}
	if (easing == 3) {
		return t * t * t;
	}

	return t * t * (3 - 2 * t);
}

static double motion_sample(const struct motion_profile *profile, double t) {

	if (t <= 0) {
		return 0;
	}
	if (t >= 1) {
		return 1;
	}
	if (!profile->count) {
		return motion_ease(profile->easing, t);
	}
	for (int i = 1; i < profile->count; i++) {
		if (t <= profile->frames[i].at) {
			double start = profile->frames[i - 1].at, finish = profile->frames[i].at;
			double local = motion_ease(profile->easing, (t - start) / (finish - start));
			return profile->frames[i - 1].value +
				local * (profile->frames[i].value - profile->frames[i - 1].value);
		}
	}

	return 1;
}

static const char *header_motion_plugin_id(void) {

	return header_motion.plugin_id;
}

static void header_motion_cancel(void) {

	if (header_motion.module) {
		dlclose(header_motion.module);
	}
	memset(&header_motion, 0, sizeof(header_motion));
}

static int header_motion_plugin_sample(double t) {

	struct phi_transition_frame frame = {.size = sizeof(frame),
		.time = t < 0 ? 0
			: t > 1   ? 1
					  : t,
		.progress = motion_sample(&header_motion.profile, t),
		.source_offset = header_motion.from,
		.rows = conversation_height,
		.columns = COLS};
	struct phi_transition_result result = {
		.offset_rows = header_motion.from, .coverage = 1, .transcript_visible = 1};
	header_motion.plugin->sample(&frame, &result);
	if (result.offset_rows < -200 || result.offset_rows > 500 ||
		!(result.coverage >= 0 && result.coverage <= 1) ||
		(result.transcript_visible != 0 && result.transcript_visible != 1) ||
		(result.hide_welcome != 0 && result.hide_welcome != 1)) {
		int from = header_motion.from;
		header_motion_cancel();
		header_motion.offset = from;
		return 0;
	}
	if (t >= 1) {
		result.transcript_visible = 1;
	}
	header_motion.frame = frame;
	header_motion.result = result;
	header_motion.offset = result.offset_rows;
	header_motion.visibility = result.coverage;

	return 1;
}

static void header_motion_start(int from) {

	header_motion_cancel();
	if (skin.welcome_motion.enabled == 5) {
		header_motion.from = header_motion.offset = from > 0 ? from : 0;
		header_motion.profile = skin.welcome_motion;
		for (struct transition_registration *p = native_transitions; p; p = p->next) {
			if (!strcmp(p->id, skin.welcome_transition)) {
				header_motion.module = dlopen(p->path, RTLD_NOW | RTLD_LOCAL);
				break;
			}
		}
		if (header_motion.module) {
			const struct phi_transition_v1 *(*entry)(void) =
				(const struct phi_transition_v1 *(*)(void))dlsym(
					header_motion.module, "phi_transition_entry");
			const struct phi_transition_v1 *plugin = entry ? entry() : NULL;
			if (plugin && plugin->abi == PHI_TRANSITION_ABI && plugin->size >= sizeof(*plugin) &&
				plugin->sample) {
				header_motion.plugin = plugin;
				snprintf(header_motion.plugin_id, sizeof(header_motion.plugin_id), "%s",
					skin.welcome_transition);
				header_motion.running = skin.welcome_motion.duration > 0;
				header_motion.started = now();
				header_motion.next = now() + .020;
				header_motion_plugin_sample(header_motion.running ? 0 : 1);
				return;
			}
		}
		header_motion_cancel();
		header_motion.offset = from > 0 ? from : 0;
		return;
	}
	if (skin.welcome_motion.enabled == 3) {
		/* Keep the welcome's original padding as ordinary transcript space. */
		header_motion.offset = from > 0 ? from : 0;
		return;
	}
	if (skin.welcome_motion.enabled == 4 && !skin.welcome_motion.duration) {
		header_motion.offset = from > 0 ? from : 0;
		header_motion.profile = skin.welcome_motion;
		return;
	}
	if ((from <= 0 && skin.welcome_motion.enabled != 2 && skin.welcome_motion.enabled != 4) ||
		!skin.welcome_motion.enabled || !skin.welcome_motion.duration) {
		return;
	}
	if (from < 0) {
		from = 0;
	}
	header_motion.running = 1;
	header_motion.from = header_motion.offset = from;
	header_motion.profile = skin.welcome_motion;
	header_motion.visibility = 1;
	header_motion.started = now();
	header_motion.next = header_motion.started + .020;
}

static int header_motion_gone(void) {

	return !header_motion.running &&
		((header_motion.profile.enabled == 4 && header_motion.visibility == 0) ||
			(header_motion.plugin && header_motion.result.hide_welcome));
}

static void header_motion_interrupt(void) {

	if (header_motion.plugin) {
		header_motion_plugin_sample(1);
		header_motion.running = 0;
	} else if (header_motion.profile.enabled == 4) {
		header_motion.running = 0;
		header_motion.visibility = 0;
	} else {
		header_motion_cancel();
	}
}

static int header_motion_offset(void) {

	return header_motion.offset;
}

static int header_motion_body_visible(void) {

	if (header_motion.plugin) {
		return !header_motion.running || header_motion.result.transcript_visible;
	}

	return !header_motion.running ||
		(header_motion.profile.enabled != 2 && header_motion.profile.enabled != 4) ||
		(header_motion.profile.enabled == 2 && header_motion.relocated);
}

static int header_motion_cell_visible(int x, int y) {

	if (header_motion.plugin && header_motion.plugin->cell_visible) {
		return header_motion.plugin->cell_visible(&header_motion.frame, &header_motion.result, x, y,
				   header_motion.mask_width, header_motion.mask_height) != 0;
	}
	if (!header_motion.plugin && header_motion.profile.enabled != 4 &&
		(!header_motion.running || header_motion.profile.enabled != 2)) {
		return 1;
	}
	static const unsigned char bayer[4][4] = {
		{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
	int level = (int)(header_motion.visibility * 16 + .5);

	return bayer[y & 3][x & 3] < level;
}

static int header_motion_due(void) {

	if (!header_motion.running || now() < header_motion.next) {
		return 0;
	}
	double t = (now() - header_motion.started) * 1000 / header_motion.profile.duration;
	if (header_motion.plugin) {
		if (header_motion_plugin_sample(t) && t >= 1) {
			header_motion.running = 0;
		}
		header_motion.next = now() + .020;
		return 1;
	}
	double offset, visibility = 1;
	int relocated = t >= .5;
	if (header_motion.profile.enabled == 4) {
		visibility = 1 - motion_sample(&header_motion.profile, t);
		if (visibility < 0) {
			visibility = 0;
		}
		if (visibility > 1) {
			visibility = 1;
		}
		/* Disappearance is monotone even with a custom overshooting curve. */
		if (visibility > header_motion.visibility) {
			visibility = header_motion.visibility;
		}
		offset = header_motion.from;
	} else if (header_motion.profile.enabled == 2) {
		/* A fully hidden interval around the relocation avoids any visible jump. */
		visibility = t < .4 ? 1 - motion_sample(&header_motion.profile, t / .4)
			: t > .6        ? motion_sample(&header_motion.profile, (t - .6) / .4)
							: 0;
		if (visibility < 0) {
			visibility = 0;
		}
		if (visibility > 1) {
			visibility = 1;
		}
		offset = relocated ? 0 : header_motion.from;
	} else {
		offset = header_motion.from * (1 - motion_sample(&header_motion.profile, t));
	}
	int position = (int)(offset + (offset < 0 ? -.5 : .5));
	int changed = position != header_motion.offset || visibility != header_motion.visibility ||
		relocated != header_motion.relocated;
	header_motion.offset = position;
	header_motion.visibility = visibility;
	header_motion.relocated = relocated;
	header_motion.next = now() + .020;
	if (t >= 1) {
		changed = 1; /* Completion can reveal/reflow the transcript even at zero visibility. */
		header_motion.running = 0;
		if (header_motion.profile.enabled == 4) {
			header_motion.visibility = 0;
		} else {
			header_motion.offset = 0;
		}
	}

	return changed;
}

static int header_motion_wait(void) {

	if (!header_motion.running) {
		return -1;
	}
	double remaining = header_motion.next - now();

	return remaining > 0 ? (int)(remaining * 1000) + 1 : 0;
}
