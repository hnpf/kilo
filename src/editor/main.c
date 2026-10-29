#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "editor.h"

#define KILO_VER "0.1.2"
struct s g;

void err(const char *m) {
	perror(m);
	exit(1);
}

void b_add(struct b *b, const char *s, int len) {
	char *n = realloc(b->p, b->l + len);
	if (!n) return;
	memcpy(n + b->l, s, len);
	b->p = n;
	b->l += len;
}

void b_free(struct b *b) {
	free(b->p);
}

int hex_rgb(const char *hex, int *r, int *g, int *b) {
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

void th_bg(struct b *b) {
	int r, gr, bl;
	if (!strcmp(g.th.bg, "none") || !g.th.bg[0]) {
		b_add(b, "\x1b[49m", 5);
	} else if (hex_rgb(g.th.bg, &r, &gr, &bl)) {
		char cbuf[32];
		int clen = snprintf(cbuf, sizeof(cbuf), "\x1b[48;2;%d;%d;%dm", r, gr, bl);
		b_add(b, cbuf, clen);
	}
}

void th_fg(struct b *b) {
	int r, gr, bl;
	if (hex_rgb(g.th.fg, &r, &gr, &bl)) {
		char cbuf[32];
		int clen = snprintf(cbuf, sizeof(cbuf), "\x1b[38;2;%d;%d;%dm", r, gr, bl);
		b_add(b, cbuf, clen);
	} else {
		b_add(b, "\x1b[39m", 5);
	}
}

static void th_syntax(struct b *b, int style) {
	const char *color = g.th.fg;
	const char *attr = "";
	switch (style) {
	case 1: color = g.th.function; break; // headings
	case 2: color = g.th.fg; break; // punctuation
	case 3: color = g.th.string; break; // literals
	case 4: color = g.th.function; break; // links
	case 5: color = g.th.fg; attr = "3;"; break; // emphasis
	case 6: color = g.th.keyword; attr = "1;"; break; // strong emphasis
	case 7: color = g.th.keyword; break;
	case 8: color = g.th.function; break;
	case 9: color = g.th.function; break;
	case 10: color = g.th.string; break;
	case 11: color = g.th.number; break;
	case 12: color = g.th.comment; break;
	case 13: color = g.th.fg; break;
	case 14: color = g.th.fg; break;
	case 15: color = g.th.function; break;
	}
	int r, gr, bl;
	if (hex_rgb(color, &r, &gr, &bl)) {
		char seq[48];
		int n = snprintf(seq, sizeof(seq), "\x1b[%s38;2;%d;%d;%dm", attr, r, gr, bl);
		b_add(b, seq, n);
	} else {
		th_fg(b);
	}
}

static void rst(void) {
	// reset colors, disable mouse, pop kitty keyboard, reset modifyOtherKeys, exit alt screen, show cursor, restore term
	const char s[] = "\x1b[0m\x1b[49m\x1b[39m\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[<1u\x1b[>4;0m\x1b[?1049l\x1b[?25h";
	write(STDOUT_FILENO, s, sizeof(s) - 1);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &g.o);
}

void wsz(void) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		err("ioctl winsz");
	}
	g.w = ws.ws_col;
	g.h = ws.ws_row - 2; // room for status bar + msg
}

void msg(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g.msg, sizeof(g.msg), fmt, ap);
	va_end(ap);
	g.msg_t = time(NULL);
}

static void snap_free(struct snap *s) {
	free(s->data);
	free(s->r);
	if (s->bytes <= g.history_bytes) g.history_bytes -= s->bytes;
	else g.history_bytes = 0;
	s->r = NULL;
	s->data = NULL;
	s->nr = 0;
	s->bytes = 0;
}

static size_t doc_mem_usage(void) {
	size_t bytes = (size_t)(g.nr > 0 ? g.nr : 1) * sizeof(struct r);
	for (int i = 0; i < g.nr; i++) bytes += (size_t)g.r[i].l + 1;
	return bytes;
}

#define UNDO_BUDGET (64u * 1024u * 1024u)

static void evict_oldest(struct snap *stack, int *len) {
	if (*len <= 0) return;
	snap_free(&stack[0]);
	memmove(stack, stack + 1, sizeof(*stack) * (size_t)(*len - 1));
	(*len)--;
}

static void push_history(struct snap *stack, int *len, struct snap s) {
	while (g.history_bytes + s.bytes > UNDO_BUDGET) {
		if (g.u_len > 0) evict_oldest(g.u_stack, &g.u_len);
		else if (g.r_len > 0) evict_oldest(g.r_stack, &g.r_len);
		else {
			snap_free(&s);
			g.history_disabled = 1;
			return;
		}
	}
	if (*len >= 128) evict_oldest(stack, len);
	stack[(*len)++] = s;
	g.history_bytes += s.bytes;
}

static void snap_clear_redo(void) {
	for (int i = 0; i < g.r_len; i++) snap_free(&g.r_stack[i]);
	g.r_len = 0;
}

static struct snap snap_curr(void) {
	struct snap s = {0};
	s.nr = g.nr;
	s.cx = g.cx;
	s.cy = g.cy;
	s.bytes = (size_t)(g.nr > 0 ? g.nr : 1) * sizeof(struct r);
	size_t text_bytes = 0;
	s.r = malloc(sizeof(struct r) * (g.nr > 0 ? g.nr : 1));
	for (int i = 0; i < g.nr; i++) {
		size_t row_bytes = (size_t)g.r[i].l + 1;
		if (row_bytes > SIZE_MAX - text_bytes) { free(s.r); return (struct snap){0}; }
		text_bytes += row_bytes;
		s.bytes += row_bytes;
	}
	s.data = malloc(text_bytes ? text_bytes : 1);
	if (!s.r || !s.data) {
		free(s.r); free(s.data);
		return (struct snap){0};
	}
	char *text = s.data;
	for (int i = 0; i < g.nr; i++) {
		s.r[i].l = g.r[i].l;
		s.r[i].s = text;
		memcpy(s.r[i].s, g.r[i].s, g.r[i].l);
		s.r[i].s[g.r[i].l] = '\0';
		text += g.r[i].l + 1;
	}
	return s;
}

