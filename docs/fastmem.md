# Fastmem en el PPE: viabilidad y diseño

Fastmem es que un acceso a memoria de la N64 se traduzca a una carga o un
almacenamiento del PPE con base + desplazamiento, sin comprobar antes el
segmento, el tamaño de la RDRAM ni la alineación: la RDRAM vive en una
ventana de direcciones del PPE donde lo que no es RDRAM no está mapeado, y
el acceso que cae fuera **falla** y lo atiende un camino lento. Documento de
trabajo de la rama de fastmem (2026-10-13): qué se gana, lo que dice el
censo de accesos, lo que lv2 permite (y lo que hay que preguntarle a la
consola), el diseño y la prueba de viabilidad.

## 1. Qué cuesta hoy y qué se espera ganar

Con el código compacto (v0.2.2, docs/rec-ppe.md «Código compacto») cada
acceso a memoria del código generado paga, en la sección caliente:

| | Hoy | Con fastmem |
|---|---|---|
| Carga (LW) | `addi` (si hay desplazamiento) + prueba de KSEG0 `oris addis rldicl cmpdi bne` + `lwzx` + `extsw` = 7–8 palabras | `addi` + `rlwinm` + `lwzx` + `extsw` = 3–4 |
| Almacenamiento (SW) | `addi` + prueba (5) + `code_flags` (`srdi lbzx cmpwi bne`) + `stwx` = 10–11 | `addi` + `rlwinm` + `code_flags` (4) + `stwx` = 6–7 |

GoldenEye hace 0,23 cargas y 0,10 almacenamientos por instrucción MIPS
(docs/cache-ppe.md §2): fastmem quita **~1,3 de las 8,2 palabras calientes
por instrucción (−16 %)** y cuatro instrucciones ejecutadas por acceso. Como
el bench de huella dice que el fetch de código es ~37 de los ~40 ciclos de
espera (docs/perf-log.md, «Bench de huella»), la estimación es **5–10 % del
nativo**, del orden de lo que dio el código compacto (−6…−7 %).

Después, con la ventana ya en pie: **base en caché**. Lo normal en código
de libultra es `lw t0, 0(s0); lw t1, 4(s0); lw t2, 8(s0)`: si el bloque
calcula una vez `T = ventana + (s0 & máscara)` y lo conserva mientras s0 no
cambie, cada acceso siguiente es `lwz d, 4(T)` + `extsw`: 1–2 palabras. Es
lo que hacen los recompiladores con fastmem de otros emuladores.

## 2. Censo de accesos a datos (host, intérprete, 2026-10-13)

`salsa64-run` compilado con `-DSALSA64_MEM_CENSUS` (cuenta en el intérprete
por segmento y, para lo que pasa por la TLB, por tamaño de página; lo
imprime al terminar). 11 juegos, 5–8 G ciclos cada uno (GoldenEye hasta la
presa con `docs/pads/goldeneye-dam.pad`, Perfect Dark la intro, SM64 hasta
el castillo, Zelda hasta la pausa, MK64 y Banjo con sus pads; Conker, DK64,
Smash, Paper Mario y Mario Party el arranque):

| Juego | Accesos | KSEG0 | KSEG1 | Por la TLB | Páginas de la TLB (accesos) | Ranuras de kuseg tocadas |
|---|---|---|---|---|---|---|
| GoldenEye | 628 M | 99,72 % | 0,17 % | **0,10 %** (653 K) | 4 MB: todos; 4 KB: 0 (447 K escrituras de la TLB: el código paginado) | 0x70000000 |
| Perfect Dark | 527 M | 97,88 % | 0,14 % | **1,98 %** (10,4 M) | 4 MB: 9,2 M; 64 KB: 1,2 M | 0x70000000, 0x7F000000 |
| SM64 | 691 M | 99,88 % | 0,09 % | 0,03 % (201 K) | 64 KB | 0x04000000 |
| Conker | 40 M | 99,41 % | 0,59 % | 0,000 % (184) | 4 KB y 4 MB | 0x10000000, 0x15000000 |
| Zelda OoT, MK64, Banjo, DK64, Smash, Paper Mario, Mario Party | 0,4–1,3 G | 99,77–99,97 % | 0,03–0,23 % | 0 | — | — |

En todos: **0 accesos no alineados**, 0 en kseg2/kseg3, 0 de 64 bits, 0 con
ERL, y **0 colisiones** de una dirección por la TLB con la imagen de la
RDRAM al doblar la dirección a una ventana de 1 o 2 GB (sección 4).

Consecuencias:

- **Fastmem es para KSEG0 y KSEG1** (≥ 97,9 % de los accesos en todos los
  juegos). KSEG1 es la misma memoria física que KSEG0: con la máscara de 29
  bits los dos caen en la misma ventana sin espejo alguno.
- **No hace falta un espejo virtual de la TLB.** Lo mapeado por la TLB es el
  2 % de los accesos en Perfect Dark y el 0,1 % en GoldenEye (sus datos van
  por KSEG0; en 0x7F… solo hay código, y el código entra por la `ktlb` del
  bloque, no por un acceso a datos). Además lv2 mapea en páginas de 64 KB o
  1 MB y la N64 en páginas de 4 KB (GoldenEye pagina su código así): un
  espejo exacto no es posible con lv2 y el que es posible (páginas de ≥ 64
  KB) no vale el 2 %. El camino de la `ktlb` de hoy (~20 palabras frías, sin
  C) se queda para esos accesos.
