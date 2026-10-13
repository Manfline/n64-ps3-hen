# SPDX-License-Identifier: GPL-2.0-or-later
# Salsa64 — frontend para PS3: el emulador con presentación por lib/rsxkit.
PROGRAM := salsa64
TITLE   := Salsa64
APPID   := SLSA00064
SRCS    := main.c audio_out.c audio_sync.c menu.c osd.c osd_fps.c pacer.c padmap.c pausectl.c replay.c romlist.c savefile.c \
           cheats.c cover.c download.c dlutil.c i18n.c namematch.c screenshot.c selftest.c selftest_audio.c selftest_gfx.c states.c \
           filters/crt_params.c filters/filters.c
# Filtros de la presentación (filters/): fragment programs en Cg compilados
# con cgcomp. Van en fp32: cgcomp pide -fastprecision a Cg, que baja a fp16
# cuentas en píxeles que no lo aguantan.
SHADERS := $(sort $(wildcard filters/*.fcg))
CGCFLAGS := -Wcg,-nofastprecision
FPO_CHECK := 1

R       := ../..
ICON0   := $(R)/assets/ps3/ICON0.PNG
PIC1    := $(R)/assets/ps3/PIC1.PNG
COMMON  := src/platform/ps3/log.c src/platform/ps3/rec_cache.c src/platform/ps3/gfx_spu.c \
           src/platform/ps3/audio_spu.c src/platform/ps3/jpeg_spu.c src/platform/ps3/rsp_spu.c src/rdp/rdp.c src/rdp/targets.c \
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
# intérprete de src/rsp/lle con la E/S por DMA de src/spu/rsp. Tarea JPEG
# (otro SPE): src/rsp/jpeg en 4 carriles (jpeg_v.c) con el bucle de DMA de
# src/spu/jpeg.
SPU_PROGS := gfx audio rsp jpeg
gfx_SRCS  := $(R)/src/spu/gfx/main.c $(R)/src/spu/gfx/io_spu.c \
             $(filter-out %/io_mem.c %/sync.c %/state.c,$(wildcard $(R)/src/rsp/gfx/*.c))
audio_SRCS := $(R)/src/spu/audio/main.c $(R)/src/spu/audio/io_spu.c \
              $(filter-out %/io_mem.c %/sync.c %/selftest.c,$(wildcard $(R)/src/rsp/audio/*.c))
rsp_SRCS   := $(R)/src/spu/rsp/main.c $(R)/src/spu/rsp/io_spu.c \
              $(filter-out %/core.c,$(wildcard $(R)/src/rsp/lle/*.c))
jpeg_SRCS  := $(R)/src/spu/jpeg/main.c $(R)/src/rsp/jpeg/jpeg.c $(R)/src/rsp/jpeg/jpeg_v.c
# Pila del SPE 0 (src/spu/gfx/main.c): el enlace falla si entre la cadena de
# llamadas más honda y el final del programa quedan menos de 1,5 KB. Las
# variantes de Rare se llaman por puntero desde su familia; lo demás que
# llama por puntero es el arranque y la salida de la biblioteca de C.
gfx_STACK_MIN  := 1536
gfx_STACK_ARGS := --edge gfx_dl_f3d:pd_command --edge gfx_dl_f3dex2:cbfd_command \
                  --ignore exit --ignore __call_exitprocs --ignore __do_global_ctors_aux \
                  --ignore __do_global_dtors_aux --ignore register_tm_clones --ignore deregister_tm_clones
CFLAGS  := -ffp-contract=off -fno-math-errno
LIBS    := -lrsx -lgcm_sys -laudio -lio -lsysutil -lhttp -lhttputil -lssl -lnetctl -lnet -lsysmodule -lrt -llv2 -lm
# Caché del recompilador al final del segmento de texto (docs/rec-ppe.md).
LDFLAGS := -T $(abspath $(R))/mk/rec-cache.ld -T lv2.ld

include $(R)/mk/ps3.mk

# Lo que incluyen los fragment programs de los filtros.
$(OBJ)/crt_mask_dots.fpo: filters/crt.cgh
$(OBJ)/crt_cable_decode9.fpo $(OBJ)/crt_cable_decode27.fpo: filters/crt_cable_decode.cgh
$(OBJ)/crt_tube_lines_a4.fpo $(OBJ)/crt_tube_lines_a8.fpo $(OBJ)/crt_tube_lines_b4.fpo \
$(OBJ)/crt_tube_lines_b8.fpo $(OBJ)/crt_trinitron_lines_a.fpo \
$(OBJ)/crt_trinitron_lines_b.fpo: filters/crt_tube_lines.cgh

# El combiner, con -Os en el SPE 0: 5,6 KB menos de código (de 16,8 a 11,2
# KB) que quedan para la pila. Corre una vez por estado de dibujo, no por
# píxel ni por vértice.
$(OBJ)/spu/src/rsp/gfx/combine.c.o: SPU_CFLAGS += -Os
# Los fondos de S2DEX (salas prerrenderizadas de Zelda OoT), también: 0,9 KB
# menos de código y 112 bytes menos de marco en la cadena más honda de la
# pila; se dibujan en unas decenas de tiras por cuadro.
$(OBJ)/spu/src/rsp/gfx/s2dex.c.o: SPU_CFLAGS += -Os
# make no ve un cambio de banderas: que se recompilen si cambia este archivo.
$(OBJ)/spu/src/rsp/gfx/combine.c.o $(OBJ)/spu/src/rsp/gfx/s2dex.c.o: Makefile