void snap_save(void) {
	snap_clear_redo();
	size_t bytes = doc_mem_usage();
	if (bytes > UNDO_BUDGET) {
		while (g.u_len > 0) evict_oldest(g.u_stack, &g.u_len);
		g.history_disabled = 1;
		msg("undo history disabled: document exceeds 64 MiB");
		return;
	}
	g.history_disabled = 0;
	struct snap snapshot = snap_curr();
	if (!snapshot.r) { g.history_disabled = 1; return; }
	push_history(g.u_stack, &g.u_len, snapshot);
}

static void snap_restore(struct snap *s) {
	for (int i = 0; i < g.nr; i++) free(g.r[i].s);
	free(g.r);

	g.nr = s->nr;
	g.cx = s->cx;
	g.cy = s->cy;
	g.sx = -1;
	g.sy = -1;
	g.pref_cx = -1;
	g.r = malloc(sizeof(struct r) * (s->nr > 0 ? s->nr : 1));
	g.r_cap = s->nr > 0 ? s->nr : 1;
	for (int i = 0; i < s->nr; i++) {
		g.r[i].l = s->r[i].l;
		g.r[i].s = malloc(s->r[i].l + 1);
		memcpy(g.r[i].s, s->r[i].s, s->r[i].l);
		g.r[i].s[s->r[i].l] = '\0';
	}
	g.dirty = 1;
	g.text_gen++;
}

static void undo(void) {
	if (g.u_len == 0) {
		msg(g.history_disabled ? "undo history disabled for this file" : "already at oldest change");
		return;
	}
	struct snap current = {0};
	if (doc_mem_usage() <= UNDO_BUDGET) current = snap_curr();
	else g.history_disabled = 1;
	struct snap s = g.u_stack[--g.u_len];
	snap_restore(&s);
	snap_free(&s);
	if (current.r) push_history(g.r_stack, &g.r_len, current);
	msg("undo");
}

static void redo(void) {
	if (g.r_len == 0) {
		msg("already at newest change");
		return;
	}
	struct snap current = {0};
	if (doc_mem_usage() <= UNDO_BUDGET) current = snap_curr();
	else g.history_disabled = 1;
	struct snap s = g.r_stack[--g.r_len];
	snap_restore(&s);
	snap_free(&s);
	if (current.r) push_history(g.u_stack, &g.u_len, current);
	msg("redo");
}

void add_r(int at, const char *s, size_t len) {
	if (at < 0 || at > g.nr) return;
	if (g.nr == g.r_cap) {
		int cap = g.r_cap ? g.r_cap * 2 : 16;
		if (cap < g.r_cap || (size_t)cap > SIZE_MAX / sizeof(*g.r)) return;
		struct r *rows = realloc(g.r, sizeof(*g.r) * (size_t)cap);
		if (!rows) return;
		g.r = rows;
		g.r_cap = cap;
	}
	memmove(&g.r[at + 1], &g.r[at], sizeof(struct r) * (g.nr - at));
	g.r[at].l = (int)len;
	g.r[at].s = malloc(len + 1);
	memcpy(g.r[at].s, s, len);
	g.r[at].s[len] = '\0';
	g.nr++;
	g.dirty = 1;
	g.text_gen++;
}

static void del_r(int at) {
	if (at < 0 || at >= g.nr) return;
	free(g.r[at].s);
	memmove(&g.r[at], &g.r[at + 1], sizeof(struct r) * (g.nr - at - 1));
	g.nr--;
	g.dirty = 1;
	g.text_gen++;
}

static void r_ins_c(struct r *row, int at, int c) {
	if (at < 0 || at > row->l) at = row->l;
	row->s = realloc(row->s, row->l + 2);
	memmove(&row->s[at + 1], &row->s[at], row->l - at + 1);
	row->l++;
	row->s[at] = c;
	g.dirty = 1;
	g.text_gen++;
}

static void r_del_c(struct r *row, int at) {
	if (at < 0 || at >= row->l) return;
	memmove(&row->s[at], &row->s[at + 1], row->l - at);
	row->l--;
	g.dirty = 1;
	g.text_gen++;
}

static void r_app_s(struct r *row, const char *s, size_t len) {
	row->s = realloc(row->s, row->l + len + 1);
	memcpy(&row->s[row->l], s, len);
	row->l += len;
	row->s[row->l] = '\0';
	g.dirty = 1;
	g.text_gen++;
}

static void clear_history(void) {
	for (int i = 0; i < g.u_len; i++) snap_free(&g.u_stack[i]);
	for (int i = 0; i < g.r_len; i++) snap_free(&g.r_stack[i]);
	g.u_len = g.r_len = 0;
	g.history_bytes = 0;
	g.history_disabled = 0;
}

int editor_open_file(const char *fn) {
	if (g.dirty) {
		msg("Save changes before opening? y: save, n: discard, esc: cancel");
		rfsh();
		int answer = r_key();
		if (answer == 'y' || answer == 'Y') {
			sav();
			if (g.dirty) return 0;
		} else if (answer != 'n' && answer != 'N') return 0;
	}
	FILE *fp = fopen(fn, "r");
	if (!fp) { msg("open error: %s", strerror(errno)); return 0; }
	clear_history();
	for (int i = 0; i < g.nr; i++) free(g.r[i].s);
	free(g.r);
	g.r = NULL;
	g.nr = g.r_cap = 0;
	free(g.fn);
	g.fn = strdup(fn);
	g.cx = g.cy = g.rowoff = g.coloff = 0;
	g.sx = g.sy = -1;
	g.pref_cx = -1;
	g.search[0] = '\0';
	g.search_on = 0;
	g.in_type = 0;
	g.text_gen++;
	struct stat st;
	if (fstat(fileno(fp), &st) == 0 && st.st_size >= 0) g.file_sz = (size_t)st.st_size;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while ((len = getline(&line, &cap, fp)) != -1) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
		add_r(g.nr, line, (size_t)len);
	}
	free(line);
	fclose(fp);
	g.dirty = 0;
	g.msg[0] = '\0';
	g.msg_t = 0;
	return 1;
}

static char *prmpt(const char *prompt_str);

