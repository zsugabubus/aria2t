#!/usr/bin/make -f
PREFIX ?= /usr/local
MANPREFIX ?= /usr/share/man
INSTALL ?= install
GZIP ?= gzip --best --force
PKGCONFIG ?= pkg-config

TARGETS := aria2watch aria2fs
CFLAGS += -g -O2 -pedantic -Wall -Wextra -Werror -D_DEFAULT_SOURCE

.PHONY: all
all: $(TARGETS)

aria2watch: CFLAGS += -std=c89
aria2watch: PACKAGES := ncursesw

aria2fs: CFLAGS += -std=c99
aria2fs: PACKAGES := fuse3

$(TARGETS): LDLIBS  += $(shell $(PKGCONFIG) --libs $(PACKAGES))
$(TARGETS): LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))
$(TARGETS): %: %.c program.? aria2-rpc.c format.? websocket.? base64.? jeezson/jeezson.? Makefile
	$(CC) -o $@ $(CFLAGS) $(LDLIBS) $(LDFLAGS) $@.c program.c aria2-rpc.c format.c websocket.c base64.c jeezson/jeezson.c

.PHONY: bootstrap
bootstrap:
	git submodule update --init --recursive

.PHONY: install install-%
install: $(patsubst aria2%,install-%,$(TARGETS))


install-%: aria2%
	$(INSTALL) -Ds -t $(DESTDIR)$(PREFIX)/bin $<
	$(INSTALL) -Dm644 -t $(DESTDIR)$(MANPREFIX)/man1 $<.1
	$(GZIP) $(DESTDIR)$(MANPREFIX)/man1/$<.1

.PHONY: uninstall
uninstall:
	rm -f $(patsubst %,$(DESTDIR)$(PREFIX)/%,$(TARGETS)) $(patsubst %,$(DESTDIR)$(MANPREFIX)/man1/%.1.gz,$(TARGETS))

.PHONY: clean
clean:
	rm -f $(TARGET)
