#include <errno.h>
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
	K_UP = 1000,
	K_DOWN,
	K_LEFT,
	K_RIGHT,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
	K_DEL
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

// editor state
struct s {
	struct termios o;
	int cx;
	int cy;
	int rowoff;
	int coloff;
	int w;
	int h;
	int nr;
	struct r *r;
	char *fn;
	char msg[80];
	time_t msg_t;
	int dirty;
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
	// exit alt screen, show cursor, restore term
	write(STDOUT_FILENO, "\x1b[?1049l\x1b[?25h", 14);
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

static void add_r(const char *s, size_t len) {
	g.r = realloc(g.r, sizeof(struct r) * (g.nr + 1));
	g.r[g.nr].l = (int)len;
	g.r[g.nr].s = malloc(len + 1);
	memcpy(g.r[g.nr].s, s, len);
	g.r[g.nr].s[len] = '\0';
	g.nr++;
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
		add_r(line, len);
	}
	free(line);
	fclose(fp);
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
			if (len > 0) {
				b_add(&b, &g.r[filerow].s[g.coloff], len);
			}
		}

		b_add(&b, "\x1b[K", 3);
		b_add(&b, "\r\n", 2);
	}

	// status bar (inverted colors)
	b_add(&b, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), " %.20s%s - %d lines",
		g.fn ? g.fn : "[no name]",
		g.dirty ? " *" : "",
		g.nr);
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

	// enter alt screen
	write(STDOUT_FILENO, "\x1b[?1049h\x1b[H", 11);
}

static int r_key(void) {
	int n;
	char c;
	while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
		if (n == -1 && errno != EAGAIN) err("read");
	}

	if (c == '\x1b') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
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
		if (g.cx > 0) {
			g.cx--;
		} else if (g.cy > 0) {
			g.cy--;
			g.cx = g.r[g.cy].l;
		}
		break;
	case K_RIGHT:
		if (cur && g.cx < cur->l) {
			g.cx++;
		} else if (cur && g.cx == cur->l && g.cy + 1 < g.nr) {
			g.cy++;
			g.cx = 0;
		}
		break;
	case K_UP:
		if (g.cy > 0) g.cy--;
		break;
	case K_DOWN:
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

int main(int argc, char **argv) {
	if (!isatty(STDIN_FILENO)) {
		err("stdin not a tty");
	}

	raw();
	wsz();

	if (argc >= 2) {
		opn(argv[1]);
	}

	msg("ctrl-q = quit");

	while (1) {
		rfsh();
		int c = r_key();
		if (c == 'q' || c == ('q' & 0x1f)) break;
		mv(c);
	}

	return 0;
}
