# Salsa64 — pruebas unitarias del núcleo en el host, con el gcc nativo del
# contenedor (arm64 little-endian). Desde la raíz: tools/build.sh test-host
#
# Por omisión compila con AddressSanitizer y UBSan; SAN= los quita.

ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host

CC      ?= gcc
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno \
           -I$(ROOT)/src -I$(CURDIR) $(SAN) $(EXTRA_CFLAGS)
LDFLAGS := $(SAN) $(EXTRA_LDFLAGS)
LDLIBS  := -lm

CORE_SRCS := $(wildcard $(ROOT)/src/core/*.c $(ROOT)/src/cpu/*.c $(ROOT)/src/rsp/gfx/*.c) $(ROOT)/src/frontend/padmap.c
TEST_SRCS := $(wildcard $(CURDIR)/*.c)
OBJS      := $(patsubst $(ROOT)/%.c,$(OUT)/obj/%.o,$(CORE_SRCS) $(TEST_SRCS))

.PHONY: all run clean

all: $(OUT)/salsa64-unit

run: $(OUT)/salsa64-unit
	ASAN_OPTIONS=detect_leaks=0 $<

$(OUT)/salsa64-unit: $(OBJS)
	@echo "  LD      $(notdir $@)"
	@$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OUT)/obj/%.o: $(ROOT)/%.c
	@echo "  CC      $*.c"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
