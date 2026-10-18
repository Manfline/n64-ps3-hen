/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE de las tareas de video de Resident Evil 2 (rsp/jpeg/re2.c).
 *
 * Las cinemáticas del juego las decodifica el CPU hasta planos YCbCr y el
 * RSP hace el resto con tres microcódigos pequeños, sin texto de
 * identificación, en tareas de tipo gráfico (type 1) que traen sus
 * parámetros y constantes en el segmento de datos (ucode_data, 128 bytes;
 * data_ptr va en cero):
 *   RESIZE  un cuadro RGB de 320 × 3 bytes por fila se reescala en bilineal
 *           (pasos en 16.16, desde un desplazamiento inicial en x y en y) a
 *           RGBA5551, una franja de filas por tarea
 *   DECODE  los planos Y, Cb y Cr (4:2:0) se convierten a RGBA8888, un par
 *           de filas por pasada
 *   FILL    mezcla de dos cuadros RGBA8888: cada byte es el promedio de los
 *           dos (el segundo cuadro es también el destino)
 * Todo en enteros, con la aritmética del microcódigo real (medida con
 * salsa64-run --lle-check-jpeg, docs/estado.md; nada del microcódigo se
 * copia): el host, el PPE y el SPE (src/spu/jpeg) dan el mismo resultado byte
 * a byte, también en lo que el microcódigo deja sin querer (el byte de alfa
 * de DECODE, el último píxel de cada fila de RESIZE, que lee la DMEM de
 * salida, y el difuminado de RESIZE, que arranca con lo que libultra deja en
 * 0xFC0). La HLE solo toma las tareas cuyos parámetros y constantes son las
 * que se midieron (salsa64_re2_supported); las demás van al microcódigo real
 * (RSP a bajo nivel) o, sin él, terminan sin hacer nada.
 */
#ifndef SALSA64_RSP_JPEG_RE2_H
#define SALSA64_RSP_JPEG_RE2_H

#include <stdbool.h>
#include <stdint.h>

#include "rsp/jpeg/jpeg.h"

/* El segmento de datos de las tres (lo que el microcódigo trae a la DMEM). */
#define SALSA64_RE2_DATA_BYTES 128u

/* La DMEM de 4 KB de RESIZE que usa su fila: las dos filas de origen en
 * 0..1919 y, desde 0x780, la fila de salida (RGBA5551). */
#define SALSA64_RE2_DMEM       4096u
#define SALSA64_RE2_SRC_ROW    960u   /* bytes de una fila de origen (320 × 3) */
#define SALSA64_RE2_OUT_BASE   0x780u

/* Parámetros de RESIZE, de la estructura de la tarea. */
struct salsa64_re2_resize {
	uint32_t src, dst;        /* físicas */
	uint32_t width, rows;     /* píxeles por fila y filas de la franja */
	uint32_t x_ratio, y_ratio;
	uint32_t stride;          /* bytes entre filas de salida */
	uint32_t x0, y0;          /* inicio en x y en y (16.16, y con la fila de origen en la parte entera) */
	int32_t thresh;           /* umbral del difuminado entre vecinos (0: sin difuminar, siempre +4) */
};

/* El estado del difuminado de RESIZE (los registros v22 a v24 y las tres palabras de la DMEM
 * donde guarda el último carril): sigue de una fila a la siguiente y de un grupo de 8
 * píxeles al siguiente, dentro de una tarea. */
struct salsa64_re2_dither {
	int32_t v[3][8];          /* por canal (bytes 0, 1 y 2 del píxel de origen), por carril */
	int32_t last[3];          /* el carril 7 del grupo anterior */
};

/* Parámetros de DECODE. */
struct salsa64_re2_decode {
	uint32_t y, cb, cr, dst;  /* físicas */
	uint32_t width;           /* múltiplo de 16, hasta 240 */
	uint32_t pairs;           /* pares de filas */
	uint32_t step;            /* bytes entre un par de filas y el siguiente en la salida */
	uint32_t k[5];            /* constantes Q16 de Cr→R, Cr→G, Cb→G, Cb→B y Y */
	int32_t zero, max;        /* el cero de la croma (<< 7) y el tope de cada canal (<< 7) */
};

/* Parámetros de FILL. */
struct salsa64_re2_fill {
	uint32_t src, dst;        /* físicas */
	uint32_t len;             /* bytes de cada fila */
	uint32_t rows;            /* filas por pasada (2) */
	uint32_t skip;            /* bytes de la RDRAM entre el fin de una fila y el inicio de la siguiente */
	uint32_t iters;           /* pasadas */
	uint32_t advance;         /* bytes entre una pasada y la siguiente */
};

