CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall test

all: alt

alt: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: alt
	sh ./tests/smoke.sh

install: alt
	install -Dm755 alt "$(DESTDIR)$(BINDIR)/alt"
	install -Dm644 alt-shell.conf.example "$(DESTDIR)$(PREFIX)/share/doc/alt-shell/alt-shell.conf.example"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/alt"
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/alt-shell/alt-shell.conf.example"

clean:
	rm -f alt
