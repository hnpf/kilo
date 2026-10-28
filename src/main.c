#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KILO_VER "0.0.1"

enum k {
	K_ESC = 27,
	K_BACKSPACE = 127,
	K_UP = 1000,
	K_DOWN,
	K_LEFT,
	K_RIGHT,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
	K_DEL,
	K_S_UP,
	K_S_DOWN,
	K_S_LEFT,
	K_S_RIGHT,
	K_CMD,
	K_REDO
};

// append buffer
struct b {
	char *p;
	int l;
};

// a line of text
struct r {
	int l;
	char *s;
};

// theme
struct th {
	char name[32];
	char bg[32];
	char fg[32];
	char sel[32];
};

// snapshot for undo/redo
struct snap {
	int nr;
	struct r *r;
	int cx;
	int cy;
};

// editor state
struct s {
	struct termios o;
	int cx;
	int cy;
	int sx; // selection anchor x (-1 = no selection)
	int sy; // selection anchor y
	int rowoff;
	int coloff;
	int w;
	int h;
	int nr;
	struct r *r;
	char *fn;
	char *clip;
	char msg[80];
	time_t msg_t;
	int dirty;
	int quit_warn;
	int in_type;
	struct th th;
	struct snap u_stack[128]; // undo
	int u_len;
	struct snap r_stack[128]; // redo
	int r_len;
};

static struct s g;

static void err(const char *m) {
	perror(m);
	exit(1);
}

static void b_add(struct b *b, const char *s, int len) {
	char *n = realloc(b->p, b->l + len);
	if (!n) return;
	memcpy(n + b->l, s, len);
	b->p = n;
	b->l += len;
}

static void b_free(struct b *b) {
	free(b->p);
}

static void rst(void) {
	// disable mouse, pop kitty keyboard, reset modifyOtherKeys, exit alt screen, show cursor, restore term
	const char s[] = "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[<1u\x1b[>4;0m\x1b[?1049l\x1b[?25h";
	write(STDOUT_FILENO, s, sizeof(s) - 1);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &g.o);
}

static void wsz(void) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		err("ioctl winsz");
	}
	g.w = ws.ws_col;
	g.h = ws.ws_row - 2; // room for status bar + msg
}

static void msg(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g.msg, sizeof(g.msg), fmt, ap);
	va_end(ap);
	g.msg_t = time(NULL);
}

static void snap_free(struct snap *s) {
	for (int i = 0; i < s->nr; i++) free(s->r[i].s);
	free(s->r);
	s->r = NULL;
	s->nr = 0;
}

static void snap_clear_redo(void) {
	for (int i = 0; i < g.r_len; i++) snap_free(&g.r_stack[i]);
	g.r_len = 0;
}

static struct snap snap_curr(void) {
	struct snap s;
	s.nr = g.nr;
	s.cx = g.cx;
	s.cy = g.cy;
	s.r = malloc(sizeof(struct r) * (g.nr > 0 ? g.nr : 1));
	for (int i = 0; i < g.nr; i++) {
		s.r[i].l = g.r[i].l;
		s.r[i].s = malloc(g.r[i].l + 1);
		memcpy(s.r[i].s, g.r[i].s, g.r[i].l);
		s.r[i].s[g.r[i].l] = '\0';
	}
	return s;
}

static void snap_save(void) {
	snap_clear_redo();
	if (g.u_len >= 128) {
		snap_free(&g.u_stack[0]);
		memmove(&g.u_stack[0], &g.u_stack[1], sizeof(struct snap) * (g.u_len - 1));
		g.u_len--;
	}
	g.u_stack[g.u_len++] = snap_curr();
}

static void snap_restore(struct snap *s) {
	for (int i = 0; i < g.nr; i++) free(g.r[i].s);
	free(g.r);

	g.nr = s->nr;
	g.cx = s->cx;
	g.cy = s->cy;
	g.sx = -1;
	g.sy = -1;
	g.r = malloc(sizeof(struct r) * (s->nr > 0 ? s->nr : 1));
	for (int i = 0; i < s->nr; i++) {
		g.r[i].l = s->r[i].l;
		g.r[i].s = malloc(s->r[i].l + 1);
		memcpy(g.r[i].s, s->r[i].s, s->r[i].l);
		g.r[i].s[s->r[i].l] = '\0';
	}
	g.dirty = 1;
}

