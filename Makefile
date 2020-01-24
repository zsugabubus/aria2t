
INSTALL ?= install
RMDIR ?= rmdir
PKGCONFIG ?= pkg-config
PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib

BUILD ?= debug

CFLAGS += -std=c89 -g -O2 -D_GNU_SOURCE -pedantic -Wall -Wextra -Werror -Wno-unused-function  -Wno-unused-variable -Wno-unused-parameter

PACKAGES := ncursesw

LDLIBS += $(shell $(PKGCONFIG) --libs $(PACKAGES))
LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))

aria2t: main.c format.c websocket.c jeezson/jeezson.c jeezson/jeezson.h Makefile
	$(CC) -o $@ $(CFLAGS) $(LDLIBS) $(LDFLAGS) main.c format.c websocket.c jeezson/jeezson.c

.PHONY: bootstrap
bootstrap:
	git submodule update --init

.PHONY: run
run: aria2t
	./$^

.PHONY: install
install:

.PHONY: uninstall
uninstall:

.PHONY: clean
clean:
