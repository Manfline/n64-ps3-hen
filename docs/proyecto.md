# Salsa64: objetivos, arquitectura y fases

Emulador de Nintendo 64 para PlayStation 3 (homebrew, CFW/HEN) diseñado
alrededor de la arquitectura Cell: el PPE ejecuta el CPU de la N64, los SPEs
hacen el trabajo vectorial (RSP, audio, posprocesado) y el RSX dibuja.

## Por qué existe

- RetroArch para PS3 no tiene núcleo de N64 funcional: los núcleos existentes
  dependen de OpenGL, que la PS3 no ofrece.
- Ningún emulador de N64 para PS3 aprovecha los SPEs. Ese es el hueco que
  atacamos.
- Ventaja estructural: la N64 y la PS3 son **big-endian**. No se hace
  byte-swapping de la RAM emulada.
- Coincidencia arquitectónica: el RSP de la N64 (MIPS + unidad vectorial +
  memoria local con DMA) se parece conceptualmente a un SPE (SIMD + 256 KB de
  local store + DMA). Diseñar alrededor de eso.

## Objetivos

1. Correr juegos de N64 a velocidad completa en PS3 real, empezando por
   títulos ligeros.
2. Usar el Cell como fortaleza: SPEs para RSP, audio y posprocesado; nada de
   cargar todo al PPE.
3. Salida HD: render interno a mayor resolución + filtros (CRT, escalado,
   antialiasing) calculados en RSX/SPEs.
4. Código abierto bajo **GPLv2 o posterior**. Excepción: `lib/rsxkit/` y
   `lib/spukit/` van bajo **MIT** (ver «Capa gráfica reutilizable»).

## No-objetivos (por ahora)

- Precisión de ciclo perfecta. Prioridad: jugable y estable; la precisión se
  mejora después.
- Soporte de 64DD, Transfer Pak u otros periféricos raros.
- Juego en línea o netplay.

## Reglas legales

- Toolchain: **PSL1GHT / ps3toolchain** únicamente. Nunca el SDK oficial de
  Sony (propietario, incompatible con GPL).
- Nunca incluir, descargar ni commitear ROMs comerciales, BIOS, PIF ROMs,
  llaves ni material de Nintendo o Sony.
- Pruebas automatizadas solo con **ROMs homebrew y suites de prueba de libre
  distribución**. Los juegos comerciales se prueban con respaldos propios,
  fuera del repositorio.
- Código tomado de Not64/Wii64, Mupen64Plus u otros proyectos GPL: conservar
  los avisos de copyright, registrar el origen en `docs/THIRD_PARTY.md` y
  mantener la compatibilidad de licencia. Verificar la licencia de cada
  fuente antes de copiar código.
- `.gitignore` excluye `*.z64`, `*.n64`, `*.v64`, `*.rom` salvo
  `tests/roms/homebrew/`.

## Arquitectura

| Unidad | Responsabilidad |
|---|---|
| **PPE (hilo 0)** | CPU VR4300 (MIPS III): recompilador dinámico MIPS → PowerPC64, MMU/TLB, interrupciones, planificador de eventos |
| **PPE (hilo 1)** | Video Interface, entrada (DualShock 3 → control N64), E/S, frontend/menú |
| **SPE 0** | RSP: HLE de microcódigos gráficos (F3D, F3DEX, F3DEX2) → listas de triángulos listas para el RSX. Más adelante: LLE del RSP |
| **SPE 1** | RSP audio (HLE de microcódigos de audio) y mezcla final |
| **SPE 2–3** | Decodificación/conversión de texturas N64 → formatos del RSX, caché de texturas |
| **SPE 4–5** | Posprocesado: escalado, antialiasing tipo MLAA, filtros CRT de cómputo pesado |
| **RSX** | Rasterizado HLE del RDP (combiners → fragment programs), render a resolución interna alta, shaders finales |

Notas:
- Hay 6 SPEs disponibles para homebrew de juegos; no asumir el 7º.
- La sincronización PPE↔SPE va por mailboxes y colas en memoria principal;
  evitar diálogos finos (una petición por triángulo = desastre). Trabajar por
  lotes: una lista de comandos completa por cuadro.

