#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/Makefile - Top-level delegating Makefile
#
# Targets:
#   all    - build library (static + shared) and test binary (default)
#   static - build libfcache.a only
#   shared - build libfcache.so only
#   test   - build and run functional tests
#   bench  - build and run benchmarks
#   clean  - clean all subdirectories
#   install PREFIX=/path - install library and headers
#

.PHONY: all static shared test bench clean install

all:
	$(MAKE) -C fcache all
	$(MAKE) -C fcache2 all
	$(MAKE) -C test   all

static:
	$(MAKE) -C fcache static
	$(MAKE) -C fcache2 static

shared:
	$(MAKE) -C fcache shared
	$(MAKE) -C fcache2 shared

test: all
	$(MAKE) -C test test

bench: all
	$(MAKE) -C test bench

clean:
	$(MAKE) -C fcache clean
	$(MAKE) -C fcache2 clean
	$(MAKE) -C test   clean

install:
	$(MAKE) -C fcache install PREFIX=$(PREFIX)
	$(MAKE) -C fcache2 install PREFIX=$(PREFIX)
