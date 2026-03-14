#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/fcache/Makefile - Build libfcache.a and libfcache.so
#
# Targets:
#   all    - build both static and shared libraries (default)
#   static - build libfcache.a only
#   shared - build libfcache.so only
#   clean  - remove build artifacts
#   install PREFIX=/path - install headers and libraries
#

TOPDIR := $(shell cd ../.. && pwd)

# Detect GCC vs Clang
CC_IS_CLANG := $(shell $(CC) --version 2>&1 | grep -c "clang")

WARN_COMMON  = -Wall -Wextra \
               -Wshadow \
               -Wstrict-prototypes \
               -Wmissing-prototypes \
               -Wcast-qual \
               -Wconversion \
               -Wsign-conversion \
               -Wundef \
               -Wformat=2 \
               -Wnull-dereference \
               -Wdouble-promotion \
               -Wredundant-decls \
               -Werror

ifeq ($(CC_IS_CLANG),0)
WARN_EXTRA   = -Wcast-align=strict -Wstrict-aliasing=2
else
WARN_EXTRA   = -Wcast-align -Wstrict-aliasing
endif

WARN         = $(WARN_COMMON) $(WARN_EXTRA)

# -fPIC: required for .so, negligible cost on x86-64; one object set for both
CFLAGS       = -std=gnu11 -O2 -mavx2 -msse4.2 $(WARN) \
               -I$(TOPDIR)/include -fPIC

SRCS         = flow4_cache.c flow6_cache.c flow_unified_cache.c
OBJS         = $(SRCS:.c=.o)

# Public headers installed alongside the library
PUB_HDRS     = flow_cache.h \
               flow4_cache.h flow6_cache.h flow_unified_cache.h

LIB_STATIC   = libfcache.a
LIB_SHARED   = libfcache.so

PREFIX       ?= /usr/local

.PHONY: all static shared clean install

all: static shared

static: $(LIB_STATIC)

shared: $(LIB_SHARED)

$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(LIB_SHARED): $(OBJS)
	$(CC) -shared -Wl,-soname,$(LIB_SHARED).1 -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED) *~ core core.*

install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(PREFIX)/lib $(PREFIX)/include/fcache
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 755 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 $(PUB_HDRS)   $(PREFIX)/include/fcache/
