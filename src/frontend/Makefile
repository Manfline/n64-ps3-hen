# Salsa64 — frontend para PS3: el emulador con presentación por lib/rsxkit.
PROGRAM := salsa64
TITLE   := Salsa64
APPID   := SLSA00064
SRCS    := main.c selftest.c padmap.c

R       := ../..
COMMON  := src/platform/ps3/log.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c))
DATA    := vitest_rom
vitest_rom_FILE := tests/roms/homebrew/salsa64-vitest.z64
RSXKIT  := 1
CFLAGS  := -ffp-contract=off -fno-math-errno
LIBS    := -lrsx -lgcm_sys -lio -lsysutil -lnet -lsysmodule -lrt -llv2 -lm

include $(R)/mk/ps3.mk
