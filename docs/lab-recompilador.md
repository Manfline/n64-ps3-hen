# Laboratorio del recompilador (afinar el emisor sin la PS3)

Escrito el 2026-10-13. Código en `tools/lab/`; los ganchos que necesita el
emisor son unas 40 líneas bajo `#ifdef` (`tools/lab/lab-ganchos.patch`).

El recompilador MIPS → PPC64 (`src/cpu/rec_*.c`, docs/rec-ppe.md) se afina hoy
midiendo en la PS3, que cuesta media hora de consola por pregunta y no deja
mirar dentro (el PMU del Cell es privilegiado). Este laboratorio responde en la
Mac, en segundos, preguntas sobre **el código que se emite**: cuántas palabras
de PPC por instrucción MIPS, qué ejecuta cada una, dónde hay microcódigo, qué
cargas alcanzan un almacenamiento reciente (load-hit-store), cuánto código toca
la L1I y la L2, y qué cambia entre dos versiones del emisor sobre el mismo
perfil. El emisor es C puro: genera los bytes de PowerPC en un arreglo sin
ejecutarlos, así que el mismo `rec_emit.c` que corre en la PS3 se compila con el
cc nativo (arm64) y traduce los bloques reales de un juego. Lo que no se mide
aquí (esperas de datos, predicción de saltos, el reloj) se **calibra contra lo
ya medido en la PS3** (más abajo) y se dice cuando no se modela.

Nada de lo que escriben estas herramientas se versiona: los volcados de RDRAM,
los estados, los `.lab` y las trazas llevan código del juego (`build/lab/` está
en `.gitignore` como todo `build/`). Los perfiles `.prf` de la PS3 solo traen
direcciones y cuentas.

## Piezas

| Herramienta | Qué hace |
|---|---|
| `tools/lab/build.sh [SRC [OUT]]` | compila con el cc nativo `rec-lab` y `salsa64-run-lab` (de este árbol o de otro, p. ej. una revisión vieja) |
| `rec-lab` | traduce con el emisor de verdad los bloques de un perfil (`--prf` de la PS3, `--pcs` de ares o de otra fuente) sobre una escena: un estado (`--state`, con la TLB) o un volcado de la RDRAM (`--rdram`, con `--map` / `--romwin` para el código mapeado). Escribe un `.lab` |
| `salsa64-run-lab` + `lab-trace.c` | `salsa64-run` con un gancho en el intérprete (`SALSA64_LAB_TRACE=prefijo`): **perfil exacto**. Simula el despachador con la formación de bloques del emisor y cuenta entradas, ejecuciones por instrucción y aristas entre bloques; escribe el `.lab` y el registro de entradas (`.visits`) |
| `lab-analiza.py` | informe (`reporte`), tabla de variantes (`tabla`), antes/después (`compara`), un bloque desensamblado con sus unidades (`bloque`), una rutina compartida (`rutina`) |
| `lab-cache.py` | L1I/L2/ERAT sobre el `.visits`, con tres colocaciones del código |
| `lab-compara.sh REV_A REV_B -- args…` | compila el emisor de dos revisiones (aplica `lab-ganchos.patch` si les faltan los ganchos), traduce el mismo perfil y compara |
| `ares-lanzar.sh`, `ares-perfil.py`, `ares-bloques.py` | perfil estadístico de una sesión de juego real en ares (servidor GDB): muestreo, volcado de la RDRAM y paso de PC a bloques |
| `test-lab.sh` | pruebas sin datos de juegos: decodificador PPC y un programa MIPS sintético de punta a punta |

Formato del `.lab` (texto): cabecera con las opciones, los desplazamientos de
`struct salsa64_cpu` que usa el código generado, las rutinas compartidas del
preámbulo con su palabra de la caché y el preámbulo; después, por bloque, `B`
(pc, entradas, `n`, hot, total…), `M` (palabras MIPS), `K` (marcas: palabra
caliente/fría donde empieza cada instrucción MIPS y cómo se tradujo), `P`
(palabras PPC, las `hot` primeras son el cuerpo), y según la fuente `T`
(sitios de salida del perfil de la PS3), `H` (ejecuciones por instrucción) y
`E` (aristas). Se lee con `tools/lab/lab_data.py`.

