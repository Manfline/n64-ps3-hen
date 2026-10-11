/*
 * Salsa64 — HLE de las tareas JPEG del RSP (tipo 4 de OSTask).
 *
 * Zelda OoT decodifica en el CPU el JPEG del fondo de una sala
 * prerrenderizada hasta los coeficientes de cada macrobloque y manda al RSP
 * tareas que descuantizan, hacen la IDCT y convierten a RGBA5551, escribiendo
 * cada macrobloque en su mismo lugar de la RDRAM. Ver jpeg.c.
 */
#ifndef SALSA64_RSP_JPEG_H
#define SALSA64_RSP_JPEG_H

#include <stdbool.h>
#include <stdint.h>

/* Variante del microcódigo (la reconoce core/rsp.c por el CRC-32). */
enum salsa64_jpeg_kind {
	SALSA64_JPEG_NONE = 0,
	SALSA64_JPEG_OOT,     /* Zelda OoT: salida RGBA5551, sin reescalar Y/UV */
};

struct salsa64_jpeg_result {
	uint32_t macroblocks;  /* macrobloques escritos */
	uint32_t out_addr;     /* primer byte escrito (físico) */
	uint32_t out_bytes;    /* desde out_addr hasta el último escrito */
	uint32_t mb_stride;    /* bytes de cada macrobloque de entrada */
	uint32_t mb_out;       /* bytes escritos al principio de cada uno */
	bool bad;              /* parámetros fuera de la RDRAM o modo desconocido */
};

/* Variante por el código del microcódigo (ucode, ucode_size bytes). */
enum salsa64_jpeg_kind salsa64_jpeg_detect(const uint8_t *ucode, uint32_t size);

/* Corre una tarea sobre la RDRAM (en el orden de bytes de la N64).
 * data_ptr: la estructura de 32 bytes de la tarea (dirección del primer
 * macrobloque, cuántos, modo 0 = 4:2:2 o 2 = 4:2:0, y las tres tablas de
 * cuantización). */
void salsa64_jpeg_run(enum salsa64_jpeg_kind kind, uint8_t *rdram, uint32_t rdram_size, uint32_t data_ptr,
		      struct salsa64_jpeg_result *res);

#endif
