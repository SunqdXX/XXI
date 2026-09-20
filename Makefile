CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
PREFIX ?= $(HOME)/.local

all: xxi

xxi: xxi.c
	$(CC) $(CFLAGS) -o $@ xxi.c

install: xxi
	install -Dm755 xxi $(DESTDIR)$(PREFIX)/bin/xxi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/xxi

clean:
	rm -f xxi

.PHONY: all install uninstall clean