void sav(void) {
	if (!g.fn) {
		g.fn = prmpt("save as: %s (esc or ctrl-c to cancel)");
		if (!g.fn) return;
	}

	int fd = open(g.fn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) { msg("save error: %s", strerror(errno)); return; }
	FILE *fp = fdopen(fd, "w");
	if (!fp) {
		int saved_errno = errno;
		close(fd);
		msg("save error: %s", strerror(saved_errno));
		return;
	}
	size_t bytes = 0;
	int failed = 0;
	int saved_errno = 0;
	for (int i = 0; i < g.nr && !failed; i++) {
		if (fwrite(g.r[i].s, 1, (size_t)g.r[i].l, fp) != (size_t)g.r[i].l || fputc('\n', fp) == EOF) {
			failed = 1;
			saved_errno = errno ? errno : EIO;
		} else bytes += (size_t)g.r[i].l + 1;
	}
	if (fclose(fp) == EOF && !failed) { failed = 1; saved_errno = errno ? errno : EIO; }
	if (failed) { msg("save error: %s", strerror(saved_errno)); return; }
	g.dirty = 0;
	g.file_sz = bytes;
	msg("%zu bytes written", bytes);
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
	// clamp y2 to valid range
	if (y2 >= g.nr) { y2 = g.nr - 1; x2 = (y2 >= 0 && y2 < g.nr) ? g.r[y2].l : 0; }
	if (y < y1 || y > y2) return 0;
	if (y == y1 && y == y2) return x >= x1 && x < x2;
	if (y == y1) return x >= x1;
	if (y == y2) return x < x2;
	return 1;
}

