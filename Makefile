TESTDIRS := tests/slist tests/list tests/stailq tests/tailq tests/circleq \
            tests/rbtree tests/hashtbl tests/hashtbl32 tests/hashtbl64
SUBDIRS  := $(TESTDIRS) samples

.PHONY: all build test bench clean htags
all: test

build:
	@for d in $(SUBDIRS); do \
	  echo "[BUILD] $$d"; \
	  $(MAKE) -C $$d; \
	done

test: build
	@for d in $(SUBDIRS); do \
	  echo "[TEST] $$d"; \
	  $(MAKE) -C $$d test; \
	done

bench: build
	@for d in $(SUBDIRS); do \
	  echo "[BENCH] $$d"; \
	  $(MAKE) -C $$d bench; \
	done

clean:
	@for d in $(SUBDIRS); do \
	  echo "[CLEAN] $$d"; \
	  $(MAKE) -C $$d clean; \
	done
	rm -rf HTML

htags:
	mkdir -p HTML
	gtags HTML
	htags -aDnosF -d HTML
