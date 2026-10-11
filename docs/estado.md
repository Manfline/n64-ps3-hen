# Estado

## 2026-10-10 — Presentación: Banjo-Kazooie (FREEZE del RDP y destino equivocado), pausa de Zelda y recorte del overscan

Rama `bold-neumann-794fc0` (trabajo de presentación). Los tres casos
pendientes de `src/rdp` y uno más que apareció en el camino.

**Banjo-Kazooie no pasaba de 2 tareas gráficas** (en la Mac, en qemu
ppc64 con y sin recompilador, y en RPCS3: pantalla fija con el audio
sonando; en la PS3 real la intro corría porque el paquete naudio era
anterior). Bisección por commits del host: lo rompió la integración del
LLE (ce8351d), que dio semántica a DPC_STATUS (antes una lectura devolvía
la última palabra escrita). Rastreando los accesos al DP: el juego usa el
bit FREEZE como semáforo: en el manejador de la interrupción del DP escribe
SET_FREEZE, arranca la tarea del cuadro siguiente con el RDP congelado y
la descongela después; la interrupción de esa tarea la espera *después* de
descongelar (en la consola el RDP congelado no ejecuta). Nuestro HLE la
daba 10 000 ciclos tras el fin de la tarea, aún congelado, y el juego la
perdía. Arreglo en el núcleo (`src/core/rsp.c`, `sp.c`): si DPC_STATUS
tiene FREEZE cuando toca la interrupción del SYNC_FULL, se retiene
(`dp_held`) y se programa cuando el CPU escribe CLR_FREEZE
(`salsa64_rsp_dp_unfreeze`). Mismo principio que mupen64plus-core (que
difiere la tarea entera mientras FREEZE). Banjo corre: 644 tareas en 27 s
en la Mac. Prueba `rsp_gfx_task` intacta (la interrupción sigue llegando
`dp_cycles` después del fin de la tarea cuando no hay freeze).

**Parpadeo de Banjo en la PS3 (a):** el juego alterna dos framebuffers de
292 × 216 a 584 × 216 bytes uno del otro, y `src/rdp` supone 3/4 del ancho
de alto (219 líneas): el primero «cubre» el origen del VI del segundo con
sus últimas 2 líneas. Entre los dos, se tomaba el dibujado hace menos, que
mientras se muestra uno es el otro (el RDP ya dibuja el cuadro siguiente):
se presentaban 2 líneas negras estiradas a toda la pantalla. Simulado con
la traza de la Mac (VI y destinos por tarea, 70 s): 566 de 4142 campos con
el destino equivocado; con la regla nueva (entre destinos del ancho del VI,
el que empieza más cerca del origen; luego el más reciente), 12, todos de
arranque o cambio de escena (el otro framebuffer aún sin dibujar).

**Conker en la PS3 (3D quemado y parpadeando, capturas del RSX negras;
pedido en main tras integrar la rama de Rare):** mismo error
que Banjo. La repetición del volcado de la PS3 (campo 3600) en RPCS3 con
`tools/rpcs3-run.py --replay` dibuja la escena bien: la traducción al RSX
es correcta. En la Mac (70 s con `--trace-vi` y `--trace-gfx`) Conker
alterna dos framebuffers de 292 × 216 (0x3b7600 y 0x3d6300, a 216 líneas
uno del otro) y a mitad de cada tarea dibuja en la color image CI8 de 64 de
ancho (0xde080) y vuelve: con la regla vieja 591 de 4144 campos presentaban
las 2 últimas líneas del otro framebuffer (negras, o una franja de color
estirada a toda la pantalla: lo «quemado»); con la nueva, 0. Además, el
alto supuesto de un destino (3/4 del ancho: 48 para la de 64) ahora crece
hasta donde llegan las tijeras de los dibujos que recibe (`rdp.c` mira los
dibujos que siguen a cada TARGET; la CI8 de Conker es de 64 × 64 y su
FILL va de 0 a 65): la superficie se crea del alto real y, si una tarea
posterior llega más abajo, se vuelve a crear más grande. El alto de la
presentación (`h`) también lo usa. (rsxkit ya recortaba tijeras y
borrados a la superficie: no se escribía fuera.)

**Tabla de destinos portable:** `src/rdp/targets.c` (sin rsxkit): qué
color image tiene superficie, cuál se recicla y cuál se presenta; `rdp.c`
solo guarda las superficies por ranura. Pruebas en
`tests/unit/test_targets.c` (7): dos framebuffers alternos en direcciones
distintas que se solapan en la cuenta (Banjo, Conker), la misma dirección
con dos anchos (GoldenEye 320/440), color images de 64 sobre el
framebuffer (Zelda en pausa), VI sostenido sin dibujar (GoldenEye
cargando), dibujado con el VI apagado (SM64), el orden de reciclado y el
alto por las tijeras (Conker).

**Pausa de Zelda negra en la PS3:** con el menú, cada tarea pone 3 color
images y alternando framebuffers son 6 distintas (0x3b8800/64,
0x3b5000/64, 0x3b5000/320, 0x3de000/64, 0x3da800/64, 0x3da800/320) para 4
ranuras: cada tarea tiraba 2 y entre ellas el framebuffer de 320. Ahora
8 ranuras (`RDP_TARGETS_MAX`; memoria del RSX en docs/memory-budget.md) y,
al reciclar, nunca la presentada en el último campo, primero las viejas
(30 campos), luego las de un ancho distinto al del VI (auxiliares) y al
final la más antigua. Reproducido en la Mac con `docs/pads/zelda-oot.pad`
(la partida del autor bajada de la PS3 por FTP a `build/saves/`, fuera de
git): los TARGET de la traza son esos; el rasterizador de la Mac no cambia.

**Recorte del overscan:** `SALSA64_DATA_DIR/overscan.cfg`, en píxeles de la
N64 a 320 × 240 por lado (un número: todos; dos: horizontal y vertical;
cuatro: izquierda, derecha, arriba, abajo; `0` lo apaga). **Encendido por
omisión con 8 y 6 (2,5 % por lado)**: es lo que una tele de tubo esconde y
donde los juegos dejan basura (las columnas azules de GoldenEye; los HUD
quedan dentro). Se escala con la imagen del VI (a 640 de ancho recorta
16). Vale para el destino del RSX y para la copia de la RDRAM; las ROMs de
prueba se presentan enteras (sus verificaciones leen la pantalla por
posición) y las capturas salen del tamaño recortado. En rsxkit,
`rsxkit_target_present` acepta ahora cualquier columna (la textura toma la
fila entera y la columna va en las coordenadas de textura; antes el offset
se redondeaba a 32 píxeles).

**Fondo negro de Banjo (c) — no es presentación:** en la Mac también sale
negro (cielo y paisaje) tras Banjo tocando. En la tarea del campo 2400 el
primer dibujo tras el borrado son 8 triángulos con textura, w de −88 a
1321 y z/w hasta 117 (el domo del cielo): el rasterizador de la Mac los
descarta por w ≤ 0 y en el RSX quedan fuera por z/w > 1 (el z normalizado
se pasa de 1 también en objetos normales: 1,05–1,36). Le toca a la HLE
(trabajo de F3D): la proyección o el recorte de esos triángulos. El síntoma
(b) (escena «desplazada», mitad izquierda vacía, pasto solo abajo) coincide
con la escena sin fondo: Banjo a la derecha sobre una franja de pasto y
todo lo demás negro. Pedir al autor que lo vuelva a mirar con el fondo
arreglado antes de buscar otra cosa.

Herramientas (`salsa64-run`): `--pad "0:…"` conecta el control desde el
arranque (antes el primer estado entraba al terminar el campo 1; en la PS3
el puerto 1 siempre está); `--trace-exc` imprime los bits IP de Cause y
MI_INTR/MASK; variables `SALSA64_GFX_CYCLES`, `SALSA64_DP_CYCLES` y
`SALSA64_AUDIO_CYCLES` para probar otros tiempos del RSP; ganchos de
depuración `-DSALSA64_DEBUG_PI` (DMA del PI) y `-DSALSA64_DEBUG_DP`
(registros del DP, escrituras de SP_STATUS e interrupción del DP), como
`SALSA64_DEBUG_SPMEM`.

Verificación (con main 705f4ac fusionado): `tools/build.sh test-host` 326
(7 de targets); `tools/test-roms.sh` (nemu64-test 433 con HLE y 136 con
LLE, vitest, controles, savetest ×6, gfxtest ×3, audiotest); test-phase2 en
RPCS3 (gfxtest 22/22, spu-jobs, frontend F3DEX2 y F3D); `tools/test-rec.sh`
(unitarias big-endian con el recompilador ×4, nemu64-test 433, vitest);
`tools/build.sh` sin advertencias del compilador. Paquete subido a
`/dev_hdd0/packages/salsa64-presentacion.pkg`; los respaldos de Banjo y
Conker ya están en `/dev_hdd0/salsa64/`.

Pendiente:
- [ ] PS3 real (paquete «Salsa64 presentación», SLSA00084): Banjo sin
      parpadeo y entero (su respaldo ya está en `/dev_hdd0/salsa64/banjo.z64`),
      Conker con el 3D bien (`conker.z64`), pausa de Zelda visible, GoldenEye
      sin las columnas azules, un minuto de Zelda, SM64 y Mario Kart sin
      regresiones. El autor confirma.
- [ ] HLE: el cielo y el paisaje de Banjo (arriba). Trabajo de F3D.
- [ ] LLE: con `--lle=all` Banjo no arranca (la primera tarea corre 958 M
      instrucciones sin tocar COP0); no es del freeze. Trabajo del LLE.
- [ ] El VI de Banjo muestra 213 de 240 líneas (V_VIDEO 61–488): en una
      tele quedarían bandas; aquí se estira a 4:3. Colocar la imagen según
      la ventana del VI cuando haya un caso que lo necesite.

## 2026-10-10 — Kokiri «en colores planos»: es el sol cenital, no la HLE (referencia con mupen64plus)

Reporte (PS3 y rasterizador de la Mac): en el bosque
Kokiri de Zelda OoT el mundo sale en colores planos, «verde del shade», con
la HUD bien; la casa de Link sí se ve con texturas.

Diagnóstico (rama `frosty-golick-d1d71a`, en la Mac):
- Las texturas **sí** llegan y se decodifican bien: corteza CI8 con paleta,
  pasto RGBA16 (dos tiles, lerp por ENV_ALPHA), siluetas de troncos I4;
  los combiners son exactos (T0·SHADE·PRIM y lerp(T0,T1)·SHADE), `ntex` =
  1 o 2, las s/t por triángulo cubren ~20 texels. En el píxel (270,80) del
  tronco el rasterizador escribe T0 = 0,54 × X0 = 0,23 → 0,13.
- Lo que aplasta la textura es el **SHADE**: con `G_LIGHTING` los vértices
  verticales quedan en 80–120/255. Luces de la escena: sol blanco (255)
  con dirección (0, 0,99, 0,16), relleno (70,70,90) opuesto y ambiente
  80. El sol es cenital porque el juego fija `dayTime` = 0x8000 (mediodía
  exacto) al cargar la partida (leído de `gSaveContext` en el volcado de
  la RDRAM; OoT 1.2, 0x8011AC80). Las normales son normales de verdad
  (longitud ~120), no colores horneados.
- **Referencia**: mupen64plus 2.6 (brew) con GLide64 y rsp-hle, con la
  misma partida del autor, movido por `tools/m64p-ref.sh` (plugin de
  entrada propio que reproduce el `--pad`; `docs/pads/zelda-kokiri.pad`
  saca a Link al balcón). Mismos parches en 640×480: piso (56,56,3) vs
  Salsa64 (55,54,1); tronco (31,29,17) vs (29,27,15); pared (43,27,18) vs
  (37,23,14). Diferencias < 6/255 en todo: la escena **es así** a esa hora
  (el piso recibe el sol de lleno, pero sus texturas ya tienen media
  55/255). Imagen comparativa en `build/m64p-ref/kokiri-comparacion.png`
  (fuera de git).
- Única diferencia visible: zonas crema (arriba a la derecha y bajo el
  balcón) donde el rasterizador de la Mac descarta triángulos con algún
  w ≤ 0 (la pared lejana y el piso cruzan el plano de la cámara); GLide64
  los recorta. El RSX recorta en hardware, así que en la PS3 no deberían
  faltar; comprobarlo cuando la consola esté libre.
- **Confirmación con el microcódigo real** (pedida en main:
  GLide64 también es HLE): con main ce8351d fusionado, `salsa64-run
  --lle=all` ejecuta el F3DZEX2 real (739 M instrucciones hasta el campo
  3712) y sus triángulos crudos, con los colores de vértice que calcula el
  RSP de verdad, dan los mismos parches en el campo 3700: piso (55,55,2),
  tronco (29,27,15), pared (37,23,14), túnica (53,65,28), frente a la HLE
  (55,54,1), (29,27,15), (37,23,14), (52,65,28). Las luces, normales y
  ambiente de la HLE coinciden con el microcódigo. Y en la imagen del LLE
  la pared lejana sí aparece (el microcódigo recorta contra el frustum
  antes de emitir los triángulos): las zonas crema de la HLE son solo el
  w ≤ 0 del rasterizador de la Mac.
- No se cambió nada de la HLE. Si en la PS3 la escena se ve distinta a la
  Mac (verde liso sin la corteza tenue), el problema sería de src/rdp o
  del RSX, no de la HLE: pendiente de confirmar con el autor.

Herramientas que quedan (sin cambios funcionales):
- Depuración de la HLE en el host con `EXTRA_CFLAGS=-DSALSA64_GFX_TRACE`
  (cabecera de `src/host/run.c`): estado del RDP por dibujo (combiner,
  othermode, tiles, luces, términos k), rango de s/t, vértices de un dibujo
  (`SALSA64_GFX_VDUMP`), texturas en PPM (`SALSA64_GFX_TEXDUMP`), dibujos
  pintados de magenta (`SALSA64_GFX_MARK`) y traza de un píxel
  (`SALSA64_GFX_PIX`) en el rasterizador. En `rdp.c` es solo un gancho bajo
  `#ifdef`; nada entra en el SPE.
- `tools/m64p-ref.sh` + `tools/m64p-pad/pad_plugin.c`: captura de
  referencia con mupen64plus (requiere `brew install mupen64plus`; el
  teclado de su plugin de entrada no funciona con sdl2-compat; el `.sra` de
  mupen64plus lleva los bytes de cada palabra de 32 bits invertidos; sus
  cuadros de `--testshots` no son campos del VI).
- El pad de Kokiri sirve de punto de partida para el menú de pausa y el
  resto de pendientes de Zelda.