## Capa gráfica reutilizable (`lib/rsxkit/`)

El backend del RSX se construye en dos capas:

1. **Capa genérica (`lib/rsxkit/`)**: no sabe nada de la N64. API propia, sin
   incluir cabeceras del emulador. Contiene:
   - Inicialización del RSX, búferes de vértices e índices, sincronización
     con el PPE.
   - Subida y conversión de texturas (con conversión en SPE cuando aplique).
   - **Generador de fragment programs en tiempo de ejecución** (emite
     microcódigo del RSX y cachea programas).
   - Cadena de posprocesado: filtros CRT, escalado, antialiasing (RSX + SPE).
2. **Módulo N64 (`src/rdp/`)**: traduce combiners, modos de blending y estado
   del RDP a llamadas de la capa genérica.

Reglas:
- `lib/rsxkit/` nunca incluye nada de `src/`. Si una función necesita conocer
  la N64, va en `src/rdp/`. El build lo hace cumplir: `lib/rsxkit/` se compila
  como biblioteca estática propia sin `-Isrc`.
- Sus programas SPE (MLAA, xBR, conversión de texturas genérica) viven en
  `lib/rsxkit/spu/`, no en `src/spu/`.
- El generador de fragment programs recibe una descripción genérica
  (operaciones sobre entradas: textura, color de vértice, constantes) y cachea
  por hash de esa descripción. `src/rdp/` traduce cada combiner a esa
  descripción y guarda su propia tabla combiner → programa.
- Texturas: `lib/rsxkit/` convierte formatos genéricos (RGBA5551, intensidad,
  paletizados) a formatos del RSX; lo propio de la N64 (TMEM, TLUT,
  intercalado de líneas impares) va en `src/rdp/`.
- **Licencia MIT** (decidido el 2026-10-10), con su propio
  `lib/rsxkit/LICENSE` y `SPDX-License-Identifier: MIT` en cada archivo.
  Motivos: Snes9x y Genesis Plus GX (licencias no comerciales) no pueden
  enlazar código GPL; PSL1GHT ya es MIT; y MIT → GPL se puede cambiar
  después, al revés no. `lib/spukit/` (trabajos por lotes en SPEs) sigue la
  misma regla. El resto del emulador va en GPLv2+.
- Por eso **no entra en `lib/rsxkit/` ni en `lib/spukit/` código copiado de
  proyectos GPL** (Not64/Wii64, Mupen64Plus, RPCS3, ares) ni del port de SM64
  (sin licencia clara: solo lectura). RPCS3 sirve como documentación del
  formato del RSX. Fuentes compatibles para el generador de fragment
  programs: el ensamblador de `cgcomp` de PSL1GHT (MIT) y el driver nouveau
  nv30 de Mesa (familia NV40 como el RSX; verificar su licencia antes de
  tomar código). Todo origen se registra en `docs/THIRD_PARTY.md`.
- La API se documenta en `lib/rsxkit/README.md` pensando en usuarios externos
  (otros núcleos, homebrew). Meta: que la usen otros núcleos 3D (PS1 con
  resolución aumentada, Saturn, Dreamcast, GameCube) y que los núcleos 2D
  aprovechen sus filtros.

## Base de código y referencias

1. **Not64 / Wii64**: referencia para el recompilador MIPS → PowerPC
   (diseñado para Broadway, PPC de 32 bits).
2. **Mupen64Plus**: referencia para HLE del RSP (gráficos y audio) y
   comportamiento de periféricos.
3. **ares**: emulador de referencia de alta precisión. Se usa como «verdad»
   para comparar trazas, nunca como código a portar sin revisar licencia.
4. **Port de Super Mario 64 para PS3** (rama `ps3` de
   `github.com/fgsfdsfgs/sm64-port`): la referencia más cercana de un
   renderizador tipo N64 sobre el RSX con PSL1GHT. Útil para la inicialización
   del RSX, los búferes de vértices dobles y los shaders Cg compilados offline
   con `cgcomp`. Lo que no se copia: un shader escrito a mano por cada
   combiner, la transformación de vértices e iluminación en el PPE, las
   texturas decodificadas en el PPE y el audio en un hilo del PPE (no usa
   SPEs).

