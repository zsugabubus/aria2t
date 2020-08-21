#!/usr/bin/make -f
PREFIX ?= /usr/local
MANPREFIX ?= /usr/share/man
INSTALL ?= install
RM ?= rm -f
GZIP ?= gzip --best --force
PKGCONFIG ?= pkg-config

TARGETS := aria2t
CFLAGS += -g -O2 -pedantic -Wall -Wextra

all : $(TARGETS)

aria2t : CFLAGS += -std=c89
aria2t : PACKAGES := ncursesw

# WATTAFUCK?!
# $(TARGETS) : LDLIBS  += $(shell $(PKGCONFIG) --libs $(PACKAGES))
$(TARGETS) : LDLIBS += -lncursesw
$(TARGETS) : LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))
$(TARGETS) : %: %.c program.? format.? websocket.? b64.? jeezson/jeezson.? Makefile
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $(LDLIBS) $@.c program.c format.c websocket.c b64.c jeezson/jeezson.c

bootstrap :
	git submodule update --init --recursive

install : $(patsubst aria2%,install-%,$(TARGETS))

install-% : aria2%
	$(INSTALL) -Ds -t $(DESTDIR)$(PREFIX)/bin $<
	$(INSTALL) -Dm644 -t $(DESTDIR)$(MANPREFIX)/man1 $<.1
	$(GZIP) $(DESTDIR)$(MANPREFIX)/man1/$<.1

uninstall :
	$(RM) $(patsubst %,$(DESTDIR)$(PREFIX)/%,$(TARGETS)) $(patsubst %,$(DESTDIR)$(MANPREFIX)/man1/%.1.gz,$(TARGETS))

clean :
	$(RM) $(TARGETS)

.PHONY: all bootstrap install install-% uninstall clean
