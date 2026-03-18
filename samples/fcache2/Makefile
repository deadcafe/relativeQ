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
OPTLEVEL    ?= 3
EXTRA_CFLAGS ?=

CFLAGS_BASE  = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include -I$(CURDIR)/include -I$(CURDIR)/src -fPIC \
               $(EXTRA_CFLAGS)

SRCDIR       = src
INCDIR       = include
LIBDIR       = lib

PUB_HDRS     = $(INCDIR)/flow_cache2.h $(INCDIR)/flow4_cache2.h
OBJS         = $(LIBDIR)/flow4.o
LIB_STATIC   = $(LIBDIR)/libfcache2.a
LIB_SHARED   = $(LIBDIR)/libfcache2.so
LIB_SONAME   = libfcache2.so.1

PREFIX       ?= /usr/local

.PHONY: all static shared clean install

all: static shared
static: $(LIB_STATIC)
shared: $(LIB_SHARED)

$(LIBDIR):
	mkdir -p $@

$(LIBDIR)/flow4.o: $(SRCDIR)/flow4.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -c -o $@ $<

$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(LIB_SHARED): $(OBJS)
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -o $@ $^

clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED) *~ core core.*

install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(PREFIX)/lib $(PREFIX)/include/fcache2
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 755 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 $(PUB_HDRS) $(PREFIX)/include/fcache2/
