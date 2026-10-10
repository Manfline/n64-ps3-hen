# Recompilador MIPS → PPC64 en el PPE (Fase 3)

Código: `src/cpu/rec.h`, `src/cpu/rec.c` (despachador, caché de bloques,
enlaces, invalidación), `src/cpu/rec_emit.c` (traducción), `src/cpu/ppc.h`
(codificación de instrucciones PowerPC).

## Decisión: recompilador propio (2026-10-10)

Se revisaron las dos bases candidatas (informe verificado el 2026-10-10,
solo lectura, sin copiar código):

| | Not64 | Wii64 |
|---|---|---|
| Repositorio | github.com/extremscorner/not64, `master` | github.com/emukidid/Wii64, `master` |
| Commit revisado | `d35a7a1c454eca973995939dbca3f06725cf4a6e` (2026-04-10, tag `20260410`); último cambio en `r4300/ppc`: `deff3be` (2025-02-05) | `2e1c6fb056e1965a57f6be4accdbf5460e7de291` (2026-10-09); versión 1.5.0 (`a96d9758`, 2026-09-23); el dynarec tiene trabajo activo |
| Licencia | `LICENCE.TXT` = GPL v2; cabeceras de `r4300/ppc/*` y `Recomp-Cache*`: "either version 2 of the Licence, or any later version" → **GPLv2 o posterior**, compatible | igual |
| Copyright | Mike Slegeir (tehpola) 2007-2010 (`ppc/*`, `Recomp-Cache*`); Hacktarux 2002 (núcleo mupen64); emu_kidid (`interupt.c`, `exception.c`) | igual |
| Tamaño | `r4300/ppc/` 11 196 líneas (`MIPS-to-PPC.c` 7 011, `PowerPC.h` 1 606) | 9 705 líneas |

`r4300/Invalid_Code.c/.h` no tienen aviso de licencia (unas 30 líneas).
mupen64gc (github.com/emukidid/mupen64gc) está muerto desde 2010.

Lo que hace ese recompilador y por qué no se porta:

- **Es de 32 bits.** Cada GPR de 64 bits es un registro PPC de 32 (con el
  alto implícito por extensión de signo) o un par hi/lo; solo 10 registros
  asignables. `PowerPC.h` no tiene ni una instrucción de 64 bits (`ld`,
  `std`, `rldic*`, `mulld`, `divd`); `genCmp64` compara solo 32 bits si un
  operando está mapeado en 32 (FIXME en el código). En el PPE todo eso
  sobra: un GPR del MIPS cabe en uno del PPE.
- **ABI de la Wii.** EABI de 32 bits con SDA en r13, punteros cargados con
  `lis/ori`, llamadas a C con `bl` directo y r2 como temporal. PSL1GHT es
  ELFv1 de 64 bits: descriptores de función, TOC en r2, r13 reservado.
- **Depende del núcleo de mupen64**: globales `interp_addr`, `PC->addr`,
  `delay_slot`, `next_interupt`, `last_addr`, `reg_cop0`, `interp_ops[]`,
  las macros de memoria sobre `address`/`word`… Nuestro CPU y bus son
  otros; integrarlo sería reescribir la interfaz entera.
