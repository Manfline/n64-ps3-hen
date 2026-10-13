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

## 6. Resultados

Pendiente de la PS3 real (2026-10-13). En RPCS3 (no concluyente: no ejecuta
código generado y su contexto de fallo está vacío) sirve solo para validar
el programa.