char *get_sel(void) {
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

void del_sel(void) {
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
	g.text_gen++;
}

void ins_c(int c) {
	if (!g.in_type) {
		snap_save();
		g.in_type = 1;
	}
	if (g.sx != -1) del_sel();
	if (g.cy == g.nr) add_r(g.nr, "", 0);
	if (g.overwrite && g.cx < g.r[g.cy].l) {
		g.r[g.cy].s[g.cx] = c;
		g.dirty = 1;
		g.text_gen++;
	} else {
		r_ins_c(&g.r[g.cy], g.cx, c);
	}
	g.cx++;
}

void ins_nl(void) {
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

int gutter_width(void) {
	int ln_w = 1;
	for (int n = g.nr; n >= 10; n /= 10) ln_w++;
	if (ln_w < 3) ln_w = 3;
	return ln_w + 3 + sidebar_width();
}

// The document stores tabs as one byte, but the terminal displays each tab
// through the next 8-column stop. Keep cursor and horizontal viewport math in
// screen columns so editing tab-indented source stays aligned.
static int row_screen_col(const char *s, int byte_col) {
	int col = 0;
	int limit = (int)strlen(s);
	if (byte_col > limit) byte_col = limit;
	for (int i = 0; i < byte_col; i++) {
		if (s[i] == '\t') col += 8 - (col % 8);
		else col++;
	}
	return col;
}

static int wrap_segment(const char *s, int width, int wanted, int *start, int *end, int *visual_start) {
	int len = (int)strlen(s), i = 0, visual = 0, segment = 0;
	if (width < 1) width = 1;
	while (1) {
		int first = i, first_visual = visual, used = 0;
		int break_i = -1, break_visual = visual;
		while (i < len) {
			int cells = s[i] == '\t' ? 8 - (visual % 8) : 1;
			if (used > 0 && used + cells > width) break;
			if (cells > width) cells = width;
			used += cells;
			visual += s[i] == '\t' ? 8 - (visual % 8) : 1;
			int whitespace = isspace((unsigned char)s[i]);
			i++;
			if (whitespace) { break_i = i; break_visual = visual; }
			if (used >= width) break;
		}
		if (i < len && break_i > first) { i = break_i; visual = break_visual; }
		if (segment == wanted) {
			*start = first; *end = i; *visual_start = first_visual;
			return 1;
		}
		if (i >= len) break;
		segment++;
	}
	return 0;
}

static int wrap_count(const char *s, int width) {
	int start, end, visual, count = 0, len = (int)strlen(s);
	do {
		if (!wrap_segment(s, width, count, &start, &end, &visual)) break;
		count++;
	} while (end < len);
	return count ? count : 1;
}

static int wrapped_total_rows(int width) {
	int total = 0;
	for (int y = 0; y < g.nr; y++) total += wrap_count(g.r[y].s, width);
	return total + 1; // retain the empty cursor row after the last line
}

static void wrapped_locate(int screen_row, int width, int *line, int *segment) {
	if (screen_row < 0) screen_row = 0;
	for (int y = 0; y < g.nr; y++) {
		int count = wrap_count(g.r[y].s, width);
		if (screen_row < count) { *line = y; *segment = screen_row; return; }
		screen_row -= count;
	}
	*line = g.nr;
	*segment = 0;
}

static int wrapped_line_start(int line, int width) {
	int row = 0;
	for (int y = 0; y < line && y < g.nr; y++) row += wrap_count(g.r[y].s, width);
	return row;
}

static int wrapped_cursor_row(int width) {
	int base = wrapped_line_start(g.cy, width);
	if (g.cy >= g.nr) return base;
	int count = wrap_count(g.r[g.cy].s, width);
	for (int segment = 0; segment < count; segment++) {
		int start = 0, end = 0, visual = 0;
		wrap_segment(g.r[g.cy].s, width, segment, &start, &end, &visual);
		if (g.cx < end || (g.cx == end && segment == count - 1)) return base + segment;
	}
	return base + count - 1;
}

static int byte_at_screen_col(const char *s, int visual_col) {
	int col = 0;
	for (int i = 0; s[i]; i++) {
		int cells = s[i] == '\t' ? 8 - (col % 8) : 1;
		if (visual_col < col + cells) return i;
		col += cells;
	}
	return (int)strlen(s);
}

void cursor_from_screen(int screen_row, int screen_col, int width, int gutter) {
	if (!g.word_wrap) {
		g.cy = screen_row + g.rowoff;
		g.cx = screen_col - gutter + g.coloff;
	} else {
		int segment = 0, start = 0, end = 0, visual_start = 0;
		wrapped_locate(screen_row + g.rowoff, width, &g.cy, &segment);
		g.cx = 0;
		if (g.cy < g.nr) {
			wrap_segment(g.r[g.cy].s, width, segment, &start, &end, &visual_start);
			int target = visual_start + screen_col - gutter;
			if (target < visual_start) target = visual_start;
			g.cx = byte_at_screen_col(g.r[g.cy].s, target);
			if (g.cx > end) g.cx = end;
		}
	}
	if (g.cy > g.nr) g.cy = g.nr;
	if (g.cy < g.nr && g.cx > g.r[g.cy].l) g.cx = g.r[g.cy].l;
	g.pref_cx = -1;
}

static void scrl(void) {
	int text_w = g.w - gutter_width();
	if (text_w < 1) text_w = 1;
	if (g.word_wrap) {
		int cursor_row = wrapped_cursor_row(text_w);
		if (cursor_row < g.rowoff) g.rowoff = cursor_row;
		if (cursor_row >= g.rowoff + g.h) g.rowoff = cursor_row - g.h + 1;
		int maxoff = wrapped_total_rows(text_w) - g.h;
		if (maxoff < 0) maxoff = 0;
		if (g.rowoff > maxoff) g.rowoff = maxoff;
		if (g.rowoff < 0) g.rowoff = 0;
		g.coloff = 0;
		return;
	}
	if (g.cy < g.rowoff) {
		g.rowoff = g.cy;
	}
	if (g.cy >= g.rowoff + g.h) {
		g.rowoff = g.cy - g.h + 1;
	}
	if (g.cx < g.coloff) {
		g.coloff = g.cx;
	}
	struct r *row = g.cy < g.nr ? &g.r[g.cy] : NULL;
	if (row) {
		while (g.coloff < g.cx && row_screen_col(row->s, g.cx) - row_screen_col(row->s, g.coloff) >= text_w)
			g.coloff++;
	}
}

void rfsh(void) {
	scrl();
	syntax_prepare();

	struct b b = {0};

	// hide cursor during draw
	b_add(&b, "\x1b[?25l", 6);

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
	const char *find_on = "\x1b[2;4m"; // dim underline for ambient matches

	// gutter width for line numbers
	int margin = gutter_width();
	int side = sidebar_width();
	int gutter = margin - side;
	int ln_w = gutter - 3;
	int text_w = g.w - margin;
	if (text_w < 1) text_w = 1;

	// draw file lines
	for (int y = 0; y < g.h; y++) {
		char rowpos[32];
		int rowlen = snprintf(rowpos, sizeof(rowpos), "\x1b[%d;1H", y + 1);
		b_add(&b, rowpos, rowlen);
		int filerow, segment = 0;
		int segstart = g.coloff, segend = 0, visual_col = 0;
		if (g.word_wrap) {
			wrapped_locate(g.rowoff + y, text_w, &filerow, &segment);
			if (filerow < g.nr) wrap_segment(g.r[filerow].s, text_w, segment, &segstart, &segend, &visual_col);
		} else filerow = y + g.rowoff;
		size_t syntax_off = 0;
		for (int row = 0; row < filerow && row < g.nr; row++)
			syntax_off += (size_t)g.r[row].l + (row + 1 < g.nr);
		int col_written = 0;
		if (side) { sidebar_render_row(&b, y); col_written = side; }

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
			int nlen;
			if (segment == 0) nlen = snprintf(numbuf, sizeof(numbuf), "%*d │ ", ln_w, filerow + 1);
			else nlen = snprintf(numbuf, sizeof(numbuf), "%*s │ ", ln_w, "");
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
					"kilo (kl) editor -- version %s", KILO_VER);
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
			int rowlen = g.r[filerow].l;
			int colstart = g.word_wrap ? segstart : g.coloff;
			int colend = g.word_wrap ? segend : rowlen;
			unsigned char *find_mask = NULL;
			if (g.search_on && g.search[0]) {
				find_mask = calloc((size_t)rowlen, 1);
				if (find_mask) {
					char *p = g.r[filerow].s;
					while ((p = strstr(p, g.search)) != NULL) {
						int at = (int)(p - g.r[filerow].s);
					int qlen = strlen(g.search);
					for (int k = at; k < at + qlen && k < rowlen; k++) find_mask[k] = 1;
						p++;
					}
				}
			}
			int active_style = 0;
			if (!g.word_wrap) visual_col = row_screen_col(g.r[filerow].s, g.coloff);
			for (int col = colstart; col < colend && col_written < g.w; col++) {
				int syn = syntax_style_at(syntax_off + (size_t)col);
				int style = in_sel(filerow, col) ? 2 : (find_mask && find_mask[col] ? 1 : (syn ? 10 + syn : 0));
				if (style != active_style) {
					b_add(&b, "\x1b[0m", 4);
					th_bg(&b);
					th_fg(&b);
					if (style == 2) b_add(&b, sel_on, strlen(sel_on));
					else if (style == 1) b_add(&b, find_on, strlen(find_on));
					else if (style >= 10 && style - 10 <= 15)
						th_syntax(&b, style - 10);
					active_style = style;
				}
				if (g.r[filerow].s[col] == '\t') {
					int cells = 8 - (visual_col % 8);
					int available = g.w - col_written;
					if (cells > available) cells = available;
					for (int k = 0; k < cells; k++) b_add(&b, " ", 1);
					col_written += cells;
					visual_col += 8 - (visual_col % 8);
				} else {
					b_add(&b, &g.r[filerow].s[col], 1);
					col_written++;
					visual_col++;
				}
			}
			if (active_style) {
				b_add(&b, "\x1b[0m", 4);
				th_bg(&b);
				th_fg(&b);
			}
			free(find_mask);
		}
		// pad remainder of row to full screen width under theme background
		th_bg(&b);
		while (col_written < g.w) {
			b_add(&b, " ", 1);
			col_written++;
		}

		b_add(&b, "\x1b[K", 3);
	}

	// status bar (inverted colors / file info)
	char statuspos[32];
	int statusposlen = snprintf(statuspos, sizeof(statuspos), "\x1b[%d;1H", g.h + 1);
	b_add(&b, statuspos, statusposlen);
	b_add(&b, "\x1b[7m", 4);
	char status[128], rstatus[128];
	int len = snprintf(status, sizeof(status), " %.20s%s | %d lines, %zuB | unix (LF) | utf-8",
		g.fn ? g.fn : "[no name]",
		g.dirty ? " *" : "",
		g.nr,
		g.file_sz);
	int rlen = snprintf(rstatus, sizeof(rstatus), "ln %d, col %d [%s] %s %s",
		g.cy + 1, g.cx + 1,
		g.th.name[0] ? g.th.name : "base16",
		g.overwrite ? "OVR" : "INS",
		g.word_wrap ? "WRAP" : "");
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
	b_add(&b, "\x1b[K\x1b[m", 6);

	// status message line (padded with theme bg)
	char msgpos[32];
	int msgposlen = snprintf(msgpos, sizeof(msgpos), "\x1b[%d;1H", g.h + 2);
	b_add(&b, msgpos, msgposlen);
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
	b_add(&b, "\x1b[K", 3);

	// move cursor to cx, cy relative to viewport + gutter
	char buf[32];
	int cx = gutter, cy;
	if (g.word_wrap) {
		int text_w = g.w - gutter;
		if (text_w < 1) text_w = 1;
		int screen_row = wrapped_cursor_row(text_w);
		cy = screen_row - g.rowoff;
		if (g.cy < g.nr) {
			int seg = screen_row - wrapped_line_start(g.cy, text_w);
			int start = 0, end = 0, visual_start = 0;
			wrap_segment(g.r[g.cy].s, text_w, seg, &start, &end, &visual_start);
			cx += row_screen_col(g.r[g.cy].s, g.cx) - visual_start;
		}
	} else {
		if (g.cy < g.nr)
			cx += row_screen_col(g.r[g.cy].s, g.cx) - row_screen_col(g.r[g.cy].s, g.coloff);
		else
			cx += g.cx - g.coloff;
		cy = g.cy - g.rowoff;
	}
	int curlen = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy + 1, cx + 1);
	b_add(&b, buf, curlen);

	// Keep the editor cursor out of sight while keyboard focus is in the sidebar.
	if (!sidebar_has_focus()) b_add(&b, "\x1b[?25h", 6);

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

