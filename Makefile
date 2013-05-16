PREFIX ?= /usr/local
bindir ?= $(PREFIX)/bin
mandir ?= $(PREFIX)/share/man

CFLAGS ?= -Wall -O3
VERSION?=$(shell (git describe --tus HEAD 2>/dev/null || echo "v0.2.0") | sed 's/^v//')

###############################################################################

override CFLAGS += -DVERSION="\"$(VERSION)\""
override CFLAGS += `pkg-config --cflags jack`
LOADLIBES = `pkg-config --cflags --libs jack` -lm
man1dir   = $(mandir)/man1

###############################################################################

default: all

jack_midi_clock: jack_midi_clock.c

jack_mclk_dump: jack_mclk_dump.c

install-bin: jack_midi_clock
	install -d $(DESTDIR)$(bindir)
	install -m755 jack_midi_clock $(DESTDIR)$(bindir)

install-man: jack_midi_clock.1
	install -d $(DESTDIR)$(man1dir)
	install -m644 jack_midi_clock.1 $(DESTDIR)$(man1dir)

uninstall-bin:
	rm -f $(DESTDIR)$(bindir)/jack_midi_clock
	-rmdir $(DESTDIR)$(bindir)

uninstall-man:
	rm -f $(DESTDIR)$(man1dir)/jack_midi_clock.1
	-rmdir $(DESTDIR)$(man1dir)
	-rmdir $(DESTDIR)$(mandir)

clean:
	rm -f jack_midi_clock

man: jack_midi_clock
	help2man -N -n 'JACK MIDI Beat Clock Generator' -o jack_midi_clock.1 ./jack_midi_clock

all: jack_midi_clock jack_mclk_dump

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

.PHONY: default all man clean install install-bin install-man uninstall uninstall-bin uninstall-man
