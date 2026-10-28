CC ?= gcc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -pedantic -D_GNU_SOURCE
LDFLAGS ?= -lpthread
PREFIX ?= /usr/local

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
BIN = kilo

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/kilo
	ln -sf $(DESTDIR)$(PREFIX)/bin/kilo $(DESTDIR)$(PREFIX)/bin/kl
	install -d $(DESTDIR)/etc/kilo/themes
	install -m 644 themes/* $(DESTDIR)/etc/kilo/themes/

clean:
	rm -f $(BIN) $(OBJS)

.PHONY: all clean install