void mv(int key) {
	struct r *cur = (g.cy < g.nr) ? &g.r[g.cy] : NULL;
	int is_vert = (key == K_UP || key == K_DOWN || key == K_S_UP || key == K_S_DOWN);
	if (g.word_wrap && (is_vert || key == K_PGUP || key == K_PGDN)) {
		int gutter = gutter_width();
		int width = g.w - gutter;
		if (width < 1) width = 1;
		int screen_row = wrapped_cursor_row(width);
		int preferred = 0;
		if (g.cy < g.nr) {
			int segment = screen_row - wrapped_line_start(g.cy, width);
			int start = 0, end = 0, visual_start = 0;
			wrap_segment(g.r[g.cy].s, width, segment, &start, &end, &visual_start);
			preferred = row_screen_col(g.r[g.cy].s, g.cx) - visual_start;
		}
		if (g.pref_cx < 0) g.pref_cx = preferred;
		int delta = (key == K_UP || key == K_S_UP) ? -1 :
			(key == K_DOWN || key == K_S_DOWN) ? 1 :
			(key == K_PGUP ? -g.h : g.h);
		int total = wrapped_total_rows(width);
		int target_row = screen_row + delta;
		if (target_row < 0) target_row = 0;
		if (target_row >= total) target_row = total - 1;
		int line, segment;
		wrapped_locate(target_row, width, &line, &segment);
		g.cy = line;
		g.cx = 0;
		if (line < g.nr) {
			int start = 0, end = 0, visual_start = 0;
			wrap_segment(g.r[line].s, width, segment, &start, &end, &visual_start);
			g.cx = byte_at_screen_col(g.r[line].s, visual_start + g.pref_cx);
			if (g.cx < start) g.cx = start;
			if (g.cx > end) g.cx = end;
		}
		return;
	}

	// horizontal moves reset preferred column
	if (!is_vert) g.pref_cx = -1;

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
		// save preferred column on first vertical move
		if (g.pref_cx < 0) g.pref_cx = g.cx;
		if (g.cy > 0) g.cy--;
		break;
	case K_S_DOWN:
		// save preferred column on first vertical move
		if (g.pref_cx < 0) g.pref_cx = g.cx;
		if (g.cy < g.nr - 1) g.cy++;
		break;
	case K_DOWN:
		// save preferred column on first vertical move
		if (g.pref_cx < 0) g.pref_cx = g.cx;
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
	if (is_vert) {
		// restore preferred column, clamped to new row length
		int target = (g.pref_cx >= 0) ? g.pref_cx : g.cx;
		g.cx = (target > rowlen) ? rowlen : target;
	} else {
		if (g.cx > rowlen) g.cx = rowlen;
	}
}

// Treat the document as one byte stream with newlines between rows. The
// editor's cursor and text operations are byte-based, so these offsets match
// the existing editing model.
static int doc_len(void) {
	int n = 0;
	for (int y = 0; y < g.nr; y++) n += g.r[y].l + (y + 1 < g.nr);
	return n;
}

char *doc_text(void) {
	int n = doc_len();
	char *s = malloc((size_t)n + 1);
	int at = 0;
	for (int y = 0; y < g.nr; y++) {
		memcpy(s + at, g.r[y].s, g.r[y].l);
		at += g.r[y].l;
		if (y + 1 < g.nr) s[at++] = '\n';
	}
	s[at] = '\0';
	return s;
}

static int count_matches(const char *needle) {
	if (!needle[0]) return 0;
	char *text = doc_text();
	int count = 0;
	char *p = text;
	while ((p = strstr(p, needle)) != NULL) {
		count++;
		p += strlen(needle);
	}
	free(text);
	return count;
}

static int cursor_offset(void);
static void offset_cursor(int off);

static int match_ordinal(const char *needle, int start) {
	if (start < 0 || !needle[0]) return 0;
	char *text = doc_text();
	int n = 0;
	char *p = text;
	while ((p = strstr(p, needle)) != NULL && (int)(p - text) <= start) {
		n++;
		p += strlen(needle);
	}
	free(text);
	return n;
}

static void goto_line(void) {
	char *input = prmpt("go to line: %s");
	if (!input) return;
	char *end = NULL;
	long line = strtol(input, &end, 10);
	if (end == input || *end || line < 1 || line > g.nr) {
		msg("line must be between 1 and %d", g.nr);
		free(input);
		return;
	}
	g.cy = (int)line - 1;
	g.cx = 0;
	g.sx = -1;
	g.sy = -1;
	g.pref_cx = -1;
	g.in_type = 0;
	free(input);
}

