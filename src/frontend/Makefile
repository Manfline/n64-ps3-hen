# Salsa64 — frontend para PS3: el emulador con presentación por lib/rsxkit.
PROGRAM := salsa64
TITLE   := Salsa64
APPID   := SLSA00064
SRCS    := main.c audio_out.c audio_sync.c menu.c osd.c osd_fps.c pacer.c padmap.c replay.c romlist.c savefile.c \
           screenshot.c selftest.c selftest_audio.c selftest_gfx.c states.c

R       := ../..
ICON0   := $(R)/assets/ps3/ICON0.PNG
PIC1    := $(R)/assets/ps3/PIC1.PNG
COMMON  := src/platform/ps3/log.c src/platform/ps3/rec_cache.c src/platform/ps3/gfx_spu.c \
           src/platform/ps3/audio_spu.c src/platform/ps3/rsp_spu.c src/rdp/rdp.c src/rdp/targets.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c $(R)/src/rsp/gfx/*.c $(R)/src/rsp/audio/*.c $(R)/src/rsp/jpeg/*.c \
                                          $(R)/src/rsp/lle/*.c))
DATA    := vitest_rom gfxtest_rom audiotest_rom
vitest_rom_FILE := tests/roms/homebrew/salsa64-vitest.z64
gfxtest_rom_FILE := tests/roms/homebrew/salsa64-gfxtest.z64
audiotest_rom_FILE := tests/roms/homebrew/salsa64-audiotest.z64
RSXKIT  := 1
SPUKIT  := 1
# HLE gráfica (SPE 0) y de audio (SPE 1): el mismo código de src/rsp/gfx y
# src/rsp/audio con la E/S por DMA. RSP a bajo nivel (otro SPE): el
# intérprete de src/rsp/lle con la E/S por DMA de src/spu/rsp.
SPU_PROGS := gfx audio rsp
gfx_SRCS  := $(R)/src/spu/gfx/main.c $(R)/src/spu/gfx/io_spu.c \
             $(filter-out %/io_mem.c %/sync.c %/state.c,$(wildcard $(R)/src/rsp/gfx/*.c))
audio_SRCS := $(R)/src/spu/audio/main.c $(R)/src/spu/audio/io_spu.c \
              $(filter-out %/io_mem.c %/sync.c %/selftest.c,$(wildcard $(R)/src/rsp/audio/*.c))
rsp_SRCS   := $(R)/src/spu/rsp/main.c $(R)/src/spu/rsp/io_spu.c \
              $(filter-out %/core.c,$(wildcard $(R)/src/rsp/lle/*.c))
CFLAGS  := -ffp-contract=off -fno-math-errno
LIBS    := -lrsx -lgcm_sys -laudio -lio -lsysutil -lnet -lsysmodule -lsysfs -lrt -llv2 -lm
# Caché del recompilador al final del segmento de texto (docs/rec-ppe.md).
LDFLAGS := -T $(abspath $(R))/mk/rec-cache.ld -T lv2.ld

include $(R)/mk/ps3.mk
