CC ?= cc
PREFIX ?= /usr/local
PKG ?= pkg-config

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
FUSE_CFLAGS := $(shell $(PKG) --cflags fuse3)
FUSE_LIBS := $(shell $(PKG) --libs fuse3)

all: endless

endless: src/endless.c src/tape.c src/tape.h
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ src/endless.c src/tape.c $(FUSE_LIBS)

tests/tape_test: tests/tape_test.c src/tape.c src/tape.h
	$(CC) $(CFLAGS) -o $@ tests/tape_test.c src/tape.c -pthread

check: endless tests/tape_test
	./tests/tape_test
	python3 tests/e2e.py

install: endless
	install -D -m 0755 endless $(DESTDIR)$(PREFIX)/bin/endless

clean:
	rm -f endless tests/tape_test

.PHONY: all check install clean
