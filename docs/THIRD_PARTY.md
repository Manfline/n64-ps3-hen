# Código y herramientas de terceros

## Código incluido o adaptado en el repositorio

| Archivo | Origen | Licencia | Notas |
|---|---|---|---|
| `tests/phase0/rsx-triangle/main.c` | Sample `samples/graphics/rsxtest` de PSL1GHT (`f649a08f`) | MIT | Inicialización del RSX, superficie, viewport y flip reescritos en C a partir de `rsxutil.cpp`/`main.cpp` |
| `lib/rsxkit/src/video.c` | `tests/phase0/rsx-triangle/main.c` (propio), que sigue el sample `rsxtest` de PSL1GHT | MIT | Inicialización del RSX y del video, superficie y flip; el resto de `lib/rsxkit` es propio |
| `tests/phase0/spu-compute/spu/mix.c` | Patrón DMA del sample `samples/spu/spudma` de PSL1GHT | MIT | Solo el patrón `mfc_get`/espera por etiqueta; el cálculo y el doble búfer son propios |
| `lib/rsxkit/src/font_8x13.h` (glifos del texto en pantalla: menú del frontend) | Fuente «fixed» 8x13 de X11: `8x13.bdf` del paquete misc-misc de X.Org (gitlab.freedesktop.org/xorg/font/misc-misc, rama master consultada el 2026-10-10; versión «ucs-fonts 8x13.bdf,v 1.57 2006-01-05» de Markus Kuhn) | Dominio público (propiedad COPYRIGHT del BDF y `COPYING` del paquete: «Public domain font. Share and enjoy.») | Solo los bitmaps de 217 glifos (ASCII, Latin-1 y símbolos de menú), convertidos con `lib/rsxkit/tools/bdf2c.py` (propio, MIT). No es una fuente de Nintendo ni de Sony. Al ser de dominio público entra en `lib/rsxkit/` (MIT) |
| `lib/rsxkit/src/ui_geom.c` (agrandado de los glifos del atlas del 2D) | Regla de Scale2x (EPX) de Andrea Mazzoleni, tomada de su descripción pública del algoritmo (scale2x.it) | Algoritmo (no se copió código) | Implementación propia (MIT) de las cuatro reglas de vecinos para agrandar 2× los glifos de la fuente 8x13 |
| `lib/rsxkit/src/fprog.c` | Formato de instrucción de fragment program de `tools/cgcomp/include/nvfx_shader.h` y `fpparser.cpp` de PSL1GHT (`f649a08f`) | MIT | Solo los campos (fin de programa, tipo de fuente, bit 31 de SRC2) para limpiar el bit que quita la corrección de perspectiva; código propio |
| `lib/rsxkit/src/fpgen.c`, `include/rsxkit/fpgen.h` (generador de fragment programs) | Formato de instrucción de fragment program NV40 de `tools/cgcomp/include/nvfx_shader.h` (campos de las cuatro palabras, opcodes, tipos de fuente, swizzle, precisión) y lo que `compilerfp.cpp` y `main.cpp` de cgcomp emiten en cada instrucción (condición siempre verdadera con swizzle identidad en la palabra 1, 0x7FC en los bits 19–29 de la palabra 3 al leer un TEXCOORD, mitades de 16 bits intercambiadas al escribir el microcódigo, `fp_control` 0x40 al escribir el color, `texcoords`/`texcoord3D`) de PSL1GHT (`f649a08f`, consultado el 2026-10-10) | MIT | Solo el formato: el codificador, la asignación de registros y la caché son propios. Cotejado con los `.fpo` que cgcomp compila de `lib/rsxkit/shaders/rsxkit_lincomb*.fcg` y de `tests/unit/fpgen/*.fcg` (`tools/fpgen-ref.sh`), que `tests/unit/test_fpgen.c` interpreta con el mismo intérprete que verifica al generador. RPCS3 (GPL) no se consultó para esto |