- La comprobación de alineación puede salir del camino caliente (ningún
  juego la dispara); el intérprete y el camino lento la mantienen. Lo que
  la exige son las pruebas de excepciones (nemu64-test): con el interruptor
  apagado el código es el exacto de hoy.

## 3. Lo que lv2 ofrece (documentación pública) y lo que no se sabe

Fuentes: PSL1GHT (`sys/memory.h`, `lv2/memory.h`, `lv2/syscalls.h`,
`sys/thread.h`, `sys/event_queue.h`) y RPCS3 como documentación del
comportamiento de lv2 (`sys_mmapper.cpp`, `sys_ppu_thread.cpp`,
`Utilities/Thread.cpp`); nada del SDK de Sony.

| Pieza | Syscall | Qué hace | Lo que falta saber en la consola |
|---|---|---|---|
| Reservar direcciones | 330 `sys_mmapper_allocate_address(size, flags, align, &addr)` | Un rango de direcciones virtuales sin memoria; `size` múltiplo de 256 MB, `align` 256 MB–2 GB, `flags` el tamaño de página (64 KB o 1 MB) | Cuánto se puede reservar (512 MB, 1 GB), en qué direcciones, cuántas regiones |
| Región fija | 326 | 256 MB en 0xB0000000 (según RPCS3) | Si existe y si sirve |
| Memoria | 332 `sysMMapperAllocateMemory(size, flags, &id)` | Un objeto de memoria física (múltiplo de 64 KB o 1 MB) | Cuántos objetos; si se descuenta de los 209 MB (seguro que sí) |
| Mapear | 334 `sysMMapperMapMemory(addr, id, prot)` / 337 `SearchAndMap` / 335 `Unmap` | El objeto en una dirección de la región (alineada a su página) | µs por mapeo y desmapeo; si un objeto admite dos mapeos (espejo); si el contenido sobrevive al desmapeo |
| Permisos | 336 `sys_mmapper_change_address_access_right(addr, prot)` | Solo lectura o lectura y escritura | Si existe (en RPCS3 es un stub) |
| Atributos | 351 `sys_memory_get_page_attribute(addr, &attr)` | Tamaño de página y derechos | El formato |
| **Fallos de página** | 327 `sys_mmapper_enable_page_fault_notification(start_addr, cola)` | Un acceso a una página sin mapear de esa región no mata el proceso: el hilo queda suspendido y la cola recibe un evento (`source` = clave 0xFFFE000000000000, `data1` = dirección, `data2` = tipo << 32 \| id del hilo, `data3` = causa: 2 sin mapear, 1 solo lectura; según RPCS3) | **Si lv2 lo hace para un proceso de juego con HEN**, el contenido real del evento y el tiempo hasta que llega |
| Reanudar | 57 `sysThreadRecoverPageFault(tid)` | El hilo suspendido vuelve a ejecutar la instrucción que falló | Si repite la instrucción (y no la salta) |
| Contexto | 58 `sys_ppu_thread_get_page_fault_context(tid, &ctx)` | El contexto del hilo suspendido | **El formato y si trae el pc**: sin el pc no se sabe qué instrucción falló (en RPCS3 es un stub que no llena nada) |

lv2 no tiene señales ni manejadores de excepción en el hilo que falla: todo
pasa por **otro hilo** que recibe el evento, arregla lo que haga falta y
reanuda. Para fastmem eso significa que el arreglo tiene que dejar al hilo
listo para repetir la instrucción con éxito o, mejor, cambiar la
instrucción antes de que la repita (sección 4).

## 4. Diseño

### La ventana

Una región de **512 MB** reservada con 330 (páginas de 1 MB), con la RDRAM
(8 MB, u objeto de 8 × 1 MB) mapeada al principio y **nada más**. El índice
de un acceso es `va & 0x1FFFFFFF` (`rlwinm T, va, 0, 3, 31`: una palabra,
que además deja los 32 bits altos en cero):

| Dirección de la N64 | Índice | Qué hay |
|---|---|---|
| KSEG0 0x80000000 + x | x | la RDRAM si x < 8 MB; si no, sin mapear → fallo |
| KSEG1 0xA0000000 + x | x | lo mismo (misma memoria física: no hace falta espejo) |
| E/S (0xA4000000…), cartucho (0xB0000000…), PIF | ≥ 0x04000000 | sin mapear → fallo → camino lento |
| kuseg y (TLB) | y & 0x1FFFFFFF | sin mapear → fallo → camino lento, salvo **colisión**: y en [0, 8 MB), [0x20000000, +8 MB), [0x40000000, +8 MB) o [0x60000000, +8 MB) caería en la RDRAM sin pasar por la TLB |
| kseg2/kseg3 (TLB) | ídem | ídem |

Las colisiones no ocurren en ningún juego medido (sección 2), pero hay que
cubrirlas: al escribir la TLB (TLBWI/TLBWR, donde el recompilador ya revisa
sus traducciones) se mira si alguna entrada válida mapea una página de esas
ventanas; si sí, fastmem se apaga para esa sesión (se vacía la caché y se
compila con la prueba de hoy) y se registra en el log. Con el desplazamiento
de la instrucción (`addi` antes de la máscara, o base enmascarada + `lwz`
con desplazamiento) el análisis es el mismo: un acarreo que cruce el bit 29
solo puede llevar a una página sin mapear, nunca a la RDRAM por error.

No se comprueba la alineación: el PPE hace el acceso no alineado (lento, 55
ciclos si cruza 32 bytes) donde la N64 lanzaría AdEL. Ningún juego medido
lo hace; con el interruptor apagado el código es el de hoy.

### Los fallos

