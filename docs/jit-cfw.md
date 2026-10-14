# El recompilador con CFW (issue #2)

Reporte: PS3 Slim CECH-2501A con **CFW Evilnat 4.91**; con v0.2.1–v0.2.3
ningún juego arrancaba. El log mostraba la ROM cargada y el guardado
abierto, pero nunca «páginas:» ni «recompilador activo», y la aplicación
seguía viva (el latido salía cada 5 s, con la presentación quieta «en
arranque del juego»). Con `/dev_hdd0/salsa64/sin-recompilador` (intérprete)
los juegos arrancaban.

El recompilador solo se había probado con HEN (docs/jit-ppe.md): su caché
de código es la cola NOBITS del segmento de texto (mk/rec-cache.ld), el
único lugar escribible y ejecutable que se encontró con HEN.

## Qué se colgaba

`run_game` (src/frontend/main.c) corre en el hilo principal, que también
presenta. Entre el log del guardado y «páginas:» solo hay dos cosas que
tocan algo distinto de la memoria de datos:

1. `salsa64_rec_init` → `emit_preamble`: **la primera escritura en la caché
   de código** (segmento de texto). Ninguna otra instrucción del arranque
   escribe ahí, y con HEN la escritura funciona.
2. `page_kb` (syscall 351, `sys_memory_get_page_attribute`) para el log de
   «páginas:».

Lo más probable es lo primero: con ese CFW el cargador mapea el segmento de
texto sin escritura, la escritura da un fallo de página (DSI) y lv2, en vez
de matar el proceso como hace con HEN (ver «Caídas» en docs/jit-ppe.md),
deja el hilo detenido. Los demás hilos siguen: el latido, el audio. La
aplicación no instala ningún manejador de excepciones (no hay `sys_dbg` en
el código), así que nadie espera al hilo: se queda ahí. La prueba de
abajo (`info`, `app`, `nobits`) lo dirá con certeza.

## Respaldo automático al intérprete

`src/platform/ps3/rec_cache.c` prueba la caché antes de usarla, en un hilo
aparte que se lanza al arrancar la aplicación (solo si no existe
`sin-recompilador`), con prioridad 2000, por debajo de todos los del
frontend:

1. consulta la página de la caché (syscall 351);
2. comprueba que la cola llegó en ceros;
3. escribe `mulli r3,r3,3 ; addi r3,r3,7 ; blr` al principio, limpia
   cachés (dcbst, sync, icbi, isync), la llama y espera f(5) = 22;
4. cambia la constante, limpia, la llama de nuevo y espera 24;
5. lo mismo en los últimos 128 bytes de la caché;
6. escribe una palabra (la misma que había) en cada página de 64 KB;
7. deja en ceros lo escrito.

`run_game` espera el resultado hasta 2 s desde que empezó la prueba. Con
HEN la prueba termina en menos de un milisegundo, mientras el menú arranca,
así que al abrir un juego ya está: no cambia nada ni cuesta tiempo. Si no
vuelve o falla, al log:

```
recompilador no disponible en esta consola (la escritura en la caché no volvió en 2 s): intérprete
```

y el juego corre con el intérprete. El hilo trabado se queda donde quedó
(no se une a nadie). Si sale bien, una línea con lo que tardó y los
atributos de la página:

```
recompilador: caché de código probada (180 µs; página de 64 KB, atributo 0x…, acceso 0x…)
```

Si el fallo matara el proceso en vez de trabar el hilo, el juego no
llegaría a abrir. Para eso la prueba deja antes `/dev_hdd0/salsa64/
jit-prueba.txt` con «probando <versión>» y lo borra al terminar: si al
arrancar lo encuentra con la misma versión, no repite la prueba y va al
intérprete («una prueba anterior cerró la aplicación»). Con otra versión
prueba de nuevo; borrar el archivo también.

## Prueba del JIT para usuarios («Salsa64 JIT test»)

`tests/phase0/jit-ppe/cfw.c`, `PRUEBA=cfw`:

```
tools/build.sh -C tests/phase0/jit-ppe PRUEBA=cfw pkg
```

Paquete `build/salsa64-jit-test/salsa64-jit-test.pkg`, título «Salsa64 JIT
test», APPID SLSA00118 (no pisa la aplicación). Muestra en pantalla el
avance y, al terminar, qué archivos adjuntar.

Vías, en orden (lo que con HEN mata el proceso, al final):

| Vía | Qué prueba |
|---|---|
| `info` | PS3MAPI (versión, tipo y versión del firmware), memoria de usuario (syscall 352) y atributos de página (syscall 351) de texto, caché, datos, heap y pila |
| `app` | la prueba de la aplicación (arriba), sobre la misma caché: misma cola de mk/rec-cache.ld |
| `text` | zona de nops de 64 KB dentro de `.text`, escritura directa |
| `nobits` | la caché de la aplicación (+1 MB), escritura directa |
| `ps3mapi_mem` | la caché (+2 MB), escrita por PS3MAPI (`SET_PROC_MEM`, syscall 8 de Cobra/Mamba y de HEN; escribe aunque la página sea de solo lectura) |
| `ps3mapi_paginas` | páginas ejecutables que reserva PS3MAPI (`PROC_PAGE_ALLOCATE` con `is_executable`), escritura directa |
| `dbg905` | la caché (+3 MB), escrita por `sys_dbg_write_process_memory` (syscall 905) |
| `heap`, `sysmem64k`, `mmapper` | memoria de datos (con HEN: mata el proceso al ejecutar) |

