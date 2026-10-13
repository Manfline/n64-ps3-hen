# Salsa64 — fuzzing corto del frontend en el host (ver fuzz.c), con ASan y
# UBSan. Desde la raíz: tools/build.sh -C tests/fuzz run [N=…] [SEED=…]
# Las semillas (semillas.h) las escribe semillas.py.

ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/fuzz
N    ?= 200000
SEED ?= 1

CC     ?= gcc
SAN    ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(ROOT)/src -I$(CURDIR) $(SAN)
SRCS   := $(CURDIR)/fuzz.c $(addprefix $(ROOT)/src/frontend/,cover.c cheats.c namematch.c dlutil.c)

.PHONY: all run clean

all: $(OUT)/salsa64-fuzz

run: $(OUT)/salsa64-fuzz
	ASAN_OPTIONS=detect_leaks=1 $< $(N) $(SEED)

$(OUT)/salsa64-fuzz: $(SRCS) $(CURDIR)/semillas.h $(wildcard $(ROOT)/src/frontend/*.h)
	@mkdir -p $(@D)
	@echo "  CC      $(notdir $@)"
	@$(CC) $(CFLAGS) $(SRCS) -o $@

clean:
	rm -rf $(OUT)