## Recetas

```
tools/lab/build.sh                                   # una vez; build/lab/bin/{rec-lab,salsa64-run-lab}
ROM=…/goldeneye.z64
# 1. escena: salsa64-run con el pad de docs/pads/ y un estado al llegar a la presa
build/host-mac/salsa64-run -q --pad "$(cat docs/pads/goldeneye-dam.pad)" \
    --state-save 5400:build/lab/ge/dam.state --fields 5401 $ROM       # ~60 s en la Mac
# 2a. perfil real de la PS3 sobre esa escena
build/lab/bin/rec-lab --rom $ROM --state build/lab/ge/dam.state --prf perfil/GOLDENEYE-DCBC50D1.prf -o build/lab/ge/dam-prf.lab
# 2b. perfil exacto de la misma escena desde el intérprete (20 s de consola en ~35 s)
SALSA64_LAB_TRACE=build/lab/ge/traza build/lab/bin/salsa64-run-lab -q --pad "$(cat docs/pads/goldeneye-dam.pad)" \
    --state-load build/lab/ge/dam.state --fields 6600 $ROM
# 3. análisis
tools/lab/lab-analiza.py reporte build/lab/ge/traza.lab
tools/lab/lab-cache.py build/lab/ge/traza.lab build/lab/ge/traza.visits --max-visits 8000000
# antes / después de un cambio en el emisor (HEAD~2 contra el árbol de trabajo)
tools/lab/lab-compara.sh HEAD~2 WORK -- --rom $ROM --state build/lab/ge/dam.state --prf …/GOLDENEYE-DCBC50D1.prf
```

Variantes del emisor sin tocar el código: `rec-lab --opts N` con las banderas
de `rec.h` (1 = como el paquete de la PS3; `+128` ancho, `+512` sin base en
caché, `+1024` FPU en línea; `--far` para las llamadas a C por `mtctr`).

**GoldenEye y Perfect Dark mapean su código por la TLB.** El estado trae la TLB;
con un volcado hay que decirlo: Perfect Dark es lineal (`--map
7f000000:220000 --map 70000000:0`), GoldenEye copia páginas de la ROM a marcos
sueltos y su ventana es `7f000000 ↔ ROM + 0x34b30` (`--romwin
7f000000:34b30`; con `--state` se deduce buscando los marcos residentes en la
ROM). Cuando el estado no tiene residente una página, sus palabras se sacan de
la ROM.

**ares** (perfil de una sesión real, p. ej. la del autor jugando):

```
tools/lab/ares-lanzar.sh $ROM 9140         # instancia propia (copia ares-lab.app); o la ventana del autor en 9123
tools/lab/ares-perfil.py --port 9140 --segundos 120 --volcados 30,90 -o build/lab/ares/juego
tools/lab/ares-bloques.py build/lab/ares/juego-pcs.tsv build/lab/ares/juego-rdram-0.bin --map … --sin-espera -o pcs.tsv
build/lab/bin/rec-lab --rom $ROM --rdram build/lab/ares/juego-rdram-0.bin --map … --pcs pcs.tsv --fr 1 -o juego.lab
```

## Qué tan fiel es

* **Emisión.** Con el mismo estado de GoldenEye (presa), el recompilador de
  verdad (ppc64 bajo qemu, `SALSA64_REC_DUMP=1`, 3 campos, 4 616 bloques) y el
  laboratorio emiten **4 573 bloques (99,1 %) con las mismas palabras y las
  mismas secciones** salvo los destinos de los saltos a rutinas fuera del bloque y
  la palabra de la `ktlb` (que lleva el marco físico). Los 43 restantes
  difieren por la tabla `idle_reject` del CPU (el laboratorio la tiene vacía) y,
  sin `--far`, por las llamadas a C (`bl` contra `mtctr/bctrl`: +3 palabras).
