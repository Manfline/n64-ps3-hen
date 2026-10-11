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
- `mk64-luigi.pad`: Mario Kart 64: START en el 900, A cada 150 campos por
  los menús (1P, Mario GP, 50cc, Mario, Mushroom Cup, Luigi Raceway) y
  desde el 2650 A sostenida con el stick a la derecha en tres tramos: la
  carrera empieza en el campo ~2550 y el kart da tumbos por la pista hasta
  el ~6000. En esos campos no hay cargas a la TMEM desde un framebuffer.