Antes de importar código: confirmar repositorio vigente, licencia y versión;
documentarlo en `docs/THIRD_PARTY.md`.

## Entorno

- Todo el build ocurre en **Docker** con ps3toolchain + PSL1GHT (imagen en
  `docker/`). En hosts ARM se usa `--platform linux/amd64` solo donde una
  dependencia lo exige (el Cg Toolkit).
- Pruebas rápidas: **RPCS3**, en modo sin interfaz para automatizar.
- Pruebas reales: PS3 con HEN/CFW. Envío del binario por FTP de webMAN MOD o
  `ps3load`.
- Logs: salida por TTY en RPCS3 y un logger UDP cuando corre en PS3 real.

## Estructura

```
docker/            Dockerfile del toolchain
src/core/          CPU, memoria, eventos, interfaces (PI, SI, VI, AI)
src/cpu/           intérprete + recompilador MIPS→PPC64
src/rsp/           HLE gráfico y de audio (código PPE + código SPE)
src/rsp/gfx/       HLE de F3DEX2 y del RDP, portable (host, PPE y SPE)
src/spu/           programas SPE (un directorio por tarea)
src/spu/gfx/       la HLE en el SPE 0 (E/S por DMA)
src/rdp/           módulo N64: RDP y combiners sobre lib/rsxkit
src/frontend/      menú, configuración, entrada
lib/rsxkit/        capa gráfica genérica del RSX (sin dependencias de la N64), incluye posprocesado
lib/rsxkit/spu/    programas SPE de la capa genérica
lib/spukit/        trabajos por lotes en SPEs (cola + timbre por mailbox), MIT como rsxkit
tests/roms/homebrew/   ROMs de prueba libres
tests/traces/      trazas de referencia
tools/             scripts: build, deploy a PS3, comparación de trazas
docs/              decisiones de arquitectura, THIRD_PARTY, mediciones
```

## Convenciones de código

- C (C99/C11 según soporte del GCC de ps3toolchain). C++ solo si una
  dependencia lo exige.
- Código SPE: estructuras alineadas a 16 bytes (DMA) y preferentemente 128
  bytes; transferencias DMA en múltiplos de 16 bytes; doble búfer (procesar un
  bloque mientras llega el siguiente).
- Presupuesto de local store por programa SPE: documentarlo en la cabecera de
  cada archivo (código + datos + búferes ≤ 256 KB).
- Sin asignaciones dinámicas en el bucle de emulación; todo preasignado al
  arrancar.
- Presupuesto de memoria principal (256 MB XDR): `docs/memory-budget.md`.
- Cada optimización va acompañada de una medición antes/después en
  `docs/perf-log.md`.

## Bucle de trabajo

1. Compilar en Docker (`tools/build.sh`).
2. Correr las pruebas unitarias del host (lo que no dependa de la PS3:
   decodificadores, HLE, recompilador en modo traza).
3. Correr en RPCS3 la ROM de prueba correspondiente y capturar log/imagen.
4. Comparar contra la traza o captura de referencia.
5. Si pasa, commit con mensaje descriptivo. Si no, iterar.
6. Marcar en el commit cuándo se requiere validación en PS3 real
   (rendimiento, sincronización SPE, comportamiento del RSX). RPCS3 no
   reproduce los tiempos exactos del Cell: **nunca** concluir sobre
   rendimiento solo con RPCS3.

## Fases y criterios de éxito

**Fase 0 — Entorno**
- Imagen Docker que compila los ejemplos de PSL1GHT e incluye `cgcomp` + Cg
  Toolkit.
- Un programa que dibuja un triángulo en el RSX y otro que ejecuta un cálculo
  en un SPE; ambos corren en RPCS3 y en la PS3 real.
