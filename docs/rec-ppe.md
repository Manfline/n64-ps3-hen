# Recompilador MIPS → PPC64 en el PPE (Fase 3)

Código: `src/cpu/rec.h`, `src/cpu/rec.c` (despachador, caché de bloques,
enlaces, invalidación), `src/cpu/rec_emit.c` (traducción),
`src/cpu/rec_emit_ext.h` (conversiones de la FPU, CFC1/CTC1, LWL/LWR y
compañía, MFC0, CACHE; lo incluye rec_emit.c), `src/cpu/rec_tlb.c` (código
y datos fuera de KSEG0: KSEG1 y TLB), `src/cpu/rec_prof.c` (perfil por
causa), `src/cpu/ppc.h` (codificación de instrucciones PowerPC).

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
VI_V_CURRENT dan lo mismo. Tras una excepción o un ERET dentro del bloque
(`redirect`) el despachador busca el bloque de la dirección nueva como
cualquier otro (hasta el 2026-10-10 tras un ERET interpretaba hasta el fin
del bloque básico: en GoldenEye, ~400 ERET por campo del refill de la TLB). Modo usuario o supervisor, código en otra memoria
(SP DMEM, PIF), direcciones de 64 bits y kuseg con ERL: intérprete.

### Bloques

Desde `pc` hasta un salto con su delay slot, una instrucción de COP0 que
no sea MFC0/DMFC0, SYSCALL o BREAK (siempre vuelven a C), o 64
instrucciones. Después de la instrucción de COP0 (la corre el intérprete
desde el código, `rec_h_exec`) el bloque **enlaza** con el siguiente como
cualquier salida a dirección fija: `rec_h_exec` hace ahí lo que el
despachador haría antes del bloque siguiente (revisar la TLB del
recompilador, `salsa64_rec_x_sync`, y pedir vuelta al despachador con
`rec_abort` si cambió Status.FR) y la revisión de después de la llamada
mira el modo, las interrupciones y el evento. Hasta el 2026-10-10 cada COP0
salía al despachador y el intérprete corría el resto del bloque básico:
GoldenEye hacía así 13 000 instrucciones interpretadas por campo (su refill
de la TLB, ~400 por campo, tiene cuatro COP0 seguidos). Un salto cuyo delay slot es otro salto o una de esas
instrucciones de COP0 queda fuera del bloque (lo corre el intérprete). La condición se calcula antes del delay slot en `cr4` (no
volátil, sobrevive a las llamadas a C); branch-likely no tomado salta el
delay slot y cuenta un ciclo menos.

### Saltos condicionales internos (superbloques, 2026-10-11)

GoldenEye ejecutaba 5,3 instrucciones por bloque (perfil por bloque,
pesado por ejecuciones): cada `if` y cada salida de bucle cortaban el
bloque, y cada corte cuesta la prueba del evento, la escritura y recarga de
registros por `cpu->gpr` (load-hit-store en el PPE) y el enlace. Ahora un
salto condicional **hacia adelante** (BEQ, BNE, BLEZ, BGTZ, BLTZ, BGEZ,
BC1F/T y sus likely) no termina el bloque: su delay slot se emite en línea
y el camino **tomado** sale enlazado al destino (registros modificados
escritos, `cycles` hasta el delay slot), mientras el no tomado sigue en el
mismo bloque con los registros en el PPE (`emit_internal_branch`). El
bloque termina en un salto hacia atrás (al inicio, bucle; u otro), en
J/JAL/JR/JALR, en una instrucción que vuelve a C o a las 64
instrucciones. Los tres bucles más calientes de GoldenEye (comparación de
cadenas, búsqueda en una tabla) eran parejas de bloques y pasan a ser uno.

Detalles que mantienen la exactitud:
- Cada salto guarda su descripción en `c->br[k]` y su delay slot se marca
  en `ds_of[]`: una salida a C desde ese delay slot continúa según su salto
  (`emit_continue`, con la condición en cr4, que las llamadas a C no tocan).