* **Decodificador.** Las 68 538 palabras distintas que emite el recompilador en
  GoldenEye coinciden en mnemónico con `powerpc64-linux-gnu-objdump`.
* **Formación de bloques.** `lab-trace` reproduce el despachador: el `n` medio
  ponderado por entradas del perfil exacto es 12,32; el de la PS3 con el
  perfil del autor, 12,4. Ejecuta 8,8 instrucciones por entrada en GoldenEye
  (el bloque entero se recorre en promedio solo el 71 %): **las cifras «por
  instrucción» con el perfil de la PS3 (que no trae ejecuciones, solo `n`)
  dividen por instrucciones de más**; las del perfil exacto, no.
* **Palabras por instrucción.** El laboratorio reproduce el cuadro de
  docs/perf-log.md («Base en caché y FPU compacta»): GoldenEye, v0.2.2 15,57
  (medido) contra 16,39 (laboratorio, con los pesos de la PS3); base en caché
  14,85 contra 15,54; con la FPU compacta 12,81 contra 12,96. Perfect Dark
  13,34 / 12,49 / 11,27 contra 15,05 / 14,14 / 12,17 (otra escena).

## Calibración contra la PS3

Las medidas de la PS3 son ns por instrucción emulada, en las ventanas pesadas
(docs/perf-log.md): GoldenEye en la presa 17,26 (código ancho) → 16,03–16,30
(v0.2.2, código compacto) → 15,54–15,66 (base en caché y FPU compacta);
Perfect Dark 14,85 → 14,45 (compacto) y 15,4 → 14,1–14,7 (paquete actual).
Contra lo que mide el laboratorio en cada variante (`lab-analiza.py tabla`):

| GoldenEye (pesos de la PS3) | ancho | v0.2.2 | base en caché | actual |
|---|---|---|---|---|
| palabras que corren por instr. (cuerpo, prólogo y rutinas) | 11,09 | 10,03 | 9,46 | 9,87 |
| líneas de 128 B del cuerpo por instr. | 0,340 | 0,307 | 0,288 | 0,208 |
| palabras totales / calientes por instr. | 21,1 / 9,7 | 16,4 / 8,6 | 15,5 / 8,1 | 13,0 / 5,5 |
| medido en la PS3 (ns/instr) | 17,26 | 16,15 | — | 15,60 |
| estimado por el modelo | 0 % | −6,4 % | −9,8 % | −9,5 % |
| medido | 0 % | −6,3 % | — | −9,8 % |

Un modelo de dos parámetros, **Δt ≈ 0,58 · Δ(palabras que corren) + 0,08 ·
Δ(líneas del cuerpo)** (en cambios relativos por instrucción), ajustado a los
dos pasos de GoldenEye, predice la suma de los dos pasos de Perfect Dark
(−9,4 % contra −9,5 % medido, sumando −2,7 % y −6,5 % de escenas distintas) pero
no su reparto (−6,5 / −3,1 contra −2,7 / −6,5 medido: el ruido de las ventanas
es de ese tamaño). Lo toma `lab-analiza.py
compara`/`tabla` como «efecto estimado»: **sirve para ordenar cambios y
descartar los que no mueven nada, no para prometer un número** (±50 % por
paso). Lo que dice de fondo: más de la mitad del tiempo escala con las palabras
que se ejecutan y las líneas de código pesan poco (0,08), de acuerdo con que
mover los 762 bloques calientes a una zona contigua no se notó
(docs/perf-log.md).

La «base en caché» sola da −3,4 % en el modelo y «sin ganancia» en la PS3 con el
paquete de entonces, que tenía el defecto del delay slot likely que se arregló
después: no es una discrepancia del modelo.

## Qué dice el laboratorio

GoldenEye en la presa (perfil exacto, 20 s de consola, 7 397 bloques, 970 M de
instrucciones) y la intro de Perfect Dark (7 261 bloques, 695 M); entre
paréntesis, Perfect Dark.

