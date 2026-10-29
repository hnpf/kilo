#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "editor.h"

static int u8_width(const char *s) {
	int w = 0;
	while (*s) {
		unsigned char c = (unsigned char)*s;
		if (c < 0x80) {
			w++;
			s++;
		} else if ((c & 0xe0) == 0xc0) {
			w++;
			s += 2;
		} else if ((c & 0xf0) == 0xe0) {
			w++;
			s += 3;
		} else if ((c & 0xf8) == 0xf0) {
			w += 2;
			s += 4;
		} else {
			s++;
		}
	}
	return w;
}

static void menu_text_add(char *out, int *used, int width, const char *text, size_t length) {
	if (*used >= width) return;
	int room = width - *used;
	if (length > (size_t)room) length = (size_t)room;
	memcpy(out + *used, text, length);
	*used += (int)length;
	out[*used] = '\0';
}

static void command_menu_message(const char *query, int list_all,
		const char **matches, int nmatch, int selected) {
	char message[sizeof(g.msg)] = {0};
	int width = g.w;
	if (width > (int)sizeof(message) - 1) width = (int)sizeof(message) - 1;
	if (width < 0) width = 0;
	int used = 0;

	if (nmatch > 0 && width > 0) {
		int selected_len = (int)strlen(matches[selected]);
		int selected_cost = selected_len + 2;
		int reserve = selected_cost < width ? selected_cost : width;
		int prefix_budget = width - reserve;
		if (list_all && !query[0]) {
			if (prefix_budget >= 4) menu_text_add(message, &used, width, "cmd ", 4);
		} else if (query[0] && prefix_budget > 0) {
			menu_text_add(message, &used, width, ":", 1);
			int query_room = prefix_budget - 2;
			if (query_room > 0) {
				size_t query_len = strlen(query);
				if (query_len > (size_t)query_room) query_len = (size_t)query_room;
				menu_text_add(message, &used, width, query, query_len);
			}
			if (prefix_budget >= 2) menu_text_add(message, &used, width, " ", 1);
		}

		int available = width - used;
		if (selected_cost > available) {
			if (available >= 3) {
				menu_text_add(message, &used, width, "[", 1);
				menu_text_add(message, &used, width, matches[selected], (size_t)(available - 2));
				menu_text_add(message, &used, width, "]", 1);
			} else {
				menu_text_add(message, &used, width, matches[selected], (size_t)available);
			}
			msg("%s", message);
			return;
		}

		int marker_reserve = (selected > 0 ? 4 : 0) + (selected + 1 < nmatch ? 4 : 0);
		if (available - marker_reserve < selected_cost) marker_reserve = 0;
		int list_available = available - marker_reserve;
		int first = selected, last = selected, list_used = selected_cost;
		int left_open = 1, right_open = 1;
		while (left_open || right_open) {
			if (left_open) {
				if (first == 0) left_open = 0;
				else {
					int cost = (int)strlen(matches[first - 1]) + 2;
					if (list_used + cost <= list_available) { first--; list_used += cost; }
					else left_open = 0;
				}
			}
			if (right_open) {
				if (last + 1 >= nmatch) right_open = 0;
				else {
					int cost = (int)strlen(matches[last + 1]) + 2;
					if (list_used + cost <= list_available) { last++; list_used += cost; }
					else right_open = 0;
				}
			}
		}
		int hidden_before = first > 0;
		int hidden_after = last + 1 < nmatch;
		int indicator_space = available - list_used;
		if (hidden_before && hidden_after && indicator_space >= 8) {
			menu_text_add(message, &used, width, "... ", 4);
			indicator_space -= 4;
		} else if (hidden_before && indicator_space >= 4) {
			menu_text_add(message, &used, width, "... ", 4);
			indicator_space -= 4;
			hidden_before = 0;
		}
		for (int i = first; i <= last; i++) {
			if (i > first) menu_text_add(message, &used, width, "  ", 2);
			if (i == selected) menu_text_add(message, &used, width, "[", 1);
			menu_text_add(message, &used, width, matches[i], strlen(matches[i]));
			if (i == selected) menu_text_add(message, &used, width, "]", 1);
		}
		if (hidden_after && indicator_space >= 4)
			menu_text_add(message, &used, width, " ...", 4);
		else if (hidden_before && indicator_space >= 4)
			menu_text_add(message, &used, width, "... ", 4);
	} else if (query[0]) {
		menu_text_add(message, &used, width, "no matches: ", 12);
		menu_text_add(message, &used, width, query, strlen(query));
	} else {
		menu_text_add(message, &used, width, "type / for commands", 19);
	}
	msg("%s", message);
}

