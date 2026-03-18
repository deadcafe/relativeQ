TOPDIR   := $(shell cd ../.. && pwd)
FC2DIR   := $(shell cd ../../samples/fcache2 && pwd)

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
CFLAGS      = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
              -I$(TOPDIR)/include -I$(FC2DIR)/include \
              $(EXTRA_CFLAGS)

TARGET      = fc2_test
BENCH       = fc2_bench
SRC         = test_flow_cache2.c
BENCH_SRC   = bench_flow_cache2.c
LIBFC2      = $(FC2DIR)/lib/libfcache2.a
FCDIR       := $(shell cd ../../samples/fcache && pwd)
LIBFC       = $(FCDIR)/lib/libfcache.a

.PHONY: all test bench matrix clean

all: $(TARGET) $(BENCH)

$(TARGET): $(SRC) $(LIBFC2)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBFC2)

$(BENCH): $(BENCH_SRC) $(LIBFC2) $(LIBFC)
	$(CC) $(CFLAGS) -o $@ $(BENCH_SRC) $(LIBFC2) $(LIBFC)

$(LIBFC2):
	$(MAKE) -C $(FC2DIR) static

$(LIBFC):
	$(MAKE) -C $(FCDIR) static

test: $(TARGET)
	./$(TARGET)

bench: $(BENCH)
	./$(BENCH)

matrix: $(BENCH)
	../../samples/test/run_fc2_bench_matrix.sh flow4

clean:
	rm -f $(TARGET) $(BENCH) *.o *~ core core.*