| `src/rsp/audio/alist.c`, `src/rsp/audio/abi.c` (HLE de las listas de audio) | mupen64plus-rsp-hle (github.com/mupen64plus/mupen64plus-rsp-hle), commit `8a7a472a` (2026-06-23): `src/alist.c`, `src/audio.c`, `src/alist_audio.c`, `src/alist_nead.c` y la tabla de despacho de `src/hle.c` | GPLv2 o posterior (cabecera de cada archivo; `LICENSES` dice GPLv2) | Adaptado (2026-10-10) con los avisos de copyright originales en cada archivo: DMEM y RDRAM en el orden de la N64 con índices de la N64 (FILTER e INTERLEAVE del original dependen del host little-endian), E/S explícita de la RDRAM (DMA en el SPE), formato propio del estado de ENVMIXER, direcciones de la DMEM módulo 4 KB, sin desplazamientos de negativos. **No** se tomó la tabla `RESAMPLE_LUT` del original (viene del microcódigo de Nintendo): `src/rsp/audio/lut.c` es un diseño propio (`tools/audio-lut.py`). Compatible con GPLv2+ |
| `src/rsp/audio/abi.c` (naudio), `alist.c` (IIR y OVERLOAD), `src/rsp/audio/mp3.c` | mupen64plus-rsp-hle, commit `8a7a472a` (2026-06-23): `src/alist_naudio.c`, `alist_iirf` y `alist_overload` de `src/alist.c`, `src/mp3.c` | GPLv2 o posterior (cabecera de cada archivo) | Adaptado (2026-10-10) con los avisos de copyright originales: la codificación de cada comando y las tablas de despacho de naudio, Banjo-Kazooie, Donkey Kong 64 y los de MP3 (Perfect Dark, Banjo-Tooie, Jet Force Gemini, Conker) verificadas contra esa fuente. MP3: DMEM de trabajo propia en el orden de la N64, productos que desbordan int32 en 64 bits y truncados (mismo resultado que el original); comparado con el original compilado aparte (fuera del repo): 2304 muestras iguales bit a bit. Los cosenos en Q16 de la DCT (LUT2, LUT3, LUT6, √2) quedan como en el código del original. **No** se tomó su `DeWindowLUT` (viene del microcódigo): ver la fila siguiente |
| `src/rsp/audio/mp3_lut.c` (ventana de síntesis del MP3), `tools/audio-mp3-lut.py` | Ventana de síntesis de MPEG-1 (ISO/IEC 11172-3, tabla B.3, D[i]) tomada como `ff_mpa_enwindow[257]` de FFmpeg (`libavcodec/mpegaudiodsp_data.c`, rama master consultada el 2026-10-10) y la simetría D[512 − i] = ±D[i] con que FFmpeg arma la ventana completa | LGPL 2.1 o posterior (compatible con GPLv2+) | Solo los 257 valores de la ventana estándar; la escala (pico 32767) y el orden en bloques duplicados los genera el script para el recorrido de `mp3.c`. Coincide con la tabla del original en todas las entradas que `mp3.c` lee (difiere en 10 que nunca lee) |

| `src/rsp/jpeg/jpeg.c` (tarea JPEG del RSP de Zelda OoT: fondos de las salas prerrenderizadas) | mupen64plus-rsp-hle (github.com/mupen64plus/mupen64plus-rsp-hle), `src/jpeg.c` (rama master consultada el 2026-10-11): `jpeg_decode_PS` (sin reescalar Y/UV, salida RGBA), estructura de la tarea, orden de los bloques de un macrobloque (modos 0 y 2), descuantización con ×16, tabla zigzag, conversión YCbCr → RGBA5551 en dieciseisavos (+2048, constantes 1,4025/0,3443/0,7144/1,7729, máscara 0xF80, alfa 1) | GPLv2 o posterior (cabecera del archivo) | Reescrito (2026-10-11) con el aviso de copyright original: RDRAM en el orden de la N64, IDCT separable propia con tabla de cosenos y redondeo al más cercano, y G con cada producto truncado (así coincide mejor con el microcódigo real). Comparado contra el microcódigo real con el LLE (`salsa64-run --lle-check-jpeg`): 99,0–99,2 % de los píxeles idénticos y el resto a ±1 en un canal de 5 bits (docs/estado.md). Compatible con GPLv2+ |
| `src/rsp/gfx/s2dex.c` (tramo S2DEX2 de una tarea F3DEX2: G_BG_COPY, G_BG_1CYC) | GLideN64 (github.com/gonetz/GLideN64), `src/uCodes/S2DEX.cpp` (rama master consultada el 2026-10-11) | GPLv2 (solo consulta) | Solo el formato de uObjBg/uObjScaleBg (orden de los campos, punto fijo u10.5/u10.2/s10.2, escala u5.10 = texels por píxel). El dibujo (tiras cargadas a la TMEM con LOADTILE y TEXRECT, como hace el microcódigo) es propio; el opcode de G_BG_COPY (0x0A) se confirmó en la lista de Zelda OoT |
| `src/rsp/lle/vu.c`, `src/rsp/lle/vmem.c` (unidad vectorial del RSP y sus cargas/almacenamientos) | ares (github.com/ares-emulator/ares), commit `9408cb43` (2026-10-06): `ares/n64/rsp/interpreter-vpu.cpp` (caminos SISD), `interpreter-ipu.cpp`, `rsp.cpp` (tablas de VRCP/VRSQ) | ISC (Copyright (c) 2004-2025 ares team, Near et al; `LICENSE` del repositorio) | Reescrito en C (2026-10-10) con el aviso en la cabecera de cada archivo: la semántica elemento por elemento de VMULF/VMUDx/VMADx, VMACQ/VMULQ, VRNDx, VCH/VCL/VCR, VLT/VEQ/VNE/VGE, las saturaciones del acumulador, el recíproco y la raíz inversa (fórmulas de las tablas y el camino de 32 bits) y las reglas de alineación de LPV/LUV/LHV/LFV/LTV/SFV/SHV/SWV/STV. Todo ello lo verifica nemu64-test contra la consola (las 214 pruebas «RSP …» pasan). El resto de `src/rsp/lle` (despacho, escalar, COP0, conductor, SPE) es propio. Compatible con GPLv2+; **no** entra en `lib/` |