Pendiente:
- [ ] PS3: confirmar que Kokiri se ve como en la Mac/mupen64plus (oscuro,
      textura tenue) y que no faltan la pared lejana ni el piso con w ≤ 0.
- [ ] Rasterizador de la Mac: recorte contra w = 0 (hoy descarta el
      triángulo entero), para que las capturas de referencia no tengan
      huecos crema.

## 2026-10-10 — Microcódigos gráficos de Rare: Perfect Dark y Conker

Hecho (rama `competent-haslett-3bad2a`, con main 3270538 y la rama
de arranque `amazing-noether-39a548` con `osCicId`, sin integrar):
- **Perfect Dark** (`src/rsp/gfx/f3dpd.c`): Fast3D de Rare. Solo cambian
  G_VTX (vértices de 12 bytes: x, y, z, banderas, offset del color, s, t) y
  G_VTXCOLORBASE (0x07, la tabla de colores; 4 bytes R, G, B, A o normal y
  A con G_LIGHTING); G_TRI4 en 0xB1 ya lo tenía f3d.c (como GoldenEye) y
  G_QUAD en 0xB5 con índices ×10. Cada vértice se arma en el formato de 16
  bytes y sigue el camino de siempre. El microcódigo no trae texto: se
  reconoce por el CRC-32 de su código (c543d0a8, 0x1000 bytes) solo si
  ningún texto lo identificó (`src/core/rsp.c`); `salsa64_crc32` pasó a una
  tabla de nibbles (dos pasos por byte en vez de ocho).
- **Conker** (`src/rsp/gfx/f3dexbg.c`, «F3DEXBG.NoN fifo 2.08», por su
  texto): F3DEX2 con G_TRI4 en 0x10–0x1F (índices de 5 bits; el 0x14 era
  eso), luces de 48 bytes con luz puntual (posición e intensidad),
  G_MW_COORD_MOD, tabla de normales aparte (G_MV_NORMALS) y la luz que
  multiplica el color del vértice. **Por qué no mandaba otra tarea:** su
  G_LOAD_UCODE no cambia de GBI (pasa a la iluminación avanzada y la lista
  sigue); la HLE de F3DEX2 terminaba la tarea ahí, sin llegar al
  G_RDPFULLSYNC, y el juego esperaba la interrupción del DP para siempre.
  Con main 53fb223 F3DEX2 salta el tramo de un microcódigo ajeno
  (`gfx_dl_skip_foreign`, el S2DEX de Zelda); en Conker eso perdería todo lo
  que dibuja después, así que su hook atiende el 0xDD antes que F3DEX2.
- Cambios en lo común, mínimos: las familias aceptan un *hook* con los
  comandos propios de una variante (`gfx_dl_f3d(g, hook)`,
  `gfx_dl_f3dex2(g, hook)`); geom.c separa la transformación de la lectura
  (`gfx_vertices`) y expone las luces en espacio del modelo y el texgen;
  `gfx_state.rare` (~250 bytes) con el estado de las dos variantes; dos GBI
  nuevos en rcp.h; F3DEX2, F3D y F3DEX no cambian (mismas pruebas y
  mismas imágenes de gfxtest).
- SPE 0: Perfect Dark 1,9 KB y Conker 4,4 KB de código; texto 73 KB (con main
  755cbaa), bss 145 KB, pila ~37 KB (antes ~46; `src/spu/gfx/main.c`). `GFX_MAX_LIGHTS`
  pasó de 8 a 11: Conker pide hasta 11 luces (`G_MW_NUMLIGHT` = 0x210).
- Pruebas: suite `gfx-rare` (9: colores por tabla, luz con la normal en la
  tabla, G_TRI4 y G_QUAD de PD; G_TRI4 de 5 bits, G_LOAD_UCODE que no corta
  la lista, luz básica y avanzada, 11 luces, COORD_MOD, lookat, normales y
  texgen de Conker) y en `rsp` la detección por texto y por CRC (4 KB
  sintéticos con 4 bytes calculados para dar el CRC de PD) y el despacho al
  backend: 297 en el host. Mutaciones: 6 de 6 detectadas.
- Host (respaldos propios, fuera del repo), 3 minutos sin tocar el
  control: **Perfect Dark** 6914 tareas, 0 comandos sin implementar, 0
  errores (identificación del producto en alta resolución, logo de
  Nintendo, cubo de N64 que se vuelve el logo de Rare, la ciudad, Joanna en
  la azotea); **Conker** 4468 tareas, 0 sin implementar, 0 aproximados
  (pantalla legal, logos, Conker con la motosierra, la taberna, menú
  «GAME 1»). Detalle en docs/compatibilidad.md.
- Con main 755cbaa fusionado: build sin advertencias, test-roms (nemu64-test
  446, vitest, controles, savetest, gfxtest ×3 con 22/22, audiotest),
  test-phase1 y test-phase2 en RPCS3 (297/297 big-endian antes de la última
  fusión), test-rec en qemu (nemu64-test con la línea base y vitest con el
  recompilador; las unitarias pasan hasta `gfx-rare` en los cuatro modos y
  se caen en `sync`, como en main). Sin regresión: SM64, Mario Kart 64,
  GoldenEye, Star Fox 64 y Banjo-Kazooie dan, con main y con esta rama, el
  mismo resumen gráfico y la misma imagen a los 60 s.
- Depuración en el host: `EXTRA_CFLAGS=-DSALSA64_DEBUG_DL` vuelca las
  display lists (`SALSA64_DEBUG_DL=N`, `SALSA64_DEBUG_DL_FROM=k`) y
  `-DRASTER_W=640 -DRASTER_H=480` agranda el rasterizador para pantallas de
  alta resolución.

Pendiente:
- [x] **PS3 real** (paquete «Salsa64 Rare», SLSA00081, be8f573 con main
      ce8351d): **Perfect Dark** se ve y suena bien (el autor: «se ve y suena
      espectacular»); 59,9 campos/s (100 % de NTSC) salvo la intro en alta
      resolución (84–94 %), HLE en el SPE 1,4–8 ms por tarea, 0 tareas
      cortadas, naudio con MP3 sin errores. **Mario Kart 64** (no
      regresión): 65 s al 100 % de NTSC, 0 tareas cortadas. `embedded`
      quedó como estaba (`file:oot.z64`).
- [x] **Conker en la PS3** (resuelto en la entrada «Presentación:
      Banjo-Kazooie…» de arriba, falta confirmar en la PS3): arranca y
      suena, pero el 3D sale quemado y parpadea y no se ve el logo de N64; las capturas del RSX de las
      escenas 3D (campos 3000, 3600, 4200) salen negras y el logo 2D de
      Nintendo sí. El volcado de la HLE hecho en el SPE (`gfxdump` del campo
      3600) dibujado en la Mac con `--raster-dump` sale bien e igual al
      host: la HLE es correcta y la falla es de la traducción o la
      presentación del RSX (`src/rdp/rdp.c`, lib/rsxkit). Lo propio de
      Conker: a mitad de cada cuadro dibuja en un destino CI de 8 bits y 64
      de ancho (0xDE080) y vuelve al principal (0x3D6300, 292×213 RGBA16, z
      en 0xE9D80). Pasado al trabajo de presentación (mismo tipo que la pausa
      de Zelda).
- [ ] Perfect Dark aproxima `lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE` (el de
      GoldenEye): término S·Sa en el RSX (Fase 4).
- [ ] Efectos de framebuffer de los dos (cámara espía y monitores de PD,
      copias del z-buffer, desenfoque de la pausa y sombra de Conker): Fase
      4; lista en docs/compatibilidad.md.
- [x] Sonido: naudio de los dos (trabajo de audio, en main desde ce8351d).
- [ ] La luz de Conker sigue a GLideN64 (en el modo básico la direccional
      no cuenta); contrastar con la PS3 cuando haya escenas de juego.

## Pendiente: trabajo de presentación (anotado el 2026-10-10)

Resuelto en la entrada «Presentación: Banjo-Kazooie…» de arriba (falta
confirmar en la PS3). Los tres casos estaban en `src/rdp/rdp.c` (qué
destino del RSX se presenta en cada campo):
- [x] **Banjo-Kazooie parpadea** muy fuerte (visto por el autor en la PS3;
      video de 1,6 s en la intro, Banjo tocando el banjo). Tres síntomas: (a)
      ~1 de cada 3 cuadros del video (30 cps) sale casi negro con solo la parte
      de arriba de la cabeza de Banjo: imagen vieja o a medio dibujar
      presentada entre las buenas; (b) toda la escena sale desplazada a la
      derecha y abajo (mitad izquierda vacía, Banjo cortado en el borde
      derecho, el pasto solo en un rectángulo abajo): revisar origen/escala del
      VI, viewport y scissor; (c) falta el fondo (cielo y paisaje negros).
      Revisar cómo alterna sus framebuffers y si usa efectos que leen el
      framebuffer.
- [x] **Menú de pausa de Zelda negro en la PS3:** el juego pone color images
      de 64 de ancho en la dirección del framebuffer y se pierde el destino de
      320 («0 del RSX»); en el rasterizador de la Mac se ve bien.
- [x] **Recorte del overscan:** GoldenEye deja franjas azules a los costados
      (su scissor 3D cubre las columnas 1–318 sobre un relleno azul).

## 2026-10-10 — Fase 6: prototipo de LLE del RSP (microcódigo real) en host y SPE

Hecho (rama `nervous-lewin-ae6137`, desde main 3270538; sin
integrar; diseño y mediciones en docs/rsp-lle.md):
- **Intérprete portable del RSP** (`src/rsp/lle/`: escalar, COP0, unidad
  vectorial completa con el acumulador de 48 bits y VCO/VCC/VCE, recíprocos
  por tabla, todas las cargas/almacenamientos vectoriales). Semántica de
  los casos raros de ares (ISC, registrado en THIRD_PARTY.md). Mismo código
  en host, PPE y SPE, sin `#ifdef` de orden de bytes.
- **nemu64-test con `--lle`: fallan 136 de 4637 (97 %) frente a 433 (90 %)
  sin LLE. Las 214 pruebas «RSP …» pasan todas**; quedan 4 «RDP STATUS»
  (el RDP no escribe el framebuffer en la RDRAM). Sin regresiones; 5
  mejoras en el camino HLE (pares de bits de SP_STATUS, DPC_START/END).
  `tools/test-roms.sh` corre nemu64 dos veces (líneas base
  `nemu64-test.fallas`, 289 nombres, y `nemu64-test-lle.fallas`, 71).
- **Enganche** (`rsp.c`: backend `salsa64_lle_backend`, modos
  `off/auto/all`; `sp.c`: sincronía, SP_STATUS atómico, DPC con semántica
  real) y **RDP a bajo nivel** (`src/rsp/gfx/dpc.c`: los comandos crudos
  de DPC_START..END, también por xbus, van a `gfx_rdp_command`/
  `gfx_rdp_triangle`, el camino de triángulos crudos existente).
- **Host**: `salsa64-run --lle[=auto|all]`, `--lle-speed`,
  `--lle-check-audio` (cada tarea de audio también en la HLE sobre una
  copia de la RDRAM, comparando lo escrito). SM64: el Fast3D real corre y
  el título con la cara de Mario sale igual que en HLE. Zelda: el nead
  real suena (211 M instrucciones en 16 s); las primeras 30 tareas de
  audio idénticas a la HLE byte a byte, y desde el primer sonido difiere
  solo FILTER, donde mupen64plus-rsp-hle promedia las tablas (`>> 1`) y el
  microcódigo real no → **la HLE de FILTER probablemente va a la mitad**;
  confirmar de oído.
- **SPE** (`src/spu/rsp/`, `src/platform/ps3/rsp_spu.c`): trabajo de
  spukit con DMEM/IMEM en el local store, SP_STATUS y semáforo por DMA
  atómico (getllar/putllc) contra los `__atomic_*` del PPE, DMA del SP
  entre la RDRAM y el local store, registros del DP en el SPE con captura
  de los comandos xbus para que el PPE los reproduzca (`salsa64_dp_submit`),
  continuaciones cada 200 000 instrucciones. Frontend: archivo
  `SALSA64_DATA_DIR/lle` con `auto` o `all`; líneas «rsp lle:» en el log.
- 12 pruebas unitarias nuevas (301 en total con main).
- Medido en el host: 12,5 ns por instrucción del RSP (docs/perf-log.md).
- **PS3 real** (turno del autor): sanidad con HLE por omisión bien (Zelda
  106 s al 100 % con audio; GoldenEye intro al 100 %, nivel a 36–50
  campos/s con el audio estirado: primera medición del nivel, no es de
  esta rama). Con `lle` = `all`, **Zelda se ve y suena con el microcódigo
  real en el SPE**: 226–283 ns por instrucción (3,5–4,4 MIPS; el RSP real
  ~62), 24–55 campos/s. Tres arreglos sobre la marcha (SP_PC pisado al
  recoger el trabajo, estado del DP entre trabajos, DMA de muchas filas de
  S2DEX en una transferencia) e instrumentación (contadores de COP0,
  latido del SPE). Queda una divergencia: tras los fondos S2DEX de la
  intro (~36 s) el microcódigo entra en un bucle sin fin que el host no
  reproduce (detalle y pista en docs/rsp-lle.md).

Pendiente:
- [ ] Divergencia del SPE tras S2DEX: modelo del protocolo de trabajos del
      SPE en el host (`lle_spu_run` con memcpy y cortes por `max_instr`)
      para reproducirla con trazas; sin turnos de consola.
- [ ] Confirmar de oído FILTER (HLE a la mitad vs microcódigo real).
- [ ] Comparar la imagen de Zelda en LLE con la HLE (host, `--gfx-frame`).
- [ ] Recompilador RSP → SPU (plan en docs/rsp-lle.md): es lo único que
      lleva de 0,06× a velocidad real; mover el consumidor del RDP al SPE.
- [ ] Homebrew de libdragon (rspq/rdpq) como prueba sin HLE.
- [ ] Limitaciones del SPE: escrituras del CPU en DMEM/IMEM durante una
      tarea, limpiar la interrupción del SP desde el microcódigo.

## 2026-10-10 — Zelda OoT se colgaba al elegir la ranura: G_LOAD_UCODE (S2DEX) y el RDP que nunca terminaba

Síntoma (PS3 real, main f698afe): al elegir una partida, pantalla negra con
una rayita amarilla arriba a la izquierda, el audio seguía (en silencio) y
no llegaba ninguna tarea gráfica más; 96 % en bucles de espera.

Diagnóstico (rama `fervent-antonelli-6904bf`, en la Mac con el
intérprete, sin PS3):
- Se reproduce igual con el intérprete y la HLE síncrona: no es del
  recompilador, ni del sondeo del SPE, ni del DMA del PI.
