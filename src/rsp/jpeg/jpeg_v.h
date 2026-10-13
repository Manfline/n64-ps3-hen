/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — la tarea JPEG en 4 carriles (jpeg_v.c, v4.h): lo que corre en
 * el SPE (src/spu/jpeg). Da lo mismo byte a byte que salsa64_jpeg_mb
 * (jpeg.c): las mismas operaciones enteras, cuatro a la vez. En el host
 * compila con los carriles uno por uno, para la prueba unitaria.
 */
#ifndef SALSA64_RSP_JPEG_JPEG_V_H
#define SALSA64_RSP_JPEG_JPEG_V_H

#include <stdint.h>

#include "rsp/jpeg/v4.h"

/* Las tres tablas de cuantización de una tarea, en el orden en que llegan
 * los coeficientes (zigzag), 4 por v4. */
struct salsa64_jpeg_qv {
	v4 q[3][16];
};

/* Una vez, antes de lo demás (tablas de constantes). */
void salsa64_jpeg_v_init(void);

void salsa64_jpeg_qv_init(struct salsa64_jpeg_qv *qv, const int16_t q[3][64]);

/* Como salsa64_jpeg_mb; `in` y `out` alineados a 16 bytes (pueden ser el
 * mismo búfer). */
void salsa64_jpeg_mb_v(uint32_t mode, const struct salsa64_jpeg_qv *qv, const uint8_t *in, uint8_t *out);

#endif
