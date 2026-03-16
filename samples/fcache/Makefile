#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/fcache/Makefile - Build libfcache into lib/
#
# Targets:
#   all    - build both static and shared libraries into lib/ (default)
#   static - build lib/libfcache.a only
#   shared - build lib/libfcache.so only
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
CFLAGS_BASE  = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include -I$(CURDIR)/include -I$(CURDIR)/src -fPIC

SRCDIR       = src
INCDIR       = include
LIBDIR       = lib

WRAPPER_OBJS = $(LIBDIR)/flow4.o $(LIBDIR)/flow6.o $(LIBDIR)/flowu.o
BACKEND_OBJS = $(LIBDIR)/flow4_gen.o $(LIBDIR)/flow4_sse.o \
               $(LIBDIR)/flow4_avx2.o $(LIBDIR)/flow4_avx512.o \
               $(LIBDIR)/flow6_gen.o $(LIBDIR)/flow6_sse.o \
               $(LIBDIR)/flow6_avx2.o $(LIBDIR)/flow6_avx512.o \
               $(LIBDIR)/flowu_gen.o $(LIBDIR)/flowu_sse.o \
               $(LIBDIR)/flowu_avx2.o $(LIBDIR)/flowu_avx512.o
OBJS         = $(WRAPPER_OBJS) $(BACKEND_OBJS)

# Public headers installed alongside the library
PUB_HDRS     = $(INCDIR)/flow_cache.h $(INCDIR)/flow_cache_decl.h \
               $(INCDIR)/flow4_cache.h $(INCDIR)/flow6_cache.h \
               $(INCDIR)/flow_unified_cache.h

LIB_STATIC   = $(LIBDIR)/libfcache.a
LIB_SHARED   = $(LIBDIR)/libfcache.so
LIB_SONAME   = libfcache.so.1

PREFIX       ?= /usr/local

.PHONY: all static shared clean install

all: static shared

static: $(LIB_STATIC)

shared: $(LIB_SHARED)

$(LIB_STATIC): $(OBJS)
	@mkdir -p $(LIBDIR)
	$(AR) rcs $@ $^

$(LIB_SHARED): $(OBJS)
	@mkdir -p $(LIBDIR)
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -o $@ $^

$(LIBDIR):
	mkdir -p $@

$(LIBDIR)/%.o: $(SRCDIR)/%.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) $(CFLAGS_LOCAL) -c -o $@ $<

$(LIBDIR)/flow4.o: CFLAGS_LOCAL =

$(LIBDIR)/flow6.o: CFLAGS_LOCAL =

$(LIBDIR)/flowu.o: CFLAGS_LOCAL =

$(LIBDIR)/flow4_gen.o: $(SRCDIR)/flow4_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -DFLOW_CACHE_BACKEND_NAME=gen -c -o $@ $<
$(LIBDIR)/flow4_sse.o: $(SRCDIR)/flow4_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -msse4.2 -DFLOW_CACHE_BACKEND_NAME=sse -c -o $@ $<
$(LIBDIR)/flow4_avx2.o: $(SRCDIR)/flow4_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx2 -c -o $@ $<
$(LIBDIR)/flow4_avx512.o: $(SRCDIR)/flow4_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx512 -c -o $@ $<

$(LIBDIR)/flow6_gen.o: $(SRCDIR)/flow6_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -DFLOW_CACHE_BACKEND_NAME=gen -c -o $@ $<
$(LIBDIR)/flow6_sse.o: $(SRCDIR)/flow6_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -msse4.2 -DFLOW_CACHE_BACKEND_NAME=sse -c -o $@ $<
$(LIBDIR)/flow6_avx2.o: $(SRCDIR)/flow6_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx2 -c -o $@ $<
$(LIBDIR)/flow6_avx512.o: $(SRCDIR)/flow6_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx512 -c -o $@ $<

$(LIBDIR)/flowu_gen.o: $(SRCDIR)/flowu_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -DFLOW_CACHE_BACKEND_NAME=gen -c -o $@ $<
$(LIBDIR)/flowu_sse.o: $(SRCDIR)/flowu_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -msse4.2 -DFLOW_CACHE_BACKEND_NAME=sse -c -o $@ $<
$(LIBDIR)/flowu_avx2.o: $(SRCDIR)/flowu_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx2 -c -o $@ $<
$(LIBDIR)/flowu_avx512.o: $(SRCDIR)/flowu_backend.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DFLOW_CACHE_BACKEND_NAME=avx512 -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED) *~ core core.*

install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(PREFIX)/lib $(PREFIX)/include/fcache
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 755 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 $(PUB_HDRS)   $(PREFIX)/include/fcache/
