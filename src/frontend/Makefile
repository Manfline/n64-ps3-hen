# Salsa64 — frontend para PS3: el emulador con presentación por lib/rsxkit.
PROGRAM := salsa64
TITLE   := Salsa64
APPID   := SLSA00064
SRCS    := main.c audio_out.c audio_sync.c pacer.c padmap.c replay.c savefile.c screenshot.c selftest.c \
           selftest_audio.c selftest_gfx.c

R       := ../..
COMMON  := src/platform/ps3/log.c src/platform/ps3/rec_cache.c src/platform/ps3/gfx_spu.c \
           src/platform/ps3/audio_spu.c src/rdp/rdp.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c $(R)/src/rsp/gfx/*.c $(R)/src/rsp/audio/*.c))
DATA    := vitest_rom gfxtest_rom audiotest_rom
vitest_rom_FILE := tests/roms/homebrew/salsa64-vitest.z64
gfxtest_rom_FILE := tests/roms/homebrew/salsa64-gfxtest.z64
audiotest_rom_FILE := tests/roms/homebrew/salsa64-audiotest.z64
RSXKIT  := 1
SPUKIT  := 1
# HLE gráfica (SPE 0) y de audio (SPE 1): el mismo código de src/rsp/gfx y
# src/rsp/audio con la E/S por DMA.
SPU_PROGS := gfx audio
gfx_SRCS  := $(R)/src/spu/gfx/main.c $(R)/src/spu/gfx/io_spu.c \
             $(filter-out %/io_mem.c %/sync.c,$(wildcard $(R)/src/rsp/gfx/*.c))
audio_SRCS := $(R)/src/spu/audio/main.c $(R)/src/spu/audio/io_spu.c \
              $(filter-out %/io_mem.c %/sync.c,$(wildcard $(R)/src/rsp/audio/*.c))
CFLAGS  := -ffp-contract=off -fno-math-errno
LIBS    := -lrsx -lgcm_sys -laudio -lio -lsysutil -lnet -lsysmodule -lrt -llv2 -lm
# Caché del recompilador al final del segmento de texto (docs/rec-ppe.md).
LDFLAGS := -T $(abspath $(R))/mk/rec-cache.ld -T lv2.ld

include $(R)/mk/ps3.mk
