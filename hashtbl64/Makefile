#
# Copyright (c) 2025 deadcafe.beef@gmail.com
#

CURDIR := $(PWD)

CFLAGS       = -std=gnu11 -g -O2 -mavx2 -msse4.2 -Wall -Wextra -I$(CURDIR)
BENCH_CFLAGS = -std=gnu11 -O3 -mavx2 -msse4.2 -Wall -Wextra -I$(CURDIR)
DEPENDS      = .depend

TEST_TARGET  = hash64_test
TEST_SRC     = test_rel_hash64.c

BENCH_TARGET = hash64_bench
BENCH_SRC    = bench_rel_hash64.c

.PHONY: all clean depend test bench
all: $(TEST_TARGET) $(BENCH_TARGET)

$(TEST_TARGET): $(TEST_SRC) rel_hash64.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC)

$(BENCH_TARGET): $(BENCH_SRC) rel_hash64.h
	$(CC) $(BENCH_CFLAGS) -o $@ $(BENCH_SRC)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: $(BENCH_TARGET)
	./$(BENCH_TARGET)

clean:
	rm -f $(TEST_TARGET) $(BENCH_TARGET) $(DEPENDS) *~ core core.*

depend: $(TEST_SRC) $(BENCH_SRC) Makefile
	-@ $(CC) $(CFLAGS) -MM -MG $(TEST_SRC) $(BENCH_SRC) > $(DEPENDS)

-include $(DEPENDS)
