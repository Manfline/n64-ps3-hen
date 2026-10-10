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
