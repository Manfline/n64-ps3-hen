# Pulsaciones programadas para `salsa64-run --pad`

Una línea por archivo, con el formato de `--pad` (`CAMPO:BOTONES;…`; cada
estado vale hasta el siguiente, por eso cada pulsación lleva su suelta `-`):

    build/host/salsa64-run --pad "$(cat docs/pads/zelda-oot.pad)" --save-dir DIR rom.z64

- `zelda-oot.pad`: las pulsaciones del autor en la PS3 el 2026-10-10
  (sacadas de su log con el campo de cada control), desde el título hasta
  el menú de pausa en el bosque Kokiri (campo 19118): intro saltada con
  START, ranura 1 (necesita su partida «ZELDA» en `--save-dir`, fuera del
  repo), diálogos de Navi con A. Con `--audio-wav` el audio calla ~30 s
  tras elegir la partida y suena desde el vuelo de Navi, como en la PS3.
- `zelda-kokiri.pad`: las mismas pulsaciones hasta cargar la ranura 1 y, ya
  en la casa de Link, salir por la cortina (palanca abajo-izquierda desde el
  campo 2800, izquierda en el 3250, arriba en el 3385, suelta en el 3470):
  Link queda en el balcón del bosque Kokiri desde el campo ~3500, sin más
  entrada. Sirve también como `M64P_PAD` de `tools/m64p-ref.sh`.
  **Tienda Kokiri** (sala prerrenderizada, 2026-10-11): con el mismo pad y
  `--poke "3000:80383670=0x00C1;3000:80383672=0x00C1"` la cortina lleva a
  la tienda en vez de al bosque (se cambian las dos entradas de la lista de
  salidas de la escena de la casa, 0x0211 → 0x00C1, en la RDRAM de OoT 1.2
  con la partida del autor); Link está dentro desde el campo ~3550. Con
  `--lle-check-jpeg` además compara las 75 tareas JPEG de cada sala con el
  microcódigo real.
- `goldeneye-dam.pad`: START en el campo 200 y A cada 240 campos (pulsada
  8 campos): pasa el título, la carpeta y las pantallas de la misión hasta
  jugar en Dam desde el campo ~4800 (con `-c 7500000000` llega a unos 80 s
  de consola). Sirve para contar los combiners del nivel (0 aproximados con
  el generador de fragment programs).
- `sm64-castillo.pad`: Super Mario 64 (USA), partida nueva en el archivo A
  (sin `--save-dir`: la EEPROM empieza vacía), intro de Lakitu, el diálogo
  cerrado con A y Mario caminando hasta el puente del castillo (campo
  ~4200). Con `--poke "4200:8033B21E=0x0400"` (la energía de Mario a la
  mitad) aparece el medidor de energía desde el campo ~4220.
- `zelda-pausa-nueva.pad`: Zelda OoT sin partida (o con `--save-dir` a una
  carpeta vacía: crea el archivo «A» en la ranura 1): título con START,
  ranura 1 y nombre «A» (A, START), el archivo se abre con A, el intro del
  Árbol Deku y de Navi con A cada 40 campos hasta el 16000, y START en los
  campos 16100 (pausa), 16400 (sigue) y 16700 (pausa otra vez). La pausa
  cae en la tarea del campo 16107 (`--trace-fb`); con `-c 26500000000`
  alcanza hasta el campo ~16900. La partida que deja en `--save-dir`
  sirve para repetirlo sin el nombre (los campos no cambian).
- `zelda-casa-nueva.pad` (2026-10-12): Zelda OoT con partida nueva
  (`--save-dir` a una carpeta vacía), como `zelda-pausa-nueva.pad` pero con
  los tiempos del build de hoy: título con START, ranura 1, nombre «A»,
  START hasta END y A, la ranura se abre con A y desde el campo 2000 A cada
  40 campos por el intro. Link despierta en su casa (sala prerrenderizada)
  hacia el campo 14 300: con `--lle-check-jpeg`, 150 tareas JPEG hasta el
  15 500.
- `mk64-luigi.pad`: Mario Kart 64: START en el 900, A cada 150 campos por
  los menús (1P, Mario GP, 50cc, Mario, Mushroom Cup, Luigi Raceway) y
  desde el 2650 A sostenida con el stick a la derecha en tres tramos: la
  carrera empieza en el campo ~2550 y el kart da tumbos por la pista hasta
  el ~6000. En esos campos no hay cargas a la TMEM desde un framebuffer.
- `banjo-casa.pad`: Banjo-Kazooie (USA) sin partida: deja correr la intro
  (Banjo tocando con el fondo de la cascada, campos ~1700–2600), START en
  el título, el archivo vacío «GAME 1» con A y la presentación de
  Gruntilda, Tooty y Bottles con A cada 60 campos hasta el 16000 (la casa
  por dentro hacia el ~11000). Banjo sale de su casa a Spiral Mountain
  desde el campo ~17000. Con el arranque del 6103 completo (2026-10-12):
  con el de antes el juego cambia por otro camino y los campos no coinciden.
- `killerinstinct-pelea.pad` (Killer Instinct Gold, 2026-10-17): las
  pulsaciones de un jugador en el **control 2** (el log de la PS3 las anotó
  como «control 2»), sacadas de su log: START en el título (campo 792), A
  por las pantallas hasta la selección de personaje (campo ~1000), la
  confirmación (campo 1211) y, desde el 1300, A cada 60 campos. Se corre con
  `--pads 2 --pad-port 2` (los dos controles conectados, como en la PS3 con
  dos mandos, y el guion en el puerto 2). En el host, con la HLE, pasa por la
  selección y llega a la pelea y a «CONTINUE?» hacia el campo 3300
  (`--fields 3600`). Los tiempos del juego dependen de la máquina, así que
  en la PS3 la pelea empieza antes o después de ese campo.

Las escenas del host (7 juegos, Kirby 64 y la casa de Link de
`zelda-casa-nueva.pad`) se corren y comparan con `tools/escenas-host.sh`.
- `mk64-ghost.pad`: Mario Kart 64 en Time Trials (Mario, Luigi Raceway):
  menús hasta la carrera, tres vueltas conducidas con el stick (5:32 en
  total, buscadas campo a campo sobre estados guardados), y el menú de
  resultados hasta «SAVE GHOST» y su primera ranura (campo ~28000). Con
  `--save-dir DIR` (partida y Controller Pak nuevos) el juego crea su archivo
  de datos de 121 páginas en el pak y guarda el fantasma: `DIR/…mpk` sale
  con 43 bloques cambiados. Hace falta `--fields 28300`; el camino depende
  de que no haya otros controles conectados.
- `stadium-vs.pad`: Pokémon Stadium (USA): START en los campos 600 y 900,
  «Game Pak Check» (el juego sondea los cuatro puertos y no encuentra un
  Transfer Pak), A para entrar a VS mode y A para elegir; hasta ahí llega
  el campo ~1750. Con `--gfx-frame` (el rasterizador, que copia a la RDRAM
  como la PS3) servía para ver el cuelgue de la novena color image de una
  tarea (nodos del juego pisados); ahora la escena sigue al estadio, el
  control se lee 25 veces por 50 campos y el panel «Game Pak None» y la vista
  previa del estadio salen como textura (las nueve imágenes de la tarea del
  campo 1438: la ranura que se recicla copia antes lo suyo a la RDRAM).