- Al cargar la primera sala (la casa de Link, con fondo prerrenderizado) el
  juego manda 75 tareas tipo 4 (su decodificador JPEG; sin HLE, terminan sin
  hacer nada) y una tarea gráfica cuya lista carga **S2DEX** con
  `G_LOAD_UCODE` para copiar el fondo (`uObjBg`) y después vuelve a F3DZEX.
  La HLE terminaba la tarea en el `G_LOAD_UCODE`, sin llegar al
  `G_RDPFULLSYNC`: TASKDONE sí, interrupción del DP no. El planificador de
  libultra nunca dio la tarea por terminada, el hilo gráfico de OoT esperó
  su mensaje y a los 3 s su vigilante declaró «RCP is HUNG UP»: el hilo de
  fallas dibuja el rectangulito amarillo (el de `Fault_DrawCornerRec`) y el
  juego queda muerto con el audio vivo. Visto con el volcado de la RDRAM:
  todos los hilos esperando en colas vacías.
- Nadie había pasado de la selección de archivo: no era regresión.

Arreglo:
- `src/rsp/gfx/f3dex2.c`: un `G_LOAD_UCODE` a otra dirección que la del
  microcódigo de la tarea (`job->ucode`, nuevo en `gfx_job`) ya no termina
  la lista: se saltan sus comandos (siguiendo `G_DL` y `G_ENDDL`, que la
  familia comparte) hasta el `G_LOAD_UCODE` que vuelve al de la tarea, y la
  lista sigue (Link, objetos, HUD y el FULLSYNC se ejecutan; el fondo
  prerrenderizado queda negro). `res->skipped` cuenta los saltados; la
  bandera `GFX_RES_LOAD_UCODE` marca que hubo un tramo ajeno.
- `src/core/rsp.c`: si la HLE corta una lista antes de su fin
  (`SALSA64_GFX_CUT`: búfer lleno, lista interminable, error de dirección;
  lo traduce `salsa64_gfx_task_flags` en sync.c y gfx_spu.c), la
  interrupción del DP llega de todos modos: el juego espera al RDP por cada
  tarea gráfica. Estadística `cut` (frontend: «tareas cortadas» y «comandos
  de otro microcódigo saltados» en la línea `gfx:`).
- Pruebas: `rsp_gfx_task_cut` (DP con lista cortada; sin DP si acabó en
  ENDDL sin FULLSYNC) y `gfx_load_ucode_skips_foreign` (mismo microcódigo
  sigue; tramo ajeno con subrutina saltado, 2 triángulos, FULLSYNC; tramo
  que nunca vuelve = cortado; FULLSYNC dentro del tramo cuenta). La ROM
  gfxtest lleva al final de cada lista un tramo de otro microcódigo con un
  FILLRECT rojo que taparía la escena si se ejecutara, y en el cuadro
  GFXTEST_CUT_FRAME (7) termina dentro de él: la ROM exige la interrupción
  del DP y test-roms.sh exige «1 tareas cortadas» y tramos saltados. 272
  unitarias en el host (clang nativo y Docker con ASan); test-roms
  (nemu64-test 446, vitest, controles, savetest, gfxtest 22/22, audiotest)
  OK; build sin advertencias.
- Mac: con el arreglo el juego sigue (tareas gráficas a 20/s, la escena del
  Árbol Deku con su texto); ~7 comandos de S2DEX saltados por cuadro.

Herramientas nuevas para esto (`salsa64-run`): `--pad
"CAMPO:BOTONES[@X,Y];…"` (control 1 programado por campo del VI, cada estado
hasta el siguiente; unificado con el `--pad` que traía main: misma sintaxis,
campo por campo y también en la referencia de `--lockstep`), `--trace-from CAMPO`,
`--trace-faults N` (excepciones sin las interrupciones), `--ram-dump
ARCHIVO`, y `tools/libultra-threads.py` (hilos de libultra en un volcado:
estado, pc y la cola en la que espera cada uno, sin símbolos). Con `--pad
"2200:START,2320:A,2440:A,2560:A"` y el `.sra` del autor en `--save-dir`,
Zelda entra al juego en el campo 2712.

- **PS3 real** (paquete «Salsa64 depuración», SLSA00079, ba9bdfc): Zelda
  entra al juego y el autor jugó 7 min (intro, casa de Link, Navi, bosque
  Kokiri) a 59,8 campos/s, 0 tareas cortadas, ~7 comandos de S2DEX
  saltados por cuadro en la casa; audio −4 a −11 dBFS en juego. El silencio
  de ~30 s entre elegir la partida y el vuelo de Navi es del juego: la
  Mac con sus mismas pulsaciones calla y suena en los mismos campos, y
  en ese tramo las listas de audio no traen voces (solo CLEARBUFF,
  LOADBUFF, MIXER, SAVEBUFF, DMEMMOVE, INTERLEAVE).

Con main 485a747 traído (F3D, arranque del CIC 6105 con osCicId): Zelda
sigue pasando la ranura en la Mac (el arranque nuevo no cambia nada aquí);
el salto de tramos ajenos vive ahora en `gfx_dl_skip_foreign` (dl.c) y lo
usan F3DEX2/F3DZEX y F3DEX 1.x (F3D no tiene G_LOAD_UCODE: la variante F3D
de gfxtest no lleva el tramo). Con `job->ucode` = 0 (tarea sin microcódigo
conocido, como en las pruebas de F3D) todo G_LOAD_UCODE es ajeno.

Visto en esa corrida, para otros trabajos (reproducible en la Mac con
`--pad "$(cat docs/pads/zelda-oot.pad)"`, las pulsaciones del autor
sacadas de su log; receta en la entrada de arriba):
- [x] **Texturas del mundo**: en el bosque Kokiri la geometría salía en
      colores planos (verde del shade), igual en el rasterizador de la Mac.
      No era error: es el sol cenital de mediodía (ver la entrada «Kokiri
      “en colores planos”», con la referencia de mupen64plus). Queda
      confirmar en la PS3 que se ve como en la Mac.
- [x] **Menú de pausa en negro** (frontend/rdp; resuelto por el trabajo de
      presentación, `src/rdp/targets.c`): con el menú abierto cada
      tarea (30/s) pone tres color images: dos de **64 de ancho** (0x3b8800
      y **0x3b5000, la misma dirección del framebuffer**) y luego
      0x3b5000/320; el VI sigue mostrando 0x3b5280/0x3daa80. En la PS3 el
      log dice «0 del RSX» y ~2 destinos creados por tarea: el destino de
      320 se pierde al crear uno de 64 en la misma dirección y la
      presentación cae a la copia de la RDRAM (negra, el RSX no la
      escribe). Le toca a src/rdp (destinos por dirección y ancho, o no
      tirar el de 320). Cerrar el menú vuelve al juego. Reproducible en la
      Mac: `--pad "$(cat docs/pads/zelda-oot.pad)" --trace-from 19100
      --trace-gfx 80` muestra los TARGET, y con `--gfx-frame-every 6` el
      rasterizador de referencia dibuja el menú (mapa del bosque Kokiri)
      bien: la HLE está bien, es solo la presentación.

Falta (para ver bien la casa de Link y las salas con fondo prerrenderizado):
- [ ] HLE de S2DEX para el fondo (`G_BG_COPY`/`G_BG_1CYC` con `uObjBg` →
      rectángulos con textura por tiras) y detectar el microcódigo que
      carga `G_LOAD_UCODE` por el texto de sus datos (como en rsp.c).
- [ ] Tarea tipo 4 de OoT (decodificador JPEG `njpgdspMain`: IDCT y
      YUV→RGBA16 de los macrobloques que el CPU ya decodificó): sin ella el
      fondo sale negro aunque S2DEX lo copie. mupen64plus-rsp-hle la tiene
      (`jpeg.c`, GPLv2+; registrar en THIRD_PARTY si se adapta). Son 75
      tareas por sala, una vez (no por cuadro).

## 2026-10-10 — naudio: Banjo-Kazooie, Conker, Perfect Dark (con MP3) y Donkey Kong 64

Hecho (rama `charming-allen-a8e4b3`, con main 485a747):
- **naudio** en la HLE portable (`src/rsp/audio/abi.c`): las tablas de
  naudio, Banjo-Kazooie (la misma), Donkey Kong 64 (7 y 8 son MIXER) y los
  de MP3 (Perfect Dark, Banjo-Tooie, Jet Force Gemini y Conker: 0
  OVERLOAD, 7 MP3, 8 MP3ADDY, 0x0E filtro). Búferes fijos de 0x170 bytes
  desde 0x4F0; ADPCM y RESAMPLE con el estado en w1; SETVOL con sus tres
  formas y el 0x0E que salta a la mitad de SETVOL. IIR y OVERLOAD en
  `alist.c`. Codificación verificada contra `alist_naudio.c` de
  mupen64plus-rsp-hle (8a7a472a). La detección ya existía
  (`src/core/rsp_audio.c`, palabra [0x10] de `ucode_data`).
- **MP3** (`src/rsp/audio/mp3.c`): la síntesis de MPEG-1 Layer III que el
  microcódigo hace en el RSP (DCT de 32 y ventana de 512), adaptada de
  `mp3.c` del original e igual a él bit a bit (2304 muestras comparadas con
  el original compilado aparte, fuera del repo). La ventana **no** es la
  tabla del microcódigo: `tools/audio-mp3-lut.py` la genera desde la
  ventana estándar (ISO 11172-3, vía `ff_mpa_enwindow` de FFmpeg, LGPL);
  coincide con la del original en todas las entradas que se leen.
  Memoria de trabajo aparte de la DMEM de las listas (2 KB, persiste entre
  comandos como el anillo de la ventana). Las dos palabras de ganancia del
  comando: Perfect Dark manda +16 y −16; la segunda corrige el signo de
  las muestras 17–31 de las ranuras impares.
- **SPE 1**: text 43 456, data 832, bss 22 416 bytes (~66 KB de 256 KB; la
  cabecera de `src/spu/audio/main.c` lleva el desglose). Cabe sin
  overlays.
- **Pruebas** (suite audio, 23): cada comando naudio con listas sintéticas
  y resultados a mano; el IIR contra una referencia directa en dos bloques
  (sigue del estado); el MP3 contra la síntesis de MPEG-1 en coma flotante
  (escala 1,00002, error rms 4,6 LSB sobre 3066, máximo 19,6). Listas
  naudio con y sin MP3 en la autoprueba SPE contra PPE del arranque
  (iguales en RPCS3).
- **Mac** (`salsa64-run`, 3 min de consola, respaldos propios): Banjo-
  Kazooie 5373 tareas, Conker 5389 y Perfect Dark 5216, todas **sin
  errores ni comandos sin implementar** (el censo: todas con error). Banjo
  y Perfect Dark suenan con niveles normales (−14 a −24 dBFS); Perfect
  Dark manda 3166 comandos MP3 en 3 min y su PCM es continuo (sin saltos
  en las fronteras de las ranuras). Conker sale en silencio: sin su
  microcódigo gráfico se queda tras la primera tarea gráfica y no dispara
  voces (no manda ADPCM ni ENVMIXER; solo el bus de efectos). WAVs para
  escuchar en `build/naudio/` (fuera de git).
- Sin regresiones: `tools/build.sh` sin advertencias, test-host en
  verde (295 con main), `tools/test-roms.sh` (nemu64-test 446, vitest, controles,
  savetest, gfxtest ×3, audiotest) y test-phase1/2 en RPCS3 OK.

- **PS3 real** (paquete «Salsa64 naudio», SLSA00080): **Banjo-Kazooie y
  Perfect Dark suenan bien** (el autor), 0 errores y 0 comandos sin
  implementar; HLE en el SPE 1 por tarea: mediana 2,9 ms en Banjo y 5,7
  ms en Perfect Dark (la más larga 10,4 ms), el PPE nunca espera.
  Conker: pantalla negra y silencio como en la Mac, sus tareas sin
  errores (1,6 ms). Detalle en docs/perf-log.md. Respaldos subidos a
  `/dev_hdd0/salsa64/` como `banjo.z64` y `conker.z64`; `embedded` quedó
  en `file:perfectdark.z64`, como estaba al empezar.

- **Después, pedido en main:**
  - `tools/test-rec.sh` se caía en la suite `sync` bajo qemu: SIGILL por
    `mffscrni` en `sin`/`cos`/`floor` de la glibc estática de ppc64 (Power
    ISA 3.0; qemu 7.2 solo lo acepta desde power9). `tools/qemu-ppc64.sh`
    usa `-cpu power9` (commit df3d652, rama `qemu-sync-fix` sobre
    main); test-rec en verde, también `--lockstep`, y con naudio y MP3
    (297 pruebas en big-endian).
  - **FILTER** (nead: Zelda, Majora…) corregido contra el microcódigo real
    con la herramienta del LLE (`--lle=all --lle-check-audio`, rama del
    LLE): la tabla de la nota no se escribe; la de detrás del estado pasa a
    `(anterior + nota + 1) >> 1` y con ella se filtra. La HLE dejaba el
    filtro a la mitad (−6 dB) y reescribía la tabla de la nota. Se oye en
    los primeros 15 s de Zelda (WAVs viejo/nuevo en `build/naudio/`).
    **PS3 real: Zelda «suena bien»** (el autor), SPE 2,1 ms por tarea.
  - **MIXER** redondea el producto como el microcódigo (+0x4000): con el
    LLE, Zelda pasa de 952 k a 210 k muestras distintas, Perfect Dark de
    1,80 M a 1,46 M, SM64 de 788 a 761 tareas distintas.
  - naudio contra el microcódigo real (LLE): Conker 476 tareas idénticas;
    Perfect Dark idéntico hasta la tarea 117 (cuando empieza a sonar). Lo
    que queda es RESAMPLE (tabla propia, a propósito) y el estado de
    ENVMIXER (formato propio: la HLE lee como suyo el que escribe el LLE,
    así que solo se compara la primera tarea de cada voz). Para medir
    naudio hace falta subir AUD_MAX_RANGES en el host (con 10 los tramos se
    juntan con memoria que el CPU escribe mientras tanto).

Notas:
- Banjo-Kazooie en la PS3: el **video parpadea** (uno de cada dos cuadros
  sale incompleto: sin suelo y con medio Banjo) y el fondo sale negro. Es
  de la HLE gráfica o la presentación, no del audio: para el trabajo de F3D.
- OVERLOAD recorre 0x170 muestras como el original (el doble que los
  demás comandos, que cuentan 0x170 bytes); ningún juego lo mandó en las
  escenas a las que llegamos, así que queda sin verificar.
