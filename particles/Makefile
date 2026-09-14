PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PKGS   := cairo x11 xext xfixes xrender xrandr
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

all: particles

particles: particles_wall.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: particles
	install -Dm755 particles $(DESTDIR)$(BINDIR)/particles

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/particles

clean:
	rm -f particles

.PHONY: all install uninstall clean
