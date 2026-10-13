# Salsa64 — pruebas unitarias del núcleo en el host, con el gcc nativo del
# contenedor (arm64 little-endian). Desde la raíz: tools/build.sh test-host
#
# Por omisión compila con AddressSanitizer y UBSan; SAN= los quita.

ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host

CC      ?= gcc
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno \
           -I$(ROOT)/src -I$(CURDIR) -I$(ROOT)/lib/rsxkit/include $(SAN) $(EXTRA_CFLAGS)
LDFLAGS := $(SAN) $(EXTRA_LDFLAGS)
LDLIBS  := -lm

# lib/rsxkit/src/fpgen.c es el codificador de fragment programs del RSX:
# C puro, sin PSL1GHT, probado aquí con un intérprete del microcódigo.
CORE_SRCS := $(wildcard $(ROOT)/src/core/*.c $(ROOT)/src/cpu/*.c $(ROOT)/src/rsp/gfx/*.c $(ROOT)/src/rsp/audio/*.c $(ROOT)/src/rsp/jpeg/*.c $(ROOT)/src/rsp/lle/*.c) $(ROOT)/src/frontend/padmap.c $(ROOT)/src/frontend/pacer.c \
             $(ROOT)/src/frontend/audio_sync.c $(ROOT)/src/frontend/romlist.c $(ROOT)/src/frontend/menu.c \
             $(ROOT)/src/frontend/osd_fps.c $(ROOT)/src/frontend/pausectl.c $(ROOT)/src/frontend/cheats.c $(ROOT)/src/frontend/cover.c $(ROOT)/src/frontend/namematch.c $(ROOT)/src/frontend/dlutil.c $(ROOT)/src/frontend/i18n.c \
             $(ROOT)/src/frontend/version.c \
             $(ROOT)/src/frontend/filters/crt_params.c \
             $(ROOT)/src/rdp/targets.c $(ROOT)/src/host/raster.c $(ROOT)/lib/rsxkit/src/fpgen.c $(ROOT)/lib/rsxkit/src/text.c \
             $(ROOT)/lib/rsxkit/src/ui_geom.c
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
