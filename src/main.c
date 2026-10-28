#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

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

// editor state
struct s {
	struct termios o;
	int cx;
	int cy;
	int w;
	int h;
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
	g.h = ws.ws_row;
}

static void rfsh(void) {
	struct b b = {0};

	// hide cursor during draw
	b_add(&b, "\x1b[?25l", 6);
	b_add(&b, "\x1b[H", 3);

	for (int y = 0; y < g.h; y++) {
		b_add(&b, "~", 1);
		b_add(&b, "\x1b[K", 3);
		if (y < g.h - 1) {
			b_add(&b, "\r\n", 2);
		}
	}

	// move cursor to cx, cy
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", g.cy + 1, g.cx + 1);
	b_add(&b, buf, len);

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
	switch (key) {
	case K_LEFT:
		if (g.cx > 0) g.cx--;
		break;
	case K_RIGHT:
		if (g.cx < g.w - 1) g.cx++;
		break;
	case K_UP:
		if (g.cy > 0) g.cy--;
		break;
	case K_DOWN:
		if (g.cy < g.h - 1) g.cy++;
		break;
	case K_HOME:
		g.cx = 0;
		break;
	case K_END:
		g.cx = g.w - 1;
		break;
	case K_PGUP:
		g.cy = 0;
		break;
	case K_PGDN:
		g.cy = g.h - 1;
		break;
	}
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	if (!isatty(STDIN_FILENO)) {
		err("stdin not a tty");
	}

	raw();
	wsz();

	while (1) {
		rfsh();
		int c = r_key();
		if (c == 'q' || c == ('q' & 0x1f)) break;
		mv(c);
	}

	return 0;
}
