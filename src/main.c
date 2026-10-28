#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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
	K_HELP,
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

// theme colors
struct th {
	char name[32];
	char bg[32];
	char fg[32];
	char sel[32];
	char ln[32];
	char cur_ln[32];
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
	size_t file_sz;
	struct th th;
	struct snap u_stack[128]; // undo
	int u_len;
	struct snap r_stack[128]; // redo
	int r_len;
};

static struct s g;
static char g_themes[64][32];
static int g_nthemes = 0;

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

static int hex_rgb(const char *hex, int *r, int *g, int *b) {
	if (!hex || hex[0] != '#') return 0;
	unsigned int ur, ug, ub;
	if (sscanf(hex + 1, "%02x%02x%02x", &ur, &ug, &ub) == 3) {
		*r = (int)ur;
		*g = (int)ug;
		*b = (int)ub;
		return 1;
	}
	return 0;
}

static void th_bg(struct b *b) {
	int r, gr, bl;
	if (!strcmp(g.th.bg, "none") || !g.th.bg[0]) {
		b_add(b, "\x1b[49m", 5);
	} else if (hex_rgb(g.th.bg, &r, &gr, &bl)) {
		char cbuf[32];
		int clen = snprintf(cbuf, sizeof(cbuf), "\x1b[48;2;%d;%d;%dm", r, gr, bl);
		b_add(b, cbuf, clen);
	}
}

static void th_fg(struct b *b) {
	int r, gr, bl;
	if (hex_rgb(g.th.fg, &r, &gr, &bl)) {
		char cbuf[32];
		int clen = snprintf(cbuf, sizeof(cbuf), "\x1b[38;2;%d;%d;%dm", r, gr, bl);
		b_add(b, cbuf, clen);
	} else {
		b_add(b, "\x1b[39m", 5);
	}
}

static void rst(void) {
	// reset colors, disable mouse, pop kitty keyboard, reset modifyOtherKeys, exit alt screen, show cursor, restore term
	const char s[] = "\x1b[0m\x1b[49m\x1b[39m\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[<1u\x1b[>4;0m\x1b[?1049l\x1b[?25h";
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

	struct stat st;
	if (fstat(fileno(fp), &st) == 0) {
		g.file_sz = st.st_size;
	}

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

static char *prmpt(const char *prompt_str);

static void sav(void) {
	if (!g.fn) {
		g.fn = prmpt("save as: %s (esc or ctrl-c to cancel)");
		if (!g.fn) return;
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
				g.file_sz = len;
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
	char path[256];
	const char *home = getenv("HOME");
	FILE *fp = NULL;

	if (home) {
		snprintf(path, sizeof(path), "%s/.config/kilo/themes/%s.conf", home, name);
		fp = fopen(path, "r");
	}
	if (!fp) {
		snprintf(path, sizeof(path), "themes/%s.conf", name);
		fp = fopen(path, "r");
	}
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
			else if (!strcmp(key, "line_number")) snprintf(g.th.ln, sizeof(g.th.ln), "%s", val);
			else if (!strcmp(key, "current_line_nr")) snprintf(g.th.cur_ln, sizeof(g.th.cur_ln), "%s", val);
		}
	}
	fclose(fp);
	msg("theme: %s", name);
}

static char *cfg_path(void) {
	const char *home = getenv("HOME");
	if (!home) return NULL;
	static char path[256];
	snprintf(path, sizeof(path), "%s/.config/kilo/kilo.conf", home);
	return path;
}

static void load_cfg(void) {
	char *p = cfg_path();
	if (!p) return;
	FILE *fp = fopen(p, "r");
	if (!fp) return;

	char line[128];
	while (fgets(line, sizeof(line), fp)) {
		char key[32], val[32];
		if (sscanf(line, " %31[^= ] = \"%31[^\"]\"", key, val) == 2 ||
		    sscanf(line, " %31[^= ] = %31s", key, val) == 2) {
			if (!strcmp(key, "theme")) {
				load_th(val);
			}
		}
	}
	fclose(fp);
}

