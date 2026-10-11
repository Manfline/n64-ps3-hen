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