1. **Se ejecutan 10,7 (9,4) palabras de PPC por instrucción MIPS**: 6,8 del
   cuerpo y el prólogo y 3,95 (2,9) de rutinas compartidas, de ellas 3,1 de la
   FPU (78 llamadas por 1000 instrucciones, 40 palabras cada una) y 1,0 de
   `exit_jr` (27 por 1000, 36 palabras). Estático: 13,5 palabras por
   instrucción, 5,5 calientes y 7,9 frías.
2. **El estado del emulador pasa por memoria**: por cada 100 instrucciones MIPS
   el código generado hace 58 (52) cargas y 52 (51) almacenamientos a
   `cpu->…`, contra 18,5 y 8,2 accesos del propio programa. De ellos la FPU es
   el 67 % (55 %) de las cargas y el 56 % (41 %) de los almacenamientos:
   `fpr` y FCR31 (8,9 cargas y 7,8 almacenamientos por 100 instrucciones solo
   para FCR31).
3. **Load-hit-store dentro del cuerpo**: 10,4 (14,1) eventos por 100
   instrucciones con ventana de 16 instrucciones del PPE (4,4 con 8, 16,3 con 32),
   uno por almacenamiento alcanzado. Reparto con ventana de 32: `fpr` 7,3 (6,9),
   FCR31 6,1 (4,8; **cota alta**: el modelo supone que el almacenamiento de FCR31
   se ejecuta siempre, la rutina lo salta cuando no cambia), `gpr` 2,7 (6,7).
   Por instrucción MIPS: `mul.s` 1,35 eventos por ejecución, `add.s` 1,72,
   `sub.s` 1,38, `c.lt.s` 1,23, `cvt.s.w` 0,82; las cuatro primeras son el 52 % del total. A ~35
   ciclos cada uno, **3,6 ciclos por instrucción (7 % de los ~52)** con la
   ventana de 16. La memoria del propio juego no se modela (el código generado
   calcula cada dirección en registros nuevos).
4. **Fronteras de bloque**: el 28 % (24 %) de las entradas llegan por una arista
   donde el bloque anterior acaba de escribir a `cpu->…` algo que el siguiente
   lee en su entrada (0,32 registros por arista), a 12–19 palabras de distancia:
   dentro de la ventana. El sucesor más frecuente de un bloque recibe el 85,5 %
   (92,4 %) de sus salidas. Con 0,32 eventos por arista y 0,114 (0,100) entradas
   por instrucción son ~1,3 ciclos por instrucción: **el costo de una frontera
   sale del orden de 50 ciclos** (prólogo de 9,3 palabras, salida, el evento de
   load-hit-store y los fallos de línea), no de 100. Las trazas por el sucesor
   dominante (docs/cache-ppe.md 3.2) quitarían el prólogo y esos eventos en 6 de
   cada 7 fronteras.
5. **Caché de instrucciones** (`lab-cache.py`, 12 M de entradas, L1I 32 KB 2 vías,
   L2 de 256 o 512 KB para el código, costos de docs/cache-ppe.md §1): con la
   colocación de hoy (cuerpo y frío contiguos por bloque, en orden de primera
   visita) son 32,6 (38,9) fallos de L1I por 1000 instrucciones con precarga
   de la línea siguiente, 49 (59) sin ella, y 9,3 (7,6) de L2: **4,8 (4,4)
   ciclos por instrucción de espera de código (9 %)**, hasta 9,0 (7,6) en el
   peor caso. Los cuerpos calientes juntos y las secciones frías aparte
   (docs/cache-ppe.md 3.1.1) los bajan a 3,6 (3,2): **−1,2 (−1,1) ciclos, ~2,3 %**,
   y los fallos de ERAT de instrucciones a un tercio (3,4 → 1,1 por 1000).
   Ordenar los cuerpos por peso en vez de por primera visita **empeora** la L1I
   (32,0 contra 26,0): pierde la contigüidad de los bloques que se encadenan.
   La estimación de 5–12 % de docs/cache-ppe.md para 3.1.1 es de dos a cinco
   veces la de este modelo, en el sentido de la elasticidad baja a las líneas
   (0,08).
