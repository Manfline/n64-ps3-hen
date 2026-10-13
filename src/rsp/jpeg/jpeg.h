/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE de las tareas JPEG del RSP (tipo 4 de OSTask).
 *
 * Zelda OoT decodifica en el CPU el JPEG del fondo de una sala
 * prerrenderizada hasta los coeficientes de cada macrobloque y manda al RSP
 * tareas que descuantizan, hacen la IDCT y convierten a RGBA5551, escribiendo
 * cada macrobloque en su mismo lugar de la RDRAM. Ver jpeg.c. Pokémon
 * Stadium 2 usa el mismo microcódigo para sus fondos (la pantalla de título).
 *
 * Todo en enteros: el host, el PPE y el SPE (src/spu/jpeg) dan el mismo
 * resultado byte a byte.
 */
#ifndef SALSA64_RSP_JPEG_H
#define SALSA64_RSP_JPEG_H

#include <stdbool.h>
#include <stdint.h>

/* Variante del microcódigo (la reconoce core/rsp.c por el CRC-32). */
enum salsa64_jpeg_kind {
	SALSA64_JPEG_NONE = 0,
	SALSA64_JPEG_OOT,     /* Zelda OoT y Pokémon Stadium 1 y 2: salida RGBA5551, sin reescalar Y/UV */
};

/* Bytes de entrada de un macrobloque ((modo + 4) bloques de 64
 * coeficientes de 16 bits) y de su salida RGBA5551 (16 × 8 o 16 × 16). */
#define SALSA64_JPEG_MB_IN(mode)  (((mode) + 4u) * 128u)
#define SALSA64_JPEG_MB_OUT(mode) ((mode) ? 512u : 256u)
#define SALSA64_JPEG_TASK_BYTES   24u  /* lo que se lee de la estructura de la tarea */

struct salsa64_jpeg_result {
	uint32_t macroblocks;  /* macrobloques escritos */
	uint32_t out_addr;     /* primer byte escrito (físico) */
	uint32_t out_bytes;    /* desde out_addr hasta el último escrito */
	uint32_t mb_stride;    /* bytes de cada macrobloque de entrada */
	uint32_t mb_out;       /* bytes escritos al principio de cada uno */
	bool bad;              /* parámetros fuera de la RDRAM o modo desconocido */
};

/* La estructura de la tarea (data_ptr): dirección física del primer
 * macrobloque, cuántos, modo (0 = 4:2:2, 2 = 4:2:0) y las direcciones
 * físicas de las tres tablas de cuantización (Y, U, V). */
struct salsa64_jpeg_task {
	uint32_t addr, count, mode;
	uint32_t qaddr[3];
};

/* La aritmética (jpeg.c), compartida con la versión vectorial (jpeg_v.c):
 * tabla zigzag, IDCT en Q15, desplazamientos de las filas (Q15 → 2 bits de
 * fracción) y de las columnas (→ dieciseisavos), y YCbCr → RGB en Q14. */
extern const uint8_t salsa64_jpeg_zigzag[64];
extern const int16_t salsa64_jpeg_idct[8][8];
#define SALSA64_JPEG_ROW_SHIFT 13
#define SALSA64_JPEG_COL_SHIFT 17
#define SALSA64_JPEG_CR_R 22979  /* 1,4025 */
#define SALSA64_JPEG_CB_G 5641   /* 0,3443 */
#define SALSA64_JPEG_CR_G 11705  /* 0,7144 */
#define SALSA64_JPEG_CB_B 29047  /* 1,7729 */

/* Variante por el código del microcódigo (ucode, ucode_size bytes). */
enum salsa64_jpeg_kind salsa64_jpeg_detect(const uint8_t *ucode, uint32_t size);

/* Lee la estructura de la tarea (SALSA64_JPEG_TASK_BYTES bytes en el orden
 * de la N64). false si el modo no existe. */
bool salsa64_jpeg_task_read(const uint8_t *d, struct salsa64_jpeg_task *t);

/* Una tabla de cuantización (64 valores de 16 bits en el orden de la N64). */
void salsa64_jpeg_qtable(const uint8_t *src, int16_t q[64]);

/* Un macrobloque: `in` con SALSA64_JPEG_MB_IN(mode) bytes, `out` recibe
 * SALSA64_JPEG_MB_OUT(mode); pueden ser el mismo búfer (se leen todos los
 * bloques antes de escribir). q: las tablas de Y, U y V. */
void salsa64_jpeg_mb(uint32_t mode, const int16_t q[3][64], const uint8_t *in, uint8_t *out);

/* Corre una tarea sobre la RDRAM (en el orden de bytes de la N64).
 * data_ptr: la estructura de la tarea. */
void salsa64_jpeg_run(enum salsa64_jpeg_kind kind, uint8_t *rdram, uint32_t rdram_size, uint32_t data_ptr,
		      struct salsa64_jpeg_result *res);

#endif