Un acceso que cae en una página sin mapear falla **una vez por sitio y por
compilación**, y después ese sitio corre por el camino de hoy:

1. El hilo de emulación falla; lv2 lo suspende y manda el evento a la cola
   del **hilo ayudante** (prioridad por encima del de emulación; duerme en
   la cola, no cuesta nada mientras no hay fallos).
2. El ayudante obtiene el **pc del acceso** (contexto, syscall 58) y
   comprueba que está en la caché de código. Busca el **camino frío del
   sitio** (una tabla sitio → camino frío que el emisor llena al compilar:
   hash abierto por dirección del sitio, ~8 bytes por acceso compilado, o
   por bloque desde `blocks[]`).
3. Reemplaza la instrucción de acceso por `b camino_frío` (una palabra;
   la caché de 32 MB está al alcance de `b`), `dcbst`/`sync`/`icbi`/`isync`
   sobre esa línea, y reanuda el hilo (57): el hilo repite la instrucción,
   que ahora es el salto.
4. El camino frío del sitio es el código de hoy entero: prueba de KSEG0 y
   del tamaño (5 palabras), el acceso con el desplazamiento verificado, la
   prueba de la `ktlb` para lo mapeado por la TLB y el camino lento a C para
   E/S y excepciones; termina con `b` a la instrucción que sigue al acceso
   (el `extsw`). Vive en la sección fría del bloque, como hoy.

Lo que no cambia: los almacenamientos siguen mirando `code_flags` (la
invalidación del código es por 128 bytes; proteger páginas de 64 KB contra
escritura haría fallar sin parar los datos que conviven con código), la
`ktlb` sigue para los datos mapeados por la TLB, el camino lento en C es el
mismo. Si el pc no viene en el contexto, la alternativa es que la entrada
de cada bloque deje su id (dos palabras, como el muestreo) y el ayudante
parche todos los sitios del bloque; más caro y menos fino, pero funciona.

Fallos que no son del hilo de emulación (un SPE con una dirección mala,
tipo 1 en `data2`): se registran y se reanudan con 198; hoy serían un cuelgue.

### En el host

La misma capa con `mmap` (512 MB `PROT_NONE` y la RDRAM `MAP_FIXED` dentro) y
un manejador de `SIGSEGV` con `SA_SIGINFO`, que parchea con el pc del
`ucontext` y vuelve: la instrucción se repite. Bajo qemu-user (los lockstep
corren ahí) el fallo del invitado llega como señal con su contexto, y la
escritura sobre la página de código la ve el traductor (invalida sus
bloques). Es la forma de correr `--lockstep` y las unitarias con fastmem.

### Interruptor y medición

`SALSA64_REC_OPT_FASTMEM` (`jit-fastmem` en la PS3 mientras sea
experimental, `SALSA64_REC_FASTMEM=1` en el host): el mismo paquete con y
sin. Lo que se mide: palabras calientes por instrucción en el host
(`tools/rec-expansion.py`), ns por instrucción del nativo en GoldenEye
(presa) y Perfect Dark contra v0.2.2 (16,0–16,3 y ~15), número de fallos por
sesión (contador en el ayudante) y µs por fallo. Lockstep de nemu64-test,
vitest, Perfect Dark y GoldenEye hasta la presa.

### Lo que viene después (pasos 3 y 4 del plan)

Con la ventana: la base en caché (sección 1). Independientes de fastmem:
más registros de la N64 vivos en registros del PPE entre bloques (hoy 4 por
bloque más los 8 fijos), y la FPU casi 1 a 1 donde la semántica coincide.
Se diseñan cuando fastmem esté medido.

## 5. La prueba de viabilidad: `tests/phase0/fastmem`

Un programa aislado (paquete SLSA00111 «Salsa64 fastmem»,
`tools/build.sh -C tests/phase0/fastmem pkg`), con el mismo archivo de
estado que jit-ppe: antes de cada paso arriesgado escribe `<fase> PROBANDO
<paso>` en `/dev_hdd0/salsa64/fastmem_state.txt`; si el proceso muere, al
relanzarlo esa fase queda como CRASH y sigue con la siguiente. **Hay que
relanzarlo hasta que el log diga `RESULTADO fastmem`**; si la aplicación se
queda colgada (un hilo suspendido en un fallo que nadie atiende), salir con
el botón PS y relanzar. Log en `/dev_hdd0/salsa64/fastmem.log` y por UDP.

| Fase | Pregunta |
|---|---|
| `reservas` | Qué tamaños y alineaciones acepta 330 (256 MB a 2 GB, 64 KB y 1 MB, con derechos de acceso), en qué direcciones, cuántas regiones de 256 MB a la vez, la región fija (326), atributos (351) de la región, del código y de 0xB0000000, memoria de usuario antes y después |
| `mapeo` | Asignar objetos de 64 KB y 1 MB; mapear, escribir, leer; µs de desmapear y mapear (50 y 20 veces); si el contenido sobrevive al desmapeo; primer toque de cada página de 4 KB de 1 MB recién mapeado; 1 MB en una región de 64 KB y al revés; **espejo** (el mismo objeto en dos direcciones); `search_and_map`; permisos (336) a solo lectura y de vuelta; cuántos objetos de 64 KB (hasta 256) y el tiempo de mapear 128 seguidos |
| `datos` | ns por lectura recorriendo 8 MB a paso 128 y a paso 4224 (fallos de ERAT) en el heap (páginas de 1 MB), en 128 objetos de 64 KB y en 8 de 1 MB; y 256 KB dentro de la L2 |
| `fallo_lectura` | Con 327 y una cola: un hilo lee una página sin mapear; el ayudante recibe el evento (se vuelca entero), pide el contexto (58; se vuelcan sus palabras distintas de cero y se busca en ellas la dirección del fallo), mapea un objeto en esa página, escribe un valor y reanuda (57). ¿La víctima lee ese valor? Tiempos: en la víctima, hasta el evento, hasta reanudar |
| `fallo_escritura` | Lo mismo con un almacenamiento; la víctima relee lo que escribió |
| `fallo_solo_lectura` | Página mapeada y puesta de solo lectura (336): el almacenamiento falla con causa «solo lectura» (`data3`), el ayudante da permiso y reanuda |
| `fallo_parche` | La carga es código generado en la cola NOBITS (como la caché del recompilador: `lwz r3,0(r3); blr` con r5 = marca); el ayudante **reemplaza la carga por `li r3, 0x1234`**, limpia la caché de instrucciones y reanuda: la víctima tiene que devolver 0x1234. En el contexto se busca el pc de la carga y la marca de r5: dice si 58 trae el pc |
| `fallo_main` | Lo mismo con el hilo principal como víctima |
| `fallo_tiempo` | Diez fallos parchados seguidos: µs por fallo |