void show_help(void) {
	while (1) {
		wsz();
		rfsh();

		struct b b = {0};
		b_add(&b, "\x1b[?25l", 6);

		const char *lines[] = {
			"┌─── kilo help (esc to close) ────────────────────────┐",
			"│                                                     │",
			"│   navigation                   editing              │",
			"│   arrows / home / end          ctrl+s   save        │",
			"│   pgup / pgdn                  ctrl+z   undo        │",
			"│                                ctrl+y   redo        │",
			"│   selection                    ctrl+c   copy        │",
			"│   shift+arrows / mouse drag    ctrl+x   cut         │",
			"│   ctrl+a    select all         ctrl+v   paste       │",
			"│   ins        toggle insert / overwrite              │",
			"│   ctrl+f     find; ctrl+r adds replace field        │",
			"│   ctrl+l     go to line       ctrl+] match bracket  │",
			"│                                                     │",
			"│   menus & help                                      │",
			"│   ctrl+;    cmd / themes       ctrl+g   help        │",
			"│   ctrl+b    toggle file sidebar                    │",
			"│   git gutter: + added ~ modified - deleted          │",
			"│   git: toggle; gutter: bright/muted                 │",
			"│   ctrl+t    new tab          ctrl+w close tab       │",
			"│   alt+left/right   switch tabs  | click x to close  │",
			"│   alt+g     keybindings        ctrl+q   quit        │",
			"│                                                     │",
			"└─────────────────────────────────────────────────────┘"
		};
		int nlines = sizeof(lines) / sizeof(lines[0]);
		int total_h = g.h + 3;
		int start_y = (total_h > nlines) ? (total_h - nlines) / 2 + 1 : 1;

		int r, gr, bl;
		char border_color[32] = {0};
		if (hex_rgb(g.th.cur_ln, &r, &gr, &bl)) {
			snprintf(border_color, sizeof(border_color), "\x1b[38;2;%d;%d;%dm", r, gr, bl);
		} else {
			strcpy(border_color, "\x1b[1;33m");
		}

		for (int i = 0; i < nlines; i++) {
			int y = start_y + i;
			if (y > total_h) break;

			int vlen = u8_width(lines[i]);
			int start_x = (g.w > vlen) ? (g.w - vlen) / 2 + 1 : 1;

			char pos[32];
			int plen = snprintf(pos, sizeof(pos), "\x1b[%d;%dH", y, start_x);
			b_add(&b, pos, plen);

			th_bg(&b);

			if (i == 0 || i == nlines - 1) {
				b_add(&b, border_color, strlen(border_color));
				b_add(&b, lines[i], strlen(lines[i]));
				th_fg(&b);
			} else {
				b_add(&b, border_color, strlen(border_color));
				b_add(&b, "│", 3);
				th_fg(&b);

				int rawlen = strlen(lines[i]);
				if (rawlen > 6) {
					b_add(&b, &lines[i][3], rawlen - 6);
				}

				b_add(&b, border_color, strlen(border_color));
				b_add(&b, "│", 3);
				th_fg(&b);
			}
		}

		write(STDOUT_FILENO, b.p, b.l);
		b_free(&b);

		int c = r_key();
		if (c == '\x1b' || c == ('g' & 0x1f) || c == ('q' & 0x1f) || c == '\r' || c == ' ' || c == K_HELP) {
			break;
		}
	}
}

static void theme_menu(void) {
	scan_themes();
	char orig_th[32];
	strncpy(orig_th, g.th.name, sizeof(orig_th) - 1);
	orig_th[sizeof(orig_th) - 1] = '\0';

	char buf[64] = {0};
	int len = 0;
	int sel_idx = 0;

	while (1) {
		const char *matches[64];
		int nmatches = 0;
		for (int i = 0; i < g_nthemes; i++) {
			if (str_contains_ci(g_themes[i], buf)) {
				matches[nmatches++] = g_themes[i];
			}
		}
		if (sel_idx >= nmatches) sel_idx = (nmatches > 0) ? nmatches - 1 : 0;

		// live preview current match
		if (nmatches > 0) {
			load_th(matches[sel_idx]);
		}

		char mstr[80] = {0};
		int mlen = 0;
		for (int i = 0; i < nmatches && i < 3; i++) {
			char item[40];
			snprintf(item, sizeof(item), "%s%s%s ",
				(i == sel_idx) ? "[" : "",
				matches[i],
				(i == sel_idx) ? "]" : "");
			int ilen = strlen(item);
			if (mlen + ilen < (int)sizeof(mstr) - 1) {
				strcat(mstr, item);
				mlen += ilen;
			}
		}

		msg("theme: %s%s | %s(tab: next, enter: apply, esc: cancel)",
			buf,
			nmatches == 0 ? "(no match) " : "",
			mstr);
		rfsh();

		int c = r_key();
		if (c == '\x1b' || c == ('q' & 0x1f)) {
			// cancel: restore original theme
			load_th(orig_th);
			msg("");
			break;
		} else if (c == '\r') {
			if (nmatches > 0) {
				load_th(matches[sel_idx]);
				save_cfg();
				msg("theme applied: %s", matches[sel_idx]);
			}
			break;
		} else if (c == '\t' || c == K_RIGHT || c == K_DOWN) {
			if (nmatches > 0) sel_idx = (sel_idx + 1) % nmatches;
		} else if (c == K_LEFT || c == K_UP) {
			if (nmatches > 0) sel_idx = (sel_idx - 1 + nmatches) % nmatches;
		} else if (c == K_DEL || c == K_BACKSPACE || c == 8 || c == 127) {
			if (len > 0) {
				buf[--len] = '\0';
				sel_idx = 0;
			}
		} else if (isprint(c) && len < (int)sizeof(buf) - 1) {
			buf[len++] = c;
			buf[len] = '\0';
			sel_idx = 0;
		}
	}
}