- Conker: 4926 huecos entre DMA del AI en 3 min (el juego detenido); no es
  de la HLE.

Falta:
- [ ] Oír Conker cuando el trabajo de F3D lo haga avanzar (F3DEXBG).
- [ ] Probar Donkey Kong 64 (sin respaldo; la tabla es la del original).
- [ ] MusyX (Rogue Squadron, Resident Evil 2, Indiana Jones…).
- [ ] Opcional, con el LLE integrado: estado de ENVMIXER en el formato del
      microcódigo (para poder comparar más allá de la primera tarea de
      cada voz) y revisar los ±1 LSB que quedan en naudio.
- [ ] docs/compatibilidad.md: la columna de audio de Banjo, Conker y
      Perfect Dark (queda para main, es del censo).
## 2026-10-10 — Arranque del CIC 6105 y censo de compatibilidad

Hecho (rama `amazing-noether-39a548`; detalle en docs/cpu-vr4300.md,
«Arranque HLE», y docs/compatibilidad.md):
- `src/core/boot.c`: con el CIC 6105/7105, el arranque HLE deja lo que su
  IPL3 deja además en la RDRAM: copia de DMEM 0x554–0x888 a 0x004–0x338
  (desde la ROM del usuario; de ahí sale 0x2E8 = 0xC86E2000), el efecto del
  DMA con paso de su microcódigo del RSP (24 filas de 8 bytes en 0x2FB1F0,
  cada 0xFF0, desde RDRAM 0x1E8 vía IMEM 0x120) y el tamaño de la RDRAM en
  0x3F0. Solo direcciones, cotejadas entre fuentes (docs/THIRD_PARTY.md) y
  verificadas con dos palabras del Perfect Dark del autor. No hay más
  efectos documentados por variante (6101/6102/6103/6106/7102: dirección
  de arranque y semilla, que ya estaban).
- `osCicId` (0x310) = 6103 o 6105 con esos IPL3: Conker solo arranca su
  hilo principal si vale 6105 (se quedaba en el hilo ocioso a los 88 mil
  ciclos); Banjo-Kazooie y Perfect Dark también lo comprueban.
- `salsa64_boot_hle_as` fuerza la variante; pruebas por variante y de los
  restos del 6105 (287/287 en el host con main 3270538).
- Perfect Dark pasa la comprobación en la Mac y en la **PS3 real**
  (frontend «Salsa64 arranque», SLSA00078): 6083 bloques (6078 fuera de
  KSEG0) en vez de 201, 693 traducciones de la TLB, 59,8 campos/s con 5,8
  ms por campo, pantalla negra. Se queda en su hilo ocioso esperando al DP
  tras su primera tarea gráfica (microcódigo de Rare sin texto, sin HLE).
  `--lockstep` en qemu: 10,7 s de consola sin diferencias.
- `salsa64-run`: `--trace-rsp` muestra el CRC-32 del microcódigo y de
  `ucode_data`, los textos de `ucode_data` en las tareas gráficas y las
  palabras que distinguen el audio; muestreo del PC por segmento al final
  (KSEG0/KSEG1/TLB). `--lockstep` da a la referencia una copia del
  guardado: sin ella su PIF respondía «sin dispositivo» en el canal de la
  EEPROM y salía una diferencia falsa (Perfect Dark, ciclo 254 M).
- `tools/censo.sh`: censo de compatibilidad con los ROMs del usuario
  desde su ruta (compila `salsa64-run` con el compilador de la Mac).
- Censo de 3 minutos de 8 juegos (los 6 objetivo, Banjo-Kazooie y Conker)
  sobre main 3270538 (F3D integrado): dibujan SM64 (título),
  OoT, MK64, SF64, GoldenEye y Banjo; suenan ABI 1 y nead. Perfect Dark
  corre sin dibujar ni sonar (su microcódigo de Rare y naudio con MP3 sin
  HLE); Conker manda una sola tarea gráfica (F3DEXBG 2.08, comando 0x14) y
  su naudio da error. Sin la rama de F3D, solo Zelda dibujaba.

Siguiente (lista priorizada en docs/compatibilidad.md):
- [ ] Audio: las tres naudio (Banjo-Kazooie, Conker, Perfect Dark con MP3).
- [ ] Microcódigo de Perfect Dark (por CRC; sin texto) y F3DEXBG de Conker.
- [ ] Probar con el control y medir en la PS3 las escenas de docs/proyecto.md.

## 2026-10-10 — F3D (Super Mario 64) y F3DEX 1.x

Hecho (rama `lucid-pare-4f93fb`, con main traído hasta f698afe; sin
integrar):
- HLE de la primera generación de microcódigos en `src/rsp/gfx/f3d.c`: F3D
  (Fast3D: búfer de 16 vértices, índices ×10, G_TRI1, G_MTX con sus
  banderas, G_MOVEMEM/G_MOVEWORD con sus índices, geometry mode en dos
  comandos con otros bits, G_SETOTHERMODE_* directo, TEXRECT con
  G_RDPHALF_2 + G_RDPHALF_CONT) y F3DEX 1.x (búfer de 32, índices ×2,
  G_TRI2, G_QUAD, G_MODIFYVTX, G_CULLDL, G_BRANCH_Z, G_LOAD_UCODE). Lo
  común salió de `f3dex2.c` a `dl.c` y `geom.c` sin cambiar F3DEX2 (mismas
  31566 verificaciones). `rsp.c` manda F3D y F3DEX al backend. En el SPE 0
  son 3,3 KB de código (presupuesto en `src/spu/gfx/main.c`).
- Pruebas: 11 nuevas en el host (`tests/unit/test_gfx_f3d.c`) más la
  detección y el despacho en `test_rsp.c`: 223 en total.
- gfxtest en tres variantes (`tools/build-gfxtest.sh`; `GBI=` en su
  Makefile): F3DEX2 (la ROM sale idéntica), F3D y F3DEX. En el host las tres
  dan los mismos puntos y la misma imagen (`tools/test-roms.sh`, que además
  falla si hay comandos sin implementar): 19 de 19 antes de traer main y
  22 de 22 con las escenas de la caché de texturas. La de F3D también en
  RPCS3 (`tools/test-phase2.sh`, 22 de 22) y en la **PS3 real** (19 de 19,
  `RESULTADO frontend: OK`, 0,18 ms por tarea en el SPE). El frontend verifica la pantalla de las
  variantes cargadas con `file:salsa64-gfxtest-*.z64` como la embebida.