El núcleo de la Fase 1 (`src/core/`, `src/cpu/`, `src/host/`), el mapeo
de controles del frontend (`src/frontend/padmap.c`) y el guardado del
cartucho (`src/core/save.c`, `src/frontend/savefile.c`) son código propio, escrito desde documentación pública (manual del VR4300 de NEC, MIPS
IV ISA, n64brew.dev) y validado con nemu64-test. No incluye código de
Not64/Wii64, Mupen64Plus ni ares.

El recompilador de la Fase 3 (`src/cpu/rec.c`, `rec_emit.c`, `rec.h`,
`ppc.h`) también es propio, escrito desde Power ISA (libro I) y la
semántica de nuestro intérprete. Not64 y Wii64 se **consultaron como
referencia de diseño, sin copiar código** (decisión en docs/rec-ppe.md):

| Proyecto | Repositorio y commit revisado | Licencia | Uso |
|---|---|---|---|
| Not64 | github.com/extremscorner/not64, `d35a7a1c454eca973995939dbca3f06725cf4a6e` (2026-04-10) | GPLv2 o posterior (cabeceras de `r4300/ppc/*`; `LICENCE.TXT` = GPL v2) | Lectura de `r4300/ppc/` y `Recomp-Cache*` |
| Wii64 | github.com/emukidid/Wii64, `2e1c6fb056e1965a57f6be4accdbf5460e7de291` (2026-10-09) | GPLv2 o posterior | Lectura de `r4300/ppc/` |
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
| Lo que el IPL3 del CIC 6105/7105 deja en la RDRAM: copia de DMEM 0x554–0x888 a RDRAM 0x004–0x338 (de ahí sale la palabra 0x2E8 = 0xC86E2000 que comprueban Perfect Dark y Zelda OoT), DMA con paso del microcódigo del RSP (RDRAM 0x1E8 → IMEM 0x120, 0x1F0 bytes; 24 filas de 8 bytes en RDRAM 0x2FB1F0, una cada 0xFF0, SP_WR_LEN 0xFE817000) y tamaño de la RDRAM en 0x3F0. Solo direcciones y tamaños: los bytes los pone el cartucho del usuario al arrancar | `src/core/boot.c` | Cotejados entre fuentes (consultadas el 2026-10-10): rango de la copia, kestrel64 (`docs/GAPS.md`, `docs/baselines/timings.md`, github.com/celgadis84/kestrel64); palabra 0x2E8, decomp de Perfect Dark (`src/lib/boot.c`) y de Zelda OoT (`ovl_Bg_Zg`, «código del RSP en el desplazamiento 0x7F8 del IPL3», es decir ROM 0x838); DMA con paso, mupen64plus-rsp-hle `src/cicx105.c`; 0x3F0, clbr/n64bootloader `main.c` y kestrel64. Verificado en el Perfect Dark del autor leyendo solo dos palabras: ROM 0x838 = 0xC86E2000 (RDRAM 0x2E8) y ROM 0x750 = 0xAD170014 (fila 3 del DMA, la que comprueba Donkey Kong 64). No se tomó el residuo del IPL2 en IMEM 0x000–0x01C que escriben mupen64plus y Project64 (es código de la PIF ROM) | documentación y decomps (solo hechos); mupen64plus-rsp-hle GPLv2 (solo consulta) |
| Nombres de las variantes de microcódigo en el censo: audio por las palabras 0x00, 0x10, 0x28 y 0x30 de `ucode_data` (ABI 1, nead, naudio) y el gráfico de Perfect Dark (F3DPD) | `docs/compatibilidad.md` (solo documentación; ningún código) | mupen64plus-rsp-hle `src/main.c` (`try_audio_task_detection`); GLideN64 `src/GBI.cpp` | GPLv2 (solo consulta) |
| `osCicId` (0x80000310) = número del CIC con los IPL3 del 6103 y del 6105 | `src/core/boot.c` | Comprobaciones antipiratería documentadas en decomps (solo hechos, consultados el 2026-10-10): Banjo-Kazooie (`src/core2/ba/model.c`, compara con 6103), Diddy Kong Racing (`src/objects.h`), Perfect Dark (`docs/piracychecks.md`, compara con 6105), Donkey-Kong-64-Recompiled (pone 6105); y observado en el Conker del autor (su hilo ocioso arranca el principal solo si vale 6105) | documentación y decomps (solo hechos) |
| CRC-32 del IPL3 de cada variante de CIC (solo los valores) | `src/core/rom.c` | nust64 0.4.1, `src/rom.rs` | MIT |
| Semillas del IPL3, ajuste de la dirección de arranque por CIC, códigos de destino, mapa de memoria y reglas del bus | `src/core/` | n64brew.dev (PIF-NUS, CIC-NUS, ROM Header, Memory map, RDRAM Interface, Initial Program Load) | documentación |
| Temporización del VI (relojes por tipo de TV, VI_V_SYNC/VI_H_SYNC, área visible con VI_H_VIDEO/VI_V_VIDEO y escalas) | `src/core/vi.c` | n64brew.dev (Video Interface) | documentación |
| Comandos joybus del PIF (byte de comandos, formato de la PIF RAM, bits de error, comandos del control estándar, respuestas sin Controller Pak, CRC-8 0x85, orden de los botones) | `src/core/pif.c`, `tests/roms/src/vitest` | n64brew.dev (PIF-NUS, Joybus Protocol, diagrama `Controller-response.svg`; consultadas el 2026-10-10) | documentación |
| Reglas de hardware verificadas por las pruebas (MULT, LWR, enlaces, COP0, COP2, FPU, TLB, RE, bus) y valores de los registros de RDRAM | `src/cpu/`, `src/core/` | nemu64-test (fuente, comentarios y valores esperados) | MIT |
| EEPROM del cartucho (comandos 0x00/0xFF, 0x04, 0x05, identificadores 0x0080/0x00C0, bloques repetidos en la de 4 Kbit), SRAM en el dominio 2 del PI (DMA PI_RD_LEN/PI_WR_LEN, acceso directo de 32 bits) y cabecera de homebrew ("ED" en 0x3C, tipo en 0x3F) | `src/core/save.c`, `tests/roms/src/savetest` | n64brew.dev (Joybus Protocol, Peripheral Interface, Memory map, ROM Header; consultadas el 2026-10-10) | documentación |
| Tipo de guardado por código de juego: solo el código del encabezado y el tipo de memoria (datos de compatibilidad, sin material de Nintendo) | `src/core/savedb.c` | Cotejados entre dos fuentes y tomados solo donde coinciden (178 códigos y 6 excepciones por región): heurística de mia en ares (`mia/medium/nintendo-64.cpp`, commit `433d07fb`) y base de N64FlashcartMenu (`src/menu/rom_info.c`, commit `8c5fb11f`). No se copió código de ninguna | ares: ISC; N64FlashcartMenu: AGPL-3.0 (solo hechos) |
| DMA del SP (filas, salto, vuelta en el banco, registros al terminar), bits de SP_STATUS, estructura OSTask en DMEM 0xFC0 | `src/core/sp.c`, `src/core/rsp.c` | n64brew.dev (Reality Signal Processor/Interface); mupen64plus-rsp-hle `src/memory.h`, `src/hle.c` | documentación; GPLv2 (solo consulta) |
| Registros del DP (DPC_START válido hasta el END, CURRENT = START al escribir END, pares de bits de DPC_STATUS: xbus, freeze, flush) y que los pares (limpiar, poner) de SP_STATUS con los dos bits no cambian nada; registros de COP0 del RSP (0–7 SP, 8–15 DP) y comportamiento de los opcodes inexistentes de SPECIAL | `src/core/sp.c`, `src/rsp/lle/cpu.c` | n64brew.dev (RSP, RDP); ares `ares/n64/rsp/io.cpp`, `ares/n64/rdp/io.cpp` (ISC) como confirmación; nemu64-test lo verifica | documentación; ISC (solo consulta) |
| Fin de una tarea: HALT, BROKE y TASKDONE, e interrupción DP tras G_RDPFULLSYNC (los retardos son propios, del mismo orden) | `src/core/rsp.c` | mupen64plus-core `src/device/rcp/rsp/rsp_core.c` (`do_SP_Task`) | GPLv2 (solo consulta) |
| Audio Interface: DMA con cola de dos lugares, interrupción al empezar cada DMA, AI_LENGTH y los demás registros que se leen como él, bits de AI_STATUS, frecuencia = reloj del VI / (AI_DACRATE + 1), error del acarreo en los límites de 8 KB | `src/core/ai.c`, `tests/roms/src/audiotest` | n64brew.dev (Audio Interface, consultada el 2026-10-10); ares `ares/n64/ai/` (ISC) como confirmación | documentación; ISC (solo consulta) |
| Reconocimiento del ABI de audio por palabras de `ucode_data` (las constantes de [0x10], [0x28] y [0x30] de cada microcódigo, sin código ni datos del microcódigo) | `src/core/rsp_audio.c` | mupen64plus-rsp-hle `src/hle.c` (`try_audio_task_detection`, commit `8a7a472a`) | GPLv2+; solo se comparan valores. Las pruebas usan palabras sintéticas y el texto "Salsa64 ABI NEAD" |
| Sincronía del audio con la salida (WSOLA) | `src/frontend/audio_sync.c` | Técnica de Verhelst y Roelands (1993), escrita desde su descripción | código propio |
| Puerto de audio de la PS3 (audioInit, puertos, cola de eventos, bloques de 256 muestras en flotante) | `src/frontend/audio_out.c` | `ppu/include/audio/audio.h` de PSL1GHT | MIT |
| Detección del microcódigo por los textos de `ucode_data` ("RSP Gfx ucode", versión 2.x, F3DZEX, "RSP SW Version") | `src/core/rsp.c` | GLideN64 `src/GBI.cpp`; mupen64plus-video-rice `src/RSP_Parser.cpp` | GPLv2 (solo consulta). Las pruebas usan textos sintéticos, no los de Nintendo |
| Codificación de los comandos F3DEX2 y F3DZEX (G_VTX, G_MODIFYVTX, G_CULLDL, G_BRANCH_Z/W, G_TRI*, G_TEXTURE, G_GEOMETRYMODE y sus bits, G_MTX, G_MOVEWORD, G_MOVEMEM y sus offsets, G_SETOTHERMODE_*, G_TEXRECT en tres comandos) | `src/rsp/gfx/f3dex2.c`, `tests/roms/src/gfxtest/gbi.h` | n64brew.dev; GLideN64 `src/uCodes/F3DEX2.*`, `src/GBI.h`, `src/gSP.cpp`; mupen64plus-video-glide64mk2 `ucode02.h`; mupen64plus-video-rice `UcodeDefs.h` | documentación; GPLv2 (solo consulta). `gbi.h` de gfxtest es propio: no es el de libultra ni el de ningún decomp |
| Luces (transformar la dirección con la modelview y normalizar), mapeo de entorno ((d+1)·512 y acos(−d)·1024/π texels antes de la escala) y niebla por vértice (z/w·fm + fo) | `src/rsp/gfx/geom.c` | GLideN64 `src/gSP.cpp`, `src/3DMath.cpp`; mupen64plus-video-glide64mk2 `3dmath.cpp` | GPLv2 (solo consulta) |
| Codificación de los comandos de F3D (Fast3D) y F3DEX 1.x: opcodes inmediatos desde 0xBF, G_VTX (n−1, v0 y largo; en F3DEX v0·2 y n), índices ×10 en F3D, G_MTX (proyección 0x01, carga 0x02, push 0x04), G_MOVEMEM (viewport 0x80, lookat 0x82/0x84, luces 0x86–0x94, gSPForceMatrix 0x9E/0x98/0x9A/0x9C), G_MOVEWORD (índice en el byte bajo, número de luces 0x80000000 + 32·(n+1), color de luz cada 0x20, G_MW_POINTS de 40 bytes), bits de geometry mode (suave 0x200, culling 0x1000/0x2000), G_SETOTHERMODE_* con desplazamiento y largo directos, G_TEXTURE con el encendido en el byte bajo, G_TEXRECT seguido de G_RDPHALF_2 y G_RDPHALF_CONT; G_TRI4 (0xB1) de Fast3D 2.0G con índices de 4 bits (orden de los nibbles verificado con la imagen de GoldenEye en el host) | `src/rsp/gfx/f3d.c`, `tests/roms/src/gfxtest/gbi.h` | Documentación de la comunidad de SM64 sobre Fast3D (wiki de Skelux, "Fast3D Display List Commands", consultada el 2026-10-10); GLideN64 `src/uCodes/F3D.cpp` (consultado el 2026-10-10; G_TRI4, de memoria de su `F3D_Tri4`) | documentación; GPLv2 (solo consulta, sin copiar código). Nada del decomp de SM64 ni de libultra |
| Microcódigos de Rare. Perfect Dark: Fast3D con G_VTX de vértices de 12 bytes (x, y, z, banderas, offset del color en la tabla, s, t), G_VTXCOLORBASE (0x07) con la tabla de 4 bytes por color (R, G, B, A o normal y A), G_TRI4 en 0xB1 como Fast3D 2.0G y G_QUAD en 0xB5 con índices ×10. Conker («F3DEXBG»): G_TRI4 en 0x10–0x1F con índices de 5 bits, G_MW_NUMLIGHT en luces de 48 bytes, G_MW_COORD_MOD (0x10), G_MV_LIGHT con el offset en (w0 >> 5) & 0x3FFF y luces de 48 bytes con intensidad (byte 12) y posición (bytes 32–37) de luz puntual, G_MV_NORMALS (14) con 2 bytes de normal por lugar del búfer y Z en el byte 7 del vértice, el bit 15 de las banderas que apaga la luz, G_LOAD_UCODE como paso a la iluminación avanzada sin cortar la lista, y la fórmula de la luz (ambiental + puntuales con min(1, ca / (2·d²/65536)) + direccional en el modo avanzado, multiplicando el color del vértice). El CRC-32 del código de Perfect Dark es un valor calculado por nosotros sobre el respaldo propio | `src/rsp/gfx/f3dpd.c`, `src/rsp/gfx/f3dexbg.c`, `src/core/rsp.c` | GLideN64 (github.com/gonetz/GLideN64, commit `41c7ba27`, 2026-08-05; consultado el 2026-10-10): `src/uCodes/F3DPD.cpp`, `F3DGOLDEN.cpp` (`F3D_TriX`), `F3DEX2CBFD.cpp` y en `src/gSP.cpp` `gSPCIVertex`, `gSPCBFDVertex`, `gSPLightCBFD`, `gSPLightVertexCBFD_basic/_advanced`. Verificado con las listas de los juegos en el host (largos de G_VTX, índices de G_TRI4 de mallas, offsets de las luces) | GPLv2 (solo consulta, sin copiar código). Las pruebas usan listas, tablas y textos sintéticos; la del CRC arma 4 KB de ceros con 4 bytes calculados para dar ese valor |
| Ronda de xxHash32 (acumulador + entrada·PRIME2, rotar 13, ·PRIME1), sus constantes PRIME1–3 y la mezcla final (avalanche) | `src/rsp/gfx/texcache.c` (hash de las texturas de la caché) | xxHash de Yann Collet, especificación `doc/xxhash_spec.md` (github.com/Cyan4973/xxHash) | BSD-2-Clause. Solo la ronda y las constantes; la disposición en ocho carriles de 16 bytes y el plegado a 64 bits son propios (no es xxHash compatible) |
| Triángulos crudos del RDP (0xC8–0xCF): palabras de aristas (YL/YM/YH en s11.2, XL/XH/XM y pendientes en s11.16/s13.16), bloques de sombreado, textura (S, T, W en s10.21) y z, y que los valores iniciales están en (XH, floor(YH)) y la arista L empieza en YM; que F3D los pasa por G_RDPHALF_1/2/CONT se vio en las listas de GoldenEye | `src/rsp/gfx/rdp.c`, `dl.c`, `f3d.c` | n64brew.dev (Reality Display Processor/Commands, consultada el 2026-10-10) | documentación (CC BY-SA, reescrito) |
| RDP: campos de othermode, blender, G_SETCOMBINE y sus entradas, TMEM (LOADBLOCK con dxt, LOADTILE, LOADTLUT, RGBA32 partido, intercambio en filas impares), formatos de textura, TEXRECT/FILLRECT en COPY/FILL, intercambio de TEXEL0/TEXEL1 en el segundo ciclo | `src/rsp/gfx/rdp.c`, `tmem.c`, `combine.c` | n64brew.dev (Reality Display Processor/Commands); parallel-rdp (MIT) y angrylion-rdp-plus (licencia MAME, no comercial: solo lectura) como confirmación | documentación (CC BY-SA, reescrito); MIT; solo consulta |
| Cobertura con CVG_X_ALPHA: cvg·α redondeada a la cobertura de 3 bits (cvg = α/32 con el píxel cubierto entero) y, con AA_EN, el RDP no escribe los píxeles de cobertura 0 aunque haya FORCE_BL (G_RM_TEX_EDGE) | `src/rsp/gfx/rdp.c` (`blend_state`) | angrylion-rdp-plus (de memoria de su `combiner_1cycle`/`blender_1cycle`; licencia MAME, solo consulta); n64brew.dev (Set Other Modes) | solo consulta, sin copiar código; documentación |

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
| gcc-powerpc64-linux-gnu 12, glibc ppc64 (Debian 12) | debian.org | GPL/LGPL | Compilar el núcleo para Linux ppc64 y probar el recompilador con qemu (`docker/ppc64.Dockerfile`); los binarios quedan en `build/ppc64` |
| QEMU 7.2 (`qemu-user-static`, Debian 12) | qemu.org | GPLv2 | Ejecutar esos binarios ppc64 en la Mac |

