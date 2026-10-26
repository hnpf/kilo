#include <stdio.h>
#include <stdlib.h>
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
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &g.o);
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
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	if (!isatty(STDIN_FILENO)) {
		err("stdin not a tty");
	}

	raw();

	// read until q or ctrl-q
	char c = 0;
	while (1) {
		if (read(STDIN_FILENO, &c, 1) == 1) {
			if (c == 'q' || c == ('q' & 0x1f)) break;
		}
	}

	return 0;
}