Si `fallo_lectura` mató el proceso (lv2 no avisa), las demás fases de fallo
se omiten: fastmem queda descartado tal como está planteado y la ganancia
posible se reduce a lo que dé una prueba de segmento más corta.

## 6. El prototipo en el host (2026-10-13)

Hecho sobre main (v0.2.2), con el interruptor `SALSA64_REC_OPT_FASTMEM`
(`SALSA64_REC_FASTMEM=1` en el host; en la PS3, cuando esté, el archivo
`jit-fastmem`):

- **Emisión** (`src/cpu/rec_emit.c`): `emit_addr` con `fm` deja el índice
  en T_Y con `rlwinm` y nada más; `fm_access` emite el acceso (el sitio) y
  anota (sitio, camino frío); `emit_addr_cold` emite en la sección fría la
  prueba de siempre (`emit_kseg_test`), la copia del acceso (con su prueba
  de `code_flags` si almacena, `emit_store_check`, que con fastmem recorta
  el índice a la tabla) y el salto de vuelta, y después lo de siempre
  (`ktlb` y camino lento). Solo `emit_load` y `emit_store`; LWL/LWR y
  compañía siguen con la prueba. `finalize` pasa los sitios a palabras de la
  caché y `rec.c` los mete en una tabla abierta sitio → camino frío
  (`fm_sites`, 2¹⁸ entradas de 12 bytes, 3 MB) con la generación del tramo
  (`seg_gen[]`, o `scratch_gen` para los bloques temporales; el vaciado
  completo sube todas) para descartar los de bloques desalojados.
- **El fallo** (`salsa64_rec_fastmem_fault(rec, nip)`): busca el sitio,
  escribe `b camino_frío` en su lugar, `dcbst`/`icbi`, y quien llama reanuda.
  En el host es un manejador de `SIGSEGV` con `SA_SIGINFO` que toma el pc de
  `uc_mcontext.gp_regs[PT_NIP]` (`src/host/run.c`); bajo qemu-user el fallo
  del invitado llega así y la instrucción parchada se repite (probado con
  un programa suelto antes de tocar el emisor; la trampa: gcc ve que el
  descriptor ELFv1 es un objeto local y optimiza la llamada a basura; hay
  que ocultárselo).
- **La ventana** (`src/host/rec_host.h`): 512 MB `PROT_NONE` y la RDRAM
  `MAP_FIXED` al principio; `salsa64_rec_init` solo enciende fastmem si
  `cpu->rdram == rec->fm_window` y ninguna entrada de la TLB choca.
- **Colisiones** (`salsa64_rec_fastmem_collides`): en cada escritura de la
  TLB (`rec_tlb.c`, por el código generado o por el intérprete), una
  entrada de kuseg o kseg2/3 cuyo tramo, con los bits 31–29 quitados, cae
  en la RDRAM apaga fastmem (`salsa64_rec_fastmem_off`: vacía la caché, los
  bloques se recompilan con la prueba; `stats.fm_off`).

Resultados en qemu (ppc64, `tools/qemu-ppc64.sh`):

| Prueba | Resultado |
|---|---|
| vitest `--rec` | OK; 474 accesos sin prueba, 41 parchados tras fallar (la pantalla por KSEG1 y los registros del VI) |
| nemu64-test `--rec` con registros fijos | **440 fallas = las 433 de la línea base + 7 esperadas**: «LW/SW with address not sign extended» (una dirección de 64 bits sin extensión de signo es error de dirección en la N64; fastmem la recorta a 32 bits) y «Unaligned LW/SW/LWC1 exception» (no se comprueba la alineación). Las dos son fallos de programa en la consola real y ningún juego del censo las produce. Fastmem se apagó solo a mitad de la suite (sus pruebas de la TLB mapean kuseg bajo) y el resto corrió con la prueba: 10 201 accesos sin prueba, 31 parchados |
| nemu64-test `--lockstep` | diverge en la prueba de LW no alineado (badvaddr …a26: el intérprete lanza AdEL), como corresponde; no sirve de lockstep con fastmem |
| GoldenEye hasta la presa `--lockstep` (7,5 G ciclos, 203 M entradas a bloques comparadas), Perfect Dark 3 G `--lockstep` | **sin diferencias**; 282 y 764 accesos parchados tras fallar |
| Expansión (GoldenEye hasta la presa, `SALSA64_REC_DUMP=1 SALSA64_REC_PROF=12000 SALSA64_REC_PROF_TOP=4000 SALSA64_REC_PIN=1`, `tools/rec-expansion.py --counter 8`, pesado por ejecuciones) | **calientes 8,16 → 6,87 palabras por instrucción MIPS (−16 %)**, lo estimado en la sección 1; frías 7,40 → 10,06 (+36 %: la copia del acceso con su prueba, 7 palabras por carga y 11 por almacenamiento) y en total 15,57 → 16,93 (+9 %). 74 037 accesos sin prueba y **282 parchados** en 80 s de consola (los registros del VI, SP, AI y PI, la ROM y el PIF): el fallo es una rareza, no un costo. En los 25 bloques más ejecutados, 8,1 → 6,8 calientes |

