# Retro64 — ejecutor de línea de comandos para el host (retro64-run), con el
# gcc nativo del contenedor. Desde la raíz: tools/build.sh host
#
# Por omisión compila con AddressSanitizer y UBSan, como las pruebas
# unitarias; SAN= los quita (más rápido).

ROOT := $(abspath ../..)
OUT  := $(ROOT)/build/host

CC      ?= gcc
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno \
           -I$(ROOT)/src $(SAN)
LDFLAGS := $(SAN)
LDLIBS  := -lm

CORE_SRCS := $(wildcard $(ROOT)/src/core/*.c $(ROOT)/src/cpu/*.c)
HOST_SRCS := $(wildcard $(CURDIR)/*.c)
OBJS      := $(patsubst $(ROOT)/%.c,$(OUT)/obj/%.o,$(CORE_SRCS) $(HOST_SRCS))

.PHONY: all clean

all: $(OUT)/retro64-run

$(OUT)/retro64-run: $(OBJS)
	@echo "  LD      $(notdir $@)"
	@$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OUT)/obj/%.o: $(ROOT)/%.c
	@echo "  CC      $*.c"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OUT)/retro64-run

-include $(OBJS:.o=.d)
