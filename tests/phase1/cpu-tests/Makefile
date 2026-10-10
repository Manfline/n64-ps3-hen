PROGRAM := salsa64-cpu-tests
TITLE   := Salsa64 pruebas del CPU
APPID   := SLSA00005
SRCS    := main.c rombench.c

# El núcleo completo y las pruebas compartidas con el host (sin su main).
R       := ../../..
COMMON  := src/platform/ps3/log.c src/platform/ps3/rec_cache.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c)) \
           $(filter-out tests/unit/main_host.c,$(patsubst $(R)/%,%,$(wildcard $(R)/tests/unit/*.c)))
CFLAGS  := -ffp-contract=off -fno-math-errno -I$(CURDIR)/$(R)/tests/unit
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2 -lm
# nemu64-test embebido (homebrew MIT, tests/roms/homebrew/) para rombench.c.
DATA    := nemu64_rom
nemu64_rom_FILE := tests/roms/homebrew/nemu64-test.z64
# Caché del recompilador al final del segmento de texto (docs/rec-ppe.md).
LDFLAGS := -T $(abspath $(R))/mk/rec-cache.ld -T lv2.ld

include $(R)/mk/ps3.mk
