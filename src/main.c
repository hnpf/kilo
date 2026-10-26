#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// editor state
struct s {
	int w;
	int h;
};

static void err(const char *m) {
	perror(m);
	exit(1);
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	if (!isatty(STDIN_FILENO)) {
		err("stdin not a tty");
	}

	return 0;
}
