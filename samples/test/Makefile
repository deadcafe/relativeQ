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
# The test binary links against ../fcache/libfcache.a.
# If the library is out of date, it is rebuilt automatically.
#

TOPDIR    := $(shell cd ../.. && pwd)
FCACHEDIR := $(shell cd ../fcache && pwd)

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

# -O3 for test/bench; -I$(FCACHEDIR) resolves #include "flow_cache.h" etc.
CFLAGS       = -std=gnu11 -O3 -mavx2 -msse4.2 $(WARN) \
               -I$(TOPDIR)/include -I$(FCACHEDIR)

LIBFCACHE    = $(FCACHEDIR)/libfcache.a

TEST_TARGET  = flow_cache_test
TEST_SRC     = flow_cache_test.c
TEST_HDRS    = flow_cache_test_body.h

.PHONY: all clean test bench

all: $(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRC) $(TEST_HDRS) $(LIBFCACHE)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(LIBFCACHE)

# Rebuild library if its sources changed
$(LIBFCACHE):
	$(MAKE) -C $(FCACHEDIR) static

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: $(TEST_TARGET)
	./$(TEST_TARGET) 100000
	./$(TEST_TARGET) 1000000

clean:
	rm -f $(TEST_TARGET) *~ core core.*