## ROMs de prueba incluidas

| Archivo | Origen | Licencia | Contenido de terceros |
|---|---|---|---|
| `tests/roms/homebrew/nemu64-test.z64` | github.com/thelemmy/nemu64-test, commit `9a8b9f7d94ee2f6f57d7feed70c98c22cdc30e6c` (2026-09-08), conjunto "base" | MIT (Copyright (c) 2021 lemmy-64), `nemu64-test.LICENSE` | IPL3 libre de libdragon r7 (Unlicense) que trae nust64 0.4.1, verificado por MD5 (`7f014ed41cc53bb1a5e44f211f8c3dec`); crates enlazados (arbitrary-int y oorandom MIT; arrayref BSD-2-Clause; linked_list_allocator, lock_api, num-traits, scopeguard y spinning_top MIT/Apache-2.0), compiler-builtins y core/alloc de Rust: avisos en `nemu64-test.NOTICES` |

| `tests/roms/homebrew/salsa64-vitest.z64` | Código propio (`tests/roms/src/vitest`) | GPLv2 o posterior | IPL3 libre de libdragon r7 (Unlicense) que trae nust64 0.4.1, verificado por MD5 como arriba. Sin bibliotecas: ni libultra ni libdragon. Valores de los registros del VI (modos NTSC/PAL de 320×240) y comandos joybus tomados de n64brew.dev (Video Interface, Joybus Protocol) |
| `tests/roms/homebrew/salsa64-gfxtest.z64`, `-gfxtest-f3d.z64`, `-gfxtest-f3dex.z64` | Código propio (`tests/roms/src/gfxtest`) | GPLv2 o posterior | Lo mismo que vitest. Las tres variantes son las mismas escenas con los comandos del RSP de F3DEX2, F3D y F3DEX 1.x (`gbi.h` propio, `GBI=` en su Makefile); el "microcódigo" es un BREAK y un texto propio, sin código de Nintendo |
| `tests/roms/homebrew/salsa64-audiotest.z64` | Código propio (`tests/roms/src/audiotest`) | GPLv2 o posterior | Lo mismo que vitest. Registros del AI de n64brew.dev (Audio Interface); la tarea de audio usa la codificación de comandos de nead (como en `src/rsp/audio/abi.c`) con un microcódigo que es solo un BREAK y el texto "Salsa64 ABI NEAD" |
| `tests/roms/homebrew/salsa64-savetest-{eeprom4k,eeprom16k,sram}.z64` | Código propio (`tests/roms/src/savetest`) | GPLv2 o posterior | Lo mismo que vitest. Comandos de la EEPROM, registros del PI y cabecera de homebrew de n64brew.dev (Joybus Protocol, Peripheral Interface, ROM Header) |