- **SM64** (respaldo propio, USA, fuera del repo): en el host llega al
  título y a las demos (Bowser, Whomp's Fortress) sin comandos sin
  implementar ni errores en 2 minutos; en la **PS3 real** el autor confirmó
  el título y la selección de archivo («todo se ve bien»). Mediciones en
  docs/perf-log.md: velocidad completa en el logo y en la selección de
  archivo, 67 % de NTSC en la cara de Mario (la anima el CPU).
- Otros juegos del usuario en el host (respaldos fuera del repo), sin
  comandos sin implementar: **GoldenEye** usa Fast3D 2.0G con `G_TRI4`
  (0xB1, cuatro triángulos con índices de 4 bits; antes 140 223 comandos
  sin implementar, ahora la intro se ve con la orientación bien);
  **Mario Kart 64** trae «F3DEX 0.95», que ahora se trata como F3DEX 1.x
  (logo y carrera de demostración); **Star Fox 64** (F3DEX.NoN 1.22) y
  **Banjo-Kazooie** (F3DEX 1.21) corren con F3DEX 1.x.
- Una tarea gráfica con microcódigo sin HLE termina también del lado del
  DP (ningún juego se cuelga esperando al RDP) y el frontend la anota en
  el log con el texto del microcódigo (`salsa64_rsp_ucode_text`, también
  en `salsa64-run --trace-rsp`).
- **PS3 real con main fusionado** (recompilador, dos hilos, caché,
  sondeo): SM64 sin franjas en la transición y al 100 % de NTSC en el
  logo, la transición y la cara (antes 55–67 %); GoldenEye: la intro se
  ve, al 100 % de NTSC. El autor vio después dos errores, arreglados y
  confirmados por él en la PS3:
  - La cara del Mario 3D (de frente): el combiner de los ojos,
    (TEXEL0 − SHADE)·TEXEL0_ALPHA + SHADE, multiplica la textura por su
    alfa y combine.c lo aproximaba. Ahora pide la textura premultiplicada
    (RGB·A al decodificar) y queda exacto (2c4b3f4): 0 combiners
    aproximados en SM64.
  - El cielo de GoldenEye «infinito»: lo dibuja con triángulos crudos del
    RDP partidos en G_RDPHALF_1 + G_RDPHALF_CONT/2, que la HLE no
    dibujaba. Ahora se convierten en triángulos (2c53149).
- Después de integrar (main 3270538), confirmados por el autor en la PS3:
  - El audio de SM64, GoldenEye y Mario Kart 64 suena bien.
  - GoldenEye sin ruido entre el logo y las carpetas: el frontend elige
    el destino del ancho que muestra el VI (GE usa la misma dirección
    para 320 y 440; 66a3870) y sigue presentando el destino mientras el VI
    sostiene la imagen sin dibujar (un segundo de carga; antes salía la
    RDRAM con datos viejos; 10e869e).
  - Mario Kart 64 con título, menús y letreros: F3DEX acepta TEXRECT con
    G_RDPHALF_2 + G_RDPHALF_CONT, la secuencia de la libultra temprana
    (7343e2f), y los TEXRECT en tiras sin costuras a más resolución:
    limitar el eje si todo cae dentro de la tile y muestrear en la
    esquina del píxel con filtro bilineal (8ec50de).
- `salsa64-run --pad "CAMPO:BOTONES[@X,Y];…"`: control 1 por campos para
  llegar a escenas de juego en el host (SM64 hasta el castillo, GoldenEye
  hasta Dam).
- Arreglo en `src/rdp/rdp.c` (3729f8f): unos cuadros con franjas de
  colores en el paso del logo a la cara. SM64 apaga el VI 26 campos al
  cambiar de escena; lo dibujado entonces quedaba marcado con el campo de
  antes del apagado y el frontend, al volver la imagen, presentaba la RDRAM.
  Simulado con la traza del host: de 17 campos sin imagen del RSX a 0.

Pendiente:
- [ ] Confirmar en la PS3 el arreglo de la transición (paquete «Salsa64
      F3D», SLSA00071) y dejar `embedded` como estaba (`vitest`).
- [ ] Combiners aproximados en las demos de SM64 (cientos por minuto):
      ver cuáles son (`combine.c`).
- [ ] En la PS3: GoldenEye, Mario Kart 64 y Star Fox 64 con este
      paquete (en el host ya dibujan).
- [x] Confirmar en la PS3 la cara de Mario y el cielo de GoldenEye.
- [x] Mario Kart 64: título, menús y letreros (ver arriba). Quedan las
      pantallas gigantes de las pistas, que muestran el framebuffer
      anterior como textura (efecto de framebuffer, Fase 4).
- [ ] Perfect Dark: corre sin dibujar con un microcódigo de Rare sin
      texto (CRC c543d0a8). Conker: «F3DEXBG.NoN fifo 2.08» tomado como
      F3DEX2, deja 2 comandos sin implementar (el último 0x14) y no manda
      otra tarea.
- [ ] GoldenEye: dos combiners del nivel multiplican el sombreado por su
      alfa (lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE: niebla por luz
      precalculada) y se aproximan (75 000 en 52 s de Dam). Hace falta un
      término S·Sa por vértice en la combinación del RSX (rsxkit y
      src/rdp) o el generador de fragment programs de la Fase 4.
- [x] GoldenEye: las franjas azules de los costados son del juego (3D con
      scissor en las columnas 1–318 sobre un relleno 0x1199); en una tele
      quedan en el overscan. Opción de recorte en el frontend
      (`overscan.cfg`, trabajo de presentación).
- [ ] Star Fox 64: un cuadrito con textura abajo a la izquierda en el
      logo (host). Revisar.
- [ ] G_LINE3D de F3D (no lo usan SM64 ni GoldenEye) sin implementar.
- Para correr un respaldo en el host sin copiarlo: colima no comparte
  todas las carpetas del host, así que `salsa64-run` se compila nativo en la
  Mac (`make -C src/host OUT=$PWD/build/host-mac CC=clang SAN=`) y lee la
  ROM en su lugar.

## 2026-10-10 — Audio de punta a punta: AI, HLE de audio en el SPE 1 y salida en la PS3

Hecho (rama `amazing-varahamihira-3027c8`, con main cfa146b):
- **AI** (`src/core/ai.c`, desde n64brew.dev): cola de dos DMA, AI_STATUS
  (BUSY/FULL/ENABLED), AI_LENGTH que cuenta hacia abajo y los demás
  registros que se leen como él, frecuencia = reloj del VI de la región /
  (AI_DACRATE + 1), interrupción al **empezar** cada DMA, AI_CONTROL que
  detiene el DMA y el acarreo de los límites de 8 KB (libultra lo
  compensa). Un evento del planificador por DMA; las muestras van a un
  sumidero al empezar cada DMA.
- **HLE de audio** (`src/rsp/audio/`, adaptada de mupen64plus-rsp-hle,
  GPLv2+, avisos en cada archivo y docs/THIRD_PARTY.md): ABI1 (Super Mario
  64; variante de GoldenEye) y todas las variantes nead (Zelda OoT, Mario
  Kart 64, Star Fox 64…). DMEM en el orden de la N64 con índices de la N64
  (el original dependía del host little-endian en FILTER e INTERLEAVE),
  E/S explícita de la RDRAM, tabla de RESAMPLE **propia**
  (`tools/audio-lut.py`; la del original es del microcódigo), libro de
  códigos de 16 predictores (el original leía fuera de la tabla). El núcleo
  reconoce el ABI por las palabras de `ucode_data` (`rsp_audio.c`). ABIs
  por juego objetivo: SM64 ABI1, GoldenEye ABI1 (GE), Zelda OoT / Mario
  Kart 64 / Star Fox 64 nead, Perfect Dark naudio con MP3 (pendiente).
- **SPE 1** (`src/spu/audio/`, `src/platform/ps3/audio_spu.c`): una tarea
  por trabajo de lib/spukit, E/S por DMA (escrituras en trozos de
  alineación natural: nunca reescribe RDRAM vecina), fin de tarea por
  sondeo (`done`), aviso de lo escrito con `salsa64_bus_rdram_written`.
  Local store: 27 KB de código + 20 KB de datos. Al arrancar compara
  listas de prueba (`selftest.c`) en el SPE y en el PPE.
- **Salida** (`src/frontend/audio_out.c`, `audio_sync.c`): anillo sin
  bloqueos entre la emulación y un hilo de audio que llena el puerto de
  PSL1GHT (48 kHz, 16 bloques de 256, cuatro por delante). **Ritmo:** lo
  marca pacer.c (la tele emulada); el audio lo sigue con una corrección de
  ±0,5 % del remuestreo. **Política** (`audio.cfg`): «estirar» (por
  omisión: si la emulación se aparta más de un 3–5 % sigue su velocidad
  con WSOLA sin cambiar el tono; si se vacía repite lo reciente hasta 150
  ms y luego calla) o «silencio» (calla con 10 ms de desvanecido y sigue
  donde quedó). Toda transición pasa por la suma con ventana: sin
  chasquidos, verificado en el host con productores al 27 %, 100 % y 130 %.
  Diagnóstico: `volcado_s` (WAV de lo que sale por el AI, nunca al repo),
  `tarea_us`, pico del AI en dBFS en el log. Memoria: 0,3 MB.
- **ROM audiotest** (`tests/roms/src/audiotest`, propia, IPL3 libre): tono
  conocido por el CPU (La 4, Do izquierda, Mi derecha, Sol, Do 6) y luego
  por una tarea de audio («Salsa64 ABI NEAD»); se verifica sola y
  `salsa64-run --audiotest-check` compara lo que sale por el AI muestra por
  muestra. En `tools/test-roms.sh`.
- Pruebas: suites ai (8), audio (15) y sync (5); 269 en total en el host y
  en RPCS3 (big-endian). Pruebas de mutación: romper el acarreo, el orden
  de INTERLEAVE, la escala del ADPCM o el solapamiento del estirador las
  hace fallar. nemu64-test 446 fallas (sin cambios), vitest, controles,
  savetest, gfxtest y audiotest OK; build sin advertencias. RPCS3:
  audiotest OK en el frontend (renderer de audio Null en las pruebas).
- **PS3 real** (paquete «Salsa64 audio», SLSA00072): audiotest suena bien
  (afinador en 440 Hz, cada canal de su lado), 100 % de NTSC, 0 faltas.
  **Zelda OoT suena perfecto** (el autor): 9 min al 99,9 %, HLE en el SPE
  2,4 ms por tarea (mediana; máx. 4,0), PPE esperando 0, 0 bloques tarde.
  Mediciones en docs/perf-log.md.

Notas:
- Una vez, tras la primera corrida de Zelda (120 s), la consola quedó en
  negro después de «salida normal» y hubo que reiniciarla. No se repitió
  en tres salidas más (7 s, 9 min y SM64 100 s); ahora el cierre del audio
  anota cada paso y todos devuelven 0. Además, soltar el audio
  (`salsa64_rsp_set_audio(NULL)`) ya no termina una tarea gráfica en vuelo:
  antes hacía un reset completo del RSP y podía llamar a `wake()` del
  frontend con su semáforo ya destruido. Si vuelve a pasar, mirar esas
  líneas; `salsa64_rsp_set_gfx(NULL)` en main.c tenía el mismo riesgo
  (iba después de `sysSemDestroy(wake_sem)`): corregido al integrar, el
  semáforo se destruye después de soltar la HLE gráfica. Probablemente no
  era la causa del negro: en lv2 un semáforo destruido da error, no se
  cuelga, y el negro llegó después de «salida normal» (fin del proceso:
  ¿algún hilo, SPE o el puerto de audio aún vivo?). Pendiente si se repite.
- `.gitignore`: la regla «core» de los volcados de qemu también ignoraba
  `src/core` para archivos nuevos; se agregó «!src/core/».

Falta:
- [ ] Oír Super Mario 64 (ABI1): sus tareas corren sin errores (0,115 ms)
      pero el juego no pasa del arranque sin F3D.
- [x] naudio (Perfect Dark, Banjo, Conker; Perfect Dark con MP3): ver la entrada
      de naudio. Falta MusyX.
- [ ] Probar la política «silencio» y una escena lenta en la PS3; con el
      recompilador Zelda ya va al 100 % y casi no hace falta estirar.
- [ ] Opcional: SIMD en ENVMIXER, RESAMPLE y ADPCM del SPE (hoy 2,4 ms por
      tarea en Zelda, escalar).
- [ ] Opcional: que el audio marque el ritmo (pacer.c tiene el enganche).
## 2026-10-10 — Bucles de espera en código mapeado por la TLB

Hecho (rama `loving-spence-8951ae`, desde main bd8ac59):
- `src/cpu/idle.c` traduce el código y las cargas de la vuelta también por
  la TLB, sin efectos (`salsa64_vr4300_fetch_probe`: la búsqueda del
  intérprete sin excepciones ni cambios); antes solo KSEG0/KSEG1. El «sí»
  no se guarda (cada vez se vuelve a simular con la TLB y la memoria del
  momento) y la TLB no cambia dentro de una vuelta (COP0 la descarta): un
  remapeo o código reescrito se ve en el análisis siguiente; los descartes
  siguen por pc ^ instrucción. El recompilador no cambia (consulta lo
  mismo que el intérprete).
- Pruebas: `b .` mapeado y sondeo de una variable por dirección mapeada
  (se saltan, igual con y sin detección), y la misma dirección virtual
  remapeada a un contador (deja de saltar y queda descartado).
- Mac, 21,3 s de consola con el intérprete: GoldenEye 0,0 → **97,6 %** en
  bucles de espera (16,2 → 0,8 s); Perfect Dark 0 → 99,3 % (su `b .` del
  arranque). Registros, log e imagen del VI idénticos con y sin el cambio.
- qemu-ppc64: 233/233 con el recompilador en los cuatro modos, nemu64-test
  con la línea base, vitest OK, `--lockstep` sin diferencias en
  nemu64-test, vitest y en GoldenEye y Perfect Dark (21,3 s de consola cada
  uno, con 97,6 % y 99,3 % en bucles de espera).

- **PS3 real** (524d80e, frontend «Salsa64 TLB bucles», SLSA00074):
  GoldenEye a 59,8 campos/s con **0,6 ms de emulación por campo** (antes
  5,6; 0,5 de los 0,6 son la copia del VI) y 99 % en bucles de espera
  (antes 0 %); 1,2 MIPS ejecutados en vez de 93,7.

## 2026-10-10 — Recompilador: conversiones de la FPU, LWL/LWR y MFC0 en nativo

Hecho (rama `funny-lalande-371c7e`; diseño en docs/rec-ppe.md,
cifras en docs/perf-log.md):
- En nativo, con los mismos bits, causas y banderas de FCR31 que el
  intérprete: SQRT, CVT.D.S/CVT.S.D, ROUND/TRUNC/CEIL/FLOOR a W y L,
  CVT.W/CVT.L (redondeo al más cercano), CVT.S.W/CVT.D.W (en enteros, sin
  pasar valores entre registros enteros y de punto flotante), CFC1/CTC1.
  Lo que no da exacto (redondeo dirigido, I habilitada, operandos fuera de
  la franja, NaN, desnormales) sigue en el intérprete.
- LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR en nativo; los pares LWL+LWR y SWL+SWR
  sobre los mismos 4 bytes, byte a byte (sin desplazamientos por registro ni
  load-hit-store en copias).
- MFC0/DMFC0 ya no terminan el bloque; Count desde r29 + k + 1.
- Código nuevo en `src/cpu/rec_emit_ext.h` (lo incluye rec_emit.c); en lo
  común, ganchos de una línea, para no chocar con el trabajo de TLB.
- `rec->stats.exec_calls` (llamadas al intérprete desde el código) en
  salsa64-run y cpu-tests; `SALSA64_REC_OPT_RARE_EXEC` repite lo de antes
  con el mismo binario (cpu-tests la corre como variante).
- Pruebas: `executed_conversions_match_full` (8000 instrucciones contra el
  camino completo, más de 3000 en nativo), LWL/LWR y compañía en cada
  desplazamiento contra un modelo byte a byte, pares en los dos órdenes,
  registros fuera del PPE, caminos lentos (DMEM, fallo de TLB), código que
  se reescribe; MFC0 de cada registro y Count dentro del bloque y en un
  delay slot. 221/221 en el host y con el recompilador (plain, mtctr,
  registros fijos, escritura temprana); nemu64-test con la línea base,
  vitest OK, `--lockstep` sin diferencias en los dos. Prueba de mutación:
  10 de 10 errores puestos a propósito detectados.
- Host: llamadas al intérprete −18 % en nemu64-test y −97 % en vitest.
- Con main traído (TLB y KSEG1, bd8ac59): 240/240 en arm64 y en ppc64 en
  los cuatro modos, nemu64-test con la línea base (623 bloques fuera de
  KSEG0), vitest OK, `--lockstep` sin diferencias en los dos.

Siguiente:
- [x] **PS3 real** (cpu-tests SLSA00077 «Salsa64 CPU FPU», e210aeb):
      nemu64-test 1894 → 1816 ms (−4,1 %, ×4,47 del intérprete) con las
      mismas fallas; bench cvt 4,2 → 40,7 MIPS, no alineadas 20,5 → 218,5,
      mfc0 9,8 → 701, espera por Count 8,3 → 488 (salidas a C 2 M → 8).
      Las 4 «fallas» del RESULTADO eran verificaciones de cobertura en la
      variante `RARE_EXEC` (corregidas).
- [ ] Lo que sigue yendo al intérprete: CVT.S/D.L, CACHE, MTC0/TLB/ERET;
      MFC0 de Random.
- [x] Bench del intérprete (21 → 14,7 MIPS tras la Fase 2): en el binario
      de este trabajo da 21,1 MIPS en `t_n64` y en la máquina alineada; no se
      reproduce y la hipótesis de la página de `sched->next` quedó
      descartada (docs/perf-log.md). Depende de cómo queda el binario.
- [ ] Medir en un juego (Zelda, SM64) cuánto pesan las conversiones y las
      esperas por Count cuando la HLE gráfica deje de ser el cuello.

## 2026-10-10 — Fin de tarea del RSP por sondeo (sin bloquear al SPE)

Hecho (rama `musing-mcclintock-6b5e2c`, commits a2ffd0f + mezcla
16a544f con main 4a4d98c; diseño en docs/hilos-ppe.md «Fin de tarea sin
bloquear»):
- `src/core/rsp.c`: el backend tiene `done(ctx, idle)` (no bloquea); al
  llegar el fin de la tarea, si no terminó, el evento se vuelve a programar
  cada `poll_cycles` (10 000 ciclos) y el CPU sigue emulando. Si el CPU
  saltó ciclos (bucle de espera) desde el sondeo anterior, se le avisa al
  backend y el del SPE duerme 100 µs antes de contestar: el reloj emulado
  no corre más rápido que el real. Con la tarea en curso el RSP no está
  HALT y otro arranque se ignora (`starts_busy`). Genérico para el audio.
- Prueba unitaria `rsp_gfx_task_polls`; estadísticas `sondeos` y
  `dormida` en la línea `gfx:` del frontend.
- ROM gfxtest: la espera por una interrupción vence por tiempo (10 s de
  Count) y no por vueltas (en RPCS3 el primer trabajo del SPE compila
  segundos; antes el CPU estaba congelado mientras tanto). Reconstruida.
- Verificación: 224 pruebas en el host, cpu-tests/vitest/save-tests en
  RPCS3, gfxtest 22/22, test-roms 446, build sin advertencias.

Medido en la PS3 (docs/perf-log.md, «Fin de tarea del RSP por sondeo»):
Zelda, «esperando al SPE» 2,3–8,2 ms por tarea → 0; emulación por campo
igual (3,2–7,5 → 3,2–7,2 ms) porque el CPU está ocioso mientras el RSP
trabaja y el sondeo duerme ese tiempo; 60 campos/s sin regresiones;
gfxtest OK. La ganancia llegará donde el CPU tenga trabajo durante el
RDP.

Pendiente:
- [ ] Repetir con Super Mario 64 (F3D) cuando esté en main: 9,5 ms de CPU
      por campo, el caso donde el sondeo debería rendir.
- [ ] Recompilador: un bucle que lee Count (MFC0) sale a C en cada vuelta
      (gfxtest cayó a 9 campos/s); compilar MFC0 Count en línea (trabajo FPU, MFC0 nativo).

## 2026-10-10 — Recompilador: código en KSEG1 y mapeado por la TLB

Hecho (diseño en docs/rec-ppe.md, «Código fuera de KSEG0»):
- El recompilador compila también código en KSEG1 y mapeado por la TLB
  (kuseg, kseg2/3), con la instrucción en la RDRAM o en la ROM del
  cartucho. Antes eso lo corría el intérprete; GoldenEye y Perfect Dark
  corren casi todo su código mapeado.
- Clave de bloque (vpc, física) en una tabla aparte (`xhash`); KSEG0 sigue
  igual. Todo en `src/cpu/rec_tlb.c` (para que los merges con otros
  cambios del recompilador sean fáciles); rec.c y rec_emit.c solo lo llaman. Los bloques mapeados comparan su traducción al entrar contra una
  TLB del recompilador por página de 4 KB (`vtlb[]`, 4 instrucciones),
  también al llegar por un enlace o por JR. Escribir la TLB no mata ni
  desenlaza bloques: antes de correr código, el despachador vuelve a
  traducir las páginas anotadas si cambiaron la TLB, el ASID o ERL.
- JR/JALR encuentran en el propio código los bloques fuera de KSEG0.
- `cop0.c`: `salsa64_vr4300_fetch_probe` (búsqueda en la TLB sin
  excepciones) y `cpu->tlb_writes`.
- Pruebas nuevas (también en modo estricto): código mapeado con JAL entre
  páginas, remapeo con enlace, cambio de ASID, código mapeado reescrito por
  la dirección mapeada y por KSEG0, KSEG1, ROM por TLB y por KSEG1, dos
  páginas en la misma entrada de `vtlb[]`, bucle mapeado; bucles de espera
  en KSEG1 (se saltan) y mapeados (se descartan, como con el intérprete).
- qemu-ppc64: unitarias 222/222 con el recompilador en los cuatro modos;
  nemu64-test con las fallas de la línea base y 623 bloques fuera de KSEG0;
  vitest OK; `--lockstep` sin diferencias en nemu64-test y vitest.
- Cambiar la ROM o el ISViewer vacía el recompilador (puede haber código
  compilado desde la ROM).
- Memoria: +572 KB del estado del recompilador (6,84 MB).

Siguiente:
- **PS3 real** (b11861c, cpu-tests «Salsa64 CPU TLB», SLSA00076): 232/232
  en el intérprete y en las cuatro variantes; nemu64-test igual al
  intérprete (446 fallas, hash y ciclo), ×4,04–4,29; bench enteros con
  registros fijos 760 MIPS (el préstamo en bucles funciona; antes 128).
- qemu-ppc64 con `--lockstep` (respaldos propios, fuera del repo):
  GoldenEye y Perfect Dark 200 M ciclos cada uno sin diferencias con el
  intérprete. GoldenEye: 3518 de 3524 bloques mapeados y 5050
  traducciones en 2,1 s (reescribe la TLB seguido); Perfect Dark con 8 MB:
  3 traducciones.
- **PS3 real, frontend «Salsa64 TLB»** (SLSA00074, respaldos propios):
  - GoldenEye: 59,8 campos/s a 93,7 MIPS (velocidad completa del VR4300),
    **5,6 ms de emulación por campo**; 3512 de 3518 bloques mapeados, 5050
    traducciones en el arranque y ninguna después, ~0,08 % interpretado;
    estable 100 s. En pantalla, ruido: el juego manda una sola tarea
    gráfica con un microcódigo que la HLE no reconoce (ucode en 0x20e60,
    su F3D) y después solo audio; con el intérprete solo, en la Mac, queda
    igual (mismo bucle en 0x7000071c a los 21 s). Le toca al trabajo de F3D.
  - Perfect Dark: 59,8 campos/s, 5,1 ms por campo, pero se queda en el
    arranque (201 bloques, VI sin configurar, pantalla negra); con el
    intérprete solo también (pc 0x700017f4 a los 21 s, sin tareas del RSP
    ni excepciones). No es del recompilador: falta ver el arranque (CIC
    6105) o algún registro del hardware.
- Diagnóstico (arnés en la Mac, con el intérprete; lockstep de 10,7 s de
  consola sin diferencias en los dos juegos):
  - GoldenEye llega a su hilo ocioso (`b .` en 0x7000071c) y se queda: su
    única tarea gráfica (ucode en 0x20e60) no la reconoce la HLE, y la
    tarea nunca termina para el juego (sin DP), así que no vuelve a
    dibujar; el VI muestra RDRAM sin inicializar (el ruido). Pagina su
    código por fallos de TLB (110 en 10 s). Además el `b .` mapeado no lo
    salta idle.c (solo KSEG0/KSEG1): 0 % en bucles de espera.
  - Perfect Dark (CIC 6105) compara la palabra en 0xA00002E8 con
    0xC86E2000 y, si no coincide, se queda en `b .` (0x700017f0). Ningún
    código del juego la escribe: la deja el arranque del 6105 y nuestro
    arranque HLE no. Escribiéndola a mano (experimento, no en el repo)
    pasa, monta 0x7F000000 en la TLB y llega a su hilo ocioso.
- [x] Arranque HLE del CIC 6105 (`src/core/boot.c`; trabajo «Arranque del CIC
      6105 y censo»): lo que el IPL3 deja
      en la RDRAM (al menos 0x2E8 = 0xC86E2000), desde documentación
      pública, sin código de Nintendo. (Hecho: entrada de arriba.)
- [ ] HLE gráfica: el microcódigo de GoldenEye (F3D; trabajo de F3D) y que
      una tarea con microcódigo desconocido no deje colgado al juego.
- [x] idle.c: bucles de espera en código mapeado por la TLB (entrada de
      arriba).
- [x] GoldenEye ejecuta 0x7F000000 desde la RDRAM: pagina su código por
      fallos de TLB (visto en la emulación, ver abajo).

## 2026-10-10 — Caché de texturas entre tareas (HLE gráfica)

Hecho (rama `unruffled-maxwell-2af921`; diseño en docs/texturas.md):
- Las texturas decodificadas viven en una región de 8 MB mapeada al RSX
  (`src/rdp`) que la HLE del SPE 0 administra entre tareas
  (`src/rsp/gfx/texcache.c`). Identidad: hash de 64 bits (ronda de
  xxHash32 en ocho carriles, SIMD en el SPE) de los bytes de la TMEM que
  lee la decodificación —texels y paleta— y de los parámetros de la tile:
  un cambio en la RDRAM o en la paleta da otra textura sin vigilar
  escrituras (cubre las del CPU, que no avisan). Al arrancar, el frontend
  comprueba que el hash del SPE da lo mismo que el del PPE.
- Anillo con desalojo en orden de llegada; lo que el RSX aún puede leer
  (`job->done_task`, de las barreras de `src/rdp`) se salta. Sin lugar, la
  textura va al arena de la tarea. `salsa64_rdp_dump` copia las texturas
  de la caché al volcado (mismo formato).
- Cargas a la TMEM en bloque (sin `memcpy` de 8 bytes): ×2,4.
- Medición en el frontend: ventanas fijas de 100 campos («gfx campos A–B»,
  con el peor campo), capturas en campos fijos y salida sola
  (`capturas` = «N M»); acomodado sobre los dos hilos de main.
- PS3 real, Zelda (intro y título, con el recompilador; docs/perf-log.md):
  HLE en el SPE 14–31 → 5–9,6 ms por tarea; decodificar 6,7–17,2 → 0–0,09
  ms; las escenas a 41–48 campos/s llegan a 60; capturas idénticas byte a
  byte. Decisión: todo en el SPE 0; los SPE 2–3 quedan para el escalado de
  texturas (docs/texturas.md).
- Pruebas: caché entre tareas, texel y paleta cambiados en la RDRAM, anillo
  con texturas en uso y directorio lleno, hash por tramos; gfxtest con una
  textura y una paleta que el CPU cambia en el cuadro 60 (22 puntos).

Pendiente:
- [ ] LOADTILE pide cada fila con un DMA que espera (~2300 lecturas por
      tarea en Zelda): pedirlas por adelantado. Es lo más caro que queda
      en el SPE junto con el estado de dibujo.
- [ ] Decodificación por formato con SIMD y salidas de 16 bits en
      `lib/rsxkit/`: solo cuando falla la caché; medir antes el peor campo
      en cambios de escena (Kokiri, partida en juego).
- [ ] Achicar el arena de texturas por juego (casi no se usa).

## 2026-10-10 — Dos hilos en el PPE (frontend)

Hecho (rama `musing-mcclintock-6b5e2c`; diseño en docs/hilos-ppe.md):
- `src/frontend/main.c`: hilo de emulación (`sysThreadCreate`, prioridad
  1000, pila de 256 KB) con el CPU, el planificador, el envío de tareas al
  SPE, los controles aplicados al PIF y la copia de la imagen del VI; el
  hilo principal (1001) traduce la salida de la HLE al RSX, presenta con
  flip en vsync, lee los DualShock 3, escribe el guardado, el log de
  estadísticas, capturas y volcados. Duerme en un semáforo (la emulación
  lo despierta por campo y por tarea); nunca espera activa (SMT).
- Colas preasignadas: cola de campos (8 mensajes) con copia del VI en 3
  ranuras (4,4 MB; estados por `lwarx/stwcx.`), cola de tareas gráficas
  de 3 entradas dentro de `src/rdp` (`salsa64_rdp_flush` traduce en el
  hilo de presentación; `acquire` espera un juego libre con
  `rsxkit_fence_done`, la única función de rsxkit documentada como segura
  desde otro hilo), buzón de controles con seqlock, zona de paso del
  guardado (`savefile_stage` / `savefile_write_staged`), lwmutex en el log.
- Si la emulación va más rápida que la tele: se presenta el campo más
  nuevo y los viejos se saltan (`saltados` en el log); la emulación nunca
  espera al vsync. Los campos con verificación del selftest no se saltan.
- Verificación igual que antes: selftest de vitest (RDRAM en emulación,
  pantalla en presentación), gfxtest, capturas, `gfxdump`, repetición
  (`dump:`, de un hilo: llama a `salsa64_rdp_flush`). Pruebas: 212 en el
  host; test-roms (nemu64-test 446 fallas, vitest, controles, savetest,
  gfxtest); fase 1 y fase 2 en RPCS3.
- Lugar para el audio marcado con `audio_out:` en main.c (inicio, cada
  campo presentado, cierre; y `emu_thread` si necesita el hilo de
  emulación).
- Ritmo (`src/frontend/pacer.c`, pedido en main): cada
  campo a la hora de la tele emulada con el reloj real, sin acumular
  deuda; `SALSA64_DATA_DIR/turbo` lo quita para medir el máximo; la
  medición A/B de vitest va sin límite. Enganche para que el audio marque
  el ritmo (`sleep` reemplazable, `pacer_nudge`). 5 pruebas unitarias.

Medido en la PS3 real con el recompilador (main 697922c mezclado; tabla
en docs/perf-log.md, «Dos hilos en el PPE»): Zelda en el título con el
castillo 41–52 → 57–60 campos/s (el flip del vsync ya no frena a la
emulación); vitest sin límite 187 → 363 campos/s con la presentación a
60/s saltando el resto, y con ritmo 60,0 exactos; guardado por hilos y
START + SELECT bien. Las capturas periódicas `screen-N.ppm` pasaron a ser
opcionales (`SALSA64_DATA_DIR/capturas`): cada una frena ~400 ms y era el
«tirón» cada 10 s; en prueba automática siguen siempre.

Pendiente:
- [ ] La espera del hilo de emulación al SPE al terminar cada tarea (10–27
      ms por tarea, 20–53 % del hilo en Zelda) es el cuello siguiente:
      que el evento del RSP se vuelva a programar en vez de bloquear
      (`src/core/rsp.c`), o acortar la HLE (caché de texturas).
- [ ] Copia del VI (0,2–0,3 ms por campo) aunque el VI muestre un destino
      del RSX: evitarla con una pista del hilo de presentación.
- [ ] Repetir la medición con Super Mario 64 cuando F3D esté en main (caso
      de libro: 9,5 ms de emulación + 7–8 ms de presentación en un hilo).
- [ ] Menú y audio en el hilo de presentación (trabajos aparte).

## 2026-10-10 — Fase 3: recompilador MIPS → PPC64 en el PPE (×4,4 con código real)

Hecho (diseño y decisión en docs/rec-ppe.md, números en docs/perf-log.md):
- **Decisión: recompilador propio.** Not64 y Wii64 (GPLv2+, commits en
  docs/THIRD_PARTY.md) se revisaron y descartaron como base: emisor de 32
  bits con pares hi/lo, EABI de la Wii, globales de mupen64, Count solo en
  saltos y FPU sin FCR31. Se usaron solo como referencia de diseño.
- `src/cpu/rec.c`, `rec_emit.c`, `rec.h`, `ppc.h`: bloques desde KSEG0
  hasta un salto con su delay slot; en nativo solo si caben antes del
  próximo evento, así que eventos, Count, interrupciones y excepciones caen
  en el mismo ciclo que con el intérprete. 12 registros MIPS por bloque,
  escritura temprana (contra el load-hit-store entre bloques), enlaces
  parcheados, JR/JALR sin volver a C, invalidación por código escrito
  (también desde el intérprete, DMA del PI/SI/SRAM y cargador), FPU nativa
  con las mismas causas de FCR31 que el intérprete, bucles de espera con la
  misma consulta que `idle.c`. Lo raro (LWL/LWR, COP0, conversiones de la
  FPU, TLB, KSEG1, modo usuario) lo corre el intérprete.
- Caché en la cola NOBITS de 16 MB del segmento de texto
  (`mk/rec-cache.ld`): llamadas a C con `bl` directo. Estado: 6,3 MB.
- Pruebas en la Mac sin PS3: Linux ppc64 + qemu (`tools/test-rec.sh`,
  `docker/ppc64.Dockerfile`): las 197 unitarias también con el recompilador
  en modo estricto; nemu64-test con la línea base; vitest OK; `salsa64-run
  --lockstep` compara contra el intérprete bloque a bloque (sin
  diferencias en nemu64-test).
- **PS3 real** (f7b655d): 163/163 unitarias ejecutando código generado;
  nemu64-test 7,9 s → 1,8 s (**×4,36, 104 MIPS**) con las mismas fallas y
  el mismo ciclo final; bench mixto 21,3 → 156,6 MIPS.
- RPCS3 no ejecuta código generado: ahí se apaga
  (`/dev_hdd0/salsa64/sin-recompilador`, lo crea `tools/rpcs3-run.py`).
- Merges de main (controles, bucles de espera, guardado, Fase 2 gráfica)
  sin regresiones; el DMA del SP, del SI y de la SRAM avisan al
  recompilador.
- Registros fijos (sp, ra, v0, v1, a0–a3) activos en el frontend: +7 % en
  nemu64-test (×4,45 del intérprete); los bucles toman prestados los que no
  usan. La escritura temprana no compensó: opción apagada.
- **Zelda OoT en la PS3 real con el recompilador: 41–60 campos/s** (con el
  intérprete 15–17), 4–14 ms de emulación por campo; se ve igual. Donde no
  llega a 60, el PPE espera a la HLE gráfica del SPE.
- vitest en el frontend: 60 campos/s, 2,9–5,4 ms de emulación por campo.
- Paquetes de prueba con APPID propio: SLSA00063 «Salsa64 recompilador»
  (frontend) y SLSA00065 «Salsa64 CPU recompilador» (cpu-tests con cuatro
  variantes del recompilador y nemu64-test contra el intérprete).

Siguiente:
- [ ] Medir en la PS3 el préstamo de registros fijos en bucles (bench
      enteros con registros fijos: esperado ~750 MIPS, antes 128).
- [x] Más juegos de la tabla de docs/proyecto.md (SM64, MK64, SF64; GoldenEye y
      Perfect Dark usan TLB: ver la entrada de arriba). (Censo en
      docs/compatibilidad.md.)
- [x] Más en nativo: CVT/TRUNC/ROUND de la FPU, LWL/LWR/SWL/SWR, MFC0 sin
      salir del bloque (entrada de arriba). Código por TLB y KSEG1: hecho.
- [ ] CACHE.
- [ ] Fase 2: el DMA del SP hacia la RDRAM tiene que llamar a
      `salsa64_bus_rdram_written`.
## 2026-10-10 — Fase 2 (parte gráfica): criterio cumplido en PS3 real

Hecho (rama `reverent-leavitt-d0f943`, integrada en main después del
guardado y los controles):
- `lib/spukit` (MIT): trabajos por lotes en SPEs, cola en memoria principal
  y timbre por mailbox (1,76 µs ida y vuelta en la PS3).
- HLE de F3DEX2/F3DZEX2 portable (`src/rsp/gfx/`) que corre en el SPE 0
  (`src/spu/gfx/`, E/S por DMA con doble búfer) y en el host; RDP sobre
  `lib/rsxkit` (`src/rdp/`): combiners como combinación lineal, texturas,
  z-buffer, culling, niebla, mezcla según la fórmula del blender.
- Criterio: gfxtest (homebrew propio con el IPL3 libre) 19 de 19 puntos en
  host, RPCS3 y PS3; Zelda OoT (respaldo propio) llega al título y la
  intro se ve sin errores gráficos en la PS3 real (confirmado por el autor).
- Arreglos de la ronda con la PS3: TASKDONE en tareas de libultra sin HLE,
  destinos alineados a 128 (temblor), frente antihorario, mezcla opaca del
  cielo, corrección de perspectiva que `cgcomp` quita en MUL/DP (la luna;
  docs/rsx-notes.md) y FILLRECT inclusivo en 1 ciclo (columna del fundido).
  Todos verificados por el autor en la PS3 real con `salsa64-fase2.pkg`
  (APPID propio SLSA00062, título «Salsa64 Fase 2 (gfx)»).
- Depuración: volcado de la salida de una tarea (`gfxdump`), repetición en
  el RSX sin emular (`embedded` = `dump:<nombre>`), `tools/gfxdump.py` para
  editar volcados y `salsa64-run --raster-dump`.
- Mediciones en docs/perf-log.md: el cuello es el CPU (64 ms por campo, 26 %
  de NTSC); la HLE en el SPE cuesta 1–21 ms por tarea, sobre todo
  decodificando texturas.

Pendiente:
- [ ] Caché de texturas entre tareas y decodificación por formato en el SPE
      (medir antes/después en la PS3).
- [x] F3D (Super Mario 64): ver la entrada «F3D (Super Mario 64) y F3DEX 1.x».
- [ ] Casos de Cell64 (espada Kokiri sin textura, fuente de la pantalla de
      nombre).
- [ ] Al integrar con la Fase 3: `sp_dma` hacia la RDRAM debe llamar
      `salsa64_bus_rdram_written`.
- [ ] «NO CONTROLLER» en Zelda: el control por SI/PIF ya está integrado;
      comprobar en la PS3 que Zelda lo ve.
- [ ] Comprobar en la PS3 Zelda con la HLE en el SPE y la detección de
      bucles de espera juntas: lo que el RSP escriba o avise fuera del
      CPU debe ir en un evento del planificador (nota de los controles).

## 2026-10-10 — Guardado del cartucho: EEPROM y SRAM

Hecho:
- **EEPROM de 4 y 16 Kbit** en el canal 4 del PIF (`src/core/save.c`,
  desde n64brew.dev: Joybus Protocol): información (0x00/0xFF →
  0x0080/0x00C0), lectura (0x04) y escritura (0x05) de bloques de 8 bytes;
  en la de 4 Kbit los bloques 64–255 repiten 0–63. Escritura instantánea
  (en la consola, hasta 6 ms), nunca "ocupada".
- **SRAM de 256 Kbit** en el dominio 2 del PI (0x08000000, espejo cada
  32 KB): DMA en los dos sentidos y accesos directos de 32 bits del CPU.
- **Tipo por juego** (`salsa64_save_detect`): `guardado.cfg` (manual; por
  código de 3 o 4 caracteres, CRC1 o `*`) > cabecera de homebrew ("ED" en
  0x3C, tipo en 0x3F; la de n64tool de libdragon) > tabla por código
  (`src/core/savedb.c`: 178 códigos y 6 excepciones por región, solo donde
  coinciden ares y N64FlashcartMenu) > ninguno, con aviso en el log. Los
  seis juegos objetivo están en la tabla (SM64, MK64, SF64 y GoldenEye
  EEPROM 4K; Perfect Dark 16K; Zelda OoT SRAM). FlashRAM, SRAM de 768 Kbit
  y de 1 Mbit se reconocen pero no se emulan todavía.
- **Persistencia** (`src/frontend/savefile.c`, igual en la PS3 y en el
  host): un archivo por juego y tipo en `/dev_hdd0/salsa64/saves/`
  (`<título>-<CRC1>.eep|sra`, los nombres de Project64). Búfer preasignado
  de 32 KB; se cuentan las escrituras del juego que cambian datos y se
  vuelca tras 2 s sin escrituras (a lo más 30 s si no paran) y al salir.
  Escritura segura: `.tmp`, borrar y renombrar.
- Ganchos mínimos en `pif.c`, `pi.c` y `bus.c` (24 líneas); `bus->save`
  sobrevive al reset.
- Pruebas: suite `save` (14 pruebas; 196 en total) en el host y en RPCS3
  (big-endian). ROM propia `savetest` (`tests/roms/src/savetest`, tres
  variantes por la cabecera, `tools/build-savetest.sh`): escribe un patrón,
  lo relee y en el arranque siguiente verifica que persistió. En
  `tools/test-roms.sh` (dos arranques por tipo con `salsa64-run
  --save-dir`) y en RPCS3 con `salsa64-save-tests` (SLSA00020, las tres
  ROMs sin pantalla; `tools/test-phase1.sh` lo abre dos veces): pasan,
  con el volcado diferido a los 2,0 s. Prueba de mutación: romper la
  repetición de bloques, el identificador, la escritura directa o el DMA
  hacia la SRAM lo atrapan las pruebas y la ROM.
- Sin regresiones: nemu64-test 462 fallas, vitest y controles OK; compilan
  fase 0, fase 1 y el frontend sin advertencias.

- **PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN): `salsa64-save-tests`
  abierto dos veces pasa en los tres tipos ("persistió el arranque 1");
  volcado diferido a los 2,0 s y, en la EEPROM de 16 Kbit, escritura al
  cerrar con cambios pendientes 26 ms después del resultado, que también
  persistió. El `rename` del `.tmp` funciona en el disco de la PS3. El
  frontend real (variante «Salsa64 guardado», SLSA00021, para no pisar la
  app de la Fase 2) con `savetest-sram` como `rom.z64`: azul con un cuadro
  y, al volver a abrir, verde con dos; START + SELECT llega a
  `savefile_close` y a "salida normal".
