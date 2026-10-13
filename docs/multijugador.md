# Multijugador local con varios DualShock 3

Plan del 2026-10-13. Objetivo: jugar de 2 a 4 personas en la misma PS3, un
DualShock 3 por jugador, como en la N64. El juego en red sigue fuera de los
objetivos (docs/proyecto.md).

## Lo que ya hay

- `update_pads` (src/frontend/main.c) lee los 4 puertos de `ioPadGetInfo`.
  Cada DS3 conectado va al puerto de la N64 del mismo número, con el mismo
  mapeo de `controles.cfg` (src/frontend/padmap.c).
- La PIF (src/core/pif.c) responde por los 4 puertos. Un puerto sin mando se
  reporta desconectado, salvo el 1, que siempre está conectado.
- El log ya escribe «mando N conectado/desconectado» y «control N: …» por
  puerto.
- Nunca se ha probado con más de un mando en la PS3 real.

## Fase 1: probar lo que hay (sin código nuevo)

Con la app normal y 2 mandos (4 si los hay):

| Juego | Qué ver |
|---|---|
| Mario Kart 64 | el menú reconoce al J2; carrera a 2 (y a 3–4): control, pantalla dividida, velocidad |
| Super Smash Bros. / Smash Remix | elegir personaje con cada mando; pelea a 2–4; KO |
| GoldenEye 007 | multijugador a 2 en un mapa chico (Facility o Complex) |
| Mario Party | minijuegos a 4 |

Anotar por juego: si cada mando mueve a su jugador, si el juego ve conectado
a un mando que no está (o al revés) y los campos/s con la pantalla dividida.
Los logs muestran la conexión por puerto.

## Fase 2: asignación de jugadores

- La PS3 numera los mandos por orden de conexión. Mostrar en el menú quién
  es J1–J4 y poder reordenarlos (por ejemplo: «pulsa × en el mando que será
  el J1»).
- Conexión y desconexión a media partida: el puerto de la N64 se marca
  desconectado al soltar el mando y vuelve al reconectarlo. Muchos juegos solo
  revisan los mandos en sus menús, y no hay que forzar nada.
- Una opción por juego para limitar el número de puertos que se le dan.
  Algunos juegos cambian de comportamiento si ven mandos de más.

## Fase 3: pausa y atajos con varios mandos

- Hoy START + SELECT y los atajos de SELECT funcionan desde cualquier mando.
  Propuesta: el menú de pausa sigue abriéndose desde cualquiera, pero los
  atajos de estados (SELECT + R1/L1) solo desde el J1, para que nadie cargue
  un estado por accidente.
- El botón PS pausa para todos (como hoy).

## Fase 4: controles por jugador

- `controles.cfg` aplica a todos. Si se pide, secciones `[jugador2]`… con el
  mismo formato; sin sección, el mapeo común.

## Fase 5: accesorios por puerto

- **Rumble Pak** con la vibración del DS3 (`ioPadSetActDirect` de PSL1GHT):
  el juego escribe al accesorio y el frontend enciende el motor del mando de
  ese puerto. Elegible por juego (`guardado.cfg` o un `accesorios.cfg`), ya
  que el juego pregunta qué accesorio hay.
- **Controller Pak**: 32 KB por paquete, un archivo por juego y puerto en
  `/dev_hdd0/salsa64/saves` (por ejemplo `<juego>-pak1.mpk`). Lo usan los
  fantasmas de Mario Kart y varios juegos de deportes. Formato compatible con
  los `.mpk` de otros emuladores para poder traer partidas.
- Transfer Pak: fuera de los objetivos.

## Fase 6: rendimiento con pantalla dividida

- Con 2–4 vistas, el juego hace más trabajo de CPU y más tareas gráficas por
  cuadro. GoldenEye a 4 ya iba lento en la N64 real.
- Medir en la PS3 (campos/s, ns por instrucción, ms del RSX por cuadro) en
  Mario Kart y Smash a 4, y GoldenEye a 2 y 4, y anotarlo en
  docs/perf-log.md.
- Lo que salga de ahí se ataca con lo de siempre (recompilador, HLE en el
  SPE). Más velocidad que la de la consola real no es un objetivo; igualar
  la de la N64 sí.

## Orden propuesto

1. Fase 1 en la PS3 (sin paquete nuevo).
2. Arreglar lo que falle en la fase 1.
3. Fase 2 (asignación) y fase 3 (pausa).
4. Rumble Pak (barato y se nota) y después Controller Pak.
5. Fase 6, por juego.
6. Fase 4 si alguien la pide.

## Muy a futuro: en línea

Fuera de los objetivos por ahora (docs/proyecto.md), pero anotado. Lo
mínimo sería por retraso de entradas: cada consola emula lo mismo y solo
viajan los botones, con unos cuadros de retraso fijo. Exige que la emulación
sea determinista (misma entrada, mismo resultado en las dos PS3) y un
arranque desde el mismo estado. Lo bueno, el rollback (volver atrás y
re-emular al llegar la entrada del otro), necesita estados guardados muy
rápidos y margen de CPU, que hoy no sobra.
