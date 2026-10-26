cc ?= gcc
cflags ?= -Wall -Wextra -std=c11 -O3 -D_GNU_SOURCE

all: kilo

kilo: src/main.c
	$(cc) $(cflags) $< -o $@

clean:
	rm -f kilo

.PHONY: all clean