Opcodes y argumentos de PS3MAPI como en ps3mapi-lib (MIT,
github.com/bucanero/ps3mapi-lib); no se copia código. Cada vía mide
también cuánto cuesta escribir 12 bytes y 64 KB por su camino y limpiar
64 KB de caché: si con CFW solo sirve escribir por el sistema, eso decide
si el recompilador puede escribir sus bloques así.

Cada vía corre en un hilo aparte; el principal sigue presentando y la
espera con un límite de 4 s por fase. Lo que no vuelve queda `COLGADO
<fase>` y se sigue con la siguiente sin reiniciar. Antes de cada fase se
agrega `<vía> PROBANDO <fase>` a `/dev_hdd0/salsa64/jit_state.txt`: si el
proceso muere o la consola se cuelga, al abrir la prueba de nuevo esa vía
queda `CRASH <fase>` y sigue. Al terminar, el archivo pasa a
`/dev_hdd0/salsa64/jit_result.txt`; el log, en
`/dev_hdd0/salsa64/jit-test.log`. Estados: `OK`, `FALLO`, `COLGADO`,
`CRASH`, `NO_DISPONIBLE`.

### Instructions (English)

This test checks how your console lets Salsa64 run the code its dynamic
recompiler (JIT) generates. It doesn't change Salsa64 or your saves, and it
installs as a separate app («Salsa64 JIT test»). It takes about a minute.

1. Copy `salsa64-jit-test.pkg` to the root of a USB drive (or to
   `/dev_hdd0/packages`) and install it from **Package Manager → Install
   Package Files**.
2. Open **Salsa64 JIT test** from the Game column. The screen lists ten
   checks and fills in OK, FAIL, HUNG, CRASH or N/A next to each one. Wait;
   some checks are expected to fail.
3. **If it goes back to the XMB by itself**, just open Salsa64 JIT test
   again: it continues where it stopped. Repeat until the screen says
   **DONE**.
4. **If the console freezes completely** (no picture change and the PS
   button does nothing for one minute), hold the power button until the
   console turns off, turn it on and open Salsa64 JIT test again. It
   continues where it stopped.
5. When the screen says **DONE**, press X to exit. Then get these two files
   from the console and attach them to the GitHub issue:
   - `/dev_hdd0/salsa64/jit_result.txt`
   - `/dev_hdd0/salsa64/jit-test.log`

   With webMAN MOD you can download them from a browser on your computer at
   `http://<your PS3 IP>/dev_hdd0/salsa64/`; FTP, multiMAN or IRISMAN (copy
   to USB) work too.
6. Please also tell us your CFW (for example «Evilnat 4.91 CEX» or «DEX»)
   and whether Cobra is enabled.

Opening the test again after DONE runs everything again from the start;
the previous `jit_result.txt` is only replaced when the new run finishes.

### Instrucciones (español)

Esta prueba revisa cómo deja tu consola a Salsa64 ejecutar el código que
genera su recompilador dinámico (JIT). No cambia Salsa64 ni tus partidas, y
se instala como una aplicación aparte («Salsa64 JIT test»). Tarda alrededor
de un minuto.

1. Copia `salsa64-jit-test.pkg` a la raíz de una memoria USB (o a
   `/dev_hdd0/packages`) e instálalo desde **Package Manager → Install
   Package Files** (Gestor de paquetes → Instalar archivos de paquete).
2. Abre **Salsa64 JIT test** desde la columna Juego. La pantalla muestra
   diez pruebas y va poniendo OK, FAIL, HUNG, CRASH o N/A junto a cada una.
   Espera; es normal que algunas fallen.
3. **Si vuelve sola al XMB**, ábrela de nuevo: sigue donde se quedó. Repite
   hasta que la pantalla diga **LISTO** (DONE).
4. **Si la consola se congela por completo** (la imagen no cambia y el
   botón PS no responde durante un minuto), mantén pulsado el botón de
   encendido hasta que se apague, enciéndela y abre Salsa64 JIT test otra
   vez. Sigue donde se quedó.
5. Cuando diga **LISTO**, pulsa X para salir. Luego saca estos dos archivos
   de la consola y adjúntalos al issue de GitHub:
   - `/dev_hdd0/salsa64/jit_result.txt`
   - `/dev_hdd0/salsa64/jit-test.log`

   Con webMAN MOD se bajan desde un navegador en la computadora, en
   `http://<IP de tu PS3>/dev_hdd0/salsa64/`; también sirven FTP, multiMAN
   o IRISMAN (copiar a USB).
6. Dinos también tu CFW (por ejemplo «Evilnat 4.91 CEX» o «DEX») y si Cobra
   está activado.

Si abres la prueba otra vez después de LISTO, empieza todo de nuevo; el
`jit_result.txt` anterior solo se reemplaza cuando la nueva termina.

## Resultados

| Fecha | Consola | app | text | nobits | ps3mapi_mem | ps3mapi_paginas | dbg905 | datos |
|---|---|---|---|---|---|---|---|---|
| — | — | | | | | | | |
