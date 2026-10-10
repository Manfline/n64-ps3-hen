# Recompilador MIPS → PPC64 en el PPE (Fase 3)

Código: `src/cpu/rec.h`, `src/cpu/rec.c` (despachador, caché de bloques,
enlaces, invalidación), `src/cpu/rec_emit.c` (traducción), `src/cpu/rec_tlb.c`
(código fuera de KSEG0: KSEG1 y TLB), `src/cpu/ppc.h` (codificación de
instrucciones PowerPC).

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

- modo kernel, sin interrupción ni delay slot pendientes;
- la instrucción está en la RDRAM (por KSEG0, KSEG1 o la TLB) o en la ROM
  del cartucho (por KSEG1 o la TLB; ver «Código fuera de KSEG0»);
- cabe entero antes del próximo evento: `cycles + n ≤ sched->next` (lo
  revisa el propio bloque al entrar, también al llegar por un enlace).

Si no, el intérprete avanza hasta terminar un bloque básico (hasta un delay
slot o una excepción) y el despachador vuelve a intentarlo. Así cada evento
se atiende en el mismo ciclo que con el intérprete solo, y Count, Random y
VI_V_CURRENT dan lo mismo. Modo usuario o supervisor, código en otra memoria
(SP DMEM, PIF), direcciones de 64 bits y kuseg con ERL: intérprete.

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

Al entrar se cargan los registros que el bloque lee antes de escribir. Cada
registro modificado se escribe en `cpu->gpr` apenas recibe su último valor
del bloque (no todos juntos a la salida): así el bloque siguiente no lo lee
mientras el almacenamiento sigue en la cola (load-hit-store, decenas de
ciclos en el PPE). Los que no entran en los 12 van y vienen de `cpu->gpr`
en cada uso.

Opción `SALSA64_REC_OPT_PIN` (activa en el frontend; +7 % en nemu64-test
en la PS3): sp, ra, v0, v1 y a0–a3 viven en r18–r25 todo el tiempo que
corre código generado; la entrada los carga y la salida a C los escribe, y
antes de cada llamada al intérprete se escriben los que la instrucción
toca. Quedan 4 registros por bloque (r14–r17), poco para un bucle con
muchos temporales (bench enteros 750 → 128 MIPS): un bucle que necesita
más toma prestados los fijos que no usa (los guarda al entrar y los repone
en cada salida) y vuelve a tener hasta 12.

Opción `SALSA64_REC_OPT_EAGER` (apagada): escribir cada registro dos
instrucciones después de su último valor del bloque, contra el
load-hit-store del bloque siguiente. Medido en la PS3 no compensa (−2,6 %
en nemu64-test); escribirlo justo después fue peor (−37 % en un bucle de
enteros: el almacenamiento espera al valor y detiene el pipeline).

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
  CACHE, traps, COP0, CFC1/CTC1, y de la FPU SQRT, conversiones y redondeos;
- excepciones (SYSCALL, BREAK, desbordamiento).

En nativo: ALU de 32 y 64 bits (con desbordamiento), desplazamientos,
MULT/DIV y variantes de 64 bits, HI/LO, saltos, cargas y almacenamientos de
enteros y de COP1, MFC1/MTC1/DMFC1/DMTC1, BC1T/F, y en S y D
ADD/SUB/MUL/DIV, MOV, ABS, NEG y C.cond.

La FPU nativa repite el camino rápido del intérprete (`fpu.h`): las mismas
operaciones de doble precisión en el mismo orden (suma, redondeo a simple
con `frsp`, TwoSum o fma para el inexacto), así que da los mismos bits y
las mismas causas y banderas de FCR31. Lo que ese camino no cubre (redondeo
dirigido, I habilitada, operandos fuera de la franja segura, NaN,
desnormales, sumas en S con exponentes a más de 28) salta a la sección fría
y lo hace el intérprete. La prueba `executed_matches_full` ejecuta 6000
instrucciones aleatorias por el CPU contra el camino completo.

### Memoria

Camino rápido: `va >> log2(rdram)` vale lo mismo para KSEG0 y KSEG1 tras un
`ori`; junto con los bits de alineación da una sola comparación. Los
almacenamientos además miran `code_flags[pa >> 7]`: si hay código
recompilado en esos 128 bytes, van por C, que invalida.

### Bucles de espera