static void undo(void) {
	if (g.u_len == 0) {
		msg("already at oldest change");
		return;
	}
	if (g.r_len < 128) {
		g.r_stack[g.r_len++] = snap_curr();
	}
	struct snap s = g.u_stack[--g.u_len];
	snap_restore(&s);
	snap_free(&s);
	msg("undo");
}

static void redo(void) {
	if (g.r_len == 0) {
		msg("already at newest change");
		return;
	}
	if (g.u_len < 128) {
		g.u_stack[g.u_len++] = snap_curr();
	}
	struct snap s = g.r_stack[--g.r_len];
	snap_restore(&s);
	snap_free(&s);
	msg("redo");
}

static void add_r(int at, const char *s, size_t len) {
	if (at < 0 || at > g.nr) return;
	g.r = realloc(g.r, sizeof(struct r) * (g.nr + 1));
	memmove(&g.r[at + 1], &g.r[at], sizeof(struct r) * (g.nr - at));
	g.r[at].l = (int)len;
	g.r[at].s = malloc(len + 1);
	memcpy(g.r[at].s, s, len);
	g.r[at].s[len] = '\0';
	g.nr++;
	g.dirty = 1;
}

static void del_r(int at) {
	if (at < 0 || at >= g.nr) return;
	free(g.r[at].s);
	memmove(&g.r[at], &g.r[at + 1], sizeof(struct r) * (g.nr - at - 1));
	g.nr--;
	g.dirty = 1;
}

static void r_ins_c(struct r *row, int at, int c) {
	if (at < 0 || at > row->l) at = row->l;
	row->s = realloc(row->s, row->l + 2);
	memmove(&row->s[at + 1], &row->s[at], row->l - at + 1);
	row->l++;
	row->s[at] = c;
	g.dirty = 1;
}

static void r_del_c(struct r *row, int at) {
	if (at < 0 || at >= row->l) return;
	memmove(&row->s[at], &row->s[at + 1], row->l - at);
	row->l--;
	g.dirty = 1;
}

static void r_app_s(struct r *row, const char *s, size_t len) {
	row->s = realloc(row->s, row->l + len + 1);
	memcpy(&row->s[row->l], s, len);
	row->l += len;
	row->s[row->l] = '\0';
	g.dirty = 1;
}

static void opn(const char *fn) {
	free(g.fn);
	g.fn = strdup(fn);

	FILE *fp = fopen(fn, "r");
	if (!fp) return;

	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while ((len = getline(&line, &cap, fp)) != -1) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			len--;
		add_r(g.nr, line, len);
	}
	free(line);
	fclose(fp);
	g.dirty = 0;
}

