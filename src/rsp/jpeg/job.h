/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — una tarea JPEG (o de video de Resident Evil 2) como trabajo de
 * lib/spukit para el SPE (src/spu/jpeg), compartido con el backend del PPE
 * (src/platform/ps3/jpeg_spu.c).
 */
#ifndef SALSA64_RSP_JPEG_JOB_H
#define SALSA64_RSP_JPEG_JOB_H

#include <stdint.h>

#define JPEG_JOB_TASK 1u

/* Errores del trabajo (spukit_result.error): con JPEG_ERR_ALIGN la tarea
 * no se tocó y la hace el PPE (el DMA quiere los macrobloques alineados a
 * 16 bytes; los de OoT lo están). */
#define JPEG_ERR_KIND  -1
#define JPEG_ERR_ALIGN -2

struct jpeg_job {
	uint64_t rdram_ea;    /* la RDRAM emulada, alineada a 16 */
	uint32_t rdram_size;
	uint32_t data_ptr;    /* la estructura de la tarea (OSTask.data_ptr; en las de video, ucode_data) */
	uint32_t kind;        /* enum salsa64_jpeg_kind */
	uint32_t dram_stack, dram_stack_size;  /* de la tarea: el RESIZE de video arranca con ellos (re2.h) */
	uint32_t pad;
};

/* Lo mismo que struct salsa64_jpeg_result, en palabras de 32 bits (el
 * mismo formato en el PPE y el SPU). */
struct jpeg_job_result {
	uint32_t macroblocks, out_addr, out_bytes, mb_stride, mb_out, bad;
	uint32_t pad[2];
};

#endif
