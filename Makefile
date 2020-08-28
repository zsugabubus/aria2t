#!/usr/bin/make -f
PREFIX ?= /usr/local
MANPREFIX ?= /usr/share/man

INSTALL ?= install
RM ?= rm -f
PKGCONFIG ?= pkg-config

TARGET := aria2t
CFLAGS += -std=c11 -g -O2 -pedantic -Wall -Wextra

PACKAGES := ncursesw
LDLIBS += -lncursesw
# LDLIBS  += $(shell $(PKGCONFIG) --libs $(PACKAGES))
LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))

all : $(TARGET)

keys.in : aria2t.c genkeys
	./genkeys

jeezson/% :
	git submodule update --init jeezson

fourmat/% :
	git submodule update --init fourmat

$(TARGET) : %: keys.in %.c program.? websocket.? b64.? jeezson/jeezson.? fourmat/fourmat.? Makefile
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $(LDLIBS) $@.c program.c websocket.c b64.c jeezson/jeezson.c fourmat/fourmat.c

installdirs :
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin \
	              $(DESTDIR)$(MANPREFIX)/man1

install : aria2t installdirs
	$(INSTALL) $< $(DESTDIR)$(PREFIX)/bin
	-$(INSTALL) -m 0644 $<.1 $(DESTDIR)$(MANPREFIX)/man1

install-strip :
	$(MAKE) INSTALL='$(INSTALL) -s' install

uninstall :
	$(RM) $(patsubst %,$(DESTDIR)$(PREFIX)/%,$(TARGET)) \
	      $(patsubst %,$(DESTDIR)$(MANPREFIX)/man1/%.1.gz,$(TARGET))

clean :
	$(RM) keys.in $(TARGET)

.PHONY: all clean dist install install-strip installdirs uninstall