static void sav(void) {
	if (!g.fn) {
		msg("no filename");
		return;
	}

	int len = 0;
	for (int i = 0; i < g.nr; i++) {
		len += g.r[i].l + 1;
	}
	char *buf = malloc(len + 1);
	char *p = buf;
	for (int i = 0; i < g.nr; i++) {
		memcpy(p, g.r[i].s, g.r[i].l);
		p += g.r[i].l;
		*p++ = '\n';
	}

	int fd = open(g.fn, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				g.dirty = 0;
				msg("%d bytes written", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	msg("save error: %s", strerror(errno));
}

// selection helpers
static void sel_bounds(int *y1, int *x1, int *y2, int *x2) {
	if (g.sy < g.cy || (g.sy == g.cy && g.sx <= g.cx)) {
		*y1 = g.sy; *x1 = g.sx;
		*y2 = g.cy; *x2 = g.cx;
	} else {
		*y1 = g.cy; *x1 = g.cx;
		*y2 = g.sy; *x2 = g.sx;
	}
}

static int in_sel(int y, int x) {
	if (g.sx == -1) return 0;
	int y1, x1, y2, x2;
	sel_bounds(&y1, &x1, &y2, &x2);
	if (y < y1 || y > y2) return 0;
	if (y == y1 && y == y2) return x >= x1 && x < x2;
	if (y == y1) return x >= x1;
	if (y == y2) return x < x2;
	return 1;
}

static char *get_sel(void) {
	if (g.sx == -1) return NULL;
	int y1, x1, y2, x2;
	sel_bounds(&y1, &x1, &y2, &x2);
	if (y1 == y2 && x1 == x2) return NULL;

	struct b b = {0};
	for (int y = y1; y <= y2 && y < g.nr; y++) {
		int start = (y == y1) ? x1 : 0;
		int end = (y == y2) ? x2 : g.r[y].l;
		if (start < g.r[y].l) {
			if (end > g.r[y].l) end = g.r[y].l;
			if (end > start) b_add(&b, &g.r[y].s[start], end - start);
		}
		if (y < y2) b_add(&b, "\n", 1);
	}
	b_add(&b, "\0", 1);
	return b.p;
}

// b64 encoder for osc 52
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64_enc(const char *src, int len) {
	int outlen = 4 * ((len + 2) / 3);
	char *out = malloc(outlen + 1);
	int i = 0, j = 0;
	while (i < len) {
		int a = (unsigned char)src[i++];
		int b = i < len ? (unsigned char)src[i++] : 0;
		int c = i < len ? (unsigned char)src[i++] : 0;
		out[j++] = b64[(a >> 2) & 0x3f];
		out[j++] = b64[((a & 3) << 4) | (b >> 4)];
		out[j++] = (i > len + 1) ? '=' : b64[((b & 0xf) << 2) | (c >> 6)];
		out[j++] = (i > len) ? '=' : b64[c & 0x3f];
	}
	out[j] = '\0';
	return out;
}

static void copy_sel(void) {
	char *s = get_sel();
	if (!s) {
		// no selection: copy whole current line
		if (g.cy < g.nr) {
			int len = g.r[g.cy].l;
			s = malloc(len + 2);
			memcpy(s, g.r[g.cy].s, len);
			s[len] = '\n';
			s[len + 1] = '\0';
		} else {
			return;
		}
	}

	free(g.clip);
	g.clip = strdup(s);

	// osc 52
	char *enc = b64_enc(s, strlen(s));
	write(STDOUT_FILENO, "\x1b]52;c;", 7);
	write(STDOUT_FILENO, enc, strlen(enc));
	write(STDOUT_FILENO, "\x07", 1);
	free(enc);

	// system clipboard fallback
	FILE *p = popen("wl-copy 2>/dev/null || xclip -selection clipboard 2>/dev/null", "w");
	if (p) {
		fputs(s, p);
		pclose(p);
	}

	msg("copied %zu bytes", strlen(s));
	free(s);
}

static void del_sel(void) {
	if (g.sx == -1) return;
	int y1, x1, y2, x2;
	sel_bounds(&y1, &x1, &y2, &x2);
	if (y1 == y2 && x1 == x2) {
		g.sx = -1;
		return;
	}

	if (y1 == y2) {
		if (y1 < g.nr) {
			memmove(&g.r[y1].s[x1], &g.r[y1].s[x2], g.r[y1].l - x2 + 1);
			g.r[y1].l -= (x2 - x1);
		}
	} else {
		if (y1 < g.nr && y2 < g.nr) {
			g.r[y1].l = x1;
			g.r[y1].s[x1] = '\0';
			r_app_s(&g.r[y1], &g.r[y2].s[x2], g.r[y2].l - x2);
			for (int y = y2; y > y1; y--) del_r(y);
		}
	}

	g.cy = y1;
	g.cx = x1;
	g.sx = -1;
	g.dirty = 1;
}

static void ins_c(int c) {
	if (!g.in_type) {
		snap_save();
		g.in_type = 1;
	}
	if (g.sx != -1) del_sel();
	if (g.cy == g.nr) add_r(g.nr, "", 0);
	r_ins_c(&g.r[g.cy], g.cx, c);
	g.cx++;
}

static void ins_nl(void) {
	g.in_type = 0;
	snap_save();
	if (g.sx != -1) del_sel();
	if (g.cy == g.nr) {
		add_r(g.nr, "", 0);
	} else if (g.cx == 0) {
		add_r(g.cy, "", 0);
	} else {
		struct r *row = &g.r[g.cy];
		add_r(g.cy + 1, &row->s[g.cx], row->l - g.cx);
		row = &g.r[g.cy];
		row->l = g.cx;
		row->s[row->l] = '\0';
	}
	g.cy++;
	g.cx = 0;
}

static void del_c(void) {
	g.in_type = 0;
	snap_save();
	if (g.sx != -1) {
		del_sel();
		return;
	}
	if (g.cy == g.nr) return;
	if (g.cx == 0 && g.cy == 0) return;

	struct r *row = &g.r[g.cy];
	if (g.cx > 0) {
		r_del_c(row, g.cx - 1);
		g.cx--;
	} else {
		g.cx = g.r[g.cy - 1].l;
		r_app_s(&g.r[g.cy - 1], row->s, row->l);
		del_r(g.cy);
		g.cy--;
	}
}

static void paste(void) {
	if (!g.clip) return;
	g.in_type = 0;
	snap_save();
	if (g.sx != -1) del_sel();
	for (int i = 0; g.clip[i]; i++) {
		if (g.clip[i] == '\r' || g.clip[i] == '\n') {
			ins_nl();
		} else {
			ins_c(g.clip[i]);
		}
	}
}

static void load_th(const char *name) {
	char path[128];
	snprintf(path, sizeof(path), "themes/%s.conf", name);
	FILE *fp = fopen(path, "r");
	if (!fp) {
		snprintf(path, sizeof(path), "/etc/kilo/themes/%s.conf", name);
		fp = fopen(path, "r");
	}
	if (!fp) {
		msg("theme not found: %s", name);
		return;
	}

	strncpy(g.th.name, name, sizeof(g.th.name) - 1);
	char line[128];
	while (fgets(line, sizeof(line), fp)) {
		char key[32], val[32];
		if (sscanf(line, " %31[^= ] = \"%31[^\"]\"", key, val) == 2 ||
		    sscanf(line, " %31[^= ] = %31s", key, val) == 2) {
			if (!strcmp(key, "background")) snprintf(g.th.bg, sizeof(g.th.bg), "%s", val);
			else if (!strcmp(key, "foreground")) snprintf(g.th.fg, sizeof(g.th.fg), "%s", val);
			else if (!strcmp(key, "selection")) snprintf(g.th.sel, sizeof(g.th.sel), "%s", val);
		}
	}
	fclose(fp);
	msg("theme: %s", name);
}

static void scrl(void) {
	if (g.cy < g.rowoff) {
		g.rowoff = g.cy;
	}
	if (g.cy >= g.rowoff + g.h) {
		g.rowoff = g.cy - g.h + 1;
	}
	if (g.cx < g.coloff) {
		g.coloff = g.cx;
	}
	if (g.cx >= g.coloff + g.w) {
		g.coloff = g.cx - g.w + 1;
	}
}

static void rfsh(void) {
	scrl();

	struct b b = {0};

	// hide cursor during draw
	b_add(&b, "\x1b[?25l", 6);
	b_add(&b, "\x1b[H", 3);

	// draw file lines or tildes
	for (int y = 0; y < g.h; y++) {
		int filerow = y + g.rowoff;
		if (filerow >= g.nr) {
			if (g.nr == 0 && y == g.h / 3) {
				char welc[80];
				int welclen = snprintf(welc, sizeof(welc),
					"kilo editor -- version %s", KILO_VER);
				if (welclen > g.w) welclen = g.w;
				int pad = (g.w - welclen) / 2;
				if (pad) {
					b_add(&b, "~", 1);
					pad--;
				}
				while (pad--) b_add(&b, " ", 1);
				b_add(&b, welc, welclen);
			} else {
				b_add(&b, "~", 1);
			}
		} else {
			int len = g.r[filerow].l - g.coloff;
			if (len < 0) len = 0;
			if (len > g.w) len = g.w;
			int in_highlight = 0;
			for (int j = 0; j < len; j++) {
				int col = j + g.coloff;
				int sel = in_sel(filerow, col);
				if (sel && !in_highlight) {
					b_add(&b, "\x1b[7m", 4); // inverse for selection
					in_highlight = 1;
				} else if (!sel && in_highlight) {
					b_add(&b, "\x1b[m", 3);
					in_highlight = 0;
				}
				b_add(&b, &g.r[filerow].s[col], 1);
			}
			if (in_highlight) b_add(&b, "\x1b[m", 3);
		}

		b_add(&b, "\x1b[K", 3);
		b_add(&b, "\r\n", 2);
	}

	// status bar (inverted colors)
	b_add(&b, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), " %.20s%s - %d lines [%s]",
		g.fn ? g.fn : "[no name]",
		g.dirty ? " *" : "",
		g.nr,
		g.th.name[0] ? g.th.name : "base16");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d ",
		g.cy + 1, g.cx + 1);
	if (len > g.w) len = g.w;
	b_add(&b, status, len);
	while (len < g.w) {
		if (g.w - len == rlen) {
			b_add(&b, rstatus, rlen);
			break;
		}
		b_add(&b, " ", 1);
		len++;
	}
	b_add(&b, "\x1b[m", 3);
	b_add(&b, "\r\n", 2);

	// status message line
	b_add(&b, "\x1b[K", 3);
	int msglen = strlen(g.msg);
	if (msglen > g.w) msglen = g.w;
	if (msglen && time(NULL) - g.msg_t < 5) {
		b_add(&b, g.msg, msglen);
	}

	// move cursor to cx, cy relative to viewport
	char buf[32];
	int cx = g.cx - g.coloff;
	int cy = g.cy - g.rowoff;
	int curlen = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy + 1, cx + 1);
	b_add(&b, buf, curlen);

	// show cursor
	b_add(&b, "\x1b[?25h", 6);

	write(STDOUT_FILENO, b.p, b.l);
	b_free(&b);
}

