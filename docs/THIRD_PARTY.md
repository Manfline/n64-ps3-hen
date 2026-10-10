# Código y herramientas de terceros

## Código incluido o adaptado en el repositorio

| Archivo | Origen | Licencia | Notas |
|---|---|---|---|
| `tests/phase0/rsx-triangle/main.c` | Sample `samples/graphics/rsxtest` de PSL1GHT (`f649a08f`) | MIT | Inicialización del RSX, superficie, viewport y flip reescritos en C a partir de `rsxutil.cpp`/`main.cpp` |
| `lib/rsxkit/src/video.c` | `tests/phase0/rsx-triangle/main.c` (propio), que sigue el sample `rsxtest` de PSL1GHT | MIT | Inicialización del RSX y del video, superficie y flip; el resto de `lib/rsxkit` es propio |
| `tests/phase0/spu-compute/spu/mix.c` | Patrón DMA del sample `samples/spu/spudma` de PSL1GHT | MIT | Solo el patrón `mfc_get`/espera por etiqueta; el cálculo y el doble búfer son propios |
| `lib/rsxkit/src/fprog.c` | Formato de instrucción de fragment program de `tools/cgcomp/include/nvfx_shader.h` y `fpparser.cpp` de PSL1GHT (`f649a08f`) | MIT | Solo los campos (fin de programa, tipo de fuente, bit 31 de SRC2) para limpiar el bit que quita la corrección de perspectiva; código propio |

El núcleo de la Fase 1 (`src/core/`, `src/cpu/`, `src/host/`) es código
propio, escrito desde documentación pública (manual del VR4300 de NEC, MIPS
IV ISA, n64brew.dev) y validado con nemu64-test. No incluye código de
Not64/Wii64, Mupen64Plus ni ares.

La HLE gráfica de la Fase 2 (`src/rsp/gfx/`, `src/spu/gfx/`, `src/rdp/`) y
`lib/spukit/` también son código propio. Para el comportamiento de F3DEX2 y
del RDP se consultaron la documentación de n64brew.dev y el código de
emuladores GPL (abajo), sin copiar código: se reescribió a partir de la
descripción. Nada de eso entra en `lib/rsxkit/` ni en `lib/spukit/` (MIT),
que solo usan PSL1GHT.

Datos tomados de terceros (no código):