- `tools/rpcs3-run.py`: en el primer lanzamiento de un ELF nuevo a veces no
  alcanza a leer el TTY.log (RPCS3 compila sus módulos); si el programa
  termina sin RESULTADO leído, lo toma del "Accumulated RPCS3 TTY" del
  final de RPCS3.log.

Notas:
- En la PS3 quedaron `saves/SALSA64_SAVE_*` (de las ROMs de prueba) y los
  paquetes `salsa64-save-tests.pkg` y `salsa64-guardado.pkg`; el
  `rom.z64` de prueba se borró (el `oot.z64` del autor sigue ahí).
- En la primera salida del frontend en la PS3 el log no tuvo "salida
  normal" (no se sabe cómo se salió); en la siguiente, con START + SELECT,
  sí. Si vuelve a faltar, revisar `rsxkit_shutdown`: el guardado se vuelca
  antes, así que no se pierde.

Falta:
- [ ] Probarlo con Zelda (SRAM) y un juego con EEPROM cuando la Fase 2
      permita llegar a guardar (respaldos propios, fuera del repo).
- [ ] FlashRAM (y SRAM de 768 Kbit y 1 Mbit) cuando un juego la pida.
- [x] `src/core/rcp.h`: el comentario del SI ya menciona la EEPROM (al
      integrar con la Fase 2) y el del PI, la SRAM.