`salsa64-vitest.z64` se compila con `tools/build-vitest.sh`
(`docker/vitest.Dockerfile`): clang y lld 14.0.6 de Debian 12 (MIPS II
big-endian, o32) y nust64 0.4.1 instalado con Rust 1.90.0 y `--locked`.
Recompilarlo da el mismo SHA-256 (`salsa64-vitest.z64.sha256`). Las tres
`salsa64-savetest-*.z64` salen igual (`tools/build-savetest.sh`, con
`ROM=savetest`) y después se les parchea el título y la cabecera de
homebrew; también son reproducibles (`*.sha256`). Las tres
`salsa64-gfxtest*.z64` salen de `tools/build-gfxtest.sh` (`ROM=gfxtest` y
`GBI=`), igual de reproducibles.

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
| clang/lld 14 (Debian 12) | llvm.org | Apache-2.0 con excepción LLVM | Compilar vitest, savetest, gfxtest y audiotest para MIPS |

## Suites de prueba de CPU

Licencias verificadas el 2026-10-10 con la API de GitHub (campo `license`)
y revisando los repositorios.

| Suite | Commit revisado | Licencia | ¿Contiene IPL3 de Nintendo? | Decisión |
|---|---|---|---|---|
| nemu64-test (github.com/thelemmy/nemu64-test; sucesor de lemmy-64/n64-systemtest) | `9a8b9f7d` (2026-09-08) | MIT | No: usa el IPL3 abierto de libdragon (Unlicense) que trae nust64 0.4.1 (MIT) | **Principal; incluida** (ver arriba). Cubre ALU, saltos, LL/SC, COP0, COP1 (modo FR), TLB, excepciones, y también MI/PI/SP/PIF/RSP/RDP. Reporta en pantalla e ISViewer; el arranque HLE reproduce lo que espera en DMEM. Corre con `tools/test-roms.sh`. |
| CPUTest de krom (github.com/PeterLemon/N64) | `7085543e` (2026-02-01) | Unlicense (desde 2025-09-27) | **Sí.** Cada ROM y `LIB/N64_BOOTCODE.BIN` contienen el IPL3 CIC-6102 de Nintendo; `BOOTCODE/BOOTCODE.asm` es su desensamblado | **Secundaria, pendiente: solo ensamblada desde fuente** con un bootcode libre (ceros, ya que arrancamos por HLE; el camino "plano" del HLE la arrancaría) en lugar de `N64_BOOTCODE.BIN`. Un `git clone` bajaría esos binarios: hay que traer solo los `.asm` de texto por la API. Nunca descargar ese archivo, `BOOTCODE.asm` ni los `.N64` precompilados. Reporta PASS/FAIL en el framebuffer (falta el VI para leerlo). |
| Dillonb/n64-tests | — | Ninguna | Sí | Descartada: sin licencia no es redistribuible. |
| Pruebas de libdragon | — | Unlicense | No | Descartada: prueban la biblioteca, no el CPU. |

## Recursos gráficos

| Recurso | Origen | Licencia |
|---|---|---|
| `assets/ps3/ICON0.PNG` (320×176): logo de Salsa64 (chile low-poly de 8 segmentos y «SALSA64») | Aportado por el autor el 2026-10-10; escalado con `sips` | Del proyecto. Sin marcas de Nintendo ni de Sony |
| `assets/ps3/PIC1.PNG` (1920×1080): fondo del XMB (chile brillante, cintas de luz, sin texto) | Aportado por el autor el 2026-10-10; escalado y recortado a 16:9 con `sips` | Del proyecto. Sin marcas de Nintendo ni de Sony |
