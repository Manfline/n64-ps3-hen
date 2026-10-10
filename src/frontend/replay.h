/*
 * Salsa64 — repetición de un volcado de la HLE gráfica (gfxdump.bin) en el
 * RSX, sin emular nada: para depurar el lado del RSX con la salida exacta
 * de una tarea (ver salsa64_rdp_dump y salsa64-run --raster-dump).
 */
#ifndef SALSA64_FRONTEND_REPLAY_H
#define SALSA64_FRONTEND_REPLAY_H

/* Dibuja el volcado y presenta su primera color image unos cuadros; guarda
 * la pantalla en SALSA64_DATA_DIR/replay.ppm. Devuelve 0 si pudo. */
int salsa64_replay(const char *path);

#endif