Lo que la expansión dice para la PS3: la ganancia está en las líneas
calientes; la sección fría crece, y como hoy va pegada al cuerpo (en main;
la rama de la caché del PPE la separa), parte de ese crecimiento entra en
la L2 con el cuerpo. El A/B en la consola lo dirá.

### El lado de la PS3 (`src/platform/ps3/fastmem.c`, por probar)

Con el archivo `jit-fastmem`: la RDRAM se pide a sys_mmapper (región de 512
MB en páginas de 1 MB, objeto de 8 MB mapeado al principio) y el frontend
pasa la ventana al recompilador; al abrir el primer juego, un hilo
ayudante (prioridad 900) espera en una cola de eventos con la notificación
de fallos de la región (327) y una **autoprueba** hace fallar una carga
desde un hilo propio con código en el final de la caché: si el evento llega,
el contexto (58) trae el pc (la autoprueba lo localiza buscando el pc de su
carga en el volcado, y ese índice vale después) y el hilo sigue por la
instrucción parchada, fastmem queda activo; si no, apagado con el motivo en
el log. Los fallos del juego: el ayudante toma el pc del contexto,
`salsa64_rec_fastmem_fault` parchea y reanuda (57); un fallo que no es de un
sitio conocido deja el hilo detenido y lo dice. El log de cada 5 s cuenta
accesos sin prueba, parchados, fallos atendidos, desconocidos y de SPE.
Pendiente de la consola: que lv2 avise, que el contexto traiga el pc, y que
los SPE (HLE gráfica, audio, JPEG) puedan hacer DMA sobre la ventana.

## 7. Resultados en la PS3

**Primera corrida (2026-10-13, Slim CECH-3011A, HFW 4.91 + HEN, paquete
SLSA00111 f1d53f6d).** Tres arranques. En el primero y el segundo
`sys_mmapper_enable_page_fault_notification(0x60000000)` devolvió 0 (lv2
aceptó la notificación), corrieron las fases de fallo de lectura, escritura
y solo lectura, y en `fallo_parche` **la aplicación murió sola** (volvió al
XMB). En el tercero, 327 devolvió 0x80010002 (EINVAL) con el mismo rango y
las fases de fallo quedaron NO_DISPONIBLE: lo más probable es que lv2 se
quedara con el registro de los procesos muertos; antes de la siguiente
corrida hay que reiniciar la consola. **Lo que decía el primer arranque se
perdió**: el log de la consola se pisa en cada arranque y el archivo de
estado no sobrevivió a la muerte del proceso (el de jit-ppe sí; por qué,
no se sabe). Lección: log por arranque y estado sin búfer (la segunda
versión de la prueba).

Lo que sí quedó (tercer arranque), todo con lv2 real:

| | PS3 |
|---|---|
| Regiones | 256 MB, 512 MB y 1 GB en 0x80000000 (con alineación de 256 MB también); **2 GB no** (ENOMEM); 4 regiones de 256 MB a la vez más la de la prueba (0x60000000): 0x80000000–0xBFFFFFFF; `sys_mmapper_allocate_fixed_address` devuelve 0 y 0xB0000000 se libera; las direcciones reservadas no se descuentan de la memoria de usuario |
| Objetos y mapeo | 256 objetos de 64 KB (tope de la prueba); mapear 64 KB 8,2 µs, desmapear 15,9; 1 MB mapear 50,6, desmapear 47,7; el contenido sobrevive al desmapeo; **espejo** del mismo objeto en dos direcciones sí; 1 MB en una región de 64 KB sí, 64 KB en una de 1 MB no; `search_and_map` sí; primer toque de una página de 4 KB recién mapeada 93 ns (no hay poblado perezoso); 128 objetos mapeados seguidos en 1,1 ms |
| Permisos | `sys_mmapper_change_address_access_right` (336) devuelve EINVAL: no hay solo lectura por página |
| Atributos (351) | solo de páginas mapeadas: tamaño, derechos (0xF; el código 0x8), un tercer campo 0x0001000000000000 |
| Datos | 8 MB a paso 128: **~70 ns por lectura** (560 ciclos, fuera de la L2) igual en el heap, en 64 KB y en 1 MB; a paso 4224, 17,5 ns (los fallos de ERAT no se notan sobre el de L2); 256 KB en la L2: 14,3 ns a paso 128 y 2,0 a paso 4224 |

Para fastmem: la ventana de 512 MB existe (en 0x80000000 o 0xA0000000: el
índice `va & 0x1FFFFFFF` sumado a la base queda dentro de los 32 bits), la
RDRAM va como un objeto de 8 MB en páginas de 1 MB sin costo de acceso
distinto al heap, y los fallos son lo único por confirmar.

## 8. Base en caché: la misma ganancia sin fallos (diseño, 2026-10-13)

