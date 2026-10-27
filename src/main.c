#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// editor state
struct s {
	struct termios o;
	int w;
	int h;
};

static struct s g;

static void err(const char *m) {
	perror(m);
	exit(1);
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

	// alt screen
	write(STDOUT_FILENO, "\x1b[?1049h\x1b[H", 11);
}

static void drw(void) {
	write(STDOUT_FILENO, "\x1b[H", 3);
	for (int y = 0; y < g.h; y++) {
		write(STDOUT_FILENO, "~", 1);
		if (y < g.h - 1) {
			write(STDOUT_FILENO, "\r\n", 2);
		}
	}
	write(STDOUT_FILENO, "\x1b[H", 3);
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	if (!isatty(STDIN_FILENO)) {
		err("stdin not a tty");
	}

	raw();
	wsz();
	drw();

	// read until q or ctrl-q
	char c = 0;
	while (1) {
		if (read(STDIN_FILENO, &c, 1) == 1) {
			if (c == 'q' || c == ('q' & 0x1f)) break;
		}
	}

	return 0;
}
