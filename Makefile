#!/usr/bin/make -f
PREFIX ?= /usr/local
MANPREFIX ?= /usr/share/man
INSTALL ?= install
GZIP ?= gzip
PKGCONFIG ?= pkg-config

TARGET := aria2t
CFLAGS += -std=c89 -g -O2 -pedantic -Wall -Wextra -Werror

PACKAGES := ncursesw

LDLIBS += $(shell $(PKGCONFIG) --libs $(PACKAGES))
LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))

$(TARGET): main.c format.c websocket.c jeezson/jeezson.c jeezson/jeezson.h Makefile
	$(CC) -o $@ $(CFLAGS) $(LDLIBS) $(LDFLAGS) main.c format.c websocket.c jeezson/jeezson.c

.PHONY: bootstrap
bootstrap:
	git submodule update --init

.PHONY: run
run: $(TARGET)
	./$^

.PHONY: install
install: $(TARGET)
	$(INSTALL) -Ds $(TARGET) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -D $(TARGET).1 $(DESTDIR)$(MANPREFIX)/man1

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/$(TARGET) $(DESTDIR)$(MANPREFIX)/man1/$(TARGET).1

.PHONY: clean
clean:
	rm -f $(TARGET)
