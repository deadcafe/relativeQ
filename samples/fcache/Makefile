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
include $(TOPDIR)/mk/simd.mk

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
OPTLEVEL    ?= 3

# -fPIC: required for .so, negligible cost on x86-64; one object set for both
CFLAGS_BASE  = -std=gnu11 -O$(OPTLEVEL) $(WARN) -I$(TOPDIR)/include -fPIC

WRAPPER_SRCS = flow4_cache.c flow6_cache.c flow_unified_cache.c
WRAPPER_OBJS = $(WRAPPER_SRCS:.c=.o)
BACKEND_OBJS = flow4_cache_gen.o flow4_cache_sse.o flow4_cache_avx2.o flow4_cache_avx512.o \
               flow6_cache_gen.o flow6_cache_sse.o flow6_cache_avx2.o flow6_cache_avx512.o \
               flow_unified_cache_gen.o flow_unified_cache_sse.o \
               flow_unified_cache_avx2.o flow_unified_cache_avx512.o
OBJS         = $(WRAPPER_OBJS) $(BACKEND_OBJS)

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
	$(CC) $(CFLAGS_BASE) $(CFLAGS_LOCAL) -c -o $@ $<

flow4_cache.o: CFLAGS_LOCAL =

flow6_cache.o: CFLAGS_LOCAL =

flow_unified_cache.o: CFLAGS_LOCAL =

flow4_cache_gen.o: flow4_cache_backend.c
	$(CC) $(CFLAGS_BASE) -DFLOW_CACHE_BACKEND_NAME=gen -c -o $@ $<
flow4_cache_sse.o: flow4_cache_backend.c
	$(CC) $(CFLAGS_BASE) -msse4.2 -DFLOW_CACHE_BACKEND_NAME=sse -c -o $@ $<
flow4_cache_avx2.o: flow4_cache_backend.c
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx2 -c -o $@ $<
flow4_cache_avx512.o: flow4_cache_backend.c
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx512 -c -o $@ $<

flow6_cache_gen.o: flow6_cache_backend.c
	$(CC) $(CFLAGS_BASE) -DFLOW_CACHE_BACKEND_NAME=gen -c -o $@ $<
flow6_cache_sse.o: flow6_cache_backend.c
	$(CC) $(CFLAGS_BASE) -msse4.2 -DFLOW_CACHE_BACKEND_NAME=sse -c -o $@ $<
flow6_cache_avx2.o: flow6_cache_backend.c
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx2 -c -o $@ $<
flow6_cache_avx512.o: flow6_cache_backend.c
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx512 -c -o $@ $<

flow_unified_cache_gen.o: flow_unified_cache_backend.c
	$(CC) $(CFLAGS_BASE) -DFLOW_CACHE_BACKEND_NAME=gen -c -o $@ $<
flow_unified_cache_sse.o: flow_unified_cache_backend.c
	$(CC) $(CFLAGS_BASE) -msse4.2 -DFLOW_CACHE_BACKEND_NAME=sse -c -o $@ $<
flow_unified_cache_avx2.o: flow_unified_cache_backend.c
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx2 -c -o $@ $<
flow_unified_cache_avx512.o: flow_unified_cache_backend.c
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx512 -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED) *~ core core.*

install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(PREFIX)/lib $(PREFIX)/include/fcache
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 755 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 $(PUB_HDRS)   $(PREFIX)/include/fcache/
