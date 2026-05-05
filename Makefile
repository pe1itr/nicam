CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -lm

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
NICAM_RX_MODULES := \
	src/nicam_rx/input.c \
	src/nicam_rx/dsp.c \
	src/nicam_rx/demod.c \
	src/nicam_rx/nicam.c \
	src/nicam_rx/output.c

.PHONY: all clean install

all: nicam-rx

nicam-rx: src/nicam_rx/nicam_cli.c $(NICAM_RX_MODULES)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

install: nicam-rx
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 nicam-rx "$(DESTDIR)$(BINDIR)/nicam-rx"

clean:
	rm -f nicam-rx nicam