Lo que fastmem quita por acceso es la prueba (5 palabras). Hay otra forma
de amortizarla que **no necesita fallos, ventana ni ayudante** y conserva
la semántica exacta del segmento y del tamaño: hacer la prueba **una vez
por registro base** y que cada acceso del grupo sea una carga con
desplazamiento. Lo normal en código de libultra es
`lw t0, 0(s0); lw t1, 4(s0); sw t2, 8(s0)`: tres pruebas hoy, una con esto.

Un **grupo** son los accesos seguidos de un bloque con el mismo registro
base `rs`, sin una escritura de `rs` en medio, con `imm` en un tramo
`[min_imm, max_imm]` de a lo más 32 KB y cada `imm − min_imm` múltiplo del
tamaño de su acceso. Al primer acceso del grupo:

```
addi  T, rs, min_imm          ; va_lo (se omite si min_imm = 0)
oris  T, T, 0x2000            ; la prueba compacta de hoy sobre va_lo…
addis T, T, 0x6000
addi  T, T, span              ; …con margen: x_lo + span, span = max_imm − min_imm
rldicl X, T, 64 − s, s − abits ; cero si x_lo + span < RDRAM y alineado
cmpdi X, 0 ; bne frío
add   Tb, R_RDRAM, T          ; Tb = RDRAM + x_lo + span
```

Siete u ocho palabras una vez. Después cada acceso es `lwz d, (imm −
min_imm − span)(Tb)` + `extsw` (el desplazamiento queda en [−32768, 0]) y
los almacenamientos agregan su prueba de `code_flags` sobre `x_lo + (imm −
min_imm)` (`addi` + las 4 de hoy). Por qué es exacto: el doblado lleva solo
KSEG0/KSEG1 a [0, 512 MB) (kuseg y kseg2/3 quedan ≥ 512 MB o negativos y
fallan la prueba), x_lo ≥ 0 para toda dirección de KSEG, y `x_lo + span <
RDRAM` cubre todos los `imm` del grupo; la alineación se prueba sobre x_lo
con la del acceso más grande, y la condición sobre `imm − min_imm` da la de
cada uno. Lo que falla va al camino frío **del acceso que lo pidió**, que es
el de hoy (prueba suelta, `ktlb`, C), y al volver recompone `Tb` (cinco
palabras frías) para los que siguen.

Costo por grupo de n cargas: 8 + 2n palabras calientes contra 7–8n hoy
(n = 1: 10 contra 7–8, no se agrupa; n = 2: 12 contra 15; n = 4: 16 contra
30). Hace falta un registro para `Tb` que sobreviva al grupo: los
temporales r3–r12 se pierden en las llamadas a C de los caminos lentos, por
eso el camino frío lo recompone; entre accesos sin camino lento basta con
reservar uno de los temporales (r10, T_S2) mientras el grupo esté abierto.
Con fastmem encima, la prueba del grupo se reduce a `addi` + `rlwinm` +
`add` (3 palabras) y lo demás es igual.

**Cuánto agrupa** (volcados del host con el perfil por bloque, los 4000
bloques más ejecutados, pesado por ejecuciones; grupos con el mismo base
sin escribirlo en medio, tramo de `imm` ≤ 32 KB y alineación compatible):

| Juego | Accesos por instrucción | En grupos de ≥ 2 | Palabras calientes de los accesos por instrucción: hoy → agrupado |
|---|---|---|---|
| GoldenEye (presa) | 0,326 | 72 % | 2,73 → 1,97 (**−0,76**) |
| Perfect Dark (intro) | 0,267 | 70 % | 2,25 → 1,57 (−0,68) |
| Mario Kart 64 | 0,268 | 70 % | 2,29 → 1,63 (−0,66) |
| Zelda OoT | 0,397 | 77 % | 3,46 → 2,25 (**−1,21**) |

Sobre las 8,16 palabras calientes por instrucción de GoldenEye es −9 %
(fastmem solo: −16 %; los dos juntos, más). Y no depende de nada de lv2.

### Hecho (2026-10-13; `group_pass`, `emit_group_head`, `emit_group_tb` en rec_emit.c)

- **`Tb` es T_HI (r7).** Los temporales r3–r12 los pisan las instrucciones de
  en medio; r7 solo lo usan MULT/DIV y compañía (`emit_hilo`), LWL/LWR y
  compañía y la escritura de FCR31 de las operaciones de COP1: el grupo se
  corta antes de cualquiera de esas. Eso le quita un quinto al ahorro
  (GoldenEye 0,76 → 0,59 por instrucción); recuperarlo es cambiar r6/r7 por
  otros temporales en la escritura de FCR31.
- **Un grupo abierto bloquea a los demás**: con un solo `Tb`, un acceso con
  otro base dentro del tramo de un grupo va con su prueba (no toca T_HI).
  Fue el primer error que encontró el lockstep de nemu64-test (dos grupos
  solapados, uno pisaba el `Tb` del otro). Costo: GoldenEye 0,59 → 0,50,
  Perfect Dark 0,45, Zelda 0,84 palabras por instrucción (estimado).
- **La C no pisa `Tb`**: toda llamada a C que vuelve al camino caliente
  pasa por `emit_slow_call` (el trampolín del bloque deja el pc en r7); con
  un grupo abierto guarda T_HI en `cpu->rec_tb` antes y lo repone después
  (dos palabras, casi siempre frías). Recomponerlo con la doblada de KSEG0,
  como estaba primero, daba un puntero fuera de la RDRAM cuando el base era
  una dirección mapeada por la TLB: Perfect Dark moría al segundo bloque.