El intérprete salta vueltas enteras de los bucles de espera (`idle.c`,
consultado en cada salto corto hacia atrás). El código generado hace la
misma consulta: un salto que el intérprete consultaría (condicional, de
enlace, BC1 o J, tomado hacia atrás a menos de 8 instrucciones) y que no
esté en la tabla de descartados sale a C en el camino tomado, antes del
delay slot y con el mismo estado (registros en memoria, cycles con el salto
contado). El despachador llama a `salsa64_vr4300_idle` igual que el
intérprete; si la tabla lo descarta, invalida el bloque y se recompila sin
la consulta. Así los ciclos saltados y las tablas quedan idénticos a los del
intérprete (las pruebas de `test_idle.c` pasan en los dos), y un bucle de
espera de verdad cuesta una salida a C por evento. Cambiar la detección
(`salsa64_cpu_set_idle`) vacía la caché.

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
prueba del próximo evento), sin recargar nada. JR/JALR buscan el destino en
el propio código (`exit_jr`): en KSEG0 por `map[]`, fuera de KSEG0 la
primera de su cabeza de `xhash` si tiene la misma vpc; si no hay bloque,
vuelven a C.

### Código fuera de KSEG0 (KSEG1 y TLB) — decisión del 2026-10-10

GoldenEye y Perfect Dark (juegos objetivo) corren casi todo su código
mapeado por la TLB en kuseg (0x7F000000). El código de un bloque depende de
su dirección virtual (`cur_pc`, el enlace de JAL, los destinos de los saltos
y las salidas) y de la física (las instrucciones). Con la TLB, una física
puede tener varias virtuales y una virtual cambia de física al reescribir la
TLB (TLBWI/TLBWR) o el ASID.

Opciones: invalidar los bloques mapeados en cada escritura de la TLB, o
dar a cada bloque la clave (vpc, física) y verificar la traducción al
entrar. Se eligió la segunda: los juegos que paginan con la TLB la
reescriben en cada falla (refill), y con la primera cada TLBWR mataría los
bloques de la página que sale y habría que recompilarlos cuando vuelve.
Visto en la emulación (2026-10-10): GoldenEye pagina su código bajo
demanda (fallos de TLB al hacer fetch en 0x7F0xxxxx, páginas de 4 KB que
trae a la RDRAM, ~0x300000–0x3A0000; 21 entradas en uso) y Perfect Dark
mapea 0x70000000 con una entrada de páginas de 4 MB y su segmento en
0x7F000000 con 15 entradas de 128 KB. Los dos ejecutan desde la RDRAM; el
código en la ROM se admite igual (KSEG1 o un mapeo a la ROM).

- **Clave:** los bloques de KSEG0 siguen en `map[]` (por física) sin
  cambios. Los demás van en `rec->x.xhash` (16 384 cabezas, por vpc)
  encadenados, con la vpc en `x.xb[]`; puede haber varios por vpc con
  físicas distintas. Solo vpc con extensión de signo de 32 bits. Todo eso
  vive en `src/cpu/rec_tlb.c` y `struct salsa64_rec_x` (`rec_tlb.h`); rec.c
  y rec_emit.c solo lo llaman.
- **Sin cruzar páginas de 4 KB** (la página mínima del VR4300): todo el
  bloque comparte la traducción. Un salto cuyo delay slot cae en la página
  siguiente queda fuera del bloque, como los demás que no caben.
- **TLB del recompilador (`vtlb[]`):** 4096 entradas por página virtual de
  4 KB (bits 23–12); el valor es el bit 31, los bits 31–24 de la virtual y
  la página física, o 0. Un bloque mapeado empieza con
  `lwz r0, vtlb[e](r28) ; xoris ; cmplwi ; bne-` (`vtlb[]` va justo antes de
  `flags`, a desplazamiento negativo de r28): si su página ya no traduce a
  su física, sale al despachador. También al llegar por un enlace o por
  `exit_jr`, así que los enlaces nunca se deshacen por la TLB.
- **Llenarla:** el despachador traduce la página con
  `salsa64_vr4300_fetch_probe` (la búsqueda de la TLB sin excepciones ni
  efectos) cuando no está en `vtlb[]`, y la anota.