- Logger UDP funcionando.
- **Prueba de JIT en el PPE (bloqueante para la Fase 3):** un programa mínimo
  que escribe instrucciones PowerPC en memoria, limpia la caché de datos y de
  instrucciones (`dcbst`/`sync`/`icbi`/`isync`), salta a ellas y obtiene el
  resultado esperado, probado en la PS3 real. Resultado en `docs/jit-ppe.md`.

**Fase 1 — Núcleo con intérprete**
- Intérprete del VR4300, memoria, PI/SI/AI/VI básicos, arranque sin depender
  de PIF ROM propietaria (HLE del boot).
- Criterio: pasan las pruebas de CPU de las suites homebrew elegidas; un
  homebrew simple muestra imagen (aunque sea framebuffer 2D).

**Fase 2 — RSP en SPE (HLE)**
- HLE de F3DEX2 en SPE 0 generando listas de triángulos; audio HLE en SPE 1.
- RDP HLE mínimo en RSX: triángulos con textura y color de vértice.
- Criterio: un homebrew 3D y un juego ligero arrancan y se ven reconocibles.

**Fase 3 — Recompilador dinámico**
- Recompilador MIPS → PPC64 en el PPE; caché de bloques; invalidación por
  escritura de código.
- Criterio: mismas trazas que el intérprete en las suites; ganancia de
  rendimiento medida en PS3 real.

**Fase 4 — Fidelidad gráfica**
- Combiners del RDP traducidos a fragment programs **generados en tiempo de
  ejecución**: no hay compilador Cg en la consola, así que se emite
  microcódigo de fragment program del RSX directamente. El generador vive en
  `lib/rsxkit/`; `src/rdp/` le pasa cada combiner como descripción genérica y
  cachea combiner → programa.
- Modos de blending, niebla, z-buffer; framebuffer effects comunes.
- Caché y conversión de texturas en SPE 2–3.

**Fase 5 — HD y filtros**
- Render interno a 2x/3x (hasta 720p/1080p de salida).
- Filtros CRT en el RSX: shaders Cg precompilados offline con `cgcomp` +
  NVIDIA Cg Toolkit (x86: en el contenedor `linux/amd64`).
- Filtros pesados en SPEs: escalado tipo xBR/ScaleFX para texturas,
  antialiasing tipo MLAA.
- Criterio: cada filtro medido en ms por cuadro en PS3 real, con opción de
  desactivarlo.

**Fase 6 — LLE del RSP (opcional)**
- Ejecutar el microcódigo real del RSP en un SPE, traduciendo sus
  instrucciones vectoriales a SIMD del SPE, para juegos con microcódigos que
  el HLE no cubre.

## Riesgos conocidos

- El PPE es in-order y débil para código general: perfilar el recompilador
  desde el principio.
- Ejecutar código generado en el PPE (JIT) depende de HEN/CFW
  (`docs/jit-ppe.md`).
- Documentación escasa del RSX en homebrew: todo hallazgo va a
  `docs/rsx-notes.md`.
- Efectos que leen el framebuffer (copias RDRAM ↔ RSX) son caros: decidir por
  juego cuándo emularlos.
- RPCS3 puede ocultar errores de sincronización entre SPEs; validar en
  hardware real.

## Juegos objetivo para medir avance

Se prueban con respaldos propios, fuera del repositorio. Medir siempre las
mismas escenas, anotar la región (NTSC/PAL), VI/s y FPS, y registrarlo en
`docs/perf-log.md`.

| Nivel | Juegos | Para qué |
|---|---|---|
| Ligero | Super Mario 64, Zelda: Ocarina of Time | Primeras metas de velocidad completa; comparables con el emulador Cell64 (`docs/referencia-cell64.md`) |
| Medio | Mario Kart 64, Star Fox 64 | Más carga de CPU y de RDP |
| Pesado | GoldenEye 007, Perfect Dark | Prueba final de CPU y de microcódigos poco comunes (Perfect Dark usa Expansion Pak) |

## Preguntas abiertas

- ¿Se integra como núcleo de RetroArch PS3 o como aplicación independiente?
  (Por omisión: independiente al inicio, núcleo libretro después.)