6. **Microcódigo y lo lento**: 7,6 (6,0) instrucciones por 1000 (`slw` casi
   todas, del cuerpo y de las rutinas compartidas; `srad` y `srw` el resto),
   ~0,08 ciclos por instrucción: no es palanca. `mulld` 3,8 y `divd` 2,1 por 1000.
7. **Casi nada queda en el intérprete**: el 99,74 % de las instrucciones son
   nativas; el 0,04 % pasa por el intérprete (MTC0 a registros sin camino
   propio), 0,9 llamadas a C por 1000 instrucciones.
8. **Composición del cuerpo caliente** (GoldenEye, por palabras ejecutadas):
   `cmp`+`bc` el 10,8 %, la prueba de KSEG0 de cada acceso (`addi, oris, addis,
   rldicl, cmpdi, bc`) el 4 % solo en `rldicl+cmpdi`, `li 0` seguido de `or`/`add`
   (mover desde `zero`: dos palabras donde sirve `mr`) el 1,1 %, `nop` el 1,6 %
   (el sitio de enlace parcheable: ~1,1 por entrada), `lwzx+extsw` el 1,2 %.
   Son retoques de uno por ciento, no palancas, pero el laboratorio mide cada
   uno contra el perfil real con `compara`.

Qué sugiere, en orden de lo que más pesa según el modelo (cada línea es una
hipótesis para probar con `compara` y luego en la PS3):

* **FPU: FCR31 y los registros por memoria.** La FPU residente se midió más
  lenta en la PS3 (docs/perf-log.md) con la FPU en línea; con las rutinas
  compartidas el costo está en 13–14 eventos de load-hit-store por 100
  instrucciones y 66 % del tráfico a `cpu->…`. Probar un FCR31 perezoso dentro
  del bloque (una escritura al salir, no una lectura y escritura por operación)
  y, entre dos operaciones encadenadas, pasar el resultado por registro del PPE
  sin pasar por `fpr` en memoria: el laboratorio dice cuántos eventos quita antes
  de escribirlo.
* **Fronteras**: trazas por el sucesor dominante (3.2): ~1,3 ciclos por
  instrucción de load-hit-store más el prólogo (9,3 palabras por entrada, 0,75
  por instrucción).
* **Colocación** (3.1.1): ~2 % según el modelo, barata y sin riesgo.

## Límites del modelo

* **Camino estático.** Cada bloque se recorre por su camino sin salto tomado
  (`bc` con pista «probable» se toma); las rutinas compartidas, por su camino
  probable (ADD.S: 46 palabras de 52). El recorrido real depende de los datos
  (FCR31, rutas de excepción de la FPU, saltos internos). Con el perfil exacto
  (`H`) cada unidad de emisión pesa lo que se ejecutó.
* **Sin predicción de saltos ni datos.** No hay modelo de la tabla de historia
  (24 ciclos por fallo), de la L1D, del ERAT de datos ni del XDR: lo que las
  cifras de arriba no explican de los ~52 ciclos por instrucción (unas 10,7
  palabras que corren, ~3,6 de load-hit-store, ~4,8 de código) es lo que el
  laboratorio no ve; el reparto de ese resto es lo que mediría el bench de
  fronteras y el de huella de datos de docs/cache-ppe.md §5.
* **Caché de instrucciones idealizada**: L2 solo de código (se prueba con 256 KB),
  colocación en orden de primera visita sin desalojos, precarga de una línea
  como piso/techo, sin la copia del SPE ni el hilo 1.
* **Load-hit-store: 35 ciclos por evento**, ventana de instrucciones y no de
  ciclos; un solo evento por almacenamiento. Solo `cpu->…`.
* **Perfil de la PS3**: trae `n`, no ejecuciones; y el que hay es de varias
  sesiones del autor (23 205 bloques en GoldenEye, solo el 36 % de sus entradas
  en páginas residentes en el estado de la presa: el resto se lee de la ROM); sirve para
  comparar variantes (mismos pesos) pero no para las cifras absolutas por
  instrucción: para esas, `lab-trace`.
