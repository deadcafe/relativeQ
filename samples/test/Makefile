#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/test/Makefile - Build flow cache test/benchmark binary
#
# Targets:
#   all   - build flow_cache_test (default)
#   test  - build and run functional tests
#   bench - build and run benchmarks (two pool sizes)
#   clean - remove build artifacts
#
# The test binary links against ../fcache/lib/libfcache.a.
# If the library is out of date, it is rebuilt automatically.
#

TOPDIR    := $(shell cd ../.. && pwd)
FCACHEDIR := $(shell cd ../fcache && pwd)
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

# -O3 for test/bench; -I$(FCACHEDIR)/include resolves public headers.
CFLAGS_BASE  = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include -I$(FCACHEDIR)/include -I$(FCACHEDIR)/src \
               -DFLOW_CACHE_TEST_CC=\"$(CC)\" \
               -DFLOW_CACHE_TEST_OPTLEVEL=$(OPTLEVEL)
CFLAGS       = $(CFLAGS_BASE) $(SIMD_FLAGS)

LIBFCACHE    = $(FCACHEDIR)/lib/libfcache.a

TEST_TARGET  = flow_cache_test
TEST_SRC     = fcache_test.c
TEST_HDRS    = fcache_test_body.h ht4.h
TEST_OBJS    = ht4_gen.o ht4_sse.o ht4_avx2.o ht4_avx512.o
PERF_SCRIPT  = perf.sh

.PHONY: all clean test bench perf

all: $(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRC) $(TEST_HDRS) $(TEST_OBJS) $(LIBFCACHE)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(TEST_OBJS) $(LIBFCACHE)

ht4_gen.o: ht4_backend.c $(TEST_HDRS)
	$(CC) $(CFLAGS_BASE) -DHT4_BACKEND_NAME=gen -c -o $@ $<

ht4_sse.o: ht4_backend.c $(TEST_HDRS)
	$(CC) $(CFLAGS_BASE) -msse4.2 -DHT4_BACKEND_NAME=sse -c -o $@ $<

ht4_avx2.o: ht4_backend.c $(TEST_HDRS)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DHT4_BACKEND_NAME=avx2 -c -o $@ $<

ht4_avx512.o: ht4_backend.c $(TEST_HDRS)
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DHT4_BACKEND_NAME=avx512 -c -o $@ $<

# Rebuild library if its sources changed (propagate SIMD selection)
$(LIBFCACHE):
	$(MAKE) -C $(FCACHEDIR) static SIMD=$(SIMD)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: $(TEST_TARGET)
	./$(TEST_TARGET) 100000
	./$(TEST_TARGET) 1000000

perf: $(TEST_TARGET) $(PERF_SCRIPT)
	CC=$(CC) OPTLEVEL=$(OPTLEVEL) SIMD=$(SIMD) \
	PERF_CASE="$(PERF_CASE)" PERF_BACKEND="$(PERF_BACKEND)" \
	PERF_EVENTS="$(PERF_EVENTS)" PERF_REPEAT="$(PERF_REPEAT)" \
	PERF_ENTRIES="$(PERF_ENTRIES)" PERF_WARMUP="$(PERF_WARMUP)" \
	PERF_SEED="$(PERF_SEED)" PERF_CPU="$(PERF_CPU)" \
	PERF_STAT_REPEAT="$(PERF_STAT_REPEAT)" \
	./$(PERF_SCRIPT)

clean:
	rm -f $(TEST_TARGET) $(TEST_OBJS) *~ core core.*