static void match_bracket(void) {
	char *text = doc_text();
	int len = strlen(text), pos = cursor_offset();
	if (pos >= len || !strchr("()[]{}", text[pos])) {
		if (pos == 0 || !strchr("()[]{}", text[pos - 1])) {
			free(text);
			msg("no bracket at cursor");
			return;
		}
		pos--;
	}
	char open = text[pos], close;
	int direction;
	if (open == '(' || open == '[' || open == '{') {
		close = open == '(' ? ')' : open == '[' ? ']' : '}';
		direction = 1;
	} else {
		close = open;
		open = close == ')' ? '(' : close == ']' ? '[' : '{';
		direction = -1;
	}
	int depth = 1, found = -1;
	for (int i = pos + direction; i >= 0 && i < len; i += direction) {
		if (direction > 0) {
			if (text[i] == open) depth++;
			else if (text[i] == close && --depth == 0) { found = i; break; }
		} else {
			if (text[i] == close) depth++;
			else if (text[i] == open && --depth == 0) { found = i; break; }
		}
	}
	free(text);
	if (found < 0) {
		msg("matching bracket not found");
		return;
	}
	offset_cursor(found);
	g.sx = -1;
	g.sy = -1;
	g.pref_cx = -1;
	g.in_type = 0;
}

static int cursor_offset(void) {
	int n = 0;
	int last = g.cy < g.nr ? g.cy : g.nr;
	for (int y = 0; y < last; y++) n += g.r[y].l + (y + 1 < g.nr);
	if (g.cy < g.nr && g.cx < g.r[g.cy].l) n += g.cx;
	else if (g.cy < g.nr) n += g.r[g.cy].l;
	return n;
}

static void offset_cursor(int off) {
	if (off < 0) off = 0;
	int total = doc_len();
	if (off > total) off = total;
	for (int y = 0; y < g.nr; y++) {
		if (off <= g.r[y].l) {
			g.cy = y;
			g.cx = off;
			return;
		}
		off -= g.r[y].l;
		if (y + 1 < g.nr) {
			if (off == 0) {
				g.cy = y + 1;
				g.cx = 0;
				return;
			}
			off--;
		}
	}
	g.cy = g.nr;
	g.cx = 0;
}

static int is_pair_open(char c) { return c == '(' || c == '[' || c == '{'; }
static char pair_close(char c) { return c == '(' ? ')' : c == '[' ? ']' : '}'; }
static int is_word_byte(int c) { return isalnum((unsigned char)c) || c == '_'; }

// Return the innermost quote/bracket interior containing pos. Cursor bounds
// exclude the delimiters so word motion cannot jump out of the active group.
static int enclosing_bounds(const char *text, int len, int pos, int *lo, int *hi) {
	int *opens = malloc((size_t)(pos + 1) * sizeof(*opens));
	char *types = malloc((size_t)(pos + 1));
	if (!opens || !types) { free(opens); free(types); return 0; }
	int depth = 0;
	for (int i = 0; i < pos && i < len; i++) {
		char c = text[i];
		if (depth && (types[depth - 1] == '\'' || types[depth - 1] == '"' || types[depth - 1] == '`')) {
			char q = types[depth - 1];
			if (c == '\\' && i + 1 < pos) { i++; continue; }
			if (c == q) depth--;
			continue;
		}
		if (c == '\'' || c == '"' || c == '`') {
			opens[depth] = i; types[depth++] = c;
		} else if (is_pair_open(c)) {
			opens[depth] = i; types[depth++] = c;
		} else if ((depth && types[depth - 1] == '(' && c == ')') ||
		           (depth && types[depth - 1] == '[' && c == ']') ||
		           (depth && types[depth - 1] == '{' && c == '}')) {
			depth--;
		}
	}
	if (!depth) { free(opens); free(types); return 0; }
	int start = opens[depth - 1];
	char type = types[depth - 1];
	int end = len;
	if (type == '\'' || type == '"' || type == '`') {
		for (int i = pos; i < len; i++) {
			if (text[i] == '\\' && i + 1 < len) { i++; continue; }
			if (text[i] == type) { end = i; break; }
		}
	} else {
		char close = pair_close(type);
		int nesting = 1;
		char quote = 0;
		for (int i = pos; i < len; i++) {
			char c = text[i];
			if (quote) {
				if (c == '\\' && i + 1 < len) { i++; continue; }
				if (c == quote) quote = 0;
			} else if (c == '\'' || c == '"' || c == '`') quote = c;
			else if (c == type) nesting++;
			else if (c == close && --nesting == 0) { end = i; break; }
		}
	}
	*lo = start + 1;
	*hi = end;
	free(opens); free(types);
	return pos >= *lo && pos <= *hi;
}

static int word_left_offset(int off) {
	char *text = doc_text();
	int len = (int)strlen(text), lo = 0, hi = len;
	if (off > len) off = len;
	int bounded = enclosing_bounds(text, len, off, &lo, &hi);
	int floor = bounded ? lo : 0;
	if (off > floor && isspace((unsigned char)text[off - 1])) {
		while (off > floor && isspace((unsigned char)text[off - 1])) off--;
	} else if (off > floor && is_word_byte((unsigned char)text[off - 1])) {
		while (off > floor && is_word_byte((unsigned char)text[off - 1])) off--;
	} else if (off > floor) off--;
	free(text);
	return off;
}

static int word_right_offset(int off) {
	char *text = doc_text();
	int len = (int)strlen(text), lo = 0, hi = len;
	if (off > len) off = len;
	int bounded = enclosing_bounds(text, len, off, &lo, &hi);
	int ceil = bounded ? hi : len;
	if (off < ceil && isspace((unsigned char)text[off])) {
		while (off < ceil && isspace((unsigned char)text[off])) off++;
	} else if (off < ceil && is_word_byte((unsigned char)text[off])) {
		while (off < ceil && is_word_byte((unsigned char)text[off])) off++;
	} else if (off < ceil) off++;
	free(text);
	return off;
}

int mouse_word_bounds_at_cursor(int *start, int *end, int *clicked) {
	char *text = doc_text();
	int len = (int)strlen(text), pos = cursor_offset();
	if (pos > len) pos = len;
	int word = pos < len && is_word_byte((unsigned char)text[pos]) ? pos :
		(pos > 0 && is_word_byte((unsigned char)text[pos - 1]) ? pos - 1 : -1);
	if (word < 0) { free(text); return 0; }
	int first = word, last = word + 1;
	while (first > 0 && is_word_byte((unsigned char)text[first - 1])) first--;
	while (last < len && is_word_byte((unsigned char)text[last])) last++;
	*start = first;
	*end = last;
	if (clicked) *clicked = pos;
	free(text);
	return 1;
}