* **ares** (muestreo): el servidor se detiene a instrucciones completas, pero
  **nunca dentro de un delay slot** (3 de 3 245 muestras; lo esperable son ~500):
  el tiempo del delay slot cae en el destino del salto, así que los destinos pesan
  de más; y cuenta los bucles de espera (el CPU en `0x70001938` en Perfect Dark
  es el 11 % de las muestras al arrancar; el intérprete y el recompilador los
  saltan: `--sin-espera`). Con 80–90 muestras por segundo (una conexión y el byte
  0x03; reconectando por muestra son ~10) una sesión de 2 minutos da ~10 000,
  suficiente para los bloques que pesan, no para la cola. La comparación con el
  perfil exacto de la misma intro solo cuadra en grande (intersección de
  histogramas 40 % por página, 29 % por 64 bytes) porque las escenas no están
  alineadas en el tiempo: ares sirve para lo que el autor juega y no se puede
  reproducir con un guion.
* Otras sesiones cierran ares con `pkill -x ares`: `ares-lanzar.sh` usa una
  copia con el ejecutable renombrado (`ares-lab`), y se cierra con `pkill -x
  ares-lab`.

## Los ganchos en el código

`tools/lab/lab-ganchos.patch` (aplicable a revisiones anteriores con
`lab-compara.sh`) toca tres archivos, todo bajo macros que solo definen las
herramientas del laboratorio:

* `src/cpu/rec.h`: bajo `SALSA64_REC_LAB`, un miembro `lab` de `struct
  salsa64_rec` con dónde empieza el código de cada instrucción MIPS en las dos
  secciones, cómo se tradujo y si el bloque es un bucle.
* `src/cpu/rec_emit.c`: `LAB_MARK` al empezar cada unidad de emisión y al
  final del cuerpo (una asignación de tres campos que sin la macro es `(void)0`),
  y la bandera del bucle.
* `src/cpu/interp.c`: bajo `SALSA64_LAB_TRACE`, el gancho `salsa64_lab_hook`,
  que el bucle del intérprete llama antes de cada instrucción si no es nulo.

Sin esas macros el binario es el mismo de siempre. `rec-lab` incluye `cpu/rec.c`
en su unidad de traducción (para ver `emit_preamble` y los tramos, que son
`static`) y no enlaza el `rec.c` normal; `lab_init` repite lo de
`salsa64_rec_init` sin lo nativo (el TOC, las funciones de C en direcciones
ficticias al final del preámbulo). Cambiar `salsa64_rec_init` pide mirar
`tools/lab/lab-emit.h`.

## Caché de destinos de JR (candidato d, primera variante medida en el lab)

`SALSA64_REC_OPT_JTC` (2048; ya integrada y encendida por omisión: `SALSA64_REC_NO_JTC=1` en el host y
el archivo `jit-sin-destinos` en la PS3 la apagan; `JTC_STATS` / `jit-destinos-cuentas` cuentan aciertos y fallos): una tabla directa de 1024
entradas vpc → palabra de entrada que `exit_jr` consulta primero (16 palabras en el acierto
contra ~36 del camino largo; la llena el camino largo; se vacía al matar o desprender el
bloque, al vaciar la caché y cuando el despachador resuelve ese pc a otro código). Lab
(`rec-lab --opts 1` contra `--opts 2049`, GoldenEye, perfil de la PS3): palabras que corren por
instrucción 9,87 → 9,49 (−3,9 % con direcciones de host de 5 palabras en `li64`; en la PS3, de 2,
≈ −3 %), estimado por el modelo −1,8…−2,2 %, más el `bctr` ya acertado y tres cargas
dependientes menos (fuera del modelo). Exacta: lockstep sin diferencias en GoldenEye (presa,
70 campos) y Perfect Dark (intro, 70 campos) con la tabla activa (en lockstep `exit_jr` se usa
solo con JTC), pruebas unitarias con el recompilador (también `FAR`) en verde. Tasa de acierto
medida: 89,0 % (GoldenEye) y 88,9 % (Perfect Dark), igual que la pila de regresos del perfilado.