- **Si la prueba del grupo falla**: el camino frío de la cabeza prueba la
  `ktlb` con va_lo (la rutina del preámbulo, con D si el grupo almacena) y,
  si el tramo cabe en la página de 4 KB, `Tb` sale de la física. Si no
  (página sin anotar, E/S, excepción), **el acceso de la cabeza va por C**
  como cualquier camino lento (anota la página si traduce, o lanza la
  excepción) y se vuelve a probar la `ktlb` (`emit_group_reprobe`): si
  ahora traduce, `Tb` y siguen los miembros; si no, el intérprete sigue
  **después de la cabeza** (exit después de k, como las demás salidas).
  Salir al intérprete *antes* de la cabeza, como estaba primero, dejaba la
  página sin anotar para siempre: los grupos sobre datos mapeados (Perfect
  Dark) se habrían interpretado en cada pasada. Los miembros no llevan
  prueba propia; las cargas de un grupo no tienen camino frío y los
  almacenamientos solo el de `code_flags` (índice: dirección del host menos
  la RDRAM, sin exigir alineación de la RDRAM).
- **Un grupo no cruza un destino de salto interno** (al volver por el lazo
  el base pudo cambiar después del grupo) **ni empieza en el delay slot de
  un branch-likely** (solo corre si se toma: los de después no verían
  `Tb`). Esto último se comprueba sobre la cabeza *final*: el filtro de
  alineación puede dejar fuera al primer acceso y dejar de cabeza a uno
  posterior; con un `sb` en el delay slot de un BC1FL como cabeza, Perfect
  Dark se congeló en la PS3 en la segunda escena de la intro (el miembro
  siguiente, por el camino no tomado, usaba r7 sin valor: en el host el
  mismo bloque muere con SIGSEGV en la dirección −44). El mínimo del tramo
  se redondea hacia abajo a la alineación del acceso más grande, para que
  un byte en +1 no deje fuera a las palabras en +0x18 y +0x30 del mismo
  base.
- Interruptor para el A/B: `SALSA64_REC_OPT_NO_GROUP` (`jit-sin-grupos` en
  la PS3, `SALSA64_REC_NO_GROUP=1` en el host). Encendido por omisión: es
  exacto (nemu64-test en `--lockstep` entero sin diferencias; las unitarias
  en los cinco modos).

Medido en el host (volcados con el perfil por bloque, `tools/rec-expansion.py`):

| | Calientes por instrucción MIPS | Frías | Total |
|---|---|---|---|
| Perfect Dark, intro (3 G ciclos) | 6,84 → 6,45 (**−6 %**) | 6,50 → 6,09 (−6 %) | 13,34 → 12,54 (−6 %) |
| GoldenEye hasta la presa (7,5 G ciclos) | 8,16 → 7,65 (**−6 %**) | 7,40 → 7,20 (−3 %) | 15,57 → 14,85 (−5 %) |

(Con la cabeza final nunca en un delay slot likely, el mínimo redondeado y
FCR31 en T_V, que ya no corta los grupos en las operaciones de COP1;
Perfect Dark 6,84 → 6,39 con lo mismo. Corrección de esta versión: unitarias
en plain, pin y FPU residente, nemu64-test 433 y lockstep entero, Perfect
Dark 9 G ciclos con enlaces y en lockstep, GoldenEye hasta la presa en
lockstep, todo sin diferencias.)

**En la PS3 (2026-10-13, primera corrida, paquete d939fb46 con el error del
delay slot likely):** GoldenEye en la presa 16,63 ns por instrucción en 12
ventanas pesadas (14,7–18,8), contra 16,03/16,30 de v0.2.2: **no gana** en
la presa pese a −5 % de palabras calientes (las escenas no son idénticas;
tampoco hay señal de mejora). Perfect Dark, primera ventana pesada de la
intro: 10,07 contra 10,51 sin grupos (−4 %); la segunda escena se congeló
por el error. Pendiente la corrida con el arreglo (PD entero con y sin
`jit-sin-grupos`).

Corrección: unitarias 449/449 en los cinco modos, nemu64-test 433 (la línea
base exacta) y en `--lockstep` entero sin diferencias, vitest, y los
lockstep de Perfect Dark (3 G ciclos) y GoldenEye hasta la presa (7,5 G)
sin diferencias. Lo que no agrupa todavía, por orden de lo que vale: los
accesos entre operaciones de COP1 (r6/r7 en la escritura de FCR31: cambiar
esos temporales), un segundo `Tb` para los grupos solapados, y los pares
LWL/LWR. En la PS3: A/B con `jit-sin-grupos` en el mismo paquete (GoldenEye
presa y Perfect Dark, ns por instrucción del nativo).

Las frías también bajan: los miembros de un grupo no tienen camino frío
(las cargas) o solo el de `code_flags` (los almacenamientos).

## 9. La FPU compacta: las operaciones como rutinas del preámbulo (2026-10-13)

Lo que señalaron el muestreo por tiempo en la PS3 (los bloques que se llevan
el tiempo son los de FPU en línea, ~20 palabras calientes por instrucción) y
docs/cache-ppe.md §7.2 de la ronda de la caché. Medido en los volcados del
host (pesado por ejecuciones): las operaciones de la FPU son **0,06 por
instrucción MIPS en GoldenEye** (MUL.S 0,020, ADD.S 0,011, C.cond.S 0,010,
SUB.S 0,010, CVT.S.W 0,005, MOV 0,003, NEG 0,002, TRUNC.W 0,001, SQRT y DIV
0,001), 0,043 en Perfect Dark y 0,018 en Zelda; a ~35–40 palabras calientes
cada una son **~2,2 de las 8,2 palabras calientes por instrucción de
GoldenEye (27 %)**: la palanca más grande que queda, por delante de fastmem
(−1,3) y de la base en caché (−0,5).

