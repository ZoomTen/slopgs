# Makefile -- two targets, two platforms, one engine (src/engine/*.c).
#
#   dist/msgs.wasm     freestanding wasm32 module (src/wasm.c + engine)
#   dist/msgs-render    native CLI renderer         (src/cli.c  + engine)
#
# No object files: the engine is small enough (a dozen .c files) that a
# single whole-program invocation per target recompiles everything in well
# under a second, so there is nothing to track dependencies for and nothing
# for `clean` to sweep up but the two binaries themselves.
#
# ponytail: no incremental .o/.d build -- add one if per-file compiles ever
# become slow enough to notice.

CC    ?= gcc
CLANG ?= clang
DIST  := dist

ENGINE_SRCS := $(wildcard src/engine/*.c)
ENGINE_HDRS := $(wildcard src/engine/*.h)
WASM_SRCS   := src/wasm.c $(ENGINE_SRCS)
CLI_SRCS    := src/cli.c  $(ENGINE_SRCS)

# -fvisibility=hidden + --export-dynamic: only symbols wasm.c marks
# __attribute__((visibility("default"))) end up exported.
CFLAGS_WASM  := --target=wasm32 -O2 -nostdlib -fvisibility=hidden -Wall -Wextra -Isrc
LDFLAGS_WASM := -Wl,--no-entry -Wl,--export-dynamic

CFLAGS_NATIVE := -std=c11 -O2 -Wall -Wextra -Isrc

UNIT_SRCS := src/unit.c src/unit_tap.c $(ENGINE_SRCS)

.PHONY: all clean probes test unit

all: $(DIST)/msgs.wasm $(DIST)/msgs-render

$(DIST)/msgs.wasm: $(WASM_SRCS) $(ENGINE_HDRS) | $(DIST)
	$(CLANG) $(CFLAGS_WASM) $(LDFLAGS_WASM) -o $@ $(WASM_SRCS)

$(DIST)/msgs-render: $(CLI_SRCS) $(ENGINE_HDRS) | $(DIST)
	$(CC) $(CFLAGS_NATIVE) -o $@ $(CLI_SRCS) -lm

$(DIST)/msgs-unit: $(UNIT_SRCS) $(ENGINE_HDRS) src/unit_tap.h | $(DIST)
	$(CC) $(CFLAGS_NATIVE) -o $@ $(UNIT_SRCS) -lm

$(DIST):
	mkdir -p $(DIST)

clean:
	rm -f $(DIST)/msgs.wasm $(DIST)/msgs-render $(DIST)/msgs-unit

probes:
	python3 artifacts/make_probes.py

test: $(DIST)/msgs-render
	./$(DIST)/msgs-render --selftest $(DIST)/gm.dls artifacts/probes/01_programs.mid

unit: $(DIST)/msgs-unit
	./$(DIST)/msgs-unit
