#!/usr/bin/make -f
PREFIX ?= /usr/local
MANPREFIX ?= /usr/share/man
INSTALL ?= install
RM ?= rm -f
GZIP ?= gzip --best --force
PKGCONFIG ?= pkg-config

TARGETS := aria2t
CFLAGS += -std=c11 -g -O2 -pedantic -Wall -Wextra

PACKAGES := ncursesw
LDLIBS += -lncursesw
# LDLIBS  += $(shell $(PKGCONFIG) --libs $(PACKAGES))
LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))

all : $(TARGETS)

keys.in : aria2t.c genkeys
	./genkeys

jeezson/% :
	git submodule update --init jeezson

fourmat/% :
	git submodule update --init fourmat

$(TARGETS) : %: keys.in %.c program.? websocket.? b64.? jeezson/jeezson.? fourmat/fourmat.? Makefile
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $(LDLIBS) $@.c program.c websocket.c b64.c jeezson/jeezson.c fourmat/fourmat.c

install : aria2t
	$(INSTALL) -Ds -t $(DESTDIR)$(PREFIX)/bin $<
	$(INSTALL) -Dm644 -t $(DESTDIR)$(MANPREFIX)/man1 $<.1
	$(GZIP) $(DESTDIR)$(MANPREFIX)/man1/$<.1

uninstall :
	$(RM) $(patsubst %,$(DESTDIR)$(PREFIX)/%,$(TARGETS)) $(patsubst %,$(DESTDIR)$(MANPREFIX)/man1/%.1.gz,$(TARGETS))

clean :
	$(RM) *.in $(TARGETS)

.PHONY: all bootstrap install uninstall clean