## 2026-10-10 — Controles (PIF + DualShock 3) y bucles de espera

Hecho (validado en PS3 real, Slim CECH-3011A, HFW 4.91 + HEN):
- **Controles** (ceae30f): comandos joybus del PIF por el SI
  (`src/core/pif.c`, desde n64brew.dev): identificación, estado, accesorio
  sin pak, RST con L+R+Start, errores de canal. Frontend
  (`src/frontend/padmap.c`): stick izquierdo → stick (±80), stick derecho
  → botones C, × → A, □ → B, L2 → Z, L1 → L, R1 → R, START; todo
  configurable en `/dev_hdd0/salsa64/controles.cfg`. El puerto 1 siempre
  tiene control (el DS3 tarda ~0,2 s en aparecer y los juegos buscan
  controles al arrancar). vitest identifica los puertos y lee el control
  1 por el SI: en la PS3 llegaron a la ROM todos los botones, los sticks
  en su sentido (arriba = Y +80), los C del stick derecho y RST.
- **Bucles de espera** (940f6f8, `src/cpu/idle.c`): punto fijo exacto de
  una vuelta y salto en vueltas enteras hasta el próximo evento. Estado
  idéntico con y sin detección (pruebas, vitest, nemu64-test). PS3 real
  con vitest: emulación 65,6 → 35,0 ms por campo (×1,9), 15,2 → 28,3
  campos/s, 45,6 % del tiempo saltado. Costo en el bench: −1 % (21,2 →
  21,0 MIPS). docs/perf-log.md.
- Intérprete optimizado en el frontend: 23,8 MIPS con vitest (antes 13,6).
- 182 pruebas en el host y en RPCS3 (big-endian) y en la PS3; nemu64-test
  sin cambios (462).

Notas:
- ppu-gcc no pone indicaciones estáticas en los saltos (bits a/t en 0): los
  `+`/`-` que muestra objdump no son predicciones del PPE.
- Quien escriba la RDRAM fuera del CPU (RSP en SPE, DMA con latencia) debe
  hacerlo en un evento del planificador, o el salto de bucles de espera se
  lo pasaría de largo (docs/cpu-vr4300.md, «Bucles de espera»).
- `salsa64-cpu-tests-idle.pkg` (título «Salsa64 CPU bucles de espera») se
  instala sobre la misma app SLSA00005 que los demás cpu-tests.

Siguiente:
- [x] EEPROM del cartucho (canal 4 del PIF) y SRAM: entrada de arriba.
- [ ] Controller Pak y Rumble.
- [ ] Bucles de espera: sondeos de E/S que solo cambian en eventos
      (MI_INTR, SP_STATUS) y esperas por Count. (Código mapeado por TLB:
      hecho, 2026-10-10.)
- [ ] Opcional: decidir el chequeo con el inmediato del salto (no depende
      del destino) y medir si recupera el 1 %.
- [ ] Repartir emulación y presentación entre los dos hilos del PPE (ya
      importa: con vitest la emulación baja a 35 ms por campo).

## 2026-10-10 — Cierre de la jornada (2): Fase 0 y Fase 1 en PS3 real

Hecho hoy en hardware real (Slim CECH-3011A, HFW 4.91 + HEN; sin CFW posible):
- Fase 0 completa: udplog, spu-compute (6 SPUs), rsx-triangle, cpu-tests y
  jit-ppe pasan. JIT viable solo en `.text`; la caché del recompilador va en
  una cola NOBITS de 16 MB del segmento de texto (docs/jit-ppe.md).
- Fase 1 cumplida: `salsa64.pkg` muestra la ROM `vitest` (entrada de abajo).
- Intérprete en el PPE: 8,7 → 21,2 MIPS; no llegará a ~60 sin recompilador.
- Referencia Cell64 en la misma consola: Zelda en Kokiri 45–50 VI/s, ~15 cps,
  y cuatro fallas gráficas como casos de prueba (docs/referencia-cell64.md).
- Proyecto renombrado a **Salsa64**; repo privado github.com/ricowp81/Salsa64.
- Herramientas: log también en disco (`/dev_hdd0/salsa64/<prog>.log`),
  `tools/ps3-live.py` (monitor en vivo), `tools/ps3.conf` con la IP.

Siguiente (trabajos lanzados al cerrar; una prueba en la PS3 a la vez):
- [ ] Fase 2: RSP gráfico (F3DEX2) en SPE 0 + RDP mínimo en el RSX sobre
      `lib/rsxkit`, con la infraestructura de trabajos SPE.
- [ ] Fase 3: recompilador MIPS → PPC64 en el PPE.
- [x] Controles (SI/PIF + DualShock 3) y detección de bucles de espera
      (entrada de arriba).
- [ ] Después de la Fase 2: audio HLE en SPE 1 sobre la misma infraestructura.

## 2026-10-10 — Fase 1: un homebrew muestra imagen (criterio cumplido)

Hecho:
- VI (`src/core/vi.c`): temporización por campos con dos eventos del
  planificador; duración desde VI_V_SYNC, VI_H_SYNC y el reloj de cada tipo
  de TV (NTSC 1 567 041 ciclos, 59,83 Hz; PAL 49,92 Hz); VI_V_CURRENT
  calculado al leerlo, interrupción en VI_V_INTR, entrelazado con campos de
  262/263 líneas. `salsa64_vi_frame` describe la imagen visible (origen,
  tamaño por VI_H_VIDEO/VI_V_VIDEO y escalas, RGBA5551/RGBA8888);
  `salsa64_n64_run_field` corre hasta el fin del campo.
- `salsa64-run --frame` guarda la imagen del VI en PPM. La pantalla de
  resultados de nemu64-test se ve completa.
- ROM `vitest` (`tests/roms/src/vitest`, propia, GPLv2+): clang 14 para MIPS
  + nust64 (IPL3 libre r7) en contenedor reproducible
  (`tools/build-vitest.sh`). Barras, rampa y cuadro animado alternando
  RGBA5551 y RGBA8888 con doble búfer en la interrupción del VI; se
  verifica sola (Count por campo exacto, V_CURRENT en el manejador).
- `lib/rsxkit` (MIT, `lib/rsxkit/README.md`): init del RSX y video, cuadro
  con doble búfer y flip en vsync, framebuffer 2D escalado (fit/entero/
  estirar, lineal/cercano) con 5 formatos leídos directo por el RSX
  (RGBA5551 = textura R5G5B5A1). Compila aparte, sin -Isrc.
- Frontend (`src/frontend`, `salsa64.pkg`, SLSA00064): ROM de
  `/dev_hdd0/salsa64/rom.z64` o vitest embebida; un campo de emulación y
  una presentación por vuelta; con vitest se verifica solo (RDRAM y pantalla
  en ambos formatos + resultado de la ROM). Sale con START + SELECT.
- Pruebas: 161 en el host y en RPCS3; nemu64-test sin cambios (462);
  `tools/test-roms.sh` corre vitest y `tools/test-phase1.sh` el frontend.
- **PS3 real** (CECH-3011A, HEN): `RESULTADO frontend: OK` en el campo 124;
  8,7 campos/s (14 % de NTSC), 13,6 MIPS, presentación 0,3 ms por campo
  (docs/perf-log.md).

Notas:
- El log UDP no llegó a `tools/udplog.py`: había otro receptor abierto en el
  mismo puerto (`jit-ppe-cache`, desde las 09:27) que se quedó con los
  paquetes unicast. El log en disco (`/dev_hdd0/salsa64/salsa64.log`, por
  FTP) sí estaba completo.
