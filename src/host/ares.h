/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — comparar la HLE gráfica con un volcado de la RDRAM de ares
 * (salsa64-run --ares). Ver ares.c.
 */
#ifndef SALSA64_HOST_ARES_H
#define SALSA64_HOST_ARES_H

/* argv[0] es "--ares"; devuelve el código de salida de salsa64-run. */
int ares_main(int argc, char **argv);

#endif