| Dato | En | Origen | Licencia |
|---|---|---|---|
| Comportamiento del IPL3 libre (búsqueda del ELF, boot flags en DMEM, $sp, variables de libultra del modo compat) | `src/core/boot.c` | libdragon `boot/README.md`, `ipl3.c`, `loader.c`, `cleanup.c`, `minidragon.h` (rama `preview`, consultada el 2026-10-10) | Unlicense |
| CRC-32 del IPL3 de cada variante de CIC (solo los valores) | `src/core/rom.c` | nust64 0.4.1, `src/rom.rs` | MIT |
| Semillas del IPL3, ajuste de la dirección de arranque por CIC, códigos de destino, mapa de memoria y reglas del bus | `src/core/` | n64brew.dev (PIF-NUS, CIC-NUS, ROM Header, Memory map, RDRAM Interface, Initial Program Load) | documentación |
| Temporización del VI (relojes por tipo de TV, VI_V_SYNC/VI_H_SYNC, área visible con VI_H_VIDEO/VI_V_VIDEO y escalas) | `src/core/vi.c` | n64brew.dev (Video Interface) | documentación |
| Reglas de hardware verificadas por las pruebas (MULT, LWR, enlaces, COP0, COP2, FPU, TLB, RE, bus) y valores de los registros de RDRAM | `src/cpu/`, `src/core/` | nemu64-test (fuente, comentarios y valores esperados) | MIT |
| DMA del SP (filas, salto, vuelta en el banco, registros al terminar), bits de SP_STATUS, estructura OSTask en DMEM 0xFC0 | `src/core/sp.c`, `src/core/rsp.c` | n64brew.dev (Reality Signal Processor/Interface); mupen64plus-rsp-hle `src/memory.h`, `src/hle.c` | documentación; GPLv2 (solo consulta) |
| Fin de una tarea: HALT, BROKE y TASKDONE, e interrupción DP tras G_RDPFULLSYNC (los retardos son propios, del mismo orden) | `src/core/rsp.c` | mupen64plus-core `src/device/rcp/rsp/rsp_core.c` (`do_SP_Task`) | GPLv2 (solo consulta) |
| Detección del microcódigo por los textos de `ucode_data` ("RSP Gfx ucode", versión 2.x, F3DZEX, "RSP SW Version") | `src/core/rsp.c` | GLideN64 `src/GBI.cpp`; mupen64plus-video-rice `src/RSP_Parser.cpp` | GPLv2 (solo consulta). Las pruebas usan textos sintéticos, no los de Nintendo |
| Codificación de los comandos F3DEX2 y F3DZEX (G_VTX, G_MODIFYVTX, G_CULLDL, G_BRANCH_Z/W, G_TRI*, G_TEXTURE, G_GEOMETRYMODE y sus bits, G_MTX, G_MOVEWORD, G_MOVEMEM y sus offsets, G_SETOTHERMODE_*, G_TEXRECT en tres comandos) | `src/rsp/gfx/f3dex2.c`, `tests/roms/src/gfxtest/gbi.h` | n64brew.dev; GLideN64 `src/uCodes/F3DEX2.*`, `src/GBI.h`, `src/gSP.cpp`; mupen64plus-video-glide64mk2 `ucode02.h`; mupen64plus-video-rice `UcodeDefs.h` | documentación; GPLv2 (solo consulta). `gbi.h` de gfxtest es propio: no es el de libultra ni el de ningún decomp |
| Luces (transformar la dirección con la modelview y normalizar), mapeo de entorno ((d+1)·512 y acos(−d)·1024/π texels antes de la escala) y niebla por vértice (z/w·fm + fo) | `src/rsp/gfx/geom.c` | GLideN64 `src/gSP.cpp`, `src/3DMath.cpp`; mupen64plus-video-glide64mk2 `3dmath.cpp` | GPLv2 (solo consulta) |
| RDP: campos de othermode, blender, G_SETCOMBINE y sus entradas, TMEM (LOADBLOCK con dxt, LOADTILE, LOADTLUT, RGBA32 partido, intercambio en filas impares), formatos de textura, TEXRECT/FILLRECT en COPY/FILL, intercambio de TEXEL0/TEXEL1 en el segundo ciclo | `src/rsp/gfx/rdp.c`, `tmem.c`, `combine.c` | n64brew.dev (Reality Display Processor/Commands); parallel-rdp (MIT) y angrylion-rdp-plus (licencia MAME, no comercial: solo lectura) como confirmación | documentación (CC BY-SA, reescrito); MIT; solo consulta |

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

## ROMs de prueba incluidas

| Archivo | Origen | Licencia | Contenido de terceros |
|---|---|---|---|
| `tests/roms/homebrew/nemu64-test.z64` | github.com/thelemmy/nemu64-test, commit `9a8b9f7d94ee2f6f57d7feed70c98c22cdc30e6c` (2026-09-08), conjunto "base" | MIT (Copyright (c) 2021 lemmy-64), `nemu64-test.LICENSE` | IPL3 libre de libdragon r7 (Unlicense) que trae nust64 0.4.1, verificado por MD5 (`7f014ed41cc53bb1a5e44f211f8c3dec`); crates enlazados (arbitrary-int y oorandom MIT; arrayref BSD-2-Clause; linked_list_allocator, lock_api, num-traits, scopeguard y spinning_top MIT/Apache-2.0), compiler-builtins y core/alloc de Rust: avisos en `nemu64-test.NOTICES` |

