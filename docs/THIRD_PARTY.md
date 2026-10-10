# Código y herramientas de terceros

## Código incluido o adaptado en el repositorio

| Archivo | Origen | Licencia | Notas |
|---|---|---|---|
| `tests/phase0/rsx-triangle/main.c` | Sample `samples/graphics/rsxtest` de PSL1GHT (`f649a08f`) | MIT | Inicialización del RSX, superficie, viewport y flip reescritos en C a partir de `rsxutil.cpp`/`main.cpp` |
| `tests/phase0/spu-compute/spu/mix.c` | Patrón DMA del sample `samples/spu/spudma` de PSL1GHT | MIT | Solo el patrón `mfc_get`/espera por etiqueta; el cálculo y el doble búfer son propios |

El núcleo de la Fase 1 (`src/core/`, `src/cpu/`) es código propio, escrito
desde documentación pública (manual del VR4300 de NEC, MIPS IV ISA,
n64brew.dev). No incluye código de Not64/Wii64, Mupen64Plus ni ares.

Aviso de PSL1GHT: Copyright (c) 2011 PSL1GHT Development Team, licencia MIT
(compatible con GPLv2 o posterior).

`lib/rsxkit/` es MIT, no GPL: solo admite código propio o de fuentes con
licencia permisiva (MIT, BSD, zlib). Nada de Not64/Wii64, Mupen64Plus,
RPCS3, ares ni del port de SM64. Sus orígenes se registran en la tabla de
arriba como el resto.

## Herramientas usadas para compilar (no se redistribuyen en el repo)

| Componente | Origen | Licencia | Uso |
|---|---|---|---|
| ps3toolchain | github.com/ps3dev/ps3toolchain | BSD-2-Clause (scripts); GCC/binutils GPL; newlib varias | Compiladores PPU/SPU dentro de la imagen Docker |
| PSL1GHT | github.com/ps3dev/PSL1GHT | MIT | SDK homebrew, enlazado con los programas |
| NVIDIA Cg Toolkit 3.1 | developer.nvidia.com/cg-toolkit | Propietaria (NVIDIA), gratuita | `libCg.so` para `cgcomp`, solo en la imagen local; no se commitea ni se publica |
| Port de SM64, rama `ps3` | github.com/fgsfdsfgs/sm64-port | — | Solo para validar `cgcomp`: los shaders se descargan a `build/validate/` y no se copian al repo |

## Suites de prueba de CPU (elegidas; aún no incluidas)

Licencias verificadas el 2026-10-10 con la API de GitHub (campo `license`)
y revisando los repositorios. Ninguna ROM está en el repo todavía: entrarán
a `tests/roms/homebrew/` cuando exista el arranque HLE para correrlas.

| Suite | Commit revisado | Licencia | ¿Contiene IPL3 de Nintendo? | Decisión |
|---|---|---|---|---|
| nemu64-test (github.com/thelemmy/nemu64-test; sucesor de lemmy-64/n64-systemtest) | `9a8b9f7d` (2026-09-08) | MIT | No: usa el IPL3 abierto de libdragon (Unlicense) que trae nust64 0.4.1 (MIT) | **Principal.** Cubre ALU, saltos, LL/SC, COP0, COP1 (modo FR), TLB, excepciones, y también MI/PI/SP/PIF/RSP/RDP. No hay releases: se compila desde fuente (Rust `nightly-2026-07-16` + `nust64` 0.4.1). Reporta en pantalla e ISViewer opcional. Depende del estado que deja el IPL3 en DMEM (tamaño de RAM en DMEM[0], tipo de TV en DMEM[9], offset del ELF en DMEM[12]): el HLE de boot debe reproducirlo. |
| CPUTest de krom (github.com/PeterLemon/N64) | `7085543e` (2026-02-01) | Unlicense (desde 2025-09-27) | **Sí.** Cada ROM y `LIB/N64_BOOTCODE.BIN` contienen el IPL3 CIC-6102 de Nintendo; `BOOTCODE/BOOTCODE.asm` es su desensamblado | **Secundaria, solo ensamblada desde fuente** con un bootcode libre (ceros, ya que arrancamos por HLE) en lugar de `N64_BOOTCODE.BIN`. Nunca descargar ese archivo, `BOOTCODE.asm` ni los `.N64` precompilados. Reporta PASS/FAIL en el framebuffer. |
| Dillonb/n64-tests | — | Ninguna | Sí | Descartada: sin licencia no es redistribuible. |
| Pruebas de libdragon | — | Unlicense | No | Descartada: prueban la biblioteca, no el CPU. |
