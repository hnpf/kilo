#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "editor.h"

static struct timespec last_word_click_time;
static int last_word_click_valid;
static int last_word_click_start, last_word_click_end;
static int word_drag_active;
static int word_drag_start, word_drag_end, word_drag_click;

static int same_word_double_click(int start, int end, int clicked, struct timespec now) {
	if (!last_word_click_valid || clicked < last_word_click_start || clicked >= last_word_click_end ||
	    start != last_word_click_start || end != last_word_click_end) return 0;
	long long elapsed = (long long)(now.tv_sec - last_word_click_time.tv_sec) * 1000000000LL +
		(now.tv_nsec - last_word_click_time.tv_nsec);
	return elapsed >= 0 && elapsed <= 450000000LL;
}

int key_from_name(const char *name) {
	char key[64];
	if (!name || strlen(name) >= sizeof(key)) return -1;
	snprintf(key, sizeof(key), "%s", name);
	for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
	if (!strncmp(key, "ctrl+shift+", 11) && key[11] && !key[12] && key[11] >= 'a' && key[11] <= 'z') {
		if (key[11] == 'z') return K_REDO;
		return K_PLUGIN_CTRL_SHIFT_BASE + key[11];
	}
	if (!strncmp(key, "ctrl+alt+", 9) && key[9] && !key[10]) {
		if (key[9] == 'g') return K_HELP;
		return K_PLUGIN_CTRL_ALT_BASE + key[9];
	}
	if (!strcmp(key, "alt+g")) return K_HELP;
	if (!strncmp(key, "alt+", 4) && key[4] && !key[5])
		return K_PLUGIN_ALT_BASE + key[4];
	if (!strncmp(key, "ctrl+", 5) && key[5] && !key[6]) {
		if (key[5] >= 'a' && key[5] <= 'z') return key[5] - 'a' + 1;
		if (key[5] == ';') return K_CMD;
	}
	if (!strcmp(key, "ctrl+pageup") || !strcmp(key, "ctrl+pgup")) return K_CTRL_PGUP;
	if (!strcmp(key, "ctrl+pagedown") || !strcmp(key, "ctrl+pgdn")) return K_CTRL_PGDN;
	if (!strcmp(key, "ctrl+left")) return K_CTRL_LEFT;
	if (!strcmp(key, "ctrl+right")) return K_CTRL_RIGHT;
	if (!strcmp(key, "ctrl+up")) return K_CTRL_UP;
	if (!strcmp(key, "ctrl+down")) return K_CTRL_DOWN;
	if (!strcmp(key, "ctrl+shift+left")) return K_CTRL_SHIFT_LEFT;
	if (!strcmp(key, "ctrl+shift+right")) return K_CTRL_SHIFT_RIGHT;
	if (!strcmp(key, "ctrl+enter")) return K_CTRL_ENTER;
	if (!strcmp(key, "ctrl+backspace")) return K_CTRL_BACKSPACE;
	if (!strcmp(key, "alt+backspace")) return K_ALT_BACKSPACE;
	if (!strcmp(key, "alt+g")) return K_HELP;
	if (!strcmp(key, "up")) return K_UP;
	if (!strcmp(key, "down")) return K_DOWN;
	if (!strcmp(key, "left")) return K_LEFT;
	if (!strcmp(key, "right")) return K_RIGHT;
	if (!strcmp(key, "home")) return K_HOME;
	if (!strcmp(key, "end")) return K_END;
	if (!strcmp(key, "pageup") || !strcmp(key, "pgup")) return K_PGUP;
	if (!strcmp(key, "pagedown") || !strcmp(key, "pgdn")) return K_PGDN;
	if (!strcmp(key, "insert") || !strcmp(key, "ins")) return K_INS;
	if (!strcmp(key, "delete") || !strcmp(key, "del")) return K_DEL;
	if (!strcmp(key, "backspace")) return K_BACKSPACE;
	if (!strcmp(key, "enter")) return '\r';
	if (!strcmp(key, "escape") || !strcmp(key, "esc")) return K_ESC;
	if (!strcmp(key, "tab")) return '\t';
	if (!strncmp(key, "shift+", 6)) {
		if (!strcmp(key + 6, "up")) return K_S_UP;
		if (!strcmp(key + 6, "down")) return K_S_DOWN;
		if (!strcmp(key + 6, "left")) return K_S_LEFT;
		if (!strcmp(key + 6, "right")) return K_S_RIGHT;
	}
	if (key[0] && !key[1]) return (unsigned char)key[0];
	return -1;
}


