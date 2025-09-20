SUBDIRS := slist list stailq tailq circleq rbtree

.PHONY: all build test clean
all: test

build:
	@for d in $(SUBDIRS); do \
	  echo "[BUILD] $$d"; \
	  $(MAKE) -C $$d; \
	done

test:	build
	@for d in $(SUBDIRS); do \
	  echo "[TEST] $$d"; \
	  $(MAKE) -C $$d test; \
	done

clean:
	@for d in $(SUBDIRS); do \
	  echo "[CLEAN] $$d"; \
	  $(MAKE) -C $$d clean; \
	done