- Un branch-likely interno no tomado salta su delay slot y cuesta un ciclo
  menos: se descuenta de r29 (`addi r29, r29, -1`) para que `cycles =
  r29 + k + 1` siga valiendo en el resto del bloque; y el registro que
  escribe el delay slot se carga a la entrada (livein), para que la
  escritura de registros al salir lo deje con el valor de antes en ese
  camino.
- Los pares LWL/LWR y SWL/SWR no cruzan un salto.
- Solo el salto final puede ser candidato a bucle de espera (idle.c); los
  internos son hacia adelante.
- `SALSA64_REC_DBG_NO_SUPER` (y LEGACY) vuelven a cortar en cada salto.

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
- camino lento de cargas y almacenamientos (E/S, TLB sin anotar en `ktlb[]`,
  fuera de la RDRAM, código en la zona escrita);
- instrucciones raras o con mucho estado, que ejecuta el intérprete
  (`salsa64_vr4300_exec_one`): LL/SC, traps, COP0 salvo MFC0/DMFC0
  (y MFC0 de Random), CVT.S/D.L, y los caminos lentos de lo que se traduce
  (operandos fuera de la franja, redondeo dirigido, causas habilitadas);
- excepciones (SYSCALL, BREAK, desbordamiento).

En nativo: ALU de 32 y 64 bits (con desbordamiento), desplazamientos,
MULT/DIV y variantes de 64 bits, HI/LO, saltos, cargas y almacenamientos de
enteros y de COP1, LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR, MFC0/DMFC0,
MFC1/MTC1/DMFC1/DMTC1, CFC1/CTC1, BC1T/F, en S y D ADD/SUB/MUL/DIV, SQRT,
MOV, ABS, NEG, C.cond, ROUND/TRUNC/CEIL/FLOOR a W y L, CVT.W/CVT.L,
CVT.D.S y CVT.S.D, CVT.S.W/CVT.D.W, y CACHE (sin cachés emuladas, en modo
kernel solo falta la dirección sin alinear a 4: esa la lanza el intérprete;
GoldenEye hace ~1000 por campo).

`rec->stats.exec_calls` cuenta las instrucciones que el código generado le
pasa al intérprete (salsa64-run, cpu-tests y las unitarias lo muestran).
`decode()` sigue marcando lo de `rec_emit_ext.h` como antes y
`decode_ext()` lo promueve a nativo; `SALSA64_REC_OPT_RARE_EXEC` se salta
la promoción y repite lo de antes del 2026-10-10 (conversiones, SQRT,
CFC1/CTC1, LWL/LWR y compañía al intérprete, MFC0 terminando el bloque),
para medir la diferencia con el mismo binario.

La FPU nativa repite el camino rápido del intérprete (`fpu.h`): las mismas
operaciones de doble precisión en el mismo orden (suma, redondeo a simple
con `frsp`, TwoSum o fma para el inexacto), así que da los mismos bits y
las mismas causas y banderas de FCR31. Lo que ese camino no cubre (redondeo
dirigido, I habilitada, operandos fuera de la franja segura, NaN,
desnormales, sumas en S con exponentes a más de 28) salta a la sección fría
y lo hace el intérprete. La prueba `executed_matches_full` ejecuta 6000
instrucciones aleatorias por el CPU contra el camino completo.

Conversiones y raíz (2026-10-10), con la misma regla: lo que el camino
nativo no puede dar con los mismos bits y causas de `cop1.c` va al
intérprete. Clasifican el operando con sus bits en enteros (exponente en
una franja, o ±0) y nunca pasan un valor entre registros enteros y de
punto flotante:
- SQRT: positivo normal o ±0, redondeo al más cercano, I deshabilitada.
  `fsqrt` (el PPE la tiene); I según `frsp` ≠ raíz (en S) o el residuo
  a − r·r de `fnmsub`, que es lo que hace `cop1.c` con fma.