**Medido en la PS3** (rec 1, GoldenEye en la presa, quieto ~30 s, 8 ventanas de 5 s): sin bandera 13,82
ns por ciclo del hilo de emulación, con `jit-destinos` 13,67 (**−1,05 %** de todo el hilo; el
código traducido es ~79 % del hilo, así que ≈ −1,3 % del nativo) y 92,4 % de acierto (82,7 M
aciertos, 6,8 M fallos; el lab dio 89 %). El modelo daba −1,8…−2,2 % del nativo: se pasó por
un factor ~1,5 (con la elasticidad de 0,58 a las palabras; lo medido pide ~0,4 para palabras de
esta clase). Calibración: los cambios que solo quitan palabras de un camino de salida (sin
quitar esperas de carga) rinden ~0,4 por palabra relativa, y hay que descontar además la parte
del hilo que no es código traducido (~21 %) al comparar con ns por ciclo del hilo completo.

## Entrada enlazada sin la prueba de la ktlb (candidato c0, no integrado)

Variante probada y descartada para main (se queda en la rama del laboratorio, con la bandera
`SALSA64_REC_OPT_LINK_E1` = 8192, `SALSA64_REC_E1=1` y el archivo `jit-entradas`): un bloque mapeado
tiene E0 (con la prueba de la traducción de su página) y E1 (justo después); el enlace estático
(`exit_link`, con el bit 0 de la palabra de destino como bandera) va a E1 si el origen es
mapeado, no tiene instrucciones de COP0 y el destino está en su misma página virtual. El
despachador, `exit_jr` y la caché de destinos entran por E0. Medido con la traza exacta: el 38,5 %
(36,1 %) de las entradas de GoldenEye (Perfect Dark) llegan por un enlace con bandera, 4,05
palabras menos por entrada: las palabras que corren bajan −1,7 % (−1,6 %), modelo −1,0 %, más la
cadena de latencia de la ktlb (~0,8 % fuera del modelo). **Estimación −1,5…−2 %** (menos que el
−3…−4,5 % que anuncié: la mitad de los enlaces cruzan de página o salen de bloques con COP0).
Como `--lockstep` no enlaza, se verificó comparando la RDRAM del recompilador (con enlaces,
E1 y JTC) con la del intérprete tras 300 campos desde el estado (GoldenEye y Perfect Dark:
idénticas), nemu64-test con las mismas fallas que la línea base, vitest OK y las pruebas
unitarias del recompilador en verde.

## FCR31 perezoso (candidato a): medido en el lab, no vale la pena solo

Prototipo del emisor (descartado, no está en el árbol): las rutinas de la FPU escriben dos bytes
(`1 + inexacto` y la bandera I pegajosa, esta solo si hubo inexacto) en vez de armar y comparar
FCR31; habría que plegarlos a FCR31 antes de cada entrada a C (`rec_enter`, los `rec_h_*`),
en CFC1/CTC1 y en C.cond. Medido con `rec-lab` (GoldenEye, perfil de la PS3): las palabras que
corren bajan solo de 9,87 a 9,74 por instrucción (−1,3 %, modelo −0,7 %): la compuerta de
FCR31 (cinco palabras) y la detección de inexacto (`frsp`, `fcmpu`, la banda) tienen que seguir,
y lo que se quita del armado de causas (~8 palabras) se paga con 6 de los bytes. Los eventos de
load-hit-store de FCR31 bajan de 6,1 a ~0 por 100 instrucciones, pero esa cifra era una cota
alta: el almacenamiento solo se ejecuta cuando cambia el estado de inexacto entre operaciones
seguidas (estimo 2–3 eventos por 100, ≈ 1 ciclo por instrucción, ~1,5 %). Total esperable
−1…−2 % por un cambio con muchos puntos de contacto con la exactitud. La estimación de
−4…−8 % de la primera pasada contaba como quitable la detección de inexacto, que no lo es.