static void save_cfg(void) {
	const char *home = getenv("HOME");
	if (!home) return;
	char dir[256];
	snprintf(dir, sizeof(dir), "%s/.config", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.config/kilo", home);
	mkdir(dir, 0755);

	char *p = cfg_path();
	if (!p) return;
	FILE *fp = fopen(p, "w");
	if (!fp) return;
	fprintf(fp, "[editor]\ntheme = \"%s\"\n", g.th.name[0] ? g.th.name : "base16_transparent");
	fclose(fp);
}

static void scan_themes(void) {
	g_nthemes = 0;
	char udir[256];
	const char *home = getenv("HOME");
	if (home) {
		snprintf(udir, sizeof(udir), "%s/.config/kilo/themes", home);
	}

	for (int d = 0; d < 3; d++) {
		const char *dirpath = (d == 0) ? "themes" : (d == 1 && home ? udir : "/etc/kilo/themes");
		DIR *dp = opendir(dirpath);
		if (!dp) continue;
		struct dirent *de;
		while ((de = readdir(dp)) != NULL && g_nthemes < 64) {
			char *ext = strstr(de->d_name, ".conf");
			if (ext && strlen(ext) == 5) {
				char name[32];
				size_t len = ext - de->d_name;
				if (len < sizeof(name)) {
					strncpy(name, de->d_name, len);
					name[len] = '\0';
					int exists = 0;
					for (int i = 0; i < g_nthemes; i++) {
						if (!strcmp(g_themes[i], name)) { exists = 1; break; }
					}
					if (!exists) {
						snprintf(g_themes[g_nthemes++], sizeof(g_themes[0]), "%s", name);
					}
				}
			}
		}
		closedir(dp);
	}
}

static int str_contains_ci(const char *haystack, const char *needle) {
	if (!needle || !needle[0]) return 1;
	size_t nlen = strlen(needle);
	size_t hlen = strlen(haystack);
	if (nlen > hlen) return 0;
	for (size_t i = 0; i <= hlen - nlen; i++) {
		size_t j = 0;
		while (j < nlen && tolower((unsigned char)haystack[i + j]) == tolower((unsigned char)needle[j])) {
			j++;
		}
		if (j == nlen) return 1;
	}
	return 0;
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
	b_add(&b, "\x1b[?25l\x1b[H", 9);

	// apply theme bg & fg
	th_bg(&b);
	th_fg(&b);

	// selection ANSI
	int r, gr, bl;
	char sel_on[32];
	if (hex_rgb(g.th.sel, &r, &gr, &bl)) {
		snprintf(sel_on, sizeof(sel_on), "\x1b[48;2;%d;%d;%dm", r, gr, bl);
	} else {
		strcpy(sel_on, "\x1b[7m");
	}

	// gutter width for line numbers
	int ln_w = 1;
	int temp = g.nr;
	while (temp >= 10) { ln_w++; temp /= 10; }
	if (ln_w < 3) ln_w = 3;
	int gutter = ln_w + 3; // e.g. " 12 │ "
	int text_w = g.w - gutter;
	if (text_w < 1) text_w = 1;

	// draw file lines
	for (int y = 0; y < g.h; y++) {
		int filerow = y + g.rowoff;
		int col_written = 0;

		// line number gutter
		th_bg(&b);
		if (filerow < g.nr) {
			int cur = (filerow == g.cy);
			if (cur && hex_rgb(g.th.cur_ln, &r, &gr, &bl)) {
				char cbuf[32];
				int clen = snprintf(cbuf, sizeof(cbuf), "\x1b[38;2;%d;%d;%dm", r, gr, bl);
				b_add(&b, cbuf, clen);
			} else if (!cur && hex_rgb(g.th.ln, &r, &gr, &bl)) {
				char cbuf[32];
				int clen = snprintf(cbuf, sizeof(cbuf), "\x1b[38;2;%d;%d;%dm", r, gr, bl);
				b_add(&b, cbuf, clen);
			} else {
				b_add(&b, cur ? "\x1b[1;33m" : "\x1b[90m", cur ? 7 : 5);
			}

			char numbuf[32];
			int nlen = snprintf(numbuf, sizeof(numbuf), "%*d │ ", ln_w, filerow + 1);
			b_add(&b, numbuf, nlen);
			col_written += gutter;
			th_fg(&b);
		} else {
			b_add(&b, "\x1b[90m", 5);
			for (int p = 0; p < ln_w; p++) b_add(&b, " ", 1);
			b_add(&b, " ~ ", 3);
			col_written += gutter;
			th_fg(&b);
		}

		if (filerow >= g.nr) {
			if (g.nr == 0 && y == g.h / 3) {
				char welc[80];
				int welclen = snprintf(welc, sizeof(welc),
					"kilo editor -- version %s", KILO_VER);
				if (welclen > text_w) welclen = text_w;
				int pad = (text_w - welclen) / 2;
				while (pad-- > 0) {
					b_add(&b, " ", 1);
					col_written++;
				}
				b_add(&b, welc, welclen);
				col_written += welclen;
			}
		} else {
			int len = g.r[filerow].l - g.coloff;
			if (len < 0) len = 0;
			if (len > text_w) len = text_w;
			int in_highlight = 0;
			for (int j = 0; j < len; j++) {
				int col = j + g.coloff;
				int sel = in_sel(filerow, col);
				if (sel && !in_highlight) {
					b_add(&b, sel_on, strlen(sel_on));
					in_highlight = 1;
				} else if (!sel && in_highlight) {
					th_bg(&b);
					in_highlight = 0;
				}
				b_add(&b, &g.r[filerow].s[col], 1);
				col_written++;
			}
			if (in_highlight) {
				th_bg(&b);
			}
		}

		// pad remainder of row to full screen width under theme background
		th_bg(&b);
		while (col_written < g.w) {
			b_add(&b, " ", 1);
			col_written++;
		}

		b_add(&b, "\x1b[K\r\n", 5);
	}

	// status bar (inverted colors / file info)
	b_add(&b, "\x1b[7m", 4);
	char status[128], rstatus[128];
	int len = snprintf(status, sizeof(status), " %.20s%s | %d lines, %zuB | unix (LF) | utf-8",
		g.fn ? g.fn : "[no name]",
		g.dirty ? " *" : "",
		g.nr,
		g.file_sz);
	int rlen = snprintf(rstatus, sizeof(rstatus), "ln %d, col %d [%s] ",
		g.cy + 1, g.cx + 1,
		g.th.name[0] ? g.th.name : "base16");
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
	b_add(&b, "\x1b[m\r\n", 5);

	// status message line (padded with theme bg)
	th_bg(&b);
	th_fg(&b);
	int msglen = strlen(g.msg);
	if (msglen > g.w) msglen = g.w;
	int mcols = 0;
	if (msglen && time(NULL) - g.msg_t < 5) {
		b_add(&b, g.msg, msglen);
		mcols = msglen;
	}
	while (mcols < g.w) {
		b_add(&b, " ", 1);
		mcols++;
	}

	// move cursor to cx, cy relative to viewport + gutter
	char buf[32];
	int cx = (g.cx - g.coloff) + gutter;
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

		// alt+g: \x1bg or \x1bG
		if (idx == 1 && (seq[0] == 'g' || seq[0] == 'G')) return K_HELP;

		// CSI u keyboard reporting (\x1b[<codepoint>;<mod>u)
		if (seq[0] == '[') {
			int key = 0, mod = 0;
			if (sscanf(seq, "[%d;%du", &key, &mod) == 2) {
				if (mod == 3 || mod == 7) { // Alt or Ctrl+Alt
					if (key == 'g' || key == 'G') return K_HELP;
				}
				if (mod == 5 || mod == 7) { // Ctrl or Ctrl+Alt
					if (key >= 'a' && key <= 'z') return key - 'a' + 1;
					if (key >= 'A' && key <= 'Z') return key - 'A' + 1;
					if (key == ';') return K_CMD;
					if (key == 'g' || key == 'G') return K_HELP;
				}
				if (mod == 6) { // Ctrl+Shift
					if (key == 'z' || key == 'Z') return K_REDO;
					if (key >= 'a' && key <= 'z') return key - 'a' + 1;
				}
				if (key == ';') return K_CMD;
			}
		}

		if (strstr(seq, "103;3") || strstr(seq, "71;3") || strstr(seq, "27;3;103~")) return K_HELP;

		// mouse report \x1b[<b;x;yM
		if (seq[0] == '[' && seq[1] == '<') {
			int b, mx, my;
			char mtype = 0;
			if (sscanf(&seq[2], "%d;%d;%d%c", &b, &mx, &my, &mtype) >= 3) {
				int ln_w = 1;
				int temp = g.nr;
				while (temp >= 10) { ln_w++; temp /= 10; }
				if (ln_w < 3) ln_w = 3;
				int gutter = ln_w + 3;

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
					g.cx = (mx - 1 - gutter) + g.coloff;
					if (g.cx < 0) g.cx = 0;
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
					g.cx = (mx - 1 - gutter) + g.coloff;
					if (g.cx < 0) g.cx = 0;
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
			return K_HELP;
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

static void show_help(void) {
	while (1) {
		struct b b = {0};
		b_add(&b, "\x1b[?25l\x1b[H", 9);

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
			"│                                                     │",
			"│   menus & help                                      │",
			"│   ctrl+;    cmd / themes       ctrl+g   help        │",
			"│   alt+g     keybindings        ctrl+q   quit        │",
			"│                                                     │",
			"└─────────────────────────────────────────────────────┘"
		};
		int nlines = sizeof(lines) / sizeof(lines[0]);
		int start_y = (g.h > nlines) ? (g.h - nlines) / 2 : 0;

		int r, gr, bl;
		char border_color[32] = {0};
		if (hex_rgb(g.th.cur_ln, &r, &gr, &bl)) {
			snprintf(border_color, sizeof(border_color), "\x1b[38;2;%d;%d;%dm", r, gr, bl);
		} else {
			strcpy(border_color, "\x1b[1;33m");
		}

		for (int y = 0; y < g.h + 2; y++) {
			th_bg(&b);
			int col_written = 0;

			if (y >= start_y && y < start_y + nlines) {
				int lidx = y - start_y;
				int vlen = u8_width(lines[lidx]);
				int pad = (g.w > vlen) ? (g.w - vlen) / 2 : 0;
				for (int p = 0; p < pad; p++) {
					b_add(&b, " ", 1);
					col_written++;
				}

				if (lidx == 0 || lidx == nlines - 1) {
					b_add(&b, border_color, strlen(border_color));
					b_add(&b, lines[lidx], strlen(lines[lidx]));
					th_fg(&b);
				} else {
					// opening border
					b_add(&b, border_color, strlen(border_color));
					b_add(&b, "│", 3);
					th_fg(&b);

					// inside text
					int rawlen = strlen(lines[lidx]);
					if (rawlen > 6) {
						b_add(&b, &lines[lidx][3], rawlen - 6);
					}

					// closing border
					b_add(&b, border_color, strlen(border_color));
					b_add(&b, "│", 3);
					th_fg(&b);
				}
				col_written += vlen;
			}

			while (col_written < g.w) {
				b_add(&b, " ", 1);
				col_written++;
			}
			b_add(&b, "\x1b[K\r\n", 5);
		}

		write(STDOUT_FILENO, b.p, b.l);
		b_free(&b);

		int c = r_key();
		if (c == '\x1b' || c == ('g' & 0x1f) || c == ('q' & 0x1f) || c == '\r' || c == ' ' || c == K_HELP) {
			break;
		}
	}
}

static char *prmpt(const char *prompt_str) {
	size_t bufsize = 128;
	char *buf = malloc(bufsize);
	size_t buflen = 0;
	buf[0] = '\0';

	while (1) {
		msg(prompt_str, buf);
		rfsh();

		int c = r_key();
		if (c == K_DEL || c == K_BACKSPACE || c == 8 || c == 127) {
			if (buflen != 0) buf[--buflen] = '\0';
		} else if (c == '\x1b' || c == ('c' & 0x1f)) {
			msg("save aborted");
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				msg("");
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			if (buflen + 1 >= bufsize) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
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

static void cmd_menu(void) {
	char buf[64] = {0};
	int len = 0;

	while (1) {
		msg("cmd: [1] theme  [2] save  [3] help  [4] quit | :%s", buf);
		rfsh();

		int c = r_key();
		if (c == '\x1b' || c == ('q' & 0x1f)) {
			msg("");
			break;
		} else if (len == 0 && (c == '1' || c == 't')) {
			theme_menu();
			break;
		} else if (len == 0 && (c == '2' || c == 's')) {
			sav();
			break;
		} else if (len == 0 && (c == '3' || c == 'h')) {
			show_help();
			break;
		} else if (len == 0 && (c == '4' || c == 'q')) {
			exit(0);
		} else if (c == '\r') {
			if (!strcmp(buf, "theme") || !strcmp(buf, "t")) {
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
			} else if (len > 0) {
				msg("unknown cmd: %s", buf);
			}
			break;
		} else if (c == K_DEL || c == K_BACKSPACE || c == 8 || c == 127) {
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
	load_cfg(); // restore saved theme if configured

	raw();
	wsz();

	if (argc >= 2) {
		opn(argv[1]);
	}

	msg("ctrl-s: save | ctrl-z/y: undo/redo | ctrl-g: help | ctrl-;: menu");

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

		// help: ctrl+g, alt+g, f1
		case ('g' & 0x1f):
		case K_HELP:
			show_help();
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

		// command menu: ctrl+;, ctrl+p, ctrl+backslash, ctrl+/
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