- CVT.D.S: `lfs` + `stfd`, exacto y sin causa. CVT.S.D: 2⁻¹²⁶ ≤ |x| < 2¹²⁷
  (ni desborda ni queda diminuto), `frsp` e I si cambió.
- ROUND/TRUNC/CEIL/FLOOR a W y L, y CVT.W/CVT.L con redondeo al más
  cercano: solo exponentes con los que ningún modo se sale del rango (W:
  |x| < 2³¹ en S, < 2³⁰ en D; L: < 2⁵³). `fctid` (el FPSCR siempre está al
  más cercano par, como `fpu_round`) o `fctidz`; CEIL y FLOOR corrigen el
  truncado en ±1 (exacto); I si el entero vuelto a double (`fcfid`) difiere.
  El resultado va de `stfd` a `cpu->fpr`, que es su destino.
- CVT.S.W y CVT.D.W en enteros: el entero está en `cpu->fpr` y llevarlo a
  un registro de punto flotante pide escribirlo y releerlo (load-hit-store;
  ppu-gcc hace eso con `(double)x`). Se normaliza |x| con `cntlzw` y un
  `slw` (microcódigo, uno solo) y, en S, se redondea a 24 bits al más
  cercano par sumando 0x7F más el bit que queda abajo.
- CFC1 lee FCR31 (o FCR0); CTC1 escribe los bits escribibles y, si una
  causa queda con su excepción habilitada (E siempre), deja la instrucción
  al intérprete, que la lanza.

La prueba `executed_conversions_match_full` (8000 instrucciones: SQRT, las
conversiones con enteros con fracción, empates, bordes de 2³¹ y 2⁵³ y de la
franja de CVT.S.D, CFC1/CTC1) compara el CPU con el camino completo y, con
el recompilador, exige que más de 3000 hayan corrido sin el intérprete.

### Cargas y almacenamientos no alineados

LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR sueltos: en la RDRAM desde KSEG0/KSEG1 (y,
al escribir, sin código en esos 128 bytes), el desplazamiento sale de la
dirección y la mezcla usa desplazamientos por registro (microcódigo en el
PPE, pero mucho menos que una llamada al intérprete); los almacenamientos
leen la palabra, la mezclan y la escriben. Un LWL y un LWR (o SWL y SWR)
consecutivos, en cualquier orden, sobre los mismos 4 bytes (`off` y
`off + 3`, misma base y mismo rt; en las cargas, la base no es rt) son un
acceso de 4 bytes sin alinear, como los escribe el compilador: se hacen
byte a byte (4 `lbz` y 3 `rlwimi`, o 4 `stb`), sin desplazamientos por
registro y sin leer la palabra antes de escribirla (en una copia, el par
siguiente escribe la palabra que el anterior acaba de escribir:
load-hit-store). Fuera de la RDRAM, con TLB o con código, el intérprete
corre las dos.

### MFC0

MFC0 y DMFC0 ya no terminan el bloque: en modo kernel (el único en que
corre un bloque) no hay excepción posible, y cada registro es un campo de
`struct salsa64_cpu` (tabla en `rec_emit.c` con lo mismo que
`salsa64_cpu_cop0_read`). Count es `(cycles >> 1) + count_offset` con
`cycles = r29 + k + 1`, lo que el intérprete tiene al ejecutar la
instrucción k. Random lo corre el intérprete (sin terminar el bloque).

### Tamaño del código generado (2026-10-11)