Diseño (`src/cpu/rec_emit_fpurtn.h`, encendido por omisión;
`jit-fpu-en-linea` en la PS3 y `SALSA64_REC_FPU_INLINE=1` en el host repiten
el código en línea para el A/B):

- **El mismo código, una vez.** Las rutinas son los mismos emisores de la
  FPU en línea (compuerta de FCR31, clasificación de los operandos con la
  franja segura, la operación con su redondeo y el inexacto, el resultado y
  FCR31) con los desplazamientos de los operandos en registros en vez de
  inmediatos (`lwzx`/`lfsx`/`stfdx` sobre r31). Se emiten al iniciar el
  recompilador, después del preámbulo (que pasa de 1024 a 4096 palabras),
  con el ensamblador de bloques (`finalize` sobre esa zona): 38 rutinas, una
  por clase: ADD/SUB/MUL/DIV × S/D, C.cond × S/D (la condición en r10),
  ABS/NEG × S/D (NEG en r10), SQRT × S/D, CVT.D.S y CVT.S.D, CVT.S.W y
  CVT.D.W, y las 20 conversiones a entero (ROUND/TRUNC/CEIL/FLOOR a W y L,
  CVT.W y CVT.L, por formato). MOV queda en línea (dos palabras).
- **El sitio**: `li r3, oa` (fs), `li r4, ob` (ft, si la hay), `li r8, od`
  (fd), `li r10, extra` si hace falta, `bl rutina` y `bne cr1 → lento`:
  **5–6 palabras calientes** en vez de 35–40. El camino lento es el de
  siempre (el intérprete, con la revisión de después). La rutina vuelve
  con cr1 eq si hizo la operación o cr1 ne sin escribir nada: la misma
  frontera entre nativo e intérprete que antes, así que los mismos bits,
  causas y banderas.
- **Registros**: la rutina pisa r0, r3–r6, r8–r12, f0–f5, cr0 y cr1; no
  toca r7 (la base en caché de un grupo sigue viva a través de una
  operación de la FPU) ni LR más allá del `blr`. Es una llamada al
  preámbulo, no a C: no guarda nada en `cpu->rec_tb`.
- **Costo**: un `bl`/`blr` por operación (destino fijo; la vuelta va por el
  registro de enlace, que el PPE predice) y las rutinas viven en el
  preámbulo, que es lo más caliente de la caché.
- **Medición**: expansión en el host (GoldenEye y Perfect Dark), unitarias
  (`executed_matches_full` y `executed_conversions_match_full` comparan
  miles de operaciones aleatorias con el camino completo), nemu64-test (433)
  y los lockstep de siempre; en la PS3, A/B con `jit-fpu-en-linea`.

Resultado en el host (2026-10-13, con la base en caché también encendida;
`tools/rec-expansion.py` sobre v0.2.2):

| | Calientes por instrucción MIPS | Frías | Total |
|---|---|---|---|
| GoldenEye hasta la presa | 8,16 → **5,54 (−32 %)** | 7,40 → 7,27 (−2 %) | 15,57 → 12,81 (−18 %) |
| Perfect Dark intro | 6,84 → **5,22 (−24 %)** | 6,50 → 6,05 (−7 %) | 13,34 → 11,27 (−16 %) |

(La base en caché sola era −6 % y −7 %: la FPU compacta pone −26 % y −17 %
más.) Corrección: unitarias 491/491 en plain, pin y FPU residente
(`executed_matches_full` y `executed_conversions_match_full` incluidas),
nemu64-test 433 y en lockstep entero, vitest, Perfect Dark 6 G ciclos y
GoldenEye hasta la presa en lockstep, todo sin diferencias.

**En la PS3 (2026-10-13, paquete `build/ps3-4/salsa64-fpu-app-SLSA00111.pkg`,
sin banderas: base en caché y FPU compacta encendidas; `tools/jit-perfil.py`
sobre `build/ps3-4/fpu-ge-pd-udp.log`, ventanas de 5 s con MIPS ≥ 40 y
espera ≤ 35 %):**

| | ns por instrucción (media / mediana) | v0.2.2 | Base en caché sola (corrida A) |
|---|---|---|---|
| GoldenEye en la presa (13 ventanas pesadas; 61,6 MIPS, 51,2 campos/s) | **15,66 / 15,54** (mín 13,67, máx 17,15) | 16,03 / 16,30 | 16,63 |
| Perfect Dark intro (4 ventanas pesadas) | **12,92 / 13,27** (mín 10,47, máx 14,69); la segunda escena 14,13–14,69 | ~15,4 en la segunda escena | 10,07 en la primera ventana |

Con el criterio laxo (MIPS > 30, espera < 40 %) GoldenEye da 17 ventanas,
15,78 / 15,66. Es decir: GoldenEye −2…−4 % contra v0.2.2 y −6 % contra la
corrida A de los grupos; Perfect Dark −5…−8 % en la segunda escena. Mucho
menos que el −32 % / −24 % de palabras calientes del host: el `bl`/`blr`
de cada operación y el que la rutina ya no comparta el fetch con el bloque
se comen buena parte de lo ahorrado en huella, y el resto de la escena
(RSP, RDP, espera) no cambia. El autor ve Perfect Dark bien y GoldenEye
todavía con atoros en las partes pesadas. Queda el A/B estricto en el
mismo paquete (`jit-fpu-en-linea`, `jit-sin-grupos`).