static void sig_winch(int sig) {
	(void)sig;
	wsz();
	rfsh();
}

static void raw(void) {
	if (tcgetattr(STDIN_FILENO, &g.o) == -1) err("tcgetattr");
	atexit(rst);

	struct termios r = g.o;
	r.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	r.c_oflag &= ~(OPOST);
	r.c_cflag |= (CS8);
	r.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	r.c_cc[VMIN] = 0;
	r.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &r) == -1) err("tcsetattr");

	signal(SIGWINCH, sig_winch);

	// enter alt screen, enable SGR mouse tracking, enable kitty disambiguate / modifyOtherKeys
	const char enter_seq[] = "\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[>1u\x1b[>4;2m\x1b[H";
	write(STDOUT_FILENO, enter_seq, sizeof(enter_seq) - 1);
}

static int r_key(void) {
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

		// mouse report \x1b[<b;x;yM
		if (seq[0] == '[' && seq[1] == '<') {
			int b, mx, my;
			char mtype = 0;
			if (sscanf(&seq[2], "%d;%d;%d%c", &b, &mx, &my, &mtype) >= 3) {
				if (b == 64) {
					// scroll up
					for (int i = 0; i < 3; i++) {
						if (g.cy > 0) g.cy--;
					}
					return 0;
				} else if (b == 65) {
					// scroll down
					for (int i = 0; i < 3; i++) {
						if (g.cy < g.nr) g.cy++;
					}
					return 0;
				} else if (b == 0 && mtype == 'M') {
					// left click
					g.cy = (my - 1) + g.rowoff;
					g.cx = (mx - 1) + g.coloff;
					if (g.cy > g.nr) g.cy = g.nr;
					struct r *cur = (g.cy < g.nr) ? &g.r[g.cy] : NULL;
					int rowlen = cur ? cur->l : 0;
					if (g.cx > rowlen) g.cx = rowlen;
					g.sx = -1;
					return 0;
				} else if (b == 32) {
					// mouse drag
					if (g.sx == -1) {
						g.sx = g.cx;
						g.sy = g.cy;
					}
					g.cy = (my - 1) + g.rowoff;
					g.cx = (mx - 1) + g.coloff;
					if (g.cy > g.nr) g.cy = g.nr;
					struct r *cur = (g.cy < g.nr) ? &g.r[g.cy] : NULL;
					int rowlen = cur ? cur->l : 0;
					if (g.cx > rowlen) g.cx = rowlen;
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
			return K_CMD;
		}

		// ctrl+shift+z (redo)
		if (strstr(seq, "122;6") || strstr(seq, "90;6")) {
			return K_REDO;
		}

		if (seq[0] == '[') {
			// shift arrows \x1b[1;2A .. D
			if (!strcmp(&seq[1], "1;2A")) return K_S_UP;
			if (!strcmp(&seq[1], "1;2B")) return K_S_DOWN;
			if (!strcmp(&seq[1], "1;2C")) return K_S_RIGHT;
			if (!strcmp(&seq[1], "1;2D")) return K_S_LEFT;

			if (seq[1] >= '0' && seq[1] <= '9') {
				if (seq[idx - 1] == '~') {
					switch (seq[1]) {
					case '1': return K_HOME;
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
			switch (seq[1]) {
			case 'H': return K_HOME;
			case 'F': return K_END;
			}
		}
		return '\x1b';
	}
	return c;
}

static void mv(int key) {
	struct r *cur = (g.cy < g.nr) ? &g.r[g.cy] : NULL;

	switch (key) {
	case K_LEFT:
	case K_S_LEFT:
		if (g.cx > 0) {
			g.cx--;
		} else if (g.cy > 0) {
			g.cy--;
			g.cx = g.r[g.cy].l;
		}
		break;
	case K_RIGHT:
	case K_S_RIGHT:
		if (cur && g.cx < cur->l) {
			g.cx++;
		} else if (cur && g.cx == cur->l && g.cy + 1 < g.nr) {
			g.cy++;
			g.cx = 0;
		}
		break;
	case K_UP:
	case K_S_UP:
		if (g.cy > 0) g.cy--;
		break;
	case K_DOWN:
	case K_S_DOWN:
		if (g.cy < g.nr) g.cy++;
		break;
	case K_HOME:
		g.cx = 0;
		break;
	case K_END:
		if (cur) g.cx = cur->l;
		break;
	case K_PGUP:
		g.cy = g.rowoff;
		for (int times = g.h; times > 0; times--) {
			if (g.cy > 0) g.cy--;
		}
		break;
	case K_PGDN:
		g.cy = g.rowoff + g.h - 1;
		if (g.cy > g.nr) g.cy = g.nr;
		for (int times = g.h; times > 0; times--) {
			if (g.cy < g.nr) g.cy++;
		}
		break;
	}

	cur = (g.cy < g.nr) ? &g.r[g.cy] : NULL;
	int rowlen = cur ? cur->l : 0;
	if (g.cx > rowlen) g.cx = rowlen;
}

static void cmd_menu(void) {
	char buf[80] = {0};
	int len = 0;

	while (1) {
		msg("cmd: 1) base16 2) helix 3) amber | :%s", buf);
		rfsh();

		int c = r_key();
		if (c == '\x1b' || c == ('q' & 0x1f)) {
			msg("");
			break;
		} else if (c == '1') {
			load_th("base16_transparent");
			break;
		} else if (c == '2') {
			load_th("helix_default");
			break;
		} else if (c == '3') {
			load_th("unix_amber");
			break;
		} else if (c == '\r') {
			if (!strcmp(buf, "w") || !strcmp(buf, "save")) {
				sav();
			} else if (!strncmp(buf, "theme ", 6)) {
				load_th(buf + 6);
			} else {
				msg("unknown cmd: %s", buf);
			}
			break;
		} else if (c == K_BACKSPACE || c == 8) {
			if (len > 0) buf[--len] = '\0';
		} else if (isprint(c) && len < (int)sizeof(buf) - 1) {
			buf[len++] = c;
			buf[len] = '\0';
		}
	}
}

int main(int argc, char **argv) {
	if (!isatty(STDIN_FILENO)) {
		err("stdin not a tty");
	}

	g.sx = -1;
	g.sy = -1;
	load_th("base16_transparent");

	raw();
	wsz();

	if (argc >= 2) {
		opn(argv[1]);
	}

	msg("ctrl-s: save | ctrl-z/y: undo/redo | ctrl-;: cmd menu (f1)");

	while (1) {
		rfsh();
		int c = r_key();

		// only ctrl-q quits
		if (c == ('q' & 0x1f)) {
			if (g.dirty && !g.quit_warn) {
				msg("unsaved changes! press ctrl-q again to quit");
				g.quit_warn = 1;
				continue;
			}
			break;
		}
		g.quit_warn = 0;

		switch (c) {
		case 0:
			// mouse event handled in r_key
			break;

		// undo: ctrl+z
		case ('z' & 0x1f):
			g.in_type = 0;
			undo();
			break;

		// redo: ctrl+y or ctrl+shift+z
		case ('y' & 0x1f):
		case K_REDO:
			g.in_type = 0;
			redo();
			break;

		// command menu: ctrl+;, ctrl+p, f1, ctrl+\, ctrl+/
		case K_CMD:
		case ('p' & 0x1f):
		case 28: // ctrl+backslash
		case 31: // ctrl+/
			g.in_type = 0;
			cmd_menu();
			break;

		// save: ctrl+s
		case ('s' & 0x1f):
			g.in_type = 0;
			sav();
			break;

		// copy: ctrl+c
		case ('c' & 0x1f):
			copy_sel();
			break;

		// paste: ctrl+v
		case ('v' & 0x1f):
			paste();
			break;

		// cut: ctrl+x (cuts selection, or whole line if no selection)
		case ('x' & 0x1f):
			g.in_type = 0;
			snap_save();
			if (g.sx != -1) {
				copy_sel();
				del_sel();
			} else if (g.cy < g.nr) {
				copy_sel();
				del_r(g.cy);
				if (g.cy >= g.nr && g.cy > 0) g.cy = g.nr - 1;
				g.cx = 0;
				msg("cut line");
			}
			break;

		// select all: ctrl+a
		case ('a' & 0x1f):
			g.in_type = 0;
			g.sx = 0;
			g.sy = 0;
			if (g.nr > 0) {
				g.cy = g.nr - 1;
				g.cx = g.r[g.cy].l;
			}
			break;

		// shift + arrows
		case K_S_UP:
		case K_S_DOWN:
		case K_S_LEFT:
		case K_S_RIGHT:
			g.in_type = 0;
			if (g.sx == -1) {
				g.sx = g.cx;
				g.sy = g.cy;
			}
			mv(c);
			break;

		// normal movement clears selection
		case K_UP:
		case K_DOWN:
		case K_LEFT:
		case K_RIGHT:
		case K_HOME:
		case K_END:
		case K_PGUP:
		case K_PGDN:
			g.in_type = 0;
			g.sx = -1;
			mv(c);
			break;

		case '\r':
			ins_nl();
			break;

		case K_BACKSPACE:
		case 8:
			del_c();
			break;

		case K_DEL:
			g.in_type = 0;
			snap_save();
			if (g.sx != -1) {
				del_sel();
			} else {
				mv(K_RIGHT);
				del_c();
			}
			break;

		case '\x1b':
			g.in_type = 0;
			g.sx = -1; // esc clears selection
			break;

		default:
			if (isprint(c) || c == '\t') {
				ins_c(c);
			}
			break;
		}
	}

	return 0;
}