void mouse_select_word(int start, int end) {
	offset_cursor(start);
	g.sx = g.cx;
	g.sy = g.cy;
	offset_cursor(end);
	g.pref_cx = -1;
	g.in_type = 0;
}

void mouse_extend_word_selection(int start, int end, int clicked) {
	int target = cursor_offset();
	int word_start, word_end;
	int has_word = mouse_word_bounds_at_cursor(&word_start, &word_end, NULL);
	int anchor = target >= clicked ? start : end;
	int endpoint = target >= clicked ? (has_word ? word_end : target) : (has_word ? word_start : target);
	offset_cursor(anchor);
	g.sx = g.cx;
	g.sy = g.cy;
	offset_cursor(endpoint);
	g.pref_cx = -1;
}

void word_left(void) {
	offset_cursor(word_left_offset(cursor_offset()));
	g.pref_cx = -1;
}

void word_right(void) {
	offset_cursor(word_right_offset(cursor_offset()));
	g.pref_cx = -1;
}

static int find_next(const char *needle, int from, int *start) {
	if (!needle[0]) return 0;
	char *text = doc_text();
	int total = strlen(text);
	if (from < 0) from = 0;
	if (from > total) from = total;
	char *hit = strstr(text + from, needle);
	if (!hit) hit = strstr(text, needle);
	if (!hit) {
		free(text);
		return 0;
	}
	*start = (int)(hit - text);
	free(text);
	return 1;
}

static int find_edge(const char *needle, int last, int *start) {
	if (!needle[0]) return 0;
	char *text = doc_text();
	char *hit = strstr(text, needle);
	if (!hit) {
		free(text);
		return 0;
	}
	if (last) {
		char *next;
		while ((next = strstr(hit + 1, needle)) != NULL) hit = next;
	}
	*start = (int)(hit - text);
	free(text);
	return 1;
}

static void reveal_match(int start, int len);

static void select_match(int start, int len) {
	offset_cursor(start);
	g.sx = g.cx;
	g.sy = g.cy;
	offset_cursor(start + len);
}

static void reveal_match(int start, int len) {
	offset_cursor(start);
	int text_w = g.w - gutter_width();
	if (text_w < 1) text_w = 1;
	if (g.cy < g.nr && len <= text_w) {
		if (g.cx < g.coloff) g.coloff = g.cx;
		if (g.cx + len > g.coloff + text_w) g.coloff = g.cx + len - text_w;
	}
}

static int replace_all(const char *needle, const char *replacement) {
	int count = 0, scan = 0, qlen = strlen(needle), rlen = strlen(replacement);
	if (!qlen) return 0;
	char *text = doc_text();
	char *hit = strstr(text, needle);
	free(text);
	if (!hit) return 0;
	snap_save();
	g.in_type = 1;
	int old_overwrite = g.overwrite;
	g.overwrite = 0;
	while (1) {
		text = doc_text();
		int total = strlen(text);
		char *next = scan <= total ? strstr(text + scan, needle) : NULL;
		int start = next ? (int)(next - text) : -1;
		free(text);
		if (start < 0) break;
		select_match(start, qlen);
		del_sel();
		g.in_type = 1;
		for (int i = 0; i < rlen; i++) {
			if (replacement[i] == '\n') ins_nl();
			else ins_c((unsigned char)replacement[i]);
		}
		g.in_type = 1;
		count++;
		scan = start + rlen;
	}
	g.overwrite = old_overwrite;
	g.in_type = 0;
	g.sx = -1;
	return count;
}

static void search_prompt(void) {
	char find[128] = {0}, replace[128] = {0};
	int flen = 0, rlen = 0, replacing = 0, in_replace = 0;
	int at_match = 0, current_match = -1, history_len = 0;
	int history[256];
	g.in_type = 0;
	g.search_on = 0;
	while (1) {
		if (replacing) {
			msg("Find: %.16s (%d) | Replace: %.16s | Enter: replace", find,
				count_matches(find), replace);
		} else {
			int total = count_matches(find);
			int ordinal = match_ordinal(find, current_match);
			msg("Find: %.16s (%d/%d) Enter next Ctrl+Enter prev", find, ordinal, total);
		}
		rfsh();
		int c = r_key();
		if (c == '\x1b') {
			msg("");
			return;
		} else if (c == ('f' & 0x1f)) {
			msg("");
			return;
		} else if (c == ('r' & 0x1f)) {
			replacing = !replacing;
			in_replace = replacing;
		} else if (c == '\r') {
			if (!find[0]) {
				msg("search text is empty");
				return;
			}
			if (replacing) {
				int count = count_matches(find);
				if (count == 0) {
					msg("no matches to replace");
				} else {
					msg("Replace all %d matches? y/n", count);
					rfsh();
					int confirm = r_key();
					if (confirm == 'y' || confirm == 'Y') {
						int replaced = replace_all(find, replace);
						msg("replaced %d occurrence%s", replaced, replaced == 1 ? "" : "s");
						at_match = 0;
					}
				}
			} else {
				int start;
				int from = cursor_offset();
				if (at_match) from++;
				if (find_next(find, from, &start)) {
					if (at_match && current_match >= 0 && history_len < 256)
						history[history_len++] = current_match;
					reveal_match(start, strlen(find));
					g.sx = -1;
					g.sy = -1;
					g.pref_cx = -1;
					at_match = 1;
					current_match = start;
					msg("match found");
				} else {
					msg("not found: %s", find);
				}
			}
		} else if (!replacing && c == K_CTRL_ENTER) {
			if (history_len > 0) {
				current_match = history[--history_len];
				reveal_match(current_match, strlen(find));
				g.sx = -1;
				g.sy = -1;
				g.pref_cx = -1;
				at_match = 1;
			}
		} else if (!replacing && c == '\n') {
			int start;
			if (find_edge(find, 1, &start)) {
				if (at_match && current_match >= 0 && history_len < 256)
					history[history_len++] = current_match;
				current_match = start;
				reveal_match(start, strlen(find));
				g.sx = -1;
				g.sy = -1;
				g.pref_cx = -1;
				at_match = 1;
			}
		} else if (!replacing && c == K_CTRL_UP) {
			int start;
			if (find_edge(find, 0, &start)) {
				if (at_match && current_match >= 0 && history_len < 256)
					history[history_len++] = current_match;
				current_match = start;
				reveal_match(start, strlen(find));
				g.sx = -1;
				g.sy = -1;
				g.pref_cx = -1;
				at_match = 1;
			}
		} else if (!replacing && c == K_CTRL_DOWN) {
			int start;
			if (find_edge(find, 1, &start)) {
				if (at_match && current_match >= 0 && history_len < 256)
					history[history_len++] = current_match;
				current_match = start;
				reveal_match(start, strlen(find));
				g.sx = -1;
				g.sy = -1;
				g.pref_cx = -1;
				at_match = 1;
			}
		} else if (c == K_BACKSPACE || c == 8 || c == 127 || c == K_DEL) {
			if (in_replace && rlen) replace[--rlen] = '\0';
			else if (flen) find[--flen] = '\0';
			if (!in_replace) {
				at_match = 0;
				current_match = -1;
				history_len = 0;
				snprintf(g.search, sizeof(g.search), "%s", find);
				g.search_on = flen > 0;
			}
		} else if (c >= 32 && c < 127) {
			if (in_replace && rlen < (int)sizeof(replace) - 1) {
				replace[rlen++] = (char)c;
				replace[rlen] = '\0';
			} else if (!in_replace && flen < (int)sizeof(find) - 1) {
				find[flen++] = (char)c;
				find[flen] = '\0';
				snprintf(g.search, sizeof(g.search), "%s", find);
				g.search_on = 1;
				at_match = 0;
				current_match = -1;
				history_len = 0;
			}
		}
	}
}

