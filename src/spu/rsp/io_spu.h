/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — LLE del RSP en el SPE: E/S del intérprete (COP0 sobre el bus
 * por DMA) y la ejecución de un trabajo. Ver io_spu.c y rsp/lle/job.h.
 */
#ifndef SALSA64_SPU_RSP_IO_SPU_H
#define SALSA64_SPU_RSP_IO_SPU_H

#include "rsp/lle/job.h"
#include "rsp/lle/lle.h"

/* Ejecuta el trabajo sobre el intérprete `s` (su DMEM e IMEM son las del
 * local store) y deja el resultado. Devuelve 0. */
int lle_spu_run(struct rsp_lle *s, const struct lle_job *job, struct lle_result *res);

#endif
