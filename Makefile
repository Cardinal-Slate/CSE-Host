# Makefile — CSE-Host: the self-hosting trampoline over psda programs. Only slate types cross.
#
# Build the deps first:
#   make -C ../CardinalSlate lib && make -C ../CSE-DSA lib && make -C ../CSE-IO lib \
#     && make -C ../CSE-PAR lib && make -C ../CSE-Pool lib && make -C ../CSE-RNS lib

SPINE ?= ../CardinalSlate
DSA   ?= ../CSE-DSA
IO    ?= ../CSE-IO
PAR   ?= ../CSE-PAR
POOL  ?= ../CSE-Pool
RNS   ?= ../CSE-RNS
CC    ?= clang
CFLAGS := -std=c11 -Iinclude -I$(PAR)/include -I$(POOL)/include -I$(RNS)/include -I$(IO)/include -I$(DSA)/include -I$(SPINE)/include -O2 -Wall -Wextra

OUT   := build
HDRS  := include/cse/host.h
SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst src/%.c,$(OUT)/%.o,$(SRCS))

# the floor (RNS) + pool objects are linked directly: their strong split/fold/CRT and grow backends sit
# beside weak defaults, and an archive would resolve the symbol with the weak one and never pull them.
FLOOR := $(RNS)/build/rns.o $(RNS)/build/k_rns_kernel.o $(POOL)/build/pool.o $(POOL)/build/k_arena.o
LIBS  := $(PAR)/build/libcse-par.a $(DSA)/build/libcse-dsa.a

.PHONY: all check clean lib
all: check lib

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/types.stamp: $(HDRS) $(SRCS) | $(OUT)
	@bad=$$(grep -rnE '\b(int|long|short|size_t|unsigned|char|bool|float|double)\b|void[[:space:]]*\*|stdint' include src 2>/dev/null || true); \
	  if [ -n "$$bad" ]; then printf "  %-10s C TYPE FOUND\n" "types:"; printf '%s\n' "$$bad" | sed 's/^/    /'; exit 1; \
	  else printf "  %-10s only slate\n" "types:"; fi; touch $@

$(OUT)/standalone.stamp: $(HDRS) | $(OUT)
	@for h in $(HDRS); do \
	  rel=$${h#include/}; \
	  printf '#include "%s"\nint main(void){return 0;}\n' "$$rel" > $(OUT)/one.c; \
	  $(CC) $(CFLAGS) -fsyntax-only $(OUT)/one.c || exit 1; \
	done; touch $@

$(OUT)/%.o: src/%.c $(HDRS) | $(OUT)
	@$(CC) $(CFLAGS) -c $< -o $@

lib: $(OUT)/libcse-host.a
$(OUT)/libcse-host.a: $(OBJS) | $(OUT)
	@ar rcs $@ $(OBJS)

$(OUT)/test_host: tests/host.c $(OBJS) | $(OUT)
	@$(CC) $(CFLAGS) tests/host.c $(OBJS) $(FLOOR) $(LIBS) -o $@

check: $(OUT)/types.stamp $(OUT)/standalone.stamp $(OUT)/test_host
	@echo "== cse-host =="; out=$$($(OUT)/test_host 2>&1); st=$$?; \
	  if [ $$st -ne 0 ] || printf '%s' "$$out" | grep -q FAIL; then printf '%s\n' "$$out" | sed 's/^/  /'; exit 1; \
	  else printf "  %-10s %s\n" "host:" "$$(printf '%s' "$$out" | tail -1)"; echo "== ALL PASS =="; fi

clean:
	@rm -rf $(OUT)