int r_key(void) {
	int n;
	char c;
	while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
		if (n == -1 && errno != EAGAIN) err("read");
	}

	if (c == '\x1b') {
		char seq[64];
		int idx = 0;
		while (idx < 63 && read(STDIN_FILENO, &seq[idx], 1) == 1) {
			char ch = seq[idx++];
			if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '~') break;
		}
		seq[idx] = '\0';

		if (idx == 0) return '\x1b';
		if (idx == 1 && (seq[0] == 8 || (unsigned char)seq[0] == 127)) return K_ALT_BACKSPACE;

		// alt+g: \x1bg or \x1bG
		if (idx == 1 && (seq[0] == 'g' || seq[0] == 'G')) return K_HELP;
		if (idx == 1 && isprint((unsigned char)seq[0]))
			return K_PLUGIN_ALT_BASE + tolower((unsigned char)seq[0]);

		// CSI u keyboard reporting (\x1b[<codepoint>;<mod>u)
		if (seq[0] == '[') {
			int key = 0, mod = 0;
			// The Kitty protocol can append event-type and alternate-key
			// fields (for example, 57351;6:1u or 57351:62;6u). Read the
			// leading key and modifier values without requiring the suffix.
			if (sscanf(seq, "[%d;%d", &key, &mod) == 2 ||
			    sscanf(seq, "[%d:%*d;%d", &key, &mod) == 2) {
				if ((mod == 5 || mod == 7) && (key == 8 || key == 127)) return K_CTRL_BACKSPACE;
				if ((mod == 3 || mod == 7) && key == 127) return K_ALT_BACKSPACE;
				// Kitty keyboard protocol functional key codes for Left/Right.
				if ((mod == 5 || mod == 7) && key == 57350) return K_CTRL_LEFT;
				if ((mod == 5 || mod == 7) && key == 57351) return K_CTRL_RIGHT;
				if (mod == 6 && key == 57350) return K_CTRL_SHIFT_LEFT;
				if (mod == 6 && key == 57351) return K_CTRL_SHIFT_RIGHT;
				if ((mod == 5 || mod == 7) && key == 57352) return K_CTRL_UP;
				if ((mod == 5 || mod == 7) && key == 57353) return K_CTRL_DOWN;
				if ((mod == 5 || mod == 7) && key == 57354) return K_CTRL_PGUP;
				if ((mod == 5 || mod == 7) && key == 57355) return K_CTRL_PGDN;
				if ((mod == 5 || mod == 7) && key == 13) return K_CTRL_ENTER;
				if (mod == 3 || mod == 7) { // Alt or Ctrl+Alt
					if (key == 'g' || key == 'G') return K_HELP;
				}
				if ((mod == 3 || mod == 7) && key >= 32 && key < 127) {
					if (mod == 7) return K_PLUGIN_CTRL_ALT_BASE + tolower(key);
					return K_PLUGIN_ALT_BASE + tolower(key);
				}
				if (mod == 5 || mod == 7) { // Ctrl or Ctrl+Alt
					if (key >= 'a' && key <= 'z') return key - 'a' + 1;
					if (key >= 'A' && key <= 'Z') return key - 'A' + 1;
					if (key == ';') return K_CMD;
					if (key == 'g' || key == 'G') return K_HELP;
				}
				if (mod == 6) { // Ctrl+Shift
					if (key == 'z' || key == 'Z') return K_REDO;
					if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))
						return K_PLUGIN_CTRL_SHIFT_BASE + tolower(key);
				}
				if (key == 57348) return K_INS;
				if (key == ';') return K_CMD;
			}
		}

		if (strstr(seq, "103;3") || strstr(seq, "71;3") || strstr(seq, "27;3;103~")) return K_HELP;
		if (strstr(seq, "27;5;127~") || strstr(seq, "27;5;8~")) return K_CTRL_BACKSPACE;
		if (strstr(seq, "27;3;127~") || strstr(seq, "27;3;8~")) return K_ALT_BACKSPACE;

		// mouse report \x1b[<b;x;yM
		if (seq[0] == '[' && seq[1] == '<') {
			int b, mx, my;
			char mtype = 0;
			if (sscanf(&seq[2], "%d;%d;%d%c", &b, &mx, &my, &mtype) >= 3) {
				int gutter = gutter_width();
				int side = sidebar_width();

				if (b == 64) {
					// scroll up
					if (!side || mx > side) sidebar_blur();
					for (int i = 0; i < 3; i++) {
						if (g.cy > 0) g.cy--;
					}
					return 0;
				} else if (b == 65) {
					// scroll down
					if (!side || mx > side) sidebar_blur();
					for (int i = 0; i < 3; i++) {
						if (g.cy < g.nr) g.cy++;
					}
					return 0;
				} else if (b == 0 && mtype == 'M') {
					// left click
					if (side && mx <= side) {
						word_drag_active = 0;
						last_word_click_valid = 0;
						sidebar_mouse_click(my - 1);
						return 0;
					}
					sidebar_blur();
					int width = g.w - gutter;
					if (width < 1) width = 1;
					cursor_from_screen(my - 1, mx - 1, width, gutter);
					g.sx = -1;
					g.sy = -1;
					int start, end, clicked;
					int is_word = mouse_word_bounds_at_cursor(&start, &end, &clicked);
					struct timespec now;
					clock_gettime(CLOCK_MONOTONIC, &now);
					if (is_word && same_word_double_click(start, end, clicked, now)) {
						mouse_select_word(start, end);
						word_drag_active = 1;
						word_drag_start = start;
						word_drag_end = end;
						word_drag_click = clicked;
						last_word_click_valid = 0;
					} else {
						word_drag_active = 0;
						last_word_click_valid = is_word;
						if (is_word) {
							last_word_click_start = start;
							last_word_click_end = end;
							last_word_click_time = now;
						}
					}
					return 0;
				} else if (b == 32) {
					// mouse drag
					if (side && mx <= side) {
						word_drag_active = 0;
						last_word_click_valid = 0;
						sidebar_mouse_drag(my - 1);
						return 0;
					}
					sidebar_blur();
					if (!word_drag_active && g.sx == -1) {
						last_word_click_valid = 0;
						g.sx = g.cx;
						g.sy = g.cy;
					}
					int width = g.w - gutter;
					if (width < 1) width = 1;
					cursor_from_screen(my - 1, mx - 1, width, gutter);
					if (word_drag_active)
						mouse_extend_word_selection(word_drag_start, word_drag_end, word_drag_click);
					return 0;
				} else if (b == 3 && mtype == 'm') {
					word_drag_active = 0;
					return 0;
				}
			}
			return 0;
		}

		// check for ctrl+; sequences (CSI u, xterm modifyOtherKeys, or literal escape ;)
		if (strstr(seq, "59;5") || strstr(seq, "59;6") || !strcmp(seq, ";") || !strcmp(seq, "O;")) {
			return K_CMD;
		}

		// F1 key
		if (!strcmp(seq, "OP") || !strcmp(seq, "[11~") || !strcmp(seq, "[[A")) {
			return K_HELP;
		}

		// ctrl+shift+z (redo)
		if (strstr(seq, "122;6") || strstr(seq, "90;6")) {
			return K_REDO;
		}

		if (seq[0] == '[') {
			// Some terminals/multiplexers put the final key byte in its own
			// parameter (CSI 1;6;D) instead of using CSI 1;6D.
			int p1 = 0, p2 = 0;
			char p3 = 0;
			if (sscanf(seq, "[%d;%d;%c", &p1, &p2, &p3) == 3 && p1 == 1 && p2 == 6) {
				if (p3 == 'D') return K_CTRL_SHIFT_LEFT;
				if (p3 == 'C') return K_CTRL_SHIFT_RIGHT;
			}
			if (!strcmp(seq, "[1;6D")) return K_CTRL_SHIFT_LEFT;
			if (!strcmp(seq, "[1;6C")) return K_CTRL_SHIFT_RIGHT;

			int key = 0, mod = 0;
			char final = 0;
			if (sscanf(seq, "[%d;%d%c", &key, &mod, &final) == 3) {
				if ((mod == 5 || mod == 7) && key == 5 && final == '~') return K_CTRL_PGUP;
				if ((mod == 5 || mod == 7) && key == 6 && final == '~') return K_CTRL_PGDN;
				if (mod == 5 && final == 'D') return K_CTRL_LEFT;
				if (mod == 5 && final == 'C') return K_CTRL_RIGHT;
				if (mod == 5 && final == 'A') return K_CTRL_UP;
				if (mod == 5 && final == 'B') return K_CTRL_DOWN;
				if (mod == 6 && final == 'D') return K_CTRL_SHIFT_LEFT;
				if (mod == 6 && final == 'C') return K_CTRL_SHIFT_RIGHT;
			}
			// shift arrows \x1b[1;2A .. D
			if (!strcmp(&seq[1], "1;2A")) return K_S_UP;
			if (!strcmp(&seq[1], "1;2B")) return K_S_DOWN;
			if (!strcmp(&seq[1], "1;2C")) return K_S_RIGHT;
			if (!strcmp(&seq[1], "1;2D")) return K_S_LEFT;

			if (seq[1] >= '0' && seq[1] <= '9') {
				if (seq[idx - 1] == '~') {
					switch (seq[1]) {
					case '1': return K_HOME;
					case '2': return K_INS;
					case '3': return K_DEL;
					case '4': return K_END;
					case '5': return K_PGUP;
					case '6': return K_PGDN;
					case '7': return K_HOME;
					case '8': return K_END;
					}
				}
			} else {
				switch (seq[1]) {
				case 'A': return K_UP;
				case 'B': return K_DOWN;
				case 'C': return K_RIGHT;
				case 'D': return K_LEFT;
				case 'H': return K_HOME;
				case 'F': return K_END;
				}
			}
		} else if (seq[0] == 'O') {
			int mod = 0;
			char final = 0;
			if (sscanf(seq, "O1;%d%c", &mod, &final) == 2 && mod == 6) {
				if (final == 'D') return K_CTRL_SHIFT_LEFT;
				if (final == 'C') return K_CTRL_SHIFT_RIGHT;
			}
			switch (seq[1]) {
			case 'H': return K_HOME;
			case 'F': return K_END;
			}
		}
		return '\x1b';
	}
	return c;
}

