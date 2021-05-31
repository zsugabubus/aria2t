#!/usr/bin/make -f
TARGET := aria2t

prefix      ?= /usr/local
exec_prefix ?= $(prefix)
bindir      ?= $(exec_prefix)/bin
datarootdir ?= $(prefix)/share
mandir      ?= $(datarootdir)/man
man1dir     ?= $(mandir)/man1

INSTALL ?= install
RM ?= rm -f
PKGCONFIG ?= pkg-config

CFLAGS += -std=c11 -g -O2 -pedantic -Wall -Wextra -Wno-override-init

PACKAGES := ncursesw
LDLIBS += -lncursesw
# LDLIBS  += $(shell $(PKGCONFIG) --libs $(PACKAGES))
LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))

VERSION := $(shell git describe --always --tags --dirty --match 'v*')

all : $(TARGET)

keys.in : keys.gen $(TARGET).c
	./$+

$(TARGET).1 : % : manpage.gen %.in $(TARGET).c
	./$+

run : $(TARGET)
	gdb $(TARGET) -ex run

jeezson/% :
	git submodule update --init jeezson

fourmat/% :
	git submodule update --init fourmat

$(TARGET) : %: keys.in %.c program.? websocket.? b64.? jeezson/jeezson.? fourmat/fourmat.? Makefile
	$(CC) -o $@ -DVERSION=\"$(VERSION)\" $(CFLAGS) $(LDFLAGS) $(LDLIBS) $@.c program.c websocket.c b64.c jeezson/jeezson.c fourmat/fourmat.c

installdirs :
	$(INSTALL) -d $(DESTDIR)$(bindir) \
	              $(DESTDIR)$(man1dir)

docs : $(TARGET).1

install : $(TARGET) docs installdirs
	$(INSTALL) $< $(DESTDIR)$(bindir)
	-$(INSTALL) -m 0644 $<.1 $(DESTDIR)$(man1dir)

install-strip :
	$(MAKE) INSTALL='$(INSTALL) -s' install

uninstall :
	$(RM) $(DESTDIR)$(bindir)/$(TARGET)
	$(RM) $(DESTDIR)$(man1dir)/$(TARGET).1*

clean :
	$(RM) keys.in $(TARGET)
	$(MAKE) -C jeezson clean
	$(MAKE) -C fourmat clean

.PHONY: all clean dist docs install install-strip installdirs uninstall run