| `tests/roms/homebrew/salsa64-vitest.z64` | Código propio (`tests/roms/src/vitest`) | GPLv2 o posterior | IPL3 libre de libdragon r7 (Unlicense) que trae nust64 0.4.1, verificado por MD5 como arriba. Sin bibliotecas: ni libultra ni libdragon. Valores de los registros del VI (modos NTSC/PAL de 320×240) tomados de n64brew.dev (Video Interface) |

`salsa64-vitest.z64` se compila con `tools/build-vitest.sh`
(`docker/vitest.Dockerfile`): clang y lld 14.0.6 de Debian 12 (MIPS II
big-endian, o32) y nust64 0.4.1 instalado con Rust 1.90.0 y `--locked`.
Recompilarlo da el mismo SHA-256 (`salsa64-vitest.z64.sha256`).

Se compila con `tools/build-nemu64-test.sh` (`docker/nemu64-test.Dockerfile`):
rustup-init 1.29.1 (SHA-256 fijo), Rust `nightly-2026-07-16` con `rust-src`,
nust64 0.4.1 instalado con Rust 1.90.0 y `--locked` (su `proc-macro2` 1.0.49
no compila con nightly), y `cargo build --release --locked`. Recompilar desde
cero (`--no-cache`) da el mismo SHA-256 (`nemu64-test.z64.sha256`). El ROM
no contiene código de Nintendo.

## Herramientas para compilar ROMs de prueba (no se redistribuyen)

| Componente | Origen | Licencia | Uso |
|---|---|---|---|
| Rust (rustup 1.29.1, nightly-2026-07-16, 1.90.0) | rust-lang.org | MIT OR Apache-2.0 | Compilar nemu64-test en el contenedor |
| nust64 0.4.1 | crates.io / github.com/rust-n64/nust64 | MIT | ELF → ROM con el IPL3 de libdragon |
| clang/lld 14 (Debian 12) | llvm.org | Apache-2.0 con excepción LLVM | Compilar vitest para MIPS |

## Suites de prueba de CPU

Licencias verificadas el 2026-10-10 con la API de GitHub (campo `license`)
y revisando los repositorios.

| Suite | Commit revisado | Licencia | ¿Contiene IPL3 de Nintendo? | Decisión |
|---|---|---|---|---|
| nemu64-test (github.com/thelemmy/nemu64-test; sucesor de lemmy-64/n64-systemtest) | `9a8b9f7d` (2026-09-08) | MIT | No: usa el IPL3 abierto de libdragon (Unlicense) que trae nust64 0.4.1 (MIT) | **Principal; incluida** (ver arriba). Cubre ALU, saltos, LL/SC, COP0, COP1 (modo FR), TLB, excepciones, y también MI/PI/SP/PIF/RSP/RDP. Reporta en pantalla e ISViewer; el arranque HLE reproduce lo que espera en DMEM. Corre con `tools/test-roms.sh`. |
| CPUTest de krom (github.com/PeterLemon/N64) | `7085543e` (2026-02-01) | Unlicense (desde 2025-09-27) | **Sí.** Cada ROM y `LIB/N64_BOOTCODE.BIN` contienen el IPL3 CIC-6102 de Nintendo; `BOOTCODE/BOOTCODE.asm` es su desensamblado | **Secundaria, pendiente: solo ensamblada desde fuente** con un bootcode libre (ceros, ya que arrancamos por HLE; el camino "plano" del HLE la arrancaría) en lugar de `N64_BOOTCODE.BIN`. Un `git clone` bajaría esos binarios: hay que traer solo los `.asm` de texto por la API. Nunca descargar ese archivo, `BOOTCODE.asm` ni los `.N64` precompilados. Reporta PASS/FAIL en el framebuffer (falta el VI para leerlo). |
| Dillonb/n64-tests | — | Ninguna | Sí | Descartada: sin licencia no es redistribuible. |
| Pruebas de libdragon | — | Unlicense | No | Descartada: prueban la biblioteca, no el CPU. |
