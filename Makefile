#!/usr/bin/make -f
PREFIX ?= /usr/local
MANPREFIX ?= /usr/share/man
INSTALL ?= install
GZIP ?= gzip --best --force
PKGCONFIG ?= pkg-config

TARGETS := aria2-watch aria2-fuse
CFLAGS += -std=c89 -g -O2 -pedantic -Wall -Wextra -Werror

PACKAGES := ncursesw

LDLIBS += $(shell $(PKGCONFIG) --libs $(PACKAGES))
LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))

.PHONY: all
all: $(TARGETS)

$(TARGETS): %: %.c program.h format.? websocket.? base64.? jeezson/jeezson.? Makefile
	$(CC) -o $@ $(CFLAGS) $(LDLIBS) $(LDFLAGS) $@.c format.c websocket.c base64.c jeezson/jeezson.c

.PHONY: bootstrap
bootstrap:
	git submodule update --init --recursive

.PHONY: install install-%
install: $(patsubst aria2-%,install-%,$(TARGETS))

install-%: aria2-%
	$(INSTALL) -Ds -t $(DESTDIR)$(PREFIX)/bin $<
	$(INSTALL) -Dm644 -t $(DESTDIR)$(MANPREFIX)/man1 $<.1
	$(GZIP) $(DESTDIR)$(MANPREFIX)/man1/$<.1

.PHONY: uninstall
uninstall:
	rm -f $(patsubst %,$(DESTDIR)$(PREFIX)/%,$(TARGETS)) $(patsubst %,$(DESTDIR)$(MANPREFIX)/man1/%.1.gz,$(TARGETS))

.PHONY: clean
clean:
	rm -f $(TARGET)