static void delete_word_left(void) {
	if (g.sx != -1) {
		g.in_type = 0;
		snap_save();
		del_sel();
		return;
	}
	int end = cursor_offset();
	int start = word_left_offset(end);
	if (start == end) return;
	int oldx = g.cx, oldy = g.cy;
	offset_cursor(start);
	int sx = g.cx, sy = g.cy;
	g.cx = oldx;
	g.cy = oldy;
	g.sx = sx;
	g.sy = sy;
	g.in_type = 0;
	snap_save();
	del_sel();
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

int main(int argc, char **argv) {
	if (!isatty(STDIN_FILENO)) {
		err("stdin not a tty");
	}

	g.sx = -1;
	g.sy = -1;
	g.pref_cx = -1;
	g.plugins_enabled = 1;
	load_th("base16_transparent");
	load_cfg(); // restore saved theme if configured

	raw();
	wsz();

	if (argc >= 2) {
		editor_open_file(argv[1]);
	}
	sidebar_init(argc < 2);
	msg("ctrl-s: save | ctrl-z/y: undo/redo | ctrl-g: help | ctrl-;: menu");
	plugins_load();

	while (1) {
		rfsh();
		int c = r_key();
		if (plugin_dispatch(c)) continue;
		if (c >= K_PLUGIN_CTRL_SHIFT_BASE + 'a' && c <= K_PLUGIN_CTRL_SHIFT_BASE + 'z')
			c = c - K_PLUGIN_CTRL_SHIFT_BASE - 'a' + 1;
		else if (c >= K_PLUGIN_CTRL_ALT_BASE + 'a' && c <= K_PLUGIN_CTRL_ALT_BASE + 'z')
			c = c - K_PLUGIN_CTRL_ALT_BASE - 'a' + 1;
		else if (c >= K_PLUGIN_ALT_BASE && c < K_PLUGIN_ALT_BASE + 127)
			c = K_ESC;

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
		if (sidebar_handle_key(c)) continue;

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

		case ('l' & 0x1f):
			goto_line();
			break;

		case 29: // ctrl+]
			match_bracket();
			break;

		// find and optional find/replace: ctrl+f, then ctrl+r
		case ('f' & 0x1f):
			search_prompt();
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
		case K_CTRL_SHIFT_LEFT:
		case K_CTRL_SHIFT_RIGHT:
			g.in_type = 0;
			if (g.sx == -1) {
				g.sx = g.cx;
				g.sy = g.cy;
			}
			if (c == K_CTRL_SHIFT_LEFT) word_left();
			else if (c == K_CTRL_SHIFT_RIGHT) word_right();
			else mv(c);
			break;

		// normal movement clears selection
		case K_CTRL_UP:
		case K_CTRL_PGUP:
			g.in_type = 0;
			g.sx = -1;
			g.sy = -1;
			g.pref_cx = -1;
			g.cy = 0;
			g.cx = 0;
			break;

		case K_CTRL_DOWN:
		case K_CTRL_PGDN:
			g.in_type = 0;
			g.sx = -1;
			g.sy = -1;
			g.pref_cx = -1;
			if (g.nr > 0) {
				g.cy = g.nr - 1;
				g.cx = g.r[g.cy].l;
			} else {
				g.cy = 0;
				g.cx = 0;
			}
			break;

		case K_UP:
		case K_DOWN:
		case K_LEFT:
		case K_RIGHT:
		case K_CTRL_LEFT:
		case K_CTRL_RIGHT:
		case K_HOME:
		case K_END:
		case K_PGUP:
		case K_PGDN:
			g.in_type = 0;
			if (g.sx != -1) {
				int y1, x1, y2, x2;
				sel_bounds(&y1, &x1, &y2, &x2);
				g.sx = -1;
				g.sy = -1;
				if (c == K_LEFT || c == K_CTRL_LEFT) {
					g.cy = y1;
					g.cx = x1;
					break;
				} else if (c == K_RIGHT || c == K_CTRL_RIGHT) {
					g.cy = y2;
					g.cx = x2;
					break;
				} else if (c == K_UP) {
					g.cy = y1;
					g.cx = x1;
					mv(K_UP);
					break;
				} else if (c == K_DOWN) {
					g.cy = y2;
					g.cx = x2;
					mv(K_DOWN);
					break;
				}
			}
			g.sx = -1;
			g.sy = -1;
			if (c == K_CTRL_LEFT) word_left();
			else if (c == K_CTRL_RIGHT) word_right();
			else mv(c);
			break;

		case '\r':
			ins_nl();
			break;

		case K_BACKSPACE:
		case 8:
			del_c();
			break;

		case K_CTRL_BACKSPACE:
		case K_ALT_BACKSPACE:
			delete_word_left();
			break;

		case K_INS:
			g.in_type = 0;
			g.overwrite = !g.overwrite;
			save_cfg();
			msg("%s mode", g.overwrite ? "overwrite" : "insert");
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

	plugins_close();
	return 0;
}