Un bloque de GoldenEye con 13 instrucciones, dos saltos internos y dos
cargas daba 180 palabras de PPC (volcado con `SALSA64_REC_DUMP=1` en
`salsa64-run`; `tools/rec-disasm.sh` lo desensambla): media 316 palabras
por bloque, mediana 171, p90 789. Cada carga o almacenamiento costaba ~47
palabras frías: el camino lento (`call_prep`, argumentos, llamada, revisión
de `redirect` y `post_check`) y dos salidas pendientes (antes y después)
que repetían la escritura de los registros modificados. Con superbloques y
la caché de 16 MB, GoldenEye la llenaba cada 7,5 s (17 vaciados en 128 s).
Ahora:
- El camino lento de cargas y almacenamientos son 11 rutinas compartidas
  del preámbulo (`slow_load[7]`, `slow_store[4]`): el sitio escribe sus
  registros sucios (solo en ese camino, el rápido no cambia), pasa k, el
  delay slot y n en r6 y el pc en r7, y la rutina deja `cycles`, `cur_pc` y
  `delay_slot` como el intérprete, llama a C (guardando LR en
  `cpu->rec_lr`), recarga `sched->next` y vuelve con cr1 ne si hubo
  excepción o cr5 gt si hay que salir. Como lo sucio ya está en memoria,
  las salidas del sitio solo ajustan `cycles` (y el valor cargado va a su
  lugar antes de salir).
- La traducción de datos por `ktlb[]` también es compartida
  (`ktlb_rtn[2][4]`, por alineación y lectura/escritura).
- El preámbulo pasa de 256 a 1024 palabras.

Resultado (mismo volcado de GoldenEye, 4392 bloques): media 316 → 248
palabras por bloque, mediana 171 → 137, p90 789 → 621. Lo que queda grande
son las operaciones de la FPU (~55 palabras cada una, con su camino lento
al intérprete) y las salidas de los saltos internos. Con 24 MB, GoldenEye
vacía la caché 2 veces en 128 s (al cargar la presa); cada vaciado
recompila el conjunto vivo (~7000 bloques) a medida que se ejecuta.

### Memoria

Camino rápido: `va >> log2(rdram)` vale lo mismo para KSEG0 y KSEG1 tras un
`ori`; junto con los bits de alineación da una sola comparación. Los
almacenamientos además miran `code_flags[pa >> 7]`: si hay código
recompilado en esos 128 bytes, van por C, que invalida. Si la dirección no
es de KSEG0/KSEG1, en la sección fría se prueba `ktlb[]` (ver «Datos
mapeados por la TLB») y, si traduce a la RDRAM, se vuelve al camino rápido
con el desplazamiento ya calculado; lo demás va a C.

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
el propio código (`exit_jr`): en KSEG0 por `map[]`, fuera de KSEG0 recorre
su cadena de `xhash` hasta el primero con la misma vpc (hasta el 2026-10-10
solo miraba la cabeza: GoldenEye, con ~70 000 bloques en 16 384 cabezas,
salía a C en ~800 JR por campo); si no hay bloque, vuelven a C.

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
- **TLB del recompilador (`ktlb[]`, hasta el 2026-10-10 `vtlb[]`):** 16 384
  entradas por página virtual de 4 KB (bits 25–12); el valor es la etiqueta
  (bits 31–26 de la virtual), válida (bit 25), D (bit 24: se puede
  escribir) y la página física (17 bits), o 0. Un bloque mapeado empieza
  con `lwz r0, ktlb[e](r28) ; xoris ; cmplwi ; bne-` (con un `addis` antes
  si la entrada queda a más de 32 KB; `ktlb[]` va justo antes de `flags`, a
  desplazamiento negativo de r28) comparando con la palabra que tenía al
  compilarse: si su página ya no traduce a su física, sale al despachador.
  También al llegar por un enlace o por `exit_jr`, así que los enlaces
  nunca se deshacen por la TLB. La misma tabla la usan las cargas y los
  almacenamientos a direcciones mapeadas («Datos mapeados por la TLB»).
- **Llenarla:** el despachador traduce la página con
  `salsa64_vr4300_kernel_probe` (la búsqueda de la TLB sin excepciones ni
  efectos, con el bit D) cuando no está en `ktlb[]`, y la anota; el camino
  lento de las cargas y almacenamientos anota la suya al traducir con
  éxito; y al escribir la TLB se anotan las páginas de la entrada nueva
  (pares de hasta 16 páginas): así el bloque o el acceso que viene a la
  página recién traída no sale a C.
