CC ?= gcc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -pedantic -D_GNU_SOURCE
LDFLAGS ?= -lpthread
PREFIX ?= /usr/local
LUA_CFLAGS ?= $(shell pkg-config --cflags lua5.4 2>/dev/null || echo -I/usr/include/lua5.4)
LUA_LIBS ?= $(shell pkg-config --libs lua5.4 2>/dev/null || echo -llua5.4)

TS_RUNTIME = vendor/tree-sitter/lib/src/lib.c
TS_MARKDOWN = vendor/tree-sitter-markdown/block/src/parser.c vendor/tree-sitter-markdown/block/src/scanner.c \
	vendor/tree-sitter-markdown/inline/src/parser.c vendor/tree-sitter-markdown/inline/src/scanner.c
TS_LANGUAGES = $(wildcard vendor/tree-sitter-languages/*/src/*.c)
APP_SRCS = $(wildcard src/editor/*.c src/config/*.c src/input/*.c src/plugins/*.c src/syntax/*.c src/ui/*.c)
SRCS = $(APP_SRCS) $(TS_RUNTIME) $(TS_MARKDOWN) $(TS_LANGUAGES)
OBJS = $(SRCS:.c=.o)
BIN = kilo

CFLAGS += $(LUA_CFLAGS) -Isrc -Ivendor/tree-sitter/lib/include -Ivendor/tree-sitter-markdown/block/src -Ivendor/tree-sitter-markdown/inline/src

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LUA_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(APP_SRCS:.c=.o): src/editor.h

vendor/tree-sitter-languages/yaml/src/scanner.o: CFLAGS += -DYAML_SCHEMA=core

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/kilo
	ln -sf $(DESTDIR)$(PREFIX)/bin/kilo $(DESTDIR)$(PREFIX)/bin/kl
	install -d $(DESTDIR)/etc/kilo/themes
	install -m 644 themes/* $(DESTDIR)/etc/kilo/themes/
	install -d $(DESTDIR)$(PREFIX)/share/kilo/queries
	for q in vendor/tree-sitter-languages/*/queries/highlights.scm; do \
		name=$$(basename $$(dirname $$(dirname $$q))); \
		install -m 644 $$q $(DESTDIR)$(PREFIX)/share/kilo/queries/$$name.scm; \
	done

clean:
	rm -f $(BIN) $(OBJS)

.PHONY: all clean install
