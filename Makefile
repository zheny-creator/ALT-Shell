CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall test

all: alt

alt: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: alt
	./tests/smoke.sh

install: alt
	install -Dm755 alt "$(DESTDIR)$(BINDIR)/alt"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/alt"

clean:
	rm -f alt