## RSP → SPU (análisis, sin tocar el traductor)

Rama del traductor: la del recompilador del RSP (`src/rsp/jit`, docs/rsp-jit.md; aún no está en main).
`tools/lab/rsp-lab.c` (se compila contra ese árbol: `tools/lab/rsp-build.sh SRC`) traduce los trozos de
64 instrucciones de una imagen de IMEM de 4 KB y escribe un `.rsplab`; `tools/lab/lab-spu.py reporte`
decodifica el SPU (opcodes de `spu_asm.h`, `lab_spu_ops.py` generado de él) y simula en orden con doble
emisión el camino recto (latencias: simples 2, desplazamientos y rotaciones 4, multiplicaciones 7, cargas
6, `shufb` y rotaciones de cuádruple 4; dos por ciclo solo la par primero en dirección múltiplo de 8 y la
impar después, sin dependencia). Las imágenes de IMEM salen del microcódigo real de la última tarea
gráfica (la dirección `ucode` del OSTask en la DMEM del estado, leída de su RDRAM): GoldenEye (F3DEX
de Rare) y Perfect Dark (F3DPD), 16 trozos cada uno; con la HLE la IMEM del estado solo tiene el
arranque, no sirve. Lo medido (pesos uniformes por instrucción, estático):
* 11,7 (11,8) palabras del SPU por instrucción del RSP: escalares 1,9; vectoriales 19,3 (`vmadn` y
  `vmadm` 35; `vmudl` 8); cargas y almacenamientos vectoriales 15,8; escalares 14,6; saltos 14,0 (la
  tabla de entradas: `lqa` + `rotqbyi` + `bi`); `vrcp/vrsq` 7,7 (la rutina compartida).
* Reparto de tuberías: 73,5 % par, 26,5 % impar. **No hay `nop`/`lnop`** de relleno y la doble emisión
  ocurre en el 2,6 % de las palabras (el modelo da 0,57 instrucciones por ciclo): el código no está
  agendado para las dos tuberías. **No emite ni una `hbr`/`hbrr`/`hbra`** (el comentario de jit.c habla de
  `hbrr` en los saltos hacia atrás, no está): los ~355 saltos por 1024 instrucciones (`br`, `brz`, `bi`
  por la tabla) pagan 18 ciclos cuando se toman.
* Modelo: 20,7 ciclos por instrucción del RSP en camino recto (9,2 de esperas por dependencias: el
  contador `ai $121,$121,1` encadenado, `lqa → rotqbyi → bi` de ~10 ciclos en cada salto, `shufb` →
  `selb` → …) y ~24 con los saltos sin pista: **~7,5 ns por instrucción a 3,2 GHz** contra los 80–86
  que midió la PS3 con ps3-5 (antes del COP0 en línea y de VRCP) y la meta de 16. El modelo no ve las
  salidas al intérprete (~12 000 por tarea en DKR), el trampolín de COP0 (~100 instrucciones), los DMA
  ni la E/S: el resto de la brecha está ahí, no en la calidad del código recto. Para ordenar
  mejoras del código recto, lo que da el modelo: quitar los `ai` del contador por bloque (ya diseñado en
  docs/rsp-jit.md, «contador por bloque») acorta la cadena par de 2 ciclos por instrucción; agendar las
  dos tuberías y poner `hbr`/`hbrr` tienen techo teórico de ~2× y ~3 ciclos por instrucción.
Faltan pesos dinámicos (qué instrucciones ejecuta de verdad cada microcódigo: `salsa64-run --lle` da el
histograma) y un microcódigo de DKR, que necesita jugarlo.

## Pendiente

* RSP → SPU: pesos dinámicos y DKR (ver arriba).
* Contar los eventos de FCR31 con la rutina real (ramas condicionales por
  datos) en vez de la cota, y modelar la memoria del juego (símbolos de
  dirección en el recorrido).
* Una fuente de perfil con la TLB de ares: hoy el código mapeado de un volcado
  necesita `--map`/`--romwin` a mano.