- **No cumple el criterio de la Fase 3** ("mismas trazas que el
  intérprete"): Count se actualiza solo en saltos (`count_per_op` fijo) y
  los eventos se revisan por bloque; ADD/ADDI/SUB no lanzan desbordamiento;
  la FPU es nativa sin FCR31 (sin causas, banderas ni flush-to-zero); los
  stores con base sp/gp/ra/k0/k1 no revisan invalidación.
- **JR/JALR no se enlazan**: cada retorno de función vuelve al trampolín C.

Portarlo sería reescribir el emisor, el mapeo de registros, la interfaz con
el núcleo y las reglas de tiempo: más trabajo que uno propio, y sin poder
compararlo instrucción por instrucción con nuestro intérprete. Se escribió
uno propio (GPLv2+, sin código de terceros) tomando de Not64/Wii64 solo
ideas de diseño, que son comunes a los recompiladores: bloques con enlace
por parcheo de un salto, mapa de páginas con código para invalidar, camino
rápido de la RDRAM con una prueba de rango y condición del salto en un
campo de CR que sobrevive al delay slot.

## Diseño

### Cuándo corre en nativo

El despachador (`salsa64_rec_run`) reemplaza al bucle del intérprete. Un
bloque corre en nativo solo si:

- modo kernel, `pc` en KSEG0 dentro de la RDRAM, sin interrupción ni delay
  slot pendientes;
- cabe entero antes del próximo evento: `cycles + n ≤ sched->next` (lo
  revisa el propio bloque al entrar, también al llegar por un enlace).

Si no, el intérprete avanza hasta terminar un bloque básico (hasta un delay
slot o una excepción) y el despachador vuelve a intentarlo. Así cada evento
se atiende en el mismo ciclo que con el intérprete solo, y Count, Random y
VI_V_CURRENT dan lo mismo. KSEG1, TLB, modo usuario: intérprete (por ahora).

### Bloques

Desde `pc` hasta un salto con su delay slot, una instrucción de COP0,
SYSCALL o BREAK (siempre vuelven a C), o 64 instrucciones. Un salto cuyo
delay slot es otro salto o COP0 queda fuera del bloque (lo corre el
intérprete). La condición se calcula antes del delay slot en `cr4` (no
volátil, sobrevive a las llamadas a C); branch-likely no tomado salta el
delay slot y cuenta un ciclo menos.

### Registros

| PPE | Uso |
|---|---|
| r14–r25 | los 12 registros MIPS más usados del bloque (GPR, HI, LO) |
| r26 | destino de JR/JALR (sobrevive al delay slot) |
| r27 | `sched->next` (se recarga después de cada llamada a C) |
| r28 | `code_flags` |
| r29 | `cycles` al entrar al bloque |
| r30 | RDRAM |
| r31 | `struct salsa64_cpu` |
| r3–r12, r0 | temporales |

Al entrar se cargan los registros que el bloque lee antes de escribir; al
salir se escriben los modificados. Los que no entran en los 12 van y vienen
de `cpu->gpr` en cada uso.

### Llamadas a C y salidas

Antes de cada llamada se dejan `cycles`, `cur_pc` y `delay_slot` como los
deja el intérprete; después se leen de una vez `irq_pending`, `redirect`,
`rec_abort` y `mode` (cuatro bytes contiguos) y se recarga `sched->next`.
Si algo cambió, el bloque escribe sus registros y vuelve al despachador con
la dirección donde sigue (en un delay slot, según el salto).

Lo que se llama a C:
- camino lento de cargas y almacenamientos (E/S, TLB, KSEG1, fuera de la
  RDRAM, código en la zona escrita);
- instrucciones raras o con mucho estado, que ejecuta el intérprete
  (`salsa64_vr4300_exec_one`): LWL/LWR/SWL/SWR/LDL/LDR/SDL/SDR, LL/SC,
  CACHE, traps, COP0 y **toda la aritmética de la FPU** (por ahora);
- excepciones (SYSCALL, BREAK, desbordamiento).

En nativo: ALU de 32 y 64 bits (con desbordamiento), desplazamientos,
MULT/DIV y variantes de 64 bits, HI/LO, saltos, cargas y almacenamientos de
enteros y de COP1, MFC1/MTC1/DMFC1/DMTC1, BC1T/F.

### Memoria

Camino rápido: `va >> log2(rdram)` vale lo mismo para KSEG0 y KSEG1 tras un
`ori`; junto con los bits de alineación da una sola comparación. Los
almacenamientos además miran `code_flags[pa >> 7]`: si hay código
recompilado en esos 128 bytes, van por C, que invalida.

### Invalidación

`code_flags` cuenta cuántos bloques tocan cada 128 bytes. Lo revisan los
almacenamientos nativos, los del intérprete (`vr4300_code_check`) y las
escrituras externas (`salsa64_bus_rdram_written`: DMA del PI y del SI,
cargador, pruebas). Un bloque invalidado se saca del mapa, se deshacen los
enlaces que llegan a él (el `b` vuelve a ser `nop`) y, si fue durante la
ejecución, el código sale al despachador (`rec_abort`). La caché se vacía
entera cuando se llena, cuando cambia Status.FR o con un reset.

**Pendiente para la Fase 2:** el DMA del SP (que aún no existe) tiene que
llamar a `salsa64_bus_rdram_written` al escribir la RDRAM.

### Enlaces

Una salida a dirección fija deja `nop ; lis r3 ; ori r3 ; bl exit_link`. La
primera vez vuelve a C; el despachador encuentra o compila el destino y
cambia el `nop` por un `b` directo. Un salto al propio bloque vuelve al
cuerpo (después de escribir los registros modificados y con la misma
prueba del próximo evento), sin recargar nada. JR/JALR vuelven siempre a C
(pendiente: caché de destinos en línea).

### Caché de código

PS3: cola NOBITS de 16 MB al final del segmento de texto
(`mk/rec-cache.ld`, docs/jit-ppe.md). Las funciones de C quedan a menos de
32 MB: el código las llama con `bl` directo. `struct salsa64_rec` (mapa,
banderas, bloques, enlaces y búferes de compilación) ocupa ~7 MB y se
reserva una vez (docs/memory-budget.md).

## Pruebas en el host

RPCS3 no ejecuta código generado (docs/rpcs3.md). En la Mac se compila el
núcleo para Linux ppc64 big-endian (misma ABI ELFv1 que PSL1GHT) y se corre
con qemu-user:

```
tools/docker-image-ppc64.sh            # una vez: compilador cruzado y qemu
tools/test-rec.sh                      # todo lo de abajo
tools/build-ppc64.sh                   # build/ppc64/salsa64-unit y salsa64-run
tools/qemu-ppc64.sh build/ppc64/salsa64-unit rec
tools/qemu-ppc64.sh build/ppc64/salsa64-run --rec rom.z64
tools/qemu-ppc64.sh build/ppc64/salsa64-run --lockstep rom.z64
```

- `salsa64-unit rec`: las pruebas unitarias otra vez con el recompilador en
  **modo estricto** (nunca interpreta lo que se puede compilar: si un bloque
  no cabe antes del evento, lo compila recortado), así cada `t_step(n)`
  ejecuta código generado.
- `salsa64-run --lockstep`: una segunda máquina con el intérprete; después
  de cada bloque (sin enlaces) la lleva al mismo ciclo y compara CPU, MI y
  cada N bloques la RDRAM. Es el "modo traza" del criterio de la Fase 3.
- La caché del host va en `.bss` con `mprotect` para que las llamadas sean
  `bl` como en la PS3; `SALSA64_REC_FAR=1` usa un `mmap` lejano y prueba el
  otro camino (`mtctr`/`bctrl`). `SALSA64_REC_DUMP=1` vuelca cada bloque.

qemu 7.2 con `-cpu 970` da resultados erróneos (el intérprete falla 15
pruebas que pasan en la PS3) y con `power5+` se cuelga: se usa `power7`. El
emisor se limita a Power ISA 2.02 a mano (sin `isel`, `popcntd`, `ldbrx`).

El compilador cruzado corre emulado como amd64 y a veces muere con "internal
compiler error: Segmentation fault"; repetir compila bien
(`tools/build-ppc64.sh` reintenta una vez).

## Resultados

| Fecha | Prueba | Entorno | Resultado |
|---|---|---|---|
| 2026-10-10 | Unitarias, intérprete | qemu-ppc64 (power7) | 162/162 |
| 2026-10-10 | Unitarias, recompilador estricto (`bl` y `mtctr`) | qemu-ppc64 | 162/162; 223 bloques, 155 recortados, 38 instrucciones interpretadas |
| 2026-10-10 | nemu64-test, `--rec` | qemu-ppc64 | 462 fallas, las mismas de la línea base; 1,0 % interpretado |
| 2026-10-10 | nemu64-test, `--lockstep` | qemu-ppc64 | sin diferencias en 28,5 M bloques |
| 2026-10-10 | vitest, `--rec` | qemu-ppc64 | `vitest: OK` |

Rendimiento: solo en la PS3 real (docs/perf-log.md).
