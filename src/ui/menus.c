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
	static const char *commands[] = {"theme", "save", "help", "quit", "plugins off", "plugins on", "word wrap", "sidebar"};
	const int ncommands = (int)(sizeof(commands) / sizeof(commands[0]));

	while (1) {
		const char *matches[8];
		int nmatch = 0;
		if (len > 0 || list_all) {
			for (int i = 0; i < ncommands; i++) {
				int qlen = list_all && len == 0 ? 0 : len;
				if (!qlen || !strncasecmp(commands[i], buf, (size_t)qlen)) matches[nmatch++] = commands[i];
			}
		}
		if (selected >= nmatch) selected = nmatch ? nmatch - 1 : 0;
		char suggestions[64] = {0};
		int used = 0;
		for (int i = 0; i < nmatch; i++) {
			int n = snprintf(suggestions + used, sizeof(suggestions) - (size_t)used,
				"%s%s%s", i ? "  " : "", i == selected ? "[" : "", matches[i]);
			if (n < 0 || n >= (int)(sizeof(suggestions) - (size_t)used)) break;
			used += n;
			if (i == selected && used + 1 < (int)sizeof(suggestions)) suggestions[used++] = ']';
			suggestions[used] = '\0';
		}
		if (nmatch) msg("%s: %s | :%s", list_all && len == 0 ? "Commands" : "Matches", suggestions, buf);
		else if (len) msg("no matching commands | :%s", buf);
		else msg("enter cmd, :%s  (type / for commands)", buf);
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

