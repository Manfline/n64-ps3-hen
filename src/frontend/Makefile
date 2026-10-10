# Salsa64 — frontend para PS3: el emulador con presentación por lib/rsxkit.
PROGRAM := salsa64
TITLE   := Salsa64
APPID   := SLSA00064
SRCS    := main.c pacer.c padmap.c replay.c savefile.c screenshot.c selftest.c selftest_gfx.c

R       := ../..
COMMON  := src/platform/ps3/log.c src/platform/ps3/gfx_spu.c src/rdp/rdp.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c $(R)/src/rsp/gfx/*.c))
DATA    := vitest_rom gfxtest_rom
vitest_rom_FILE := tests/roms/homebrew/salsa64-vitest.z64
gfxtest_rom_FILE := tests/roms/homebrew/salsa64-gfxtest.z64
RSXKIT  := 1
SPUKIT  := 1
# HLE gráfica en el SPE: el mismo código de src/rsp/gfx con la E/S por DMA.
SPU_PROGS := gfx
gfx_SRCS  := $(R)/src/spu/gfx/main.c $(R)/src/spu/gfx/io_spu.c \
             $(filter-out %/io_mem.c %/sync.c,$(wildcard $(R)/src/rsp/gfx/*.c))
CFLAGS  := -ffp-contract=off -fno-math-errno
LIBS    := -lrsx -lgcm_sys -lio -lsysutil -lnet -lsysmodule -lrt -llv2 -lm

include $(R)/mk/ps3.mk