- **Vaciarla (`salsa64_rec_x_sync`):** antes de correr cualquier bloque (KSEG0
  enlaza con mapeados) y en `rec_h_exec` tras cada COP0 (el bloque enlaza
  con el siguiente sin volver al despachador). Si cambió `cpu->tlb_writes`
  (sube con TLBWI, TLBWR y el reset), compara las 32 entradas con una
  instantánea (`snap[]`) y borra de `ktlb[]` solo las páginas de las que
  cambiaron (el par de su VPN2, antes y después); si cambiaron el ASID o
  ERL, vuelve a traducir cada página anotada y borra las que ya no dan lo
  mismo. Hasta el 2026-10-10 cada cambio volvía a traducir todas las páginas
  anotadas: GoldenEye, que trae ~400 páginas por campo con el refill, hacía
  14 000 traducciones por campo (109 M en 128 s). Basta porque todo cambio
  de la TLB pasa por C: COP0 termina el bloque, ERET y las excepciones
  salen con `redirect`.
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
- **Colisiones:** dos páginas a 64 MB caen en la misma entrada de `ktlb[]`:
  se turnan (cada cambio pasa por C). Correcto, más lento. Con 16 384
  entradas el código de Perfect Dark (0x7F000000) y sus datos (0x70000000)
  no chocan; con las 4096 de antes sí (16 MB).
- Memoria: +2,2 MB (`xb[]` 512 KB, `ktlb` 64 KB, `live` 32 KB, `xhash`
  32 KB, `xpage` 4 KB, `snap` 0,75 KB; bloques desprendidos: `copy` 1 MB,
  `copy_off` y `saved_code` 256 KB cada uno, `state` 64 KB).

### Bloques desprendidos: la página vuelve en otro marco (2026-10-11)

GoldenEye pagina su código bajo demanda con un pool de marcos: la página
virtual X vuelve en el marco que toque, y el DMA que trae otra página al
marco invalida los bloques que había. Cada (vpc, marco) era un bloque nuevo:
101 900 bloques en 128 s con unos 21 000 pc distintos, 7 vaciados completos
de la caché (cada uno recompila todo lo vivo: ~15 000 bloques de golpe) y
12–22 compilaciones por campo. El código de un bloque depende solo de su
vpc y de sus bytes, así que:

- Al compilar un bloque mapeado en la RDRAM se guarda una copia de sus
  bytes MIPS (`x.copy[]`, 1 MB; sin copia, se comporta como antes).
- Cuando una escritura invalida un bloque con copia, se **desprende**
  (`salsa64_rec_x_detach`) en vez de matarlo: pierde enlaces entrantes,
  banderas de código y `code` (así `exit_jr` y el despachador lo saltan),
  sale de `xpage`, pero sigue en `xhash` con su copia y su código guardado.
- Cuando el despachador no encuentra bloque para (vpc, pa) mapeado,
  `salsa64_rec_x_rekey` busca en la cadena uno de la misma vpc (desprendido,
  o vivo en otra física) cuya copia coincida byte a byte con lo que hay en
  pa, y lo engancha ahí: banderas y `xpage` en la física nueva, `code` de
  vuelta y la palabra de `ktlb[]` que compara al entrar parcheada en su
  código (`xoris`/`cmplwi`). Sus enlaces salientes siguen valiendo (son por
  vpc) y los entrantes se rehacen solos.
- Un vaciado completo borra copias y estados; los ids de los desprendidos
  se liberan solo al vaciar (si se agotan, se vacía, como antes).

### Datos mapeados por la TLB (2026-10-10)