- Los 13,6 MIPS se midieron antes del perfilado. La sospecha del FPSCR no
  aplicaba; el intérprete optimizado da 21,2 MIPS en el bench (entrada
  siguiente).

Siguiente (Fase 1):
- [x] Volver a medir el frontend con vitest con el intérprete optimizado:
      15,2 campos/s, 23,8 MIPS (sin detección de bucles de espera).
- [x] Detección de bucles de espera: ×1,9 con vitest (entrada de arriba).
- [x] SI: comandos del PIF (controles) y mapeo del DualShock 3.
- [ ] Repartir emulación y presentación entre los dos hilos del PPE.
- [ ] Origen del VI: verificar contra ares si libultra deja VI_ORIGIN una
      línea después del inicio del framebuffer (aquí se toma como la
      primera línea visible).
- [ ] Bus PI, cachés, CPUTest de krom y camino plano: igual que abajo.

## 2026-10-10 — Intérprete en el PPE: 8,7 → 21,2 MIPS

Hecho (números y diagnóstico en docs/perf-log.md):
- Benchmark en variantes (mixto, nop, enteros, memoria, mov.s, add.s) con
  MIPS y ns por instrucción.
- La FPU nunca cambiaba el FPSCR: la sospecha no aplicaba. Lo caro era el
  bucle (~250 ciclos por instrucción aun para un NOP: load-hit-store, una
  llamada con marco de pila por instrucción, `lwax` en los `switch`) y, en
  la FPU, pasar valores entre registros enteros y de punto flotante por la
  pila.
- Bucle con pc/next_pc/flag de salto en registros y `cpu->redirect` para
  excepciones/ERET/set_pc; r0 nunca se escribe; NOP sin segundo despacho;
  BC1 en el bucle; `-mno-relative-jumptables` en mk/ps3.mk; camino rápido
  de la FPU (src/cpu/fpu.h) con prueba de equivalencia de 20 000 casos.
- PS3 real: mixto 8,7 → 17,1 MIPS (×2,0); nop ×3,0; add.s ×2,9. 155 pruebas
  en host y RPCS3; nemu64-test sin regresiones.

- Segunda ronda (147d5ac): SPECIAL en la misma tabla de despacho, FPU
  rápida que clasifica con los bits en enteros y una franja de exponentes
  segura (una sola comparación de punto flotante), C.cond en enteros y
  MOV/ABS/NEG/MFC1/MTC1/CFC1/DMFC1/DMTC1 en el bucle. Prueba de
  equivalencia de 30 000 casos. PS3 real: mixto 17,1 → 21,2 MIPS; mov.s
  ×1,8; add.s ×1,8; nop −10 % (cadena de despacho más larga).

Siguiente:
- [ ] Opcional, poco: índice de despacho ya escalado y calculado directo de
      la palabra de la instrucción (recupera parte de los ~9 ciclos que
      perdieron NOP y LW/SW).
- El bucle cuesta ~93 ciclos aun para un NOP: el intérprete no llegará a
  ~60 MIPS en el PPE sin decodificar por adelantado, y la dependencia entre
  instrucciones por `cpu->gpr` en memoria no se quita en un intérprete. La
  meta de velocidad completa es el recompilador (Fase 3).

## 2026-10-10 — Fase 1: arranque HLE, `salsa64-run` y nemu64-test

Hecho (detalle en docs/cpu-vr4300.md):
- Arranque HLE sin PIF ROM ni IPL3 (`src/core/boot.c`, `src/core/rom.c`):
  normaliza .v64/.n64 a .z64, reconoce el IPL3 de Nintendo por CRC-32 y tiene
  dos caminos. ELF (IPL3 libre de libdragon, el de nemu64-test): carga los
  PT_LOAD, deja los boot flags en DMEM (memoria en [0], TV en [9], ELF en
  [12]) y $sp al final de la RDRAM. Plano (IPL3 de Nintendo o compat): copia
  1 MB desde 0x1000 a la dirección del encabezado (ajustada por CIC) y deja
  las variables de libultra en 0x300 y s3–s7. Común: Status 0x34000000,
  tiempos de PI DOM1, RI_SELECT, registros de RDRAM. Fuentes documentadas.
- ISViewer en 0x13FF0000, opcional (tapa los últimos 64 KB de una ROM de
  64 MB).
- `salsa64-run` (`src/host/run.c`, `tools/build.sh host`): corre una ROM N
  ciclos o hasta un texto del ISViewer, detecta bucles sin salida, traza
  excepciones (`--trace-exc`). Sin asignaciones en el bucle.
- nemu64-test compilado en contenedor reproducible
  (`tools/build-nemu64-test.sh`, mismo SHA-256 desde cero), licencias e IPL3
  verificados, en `tests/roms/homebrew/` con sus avisos.
- `tools/test-roms.sh`: corre nemu64-test y compara con la línea base de
  fallas conocidas (`tests/roms/nemu64-test.fallas`); una regresión lo hace
  fallar (probado rompiendo a propósito el enlace de JAL).
- nemu64-test pasó de no arrancar a 4175 de 4637 pruebas (90 %). Las fallas
  del CPU que encontró están arregladas: MULT con rt de 35 bits, LWR parcial,
  enlace de saltos en delay slot, MTC0 de 64 bits, Index.P, TagLo, PageMask y
  EntryLo en la TLB, Random con Wired > 31, funciones de COP0 reservadas
  como NOP, CFC0/CTC0/BC0, CACHE no alineado, latch de COP2, direccionamiento
  de 64 bits (xkuseg/xksseg/xkphys/xkseg), Status.RE; FPU: índices de
  registro con FR = 0, clasificación de NaN (estaba al revés), redondeo
  dirigido e Inexact, desbordamiento, flush con FS, rango de CVT.L,
  codificaciones inexistentes → FPE. Bus: SB/SH/SD fuera de la RDRAM y
  lecturas del bus PI de 16 bits.
- RSP provisional: quitarle HALT lo detiene en el acto con BROKE, para que
  nadie espere para siempre (Fase 2 lo reemplaza).
- Pruebas: 154 unitarias (antes 139) en host (ASan + UBSan) y en RPCS3
  (big-endian): todas pasan. Se actualizaron las 5 que codificaban
  suposiciones que nemu64-test desmintió.
- Rendimiento del intérprete en el host: 237 → 217 MIPS por la FPU exacta
  (docs/perf-log.md). nemu64-test completo: 0,75 s en el host.

Fallas que quedan en nemu64-test (fuera del intérprete): RSP 294, bus PI
del cartucho 81, cachés 46, SP (DMA y registros) 31, RDP y MI repeat 10.

Siguiente (Fase 1):
- [x] Correr `salsa64-cpu-tests` en la PS3 real: 154/154 pasan, **8,8 MIPS**
      (docs/perf-log.md).
- [x] Perfilar el intérprete en el PPE: 8,8 → 21,2 MIPS en dos rondas
      (entrada de arriba; la sospecha del FPSCR no aplicaba).
- [x] VI con contador de líneas e interrupción; framebuffer 2D en el RSX
      (primera pieza de `lib/rsxkit/`): ver la entrada de arriba.
- [ ] Bus PI: latch de escrituras al cartucho y DMA no alineado (81 pruebas
      de nemu64-test); FlashRAM (la SRAM ya está).
- [ ] Cachés del VR4300 (46 pruebas) y ciclos por instrucción (conjuntos
      "timing" y "cycle" de nemu64-test).
- [ ] CPUTest de krom ensamblado desde fuente con bootcode en ceros (traer
      solo los `.asm` por la API: el repo trae el IPL3 de Nintendo). Probaría
      el camino plano del arranque HLE.
- [x] Probar el camino plano con los juegos del usuario (fuera del repo):
      falta reproducir efectos del IPL3 de algunos CIC (6105). (Hecho:
      arranque del 6105 y censo, 2026-10-10.)

## 2026-10-10 — Fase 1 iniciada: intérprete del VR4300

Hecho (diseño en docs/cpu-vr4300.md):
- Intérprete del VR4300 en `src/cpu/`: ALU de 32/64 bits con
  desbordamiento, cargas/almacenamientos (incluidos LWL/LWR/LDL/LDR y sus
  stores), LL/SC, saltos con delay slot y branch-likely, HI/LO, MULT/DIV y
  variantes de 64 bits, traps; COP0 (Count/Compare, Status, Cause, EPC,
  ErrorEPC, excepciones, ERET, TLB de 32 entradas, modos usuario/supervisor);
  COP1 con modo FR, causas/banderas de FCR31, conversiones y comparaciones.
- Bus y mapa físico en `src/core/`: RDRAM de 4/8 MB con camino rápido desde
  KSEG0/KSEG1, SP DMEM/IMEM, ROM, PIF RAM. MI completo (interrupciones →
  IP2); PI y SI con DMA inmediato; VI, AI, SP, DP como registros. Planificador
  de eventos (Compare). Sin asignaciones en el bucle.
- Portable: mismo código para ppu-gcc (big-endian, accesos directos) y gcc
  nativo (arm64, con byte-swap).
- Pruebas: `tools/build.sh test-host` (139 pruebas, ASan + UBSan) y
  `tools/test-phase1.sh` (las mismas en RPCS3, big-endian). Todas pasan en
  ambos. Prueba de mutación: romper SRA, branch-likely o EPC las hace fallar.
- Micro-benchmark del intérprete (bucle mixto con FPU): 237 MIPS en el host
  (Mac Studio, Docker arm64); 35,6 MIPS en RPCS3 (no concluyente).
- Suites elegidas y licencias verificadas (docs/THIRD_PARTY.md):
  nemu64-test (MIT, IPL3 libre) como principal; CPUTest de krom (Unlicense)
  solo ensamblado desde fuente, porque sus ROMs traen el IPL3 de Nintendo.
  Dillonb/n64-tests descartada (sin licencia).

Siguiente (Fase 1): ver la entrada de arriba. Hechos en ella: arranque
HLE, ISViewer, contenedor de nemu64-test, direccionamiento de 64 bits y
bandera Inexact.

## 2026-10-10 — Fase 0: completa en RPCS3, falta la PS3 real

Hecho:
- Imagen `salsa64-toolchain:dev` (ver docs/toolchain.md): ps3toolchain (GCC 13.2
  PPU, 9.5 SPU) + PSL1GHT + cgcomp con Cg Toolkit vía qemu. Los 54 shaders de
  la rama `ps3` del port de SM64 compilan; 18/31 samples de PSL1GHT compilan
  (las fallas son de los samples).
- Programas de la Fase 0, compilados sin advertencias (`tools/build.sh`):
  - `tests/phase0/rsx-triangle`: triángulo + verificación de píxeles.
  - `tests/phase0/spu-compute`: 6 SPUs, DMA de doble búfer, verificación exacta.
  - `tests/phase0/jit-ppe`: 8 mecanismos de memoria, tolera caídas.
  - `tests/phase0/udplog`: 200 líneas numeradas por UDP.
- Logger TTY + UDP (`src/platform/ps3/log.c`) y receptor `tools/udplog.py`.

- RPCS3 0.0.43 + firmware 4.92 en la Mac (docs/rpcs3.md). `tools/test-phase0.sh`
  corre las cuatro pruebas: udplog, spu-compute y rsx-triangle pasan;
  jit-ppe completa su ciclo, pero RPCS3 no ejecuta código generado (no
  concluyente).

Falta para cerrar la Fase 0:
- [x] Correr los 4 programas en RPCS3 y automatizarlo.
- [x] PS3 real (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN; no admite CFW):
      udplog, spu-compute, rsx-triangle y salsa64-cpu-tests pasan; jit-ppe:
      solo `.text` es escribible y ejecutable (docs/jit-ppe.md). Números en
      docs/perf-log.md. El log también se escribe en /dev_hdd0/salsa64/<prog>.log.
- [x] Plan de la Fase 3: recompilador en el PPE con la caché en una región de
      `.text`. Falta medir el tamaño máximo de esa región (docs/jit-ppe.md).
- [ ] Antes: conectar la PS3 (está desconectada; revisar su estado). Datos a
      reunir: IP por cable, versión de firmware, apps de envío instaladas y si
      hay capturadora. Luego correr la Fase 0 y `salsa64-cpu-tests` (primer
      número de MIPS real).
- [ ] Opcional: correr Cell64 en la PS3 y guardar su `perf_trace.csv`
      (docs/referencia-cell64.md).

Nombre: **Salsa64** (decidido el 2026-10-10). Antes: "Retro64" (2026-10-10) y "Cell64" (provisional).
- Ya existe un emulador de N64 para PS3 llamado **Cell64**: v1.52 de
  "ZeBobbel", publicado en Brewology el 2026-09-27
  (store.brewology.com/ahomebrew.php?brewid=344), con una variante "HEN FIX by
  DropSonic DS". Sin código fuente publicado ni descripción técnica; el
  "sitio del desarrollador" es un canal de YouTube con dos videos de prueba.
  Por eso se renombró el proyecto. La carpeta sigue siendo `projects/cell64`.
- Referencia de rendimiento de ese emulador (videos de ZeBobbel, capturadora,
  build "40031911"): el overlay dice "INT" (intérprete; formato del overlay de
  Wii64/Not64). Super Mario 64 en el castillo: 46,9 VI/s (~78 %). Zelda OoT,
  pantalla de título: 26,4 VI/s, 9,1 FPS (~44 %). No se vio modo "DYN".
  En el video de la v1.50 (overlay nuevo, sin indicar INT/DYN): Zelda OoT en
  el menú de archivos a 50,0 VI/s y 50,0 FPS; probablemente versión PAL (50 Hz),
  o sea ~100 % en una escena ligera. En juego (bosque Kokiri): 41,3 VI/s y
  13,8 FPS (relación 3:1, coherente); ~83 % si es PAL, ~69 % si es NTSC.
  Meta para nuestro emulador: superar eso en la misma escena y región.
  Comparar contra esto nuestro intérprete (Fase 1) y el recompilador (Fase 3),
  con los mismos juegos en la misma PS3.

Notas:
- No usar `-Os` en el PPU: ICE de GCC 13.2 (docs/toolchain.md).
- En RPCS3, rsx-triangle necesita "Write Color Buffers" para su verificación.
- Archivos de Salsa64 en la PS3: `/dev_hdd0/salsa64/` (RPCS3 vacía `/dev_hdd0/tmp`).
- Juegos objetivo: tabla en docs/proyecto.md. Comparación con Cell64 en
  docs/referencia-cell64.md (no usa SPEs; todo en un hilo del PPE).
- Ideas sin decidir: versión rápida del intérprete (decodificar una vez,
  como el "FASTINT" de Cell64) y, como investigación lejana, emular la CPU en
  un SPE solo si el PPE no alcanza.
