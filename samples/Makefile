#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#

TOPDIR := $(shell cd .. && pwd)

CFLAGS       = -std=gnu11 -g -O2 -mavx2 -msse4.2 -Wall -Wextra -I$(TOPDIR)
BENCH_CFLAGS = -std=gnu11 -O3 -mavx2 -msse4.2 -Wall -Wextra -I$(TOPDIR)

COMMON_SRC   = flow4_cache.c flow6_cache.c flow_unified_cache.c
COMMON_HDR   = flow4_cache.h flow6_cache.h flow_unified_cache.h \
               flow_cache.h flow_cache_common.h flow_cache_body.h \
               flow_cache_test_body.h

TEST_TARGET  = flow_cache_test
TEST_SRC     = flow_cache_test.c $(COMMON_SRC)

.PHONY: all clean test bench

all: $(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRC) $(COMMON_HDR)
	$(CC) $(BENCH_CFLAGS) -o $@ $(TEST_SRC)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: $(TEST_TARGET)
	./$(TEST_TARGET) 100000
	./$(TEST_TARGET) 1000000

clean:
	rm -f $(TEST_TARGET) *~ core core.*