Perfect Dark tiene sus datos en una página de 4 MB en 0x70000000: en la
intro hace ~3200 cargas y ~200 almacenamientos por campo a direcciones
mapeadas, cada uno una llamada a C con la búsqueda lineal de la TLB. (En
GoldenEye solo el código es mapeado; sus datos van por KSEG0.) Cuando la
dirección no es de KSEG0/KSEG1, el código generado prueba `ktlb[]` en la
sección fría: la dirección es extensión de signo de 32 bits (las de 64
bits no se anotan) y está alineada; la entrada `(va >> 12) & 16383` tiene
la etiqueta de `va` y es válida (y D, al escribir: una página de solo
lectura va a C, que lanza la excepción de modificación); y la física
(página de la entrada más los 12 bits bajos) cae en la RDRAM. Entonces
vuelve al camino rápido con el desplazamiento en r12 (los almacenamientos
miran `code_flags` igual que los de KSEG0). Unos 20 instrucciones contra
los cientos de ciclos de la llamada. Lo demás (sin anotar, E/S, excepción)
va a C, que anota la página al traducir con éxito.

Es exacto porque la entrada de `ktlb[]` es función de la TLB del CPU (la
misma búsqueda que el intérprete: `salsa64_vr4300_kernel_probe`, primera
entrada que coincide, V y D de la mitad que toca, ASID o global) y se borra
antes de correr código si esa entrada cambió, y porque un bloque solo corre
en modo kernel (Status.RE no aplica; con KX la traducción de una dirección
de 32 bits es la misma). Los pares LWL/LWR y SWL/SWR byte a byte exigen
además que los 4 bytes queden en la misma página (`(va & 0xFFF) <= 0xFFC`):
la traducción es la del primero.

### Perfil por causa (`rec_prof.h`, 2026-10-10)

Con `rec->prof` puesto, el despachador y las llamadas a C desde el código
generado miden con el timebase del PPE (`mftb`, 79,8 MHz en la PS3) y
cuentan por causa: código generado (cada entrada menos sus llamadas a C),
cargas y almacenamientos lentos (TLB a la RDRAM, TLB a E/S, falla de TLB,
E/S de KSEG0/1, código en la zona escrita, error de dirección),
instrucciones al intérprete (COP0, FPU, LL/SC, CACHE, traps, otras),
excepciones, el intérprete en el despachador por motivo (bloque que salió
en la entrada o a mitad, interrupción, delay slot, modo, segmento, sin
código, no cabe), los eventos por tipo (el del RSP incluye el sondeo y lo
que duerme esperando al SPE), compilar, revisar `ktlb[]`, bucles de espera y
las salidas por tipo. Apagado cuesta una carga y un salto por llamada a C.
En la PS3, el archivo `SALSA64_DATA_DIR/perfil` lo enciende y el desglose
(ms por campo) sale con las estadísticas cada 5 s. En el host,
`SALSA64_REC_PROF=N` lo escribe cada N campos (solo cuentas: qemu no mide
el Cell) y al final lista los bloques más ejecutados con sus palabras MIPS
(`tools/mips-disasm.py` las lee) y el uso de registros pesado por
ejecuciones.

### Caché de código

PS3: cola NOBITS de 24 MB (16 hasta el 2026-10-11: GoldenEye la llenaba cada
~40 s, con un vaciado completo cada vez; ~900 bytes de PPC por bloque de 5
instrucciones, sobre todo caminos fríos de memoria, que ahora llaman a
rutinas compartidas del preámbulo para la `ktlb`) al final del segmento de texto
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
SALSA64_ROMS=/ruta/a/respaldos \
  tools/qemu-ppc64.sh build/ppc64/salsa64-run --lockstep /roms/goldeneye.z64
