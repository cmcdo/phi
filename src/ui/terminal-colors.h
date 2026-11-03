/* Ncurses direct-colour adapter. Logical colours/pairs stay small so existing
   cells and widgets need not store 24-bit curses colour numbers. */
#ifndef PHI_TERMINAL_COLORS_H
#define PHI_TERMINAL_COLORS_H
#include <curses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>

#ifndef PHI_TERMINFO_DIR
#define PHI_TERMINFO_DIR "build/terminfo"
#endif
#define PHI_DIRECT_ANSI 0x1000000
#define PHI_RGB_FIRST 256
#define PHI_RGB_MAX (SHRT_MAX - PHI_RGB_FIRST + 1)
static int terminal_rgb_values[PHI_RGB_MAX], terminal_rgb_count;
static SCREEN *terminal_screen;

static int terminal_direct(void) {

	return COLORS >= PHI_DIRECT_ANSI + 256;
}

static short terminal_rgb(int rgb) {

	for (int i = 0; i < terminal_rgb_count; i++) {
		if (terminal_rgb_values[i] == rgb) {
			return (short)(PHI_RGB_FIRST + i);
		}
	}
	if (terminal_rgb_count == PHI_RGB_MAX) {
		return -1;
	}
	terminal_rgb_values[terminal_rgb_count] = rgb;

	return (short)(PHI_RGB_FIRST + terminal_rgb_count++);
}

static inline int terminal_color(short color) {

	if (color < 0 || !terminal_direct()) {
		return color;
	}
	if (color < PHI_RGB_FIRST) {
		return PHI_DIRECT_ANSI + color;
	}
	int index = color - PHI_RGB_FIRST;

	return index < terminal_rgb_count ? terminal_rgb_values[index] : -1;
}

static int terminal_init_pair(short pair, short fg, short bg) {

#if NCURSES_EXT_COLORS
	if (terminal_direct()) {
		return init_extended_pair(pair, terminal_color(fg), terminal_color(bg));
	}
#endif

	return init_pair(pair, fg, bg);
}

static int terminal_pair_content(short pair, short *fg, short *bg) {

#if NCURSES_EXT_COLORS
	if (terminal_direct()) {
		int front, back;
		int result = extended_pair_content(pair, &front, &back);
		if (result == OK) {
			if (fg) {
				*fg = front < 0                ? -1
					: front >= PHI_DIRECT_ANSI ? (short)(front - PHI_DIRECT_ANSI)
											   : terminal_rgb(front);
			}
			if (bg) {
				*bg = back < 0                ? -1
					: back >= PHI_DIRECT_ANSI ? (short)(back - PHI_DIRECT_ANSI)
											  : terminal_rgb(back);
			}
		}
		return result;
	}
#endif

	return pair_content(pair, fg, bg);
}

static inline WINDOW *phi_curses_open(void) {

	if (terminal_screen) {
		set_term(terminal_screen);
		return stdscr;
	}
	const char *setting = getenv("PHI_TRUECOLOR"), *advertised = getenv("COLORTERM");
	const char *term = getenv("TERM");
	int enabled = advertised && (!strcmp(advertised, "truecolor") || !strcmp(advertised, "24bit"));
	if (term && strstr(term, "-direct")) {
		enabled = 1;
	}
	if (setting && !strcmp(setting, "0")) {
		enabled = 0;
	}
	if (setting && !strcmp(setting, "1")) {
		enabled = 1;
	}
	/* Never replace a console/unknown terminal's capabilities automatically. */
	int compatible = term &&
		(!strncmp(term, "xterm", 5) || !strncmp(term, "foot", 4) || !strncmp(term, "screen", 6) ||
			!strncmp(term, "tmux", 4) || !strncmp(term, "alacritty", 9) ||
			!strncmp(term, "kitty", 5));
#if NCURSES_EXT_COLORS
	/* A failed newterm leaks its half-made screen inside ncurses, so look
       for the entry first (the letter directory, or the hex one some
       systems use). */
	int present = access(PHI_TERMINFO_DIR "/p/phi-direct", R_OK) == 0 ||
		access(PHI_TERMINFO_DIR "/70/phi-direct", R_OK) == 0;
	if (enabled && present && (compatible || (setting && !strcmp(setting, "1")))) {
		const char *old = getenv("TERMINFO");
		char *saved = old ? strdup(old) : NULL;
		if (!old || saved) {
			if (setenv("TERMINFO", PHI_TERMINFO_DIR, 1) == 0) {
				SCREEN *screen = newterm("phi-direct", stdout, stdin);
				if (saved) {
					setenv("TERMINFO", saved, 1);
				} else {
					unsetenv("TERMINFO");
				}
				free(saved);
				if (screen) {
					terminal_screen = screen;
					return stdscr;
				}
			} else {
				free(saved);
			}
		}
	}
#else
	(void)enabled;
	(void)compatible;
#endif
	/* A stock -direct entry interprets numeric palette indices as packed RGB;
       the indexed fallback must use an indexed description instead. */
	const char *fallback = compatible && term && strstr(term, "-direct") ? "xterm-256color" : NULL;
	terminal_screen = newterm(fallback, stdout, stdin);

	return terminal_screen ? stdscr : NULL;
}

/* Apply only after defining wrappers that call the real ncurses APIs. */
#define init_pair terminal_init_pair
#define pair_content terminal_pair_content
#endif
