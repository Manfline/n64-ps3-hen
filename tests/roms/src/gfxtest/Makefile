# SPDX-License-Identifier: GPL-2.0-or-later
# Salsa64 — gfxtest. Se compila dentro de docker/vitest.Dockerfile con ROM=gfxtest
# (tools/build-vitest.sh gfxtest): clang 14 de Debian para MIPS II big-endian (o32; el VR4300 es MIPS III,
# pero LLVM no admite o32 con mips3: `cache` y `eret` van con .set mips3),
# sin bibliotecas. El ROM lo arma nust64 a partir de build/salsa64-gfxtest.elf.
CC      := clang-14
LD      := ld.lld-14
TARGET  := --target=mips-unknown-none-elf -march=mips2 -mabi=32
CFLAGS  := $(TARGET) -O2 -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
           -nostdlib -mno-abicalls -fno-pic -G0 -msoft-float -mno-check-zero-division \
           -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector
ASFLAGS := $(TARGET) -mno-abicalls -fno-pic -G0 -msoft-float

OUT := build
OBJS := $(OUT)/start.o $(OUT)/main.o

all: $(OUT)/salsa64-gfxtest.elf

$(OUT)/%.o: %.c expect.h gbi.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.o: %.S
	@mkdir -p $(OUT)
	$(CC) $(ASFLAGS) -c $< -o $@

$(OUT)/salsa64-gfxtest.elf: $(OBJS) link.ld
	$(LD) -T link.ld --build-id=none -o $@ $(OBJS)

clean:
	rm -rf $(OUT)

.PHONY: all clean
