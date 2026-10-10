# Toolchain (Fase 0)

Imagen `cell64-toolchain:dev`, definida en `docker/Dockerfile` y construida con
`tools/docker-image.sh` sobre el contexto `colima-dev`.

## Versiones fijadas

| Componente | Versión / commit |
|---|---|
| ps3toolchain | `e13aa539c07abe86cdbcd095dc1340492dbca213` (2026-09-13) |
| binutils PPU/SPU | 2.22 (parcheado por ps3toolchain) |
| GCC PPU | 13.2.0 (`powerpc64-ps3-elf`, `--with-cpu=cell`) |
| GCC SPU | 9.5.0 (`spu`) |
| newlib | 1.20.0 |
| PSL1GHT | `f649a08fd536a9e27c08c7db2d93a2d7ee4c3bbe` (2026-06-29) |
| NVIDIA Cg Toolkit | 3.1 abril 2012, x86_64 (sha256 en el Dockerfile) |
| Base | `debian:12-slim` |

`ps3libraries` (zlib, libpng, cairo…) **no** está instalado: se agregará cuando
una fase lo pida.

## Decisiones

- **Toolchain nativo arm64.** binutils/GCC y PSL1GHT se compilan para la
  arquitectura del builder. En la Mac Studio (colima-dev, 4 CPU / 8 GB) la
  imagen completa tarda ~9 min: GCC PPU 4,4 min, GCC SPU 3,2 min.
- **cgcomp en x86_64 bajo qemu-user.** `cgcomp` carga `libCg.so` con
  `dlopen()`, y el Cg Toolkit solo existe para x86_64. Una etapa amd64 compila
  `cgcomp` y junta una raíz mínima (`/opt/cg-amd64`: cargador, libc,
  libstdc++, libCg). `docker/cgcomp-wrapper.sh` la ejecuta con
  `qemu-x86_64-static -L`. En un host x86_64 se ejecuta directo.
- **Makefiles propios** (`mk/ps3.mk`) en vez de los recursivos de los samples:
  objetos en `build/<programa>/obj`, programas SPU y shaders embebidos con
  `bin2s`, headers generados `<archivo>_<ext>.h`.
- La imagen contiene el Cg Toolkit (propietario de NVIDIA): es solo de uso
  local; no publicarla en un registro sin revisar esa licencia.

## ABI del PPU (verificado)

`__LP64__`, `_CALL_ELF=1`: punteros de 64 bits y ABI ELFv1, con descriptores
de función de 24 bytes en `.opd` (entrada, TOC, entorno). El JIT llama al
código generado armando su propio descriptor (`tests/phase0/jit-ppe`).

Segmentos de un ELF de PSL1GHT (`lv2.ld`):

| Dirección | Flags | Secciones |
|---|---|---|
| 0x00010000 | R E | .init .text .fini stubs .rodata.sce* .lib.* .sys_proc_* |
| 0x00050000 | RW | .ctors .dtors .data.sceFStub **.data.rel.ro** .opd .got |
| 0x10000000 | R | .rodata |
| 0x10010000 | RW | .data .bss |

## Validación (`tools/validate-toolchain.sh`, 2026-10-10)

- **Shaders del port de SM64 (rama `ps3`, `b0fb69da`): 54/54 compilan con
  cgcomp.**
- **Samples de PSL1GHT: 18 de 31 compilan.** Las 13 fallas son de los samples,
  no del toolchain:
  - 6 usan el Makefile viejo `$(PS3DEV)/host/ppu.mk`, que ya no existe
    (audiotest, camera, kbtest, padtest, echoserv, networktest).
  - 3 necesitan ps3libraries (cairo, videoTest, ps3load).
  - 2 usan la firma vieja de `rsxInit` (gemtest, sys/save).
  - rsxtest_spu: carrera en su Makefile; compila al segundo intento.
  - **gemsample: ICE de GCC 13.2 PPU con `-Os`** (`rs6000_savres_routine_name`,
    rs6000-logue.cc:2278).

## Problemas conocidos

- **No usar `-Os` en el PPU** hasta aislar el ICE de arriba: dispara las
  rutinas fuera de línea de guardado/restauración de registros. `mk/ps3.mk`
  usa `-O2`.
- binutils 2.22 es de 2011: si algún día falla con instrucciones o
  relocalizaciones nuevas, revisar si ps3toolchain lo actualizó.
