include mk/cc.mk
export CC

TESTDIRS := tests/slist tests/list tests/stailq tests/tailq tests/circleq \
            tests/rbtree tests/hashtbl tests/hashtbl32 tests/hashtbl64
SUBDIRS  := $(TESTDIRS) samples

HTAGS_PORT   ?= 8000
HTAGS_BIND   ?= 127.0.0.1

PREFIX       ?= /usr/local

RIX_PUB_HDRS := $(filter-out %_private.h, $(wildcard include/rix/*.h))

.PHONY: all build test bench clean install htags htags-serve
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

install:
	install -d $(PREFIX)/include/rix $(PREFIX)/lib
	install -m 644 include/librix.h   $(PREFIX)/include/
	install -m 644 $(RIX_PUB_HDRS)   $(PREFIX)/include/rix/
	$(MAKE) -C samples/fcache install PREFIX=$(PREFIX)

htags:
	mkdir -p HTML
	gtags HTML
	htags -aDfnosF -d HTML
	printf '..' > HTML/GTAGSROOT

htags-serve: htags
	@echo "Serving htags HTML at http://$(HTAGS_BIND):$(HTAGS_PORT)/ (no-cache)"
	cd HTML && python3 ../scripts/nohttpserver.py $(HTAGS_PORT) $(HTAGS_BIND)