- **Vaciarla (`salsa64_rec_x_sync`):** antes de correr cualquier bloque (KSEG0 enlaza
  con mapeados), si cambió `cpu->tlb_writes` (sube con TLBWI, TLBWR y el
  reset), el ASID o ERL, vuelve a traducir cada página anotada (pocas) y
  borra las que cambiaron. Basta porque todo cambio de la TLB pasa por C y
  vuelve al despachador antes del bloque siguiente: COP0 termina el bloque,
  ERET y las excepciones salen con `redirect`.
- **Remapeo:** el bloque de la física vieja se queda (si la página vuelve,
  sirve de nuevo); el despachador desenlaza los de la misma vpc con otra
  física cuando la busca (`salsa64_rec_x_find`), para que sus enlaces se vuelvan a hacer
  con el bloque que vale.
- **Escrituras:** los de la RDRAM cuentan en `code_flags` y van también en
  `x.xpage` (por página física); `salsa64_rec_invalidate` los encuentra ahí.
  Los de la ROM nunca se invalidan (solo lectura); el ISViewer, que sí se
  escribe, no se compila.
- **Bucles de espera:** la salida lleva la instrucción del salto en
  `rec_site` (el despachador no relee memoria mapeada). `idle.c` analiza
  también código mapeado (traduce sin efectos con la misma búsqueda); si
  la tabla descarta el salto, el bloque se recompila sin la consulta
  (`salsa64_rec_x_kill_at`, por vpc).
- **Colisiones:** dos páginas a 16 MB caen en la misma entrada de `vtlb[]`:
  se turnan (cada cambio pasa por el despachador). Correcto, más lento.
- Memoria: +572 KB (`xb[]` 512 KB, `xhash` 32 KB, `vtlb` 16 KB, `vt_live`
  8 KB, `xpage` 4 KB).

### Caché de código

PS3: cola NOBITS de 16 MB al final del segmento de texto
(`mk/rec-cache.ld`, docs/jit-ppe.md). Las funciones de C quedan a menos de
32 MB: el código las llama con `bl` directo. `struct salsa64_rec` (mapa,
banderas, bloques, enlaces, la tabla de bloques fuera de KSEG0 y búferes de
compilación) ocupa 6,8 MB y se reserva una vez (docs/memory-budget.md).

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
| 2026-10-10 | Unitarias, recompilador estricto | **PS3 real** (CECH-3011A, HEN), f7b655d | 163/163; caché NOBITS, llamadas con `bl` |
| 2026-10-10 | nemu64-test, intérprete contra recompilador | **PS3 real**, f7b655d | mismas 462 fallas, mismo hash y mismo ciclo; 7,9 s → 1,8 s (**×4,36**, 104 MIPS) |
| 2026-10-10 | `bench` mixto | **PS3 real**, f7b655d | 21,3 → 156,6 MIPS (×7,4) |
| 2026-10-10 | nemu64-test, cuatro variantes | **PS3 real**, f416bee | registros fijos 1821 ms (×4,45); 213/213 en cada variante |
| 2026-10-10 | Frontend con vitest | **PS3 real**, f18b392 | 60 campos/s, 2,9–5,4 ms de emulación por campo (intérprete 35–66) |
| 2026-10-10 | **Zelda OoT** en el frontend | **PS3 real**, f416bee | 41–60 campos/s (intérprete 15–17); emulación 4–14 ms por campo; se ve igual |
| 2026-10-10 | Unitarias con código mapeado, KSEG1 y ROM; recompilador estricto (cuatro modos) | qemu-ppc64 | 222/222 |
| 2026-10-10 | nemu64-test, `--rec` y `--lockstep`, con KSEG1/TLB compilados | qemu-ppc64 | fallas de la línea base; 623 bloques fuera de KSEG0 (de 29 517), 27 traducciones; lockstep sin diferencias (también vitest) |
| 2026-10-10 | GoldenEye (USA), respaldo del usuario, `--lockstep` 200 M ciclos (2,1 s) | qemu-ppc64 | sin diferencias; 3518 de 3524 bloques mapeados, 5050 traducciones (la TLB cambia seguido), 0,1 % interpretado |
| 2026-10-10 | Perfect Dark (USA), respaldo del usuario, `--lockstep` 200 M ciclos | qemu-ppc64 | sin diferencias; 198 de 202 bloques mapeados, 3 traducciones (con 8 MB, pocas entradas grandes) |

Rendimiento: solo en la PS3 real (docs/perf-log.md, «Recompilador»).
