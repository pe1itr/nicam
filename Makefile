CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -lm

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install

all: nicam

nicam: src/nicam_rx/nicam_cli.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

install: nicam
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 nicam "$(DESTDIR)$(BINDIR)/nicam"

clean:
	rm -f nicam