```

Los respaldos de juegos van fuera del repo, en una carpeta que
colima comparta (no todas lo están); `SALSA64_ROMS` la
monta de solo lectura en `/roms`.

- `salsa64-unit rec`: las pruebas unitarias otra vez con el recompilador en
  **modo estricto** (nunca interpreta lo que se puede compilar: si un bloque
  no cabe antes del evento, lo compila recortado), así cada `t_step(n)`
  ejecuta código generado.
- `salsa64-run --lockstep`: una segunda máquina con el intérprete; después
  de cada bloque (sin enlaces) la lleva al mismo ciclo y compara CPU, MI y
  cada N bloques la RDRAM. Es el "modo traza" del criterio de la Fase 3.
  La referencia lleva su propia HLE gráfica y de audio (desde el
  2026-10-11): sin ellas no recibía la interrupción del DP de las tareas
  con HLE (G_RDPFULLSYNC) y GoldenEye y Perfect Dark, que ya dibujan,
  divergían por una interrupción que a la referencia nunca le llegaba.
- La caché del host va en `.bss` con `mprotect` para que las llamadas sean
  `bl` como en la PS3; `SALSA64_REC_FAR=1` usa un `mmap` lejano y prueba el
  otro camino (`mtctr`/`bctrl`). `SALSA64_REC_DUMP=1` vuelca cada bloque.

qemu 7.2 con `-cpu 970` da resultados erróneos (el intérprete falla 15
pruebas que pasan en la PS3) y con `power5+` se cuelga. Se usaba `power7`;
desde el 2026-10-10 es `power9` (`SALSA64_QEMU_CPU` lo cambia): `sin`, `cos`
y `floor` de la glibc estática de ppc64 usan `mffscrn`/`mffscrni` (Power ISA
3.0), que qemu con `power7`/`power8` rechaza con SIGILL (se caía la suite
`sync`, por `audio_sync.c`; en la PS3 no hay glibc). Con `power9` las
pruebas, nemu64-test y vitest dan lo mismo que antes. El emisor se limita a
Power ISA 2.02 a mano (sin `isel`, `popcntd`, `ldbrx`).

El compilador cruzado corre emulado como amd64 y a veces muere con "internal
compiler error: Segmentation fault"; repetir compila bien
(`tools/build-ppc64.sh` reintenta una vez).

## Resultados

| Fecha | Prueba | Entorno | Resultado |
|---|---|---|---|
| 2026-10-10 | Unitarias, intérprete | qemu-ppc64 (power7) | 162/162 |
| 2026-10-10 | Unitarias, recompilador estricto (`bl` y `mtctr`) | qemu-ppc64 | 162/162; 223 bloques, 155 recortados, 38 instrucciones interpretadas |
| 2026-10-10 | nemu64-test, `--rec` | qemu-ppc64 | 462 fallas, las mismas de la línea base; 1,0 % interpretado |
| 2026-10-10 | `tools/test-rec.sh` con `-cpu power9` (main f6fed36) | qemu-ppc64 | unitarias 289/289 en los cuatro modos (con `power7` se caía la suite `sync`); nemu64-test 446 fallas, las de la línea base, también con pin+eager; vitest OK; `--lockstep` sin diferencias |
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
| 2026-10-10 | Unitarias con conversiones, LWL/LWR y MFC0 en nativo | qemu-ppc64 (plain, mtctr, fijos, temprana) | 221/221; `--lockstep` sin diferencias en nemu64-test y vitest; mutación 10/10 |
| 2026-10-10 | nemu64-test y bench con y sin `RARE_EXEC` (conversiones, LWL/LWR y MFC0 al intérprete) | **PS3 real**, e210aeb | nemu64-test 1894 → 1816 ms (−4,1 %), mismas fallas; bench cvt ×9,7, no alineadas ×10,7, mfc0 ×71, espera por Count ×59 |
| 2026-10-11 | `tools/test-rec.sh --lockstep` con enlace tras COP0, ktlb de datos, bloques desprendidos, superbloques y caminos lentos compartidos | qemu-ppc64 | unitarias 351/351 en los cuatro modos; nemu64-test 433 fallas (línea base) con y sin pin+eager; vitest OK; `--lockstep` sin diferencias en nemu64-test y vitest |
| 2026-10-11 | `--lockstep` con la referencia con HLE: Perfect Dark 3 G ciclos (intro); GoldenEye con `goldeneye-dam.pad` 7,5 G ciclos (80 s de consola, hasta jugar en la presa) | qemu-ppc64 | **sin diferencias** en los dos; GoldenEye 283 M salidas comparadas en 1 h 46 min, 16 606 bloques (17 031 enganchados de vuelta) |

Rendimiento: solo en la PS3 real (docs/perf-log.md, «Recompilador»).
