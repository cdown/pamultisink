CFLAGS:=-O2 -pedantic -Wall -Wextra -Wwrite-strings -Warray-bounds -Wconversion -Wstrict-prototypes -Werror $(shell pkg-config --cflags libpulse) $(CFLAGS)
CPPFLAGS:=$(CPPFLAGS)
LDFLAGS:=$(shell pkg-config --libs libpulse) $(LDFLAGS)

INSTALL:=install
prefix:=/usr/local
bindir:=$(prefix)/bin

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

all: $(EXECUTABLES)

%: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -c -o $@

%: %.o
	$(CC) $< -o $@ $(LIBS) $(LDFLAGS)

# Noisy clang build that's expected to fail, but can be useful to find corner
# cases.
clang-everything: CC=clang
clang-everything: CFLAGS+=-Weverything -Wno-disabled-macro-expansion -Wno-padded -Wno-unused-macros -Wno-covered-switch-default
clang-everything: all

sanitisers: CFLAGS+=-fsanitize=address -fsanitize=undefined -fanalyzer
sanitisers: debug

debug: CFLAGS+=-Og -ggdb -fno-omit-frame-pointer
debug: all

clang-tidy:
	# DeprecatedOrUnsafeBufferHandling: See https://stackoverflow.com/a/50724865/945780
	clang-tidy pamultisink.c -checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling -- $(CFLAGS) $(LDFLAGS)

install: all
	mkdir -p $(DESTDIR)$(bindir)/
	$(INSTALL) -pt $(DESTDIR)$(bindir)/ $(EXECUTABLES)

clean:
	rm -f $(EXECUTABLES)
