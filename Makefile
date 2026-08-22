# Makefile — CSE-Host: the execution driver (trampoline). Iterate a step to a fixed point. Depends on the
# spine only — it knows nothing about what it drives. The step is supplied by the caller.
#
# Build the deps first: make -C ../CardinalSlate lib

SPINE ?= ../CardinalSlate
CC    ?= clang
CFLAGS := -std=c11 -Iinclude -I$(SPINE)/include -O2 -Wall -Wextra

OUT     := build
HDRS    := include/cse/host.h
SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,$(OUT)/%.o,$(SRCS))

.PHONY: all check clean lib
all: check lib

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/types.stamp: $(HDRS) $(SRCS) | $(OUT)
	@bad=$$(grep -rnE '\b(int|long|short|size_t|unsigned|char|bool|float|double)\b|void[[:space:]]*\*|stdint' include src 2>/dev/null || true); \
	  if [ -n "$$bad" ]; then printf "  %-10s C TYPE FOUND\n" "host:"; printf '%s\n' "$$bad" | sed 's/^/    /'; exit 1; \
	  else printf "  %-10s only slate (the driver; step is opaque)\n" "host:"; fi; touch $@

$(OUT)/standalone.stamp: $(HDRS) | $(OUT)
	@for h in $(HDRS); do rel=$${h#include/}; printf '#include "%s"\nint main(void){return 0;}\n' "$$rel" > $(OUT)/one.c; \
	  $(CC) $(CFLAGS) -fsyntax-only $(OUT)/one.c || exit 1; done; touch $@

$(OUT)/%.o: src/%.c $(HDRS) | $(OUT)
	@$(CC) $(CFLAGS) -c $< -o $@

lib: $(OUT)/libcse-host.a
$(OUT)/libcse-host.a: $(OBJS) | $(OUT)
	@ar rcs $@ $(OBJS)

$(OUT)/test_host: tests/host.c $(OBJS) | $(OUT)
	@$(CC) $(CFLAGS) tests/host.c $(OBJS) $(SPINE)/build/libslate.a -o $@

check: $(OUT)/types.stamp $(OUT)/standalone.stamp $(OUT)/test_host
	@echo "== cse-host =="; out=$$($(OUT)/test_host 2>&1); st=$$?; \
	  if [ $$st -ne 0 ] || printf '%s' "$$out" | grep -q FAIL; then printf '%s\n' "$$out" | sed 's/^/  /'; exit 1; \
	  else printf '%s\n' "$$out" | sed 's/^/  /'; echo "== ALL PASS =="; fi

clean:
	@rm -rf $(OUT)