void cmd_menu(void) {
	char buf[64] = {0};
	int len = 0;
	int selected = 0;
	int list_all = 0;
	static const char *commands[] = {"theme", "save", "help", "quit", "plugins off", "plugins on", "word wrap", "sidebar", "git", "git off", "git on", "gutter", "gutter bright", "gutter muted"};
	const int ncommands = (int)(sizeof(commands) / sizeof(commands[0]));

	while (1) {
		const char *matches[16];
		int nmatch = 0;
		if (len > 0 || list_all) {
			for (int i = 0; i < ncommands; i++) {
				int qlen = list_all && len == 0 ? 0 : len;
				if (!qlen || !strncasecmp(commands[i], buf, (size_t)qlen)) matches[nmatch++] = commands[i];
			}
		}
		if (selected >= nmatch) selected = nmatch ? nmatch - 1 : 0;
		command_menu_message(buf, list_all, matches, nmatch, selected);
		rfsh();

		int c = r_key();
		if (c == '\x1b' || c == ('q' & 0x1f)) {
			msg("");
			break;
		} else if (c == '/') {
			buf[0] = '\0';
			len = 0;
			list_all = 1;
			selected = 0;
		} else if (c == K_UP || c == K_DOWN || c == K_LEFT || c == K_RIGHT) {
			if (nmatch) {
				int forward = c == K_DOWN || c == K_RIGHT;
				selected = (selected + (forward ? 1 : nmatch - 1)) % nmatch;
			}
		} else if (c == '\t') {
			if (nmatch) {
				snprintf(buf, sizeof(buf), "%s", matches[selected]);
				len = (int)strlen(buf);
				list_all = 0;
				if (!strcmp(buf, "theme")) {
					snprintf(buf, sizeof(buf), "theme:");
					len = (int)strlen(buf);
					theme_menu();
					break;
				}
			}
		} else if (c == '\r') {
			int exact = 0;
			for (int i = 0; i < ncommands; i++) if (!strcasecmp(buf, commands[i])) exact = 1;
			if (nmatch && !exact) {
				snprintf(buf, sizeof(buf), "%s", matches[selected]);
				len = (int)strlen(buf);
			}
			if (!strcmp(buf, "theme") || !strcmp(buf, "theme:") || !strcmp(buf, "t")) {
				theme_menu();
			} else if (!strncmp(buf, "theme ", 6)) {
				load_th(buf + 6);
				save_cfg();
			} else if (!strcmp(buf, "w") || !strcmp(buf, "save")) {
				sav();
			} else if (!strcmp(buf, "h") || !strcmp(buf, "help")) {
				show_help();
			} else if (!strcmp(buf, "q") || !strcmp(buf, "quit")) {
				exit(0);
			} else if (!strcmp(buf, "plugins off") || !strcmp(buf, "plugins on")) {
				g.plugins_enabled = !strcmp(buf, "plugins on");
				save_cfg();
				plugins_load();
				msg("Lua plugins %s", g.plugins_enabled ? "enabled" : "disabled");
			} else if (!strcmp(buf, "word wrap")) {
				g.word_wrap = !g.word_wrap;
				g.rowoff = 0;
				g.coloff = 0;
				g.pref_cx = -1;
				save_cfg();
				msg("word wrap %s", g.word_wrap ? "enabled" : "disabled");
			} else if (!strcmp(buf, "sidebar")) {
				sidebar_handle_key('b' & 0x1f);
				msg("file sidebar %s", sidebar_is_open() ? "shown" : "hidden");
			} else if (!strcmp(buf, "git") || !strcmp(buf, "git on") || !strcmp(buf, "git off")) {
				g.git_enabled = !strcmp(buf, "git") ? !g.git_enabled : !strcmp(buf, "git on");
				if (!g.git_enabled) git_suspend_active();
				save_cfg();
				msg("Git integration %s", g.git_enabled ? "enabled" : "disabled");
			} else if (!strcmp(buf, "gutter") || !strcmp(buf, "gutter bright") || !strcmp(buf, "gutter muted")) {
				g.gutter_bright = !strcmp(buf, "gutter") ? !g.gutter_bright : !strcmp(buf, "gutter bright");
				save_cfg();
				msg("non-repo gutter colors %s", g.gutter_bright ? "bright" : "muted");
			} else if (len > 0) {
				msg("unknown cmd: %s", buf);
			}
			break;
		} else if (c == K_DEL || c == K_BACKSPACE || c == 8 || c == 127) {
			if (len > 0) buf[--len] = '\0';
			selected = 0;
			list_all = 0;
		} else if (isprint(c) && len < (int)sizeof(buf) - 1) {
			buf[len++] = c;
			buf[len] = '\0';
			selected = 0;
			list_all = 0;
			if (!strcmp(buf, "theme")) {
				snprintf(buf, sizeof(buf), "theme:");
				len = (int)strlen(buf);
				theme_menu();
				break;
			}
		}
	}
}

