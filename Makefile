#!/usr/bin/make -f
PREFIX ?= /usr/local
MANPREFIX ?= /usr/share/man
INSTALL ?= install
GZIP ?= gzip --best --force
PKGCONFIG ?= pkg-config

TARGET := aria2t
CFLAGS += -std=c89 -g -O2 -pedantic -Wall -Wextra -Werror

PACKAGES := ncursesw

LDLIBS += $(shell $(PKGCONFIG) --libs $(PACKAGES))
LDFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))

$(TARGET): main.c format.? websocket.? base64.? jeezson/jeezson.? Makefile
	$(CC) -o $@ $(CFLAGS) $(LDLIBS) $(LDFLAGS) main.c format.c websocket.c base64.c jeezson/jeezson.c

.PHONY: bootstrap
bootstrap:
	git submodule update --init --recursive

.PHONY: run
run: $(TARGET)
	./$^

.PHONY: install
install: $(TARGET)
	$(INSTALL) -Ds $(TARGET) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -Dm644 $(TARGET).1 $(DESTDIR)$(MANPREFIX)/man1/$(TARGET).1
	$(GZIP) $(DESTDIR)$(MANPREFIX)/man1/$(TARGET).1

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/$(TARGET) $(DESTDIR)$(MANPREFIX)/man1/$(TARGET).1.gz

.PHONY: clean
clean:
	rm -f $(TARGET)