/* Qué microcódigo es (suma de los 256 primeros bytes del código, como
 * mupen64plus-rsp-hle `try_normal_task_detection`). SALSA64_JPEG_NONE si
 * no es ninguno. */
enum salsa64_jpeg_kind salsa64_re2_detect(const uint8_t *ucode, uint32_t size);

/* ¿Es una de las tres? */
static inline bool salsa64_jpeg_is_re2(enum salsa64_jpeg_kind k)
{
	return k >= SALSA64_JPEG_RE2_RESIZE && k <= SALSA64_JPEG_RE2_FILL;
}

/* Lee los parámetros del segmento de datos `d` (SALSA64_RE2_DATA_BYTES bytes en
 * el orden de la N64). false si la tarea no es de las que la HLE hace igual
 * que el microcódigo (constantes distintas, anchos o disposiciones que no se
 * midieron). */
bool salsa64_re2_resize_read(const uint8_t *d, struct salsa64_re2_resize *t);
bool salsa64_re2_decode_read(const uint8_t *d, struct salsa64_re2_decode *t);
bool salsa64_re2_fill_read(const uint8_t *d, struct salsa64_re2_fill *t);

/* ¿La HLE hace esta tarea (segmento de datos en `data`, dentro de la RDRAM)? */
bool salsa64_re2_supported(enum salsa64_jpeg_kind kind, const uint8_t *rdram, uint32_t rdram_size, uint32_t data);

/* ---- Núcleos (compartidos por el host, el PPE y el SPE) ---- */

/* El estado inicial del difuminado: los registros en cero y, en la DMEM, lo que hay en
 * 0xFE0, 0xFE2 y 0xFE4 al arrancar (las palabras de la tarea dram_stack y
 * dram_stack_size, que libultra deja en 0xFC0). */
void salsa64_re2_resize_begin(struct salsa64_re2_dither *st, uint32_t dram_stack, uint32_t dram_stack_size);

/* La fila `n` de RESIZE (a partir de y0 + n · y_ratio): `dm` es la DMEM de
 * 4 KB con las dos filas de origen ya en 0..1919 (las que empiezan en la
 * fila (y >> 16) del cuadro); escribe los píxeles de salida desde
 * dm + SALSA64_RE2_OUT_BASE (la salida de las filas anteriores, que sigue ahí,
 * es lo que lee el último píxel de cada fila cuando se pasa del ancho del
 * origen) en grupos de 8 píxeles. Hasta SALSA64_RE2_OUT_BASE + 16 · ⌈ancho/8⌉
 * bytes. */
void salsa64_re2_resize_row(uint8_t *dm, const struct salsa64_re2_resize *t, struct salsa64_re2_dither *st,
			    uint32_t n);

/* El tramo de la RDRAM (desde `src`, físico) de la fila `n` de RESIZE: la
 * dirección de las dos filas de origen. */
static inline uint32_t salsa64_re2_resize_src(const struct salsa64_re2_resize *t, uint32_t n)
{
	return t->src + ((t->y0 + n * t->y_ratio) >> 16) * SALSA64_RE2_SRC_ROW;
}

/* Un par de filas de DECODE: `y`: las dos filas de luminancia (2 · ancho
 * bytes); `cb`, `cr`: lo que trae el DMA de cada croma (ancho bytes: la fila
 * del par, ancho / 2, y la siguiente); escribe `row0` y `row1` (4 · ancho bytes
 * cada una, RGBA8888). */
void salsa64_re2_decode_pair(const struct salsa64_re2_decode *t, const uint8_t *y, const uint8_t *cb,
			     const uint8_t *cr, uint8_t *row0, uint8_t *row1);

/* Una fila de FILL: dst[i] = (src[i] + dst[i]) >> 1, n bytes (múltiplo de 8). */
void salsa64_re2_fill_row(const uint8_t *src, uint8_t *dst, uint32_t n);

/* Corre una tarea sobre la RDRAM (en el orden de bytes de la N64). `data`:
 * el segmento de datos (ucode_data); `dram_stack` y `dram_stack_size`, de la
 * tarea (OSTask). El resultado describe lo escrito como unidades:
 * `macroblocks` unidades de `mb_out` bytes, una cada `mb_stride` desde
 * `out_addr` (una fila en RESIZE, un par de filas en DECODE, una pasada en
 * FILL). La tarea debe ser de las que salsa64_re2_supported acepta. */
void salsa64_re2_run(enum salsa64_jpeg_kind kind, uint8_t *rdram, uint32_t rdram_size, uint32_t data,
		     uint32_t dram_stack, uint32_t dram_stack_size, struct salsa64_jpeg_result *res);

#endif
