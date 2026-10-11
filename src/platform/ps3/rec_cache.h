/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caché de código del recompilador en la PS3 (src/cpu/rec.h).
 *
 * 16 MB en una cola NOBITS del segmento de texto (mk/rec-cache.ld): el
 * programa que la use enlaza src/platform/ps3/rec_cache.c y agrega a su
 * Makefile `LDFLAGS := -T $(ROOT)/mk/rec-cache.ld -T lv2.ld`.
 *
 * RPCS3 no ejecuta código generado (docs/rpcs3.md): tools/rpcs3-run.py crea
 * SALSA64_NOREC_FLAG y entonces todo corre en el intérprete. El mismo
 * archivo sirve para apagar el recompilador en la PS3 real.
 */
#ifndef SALSA64_PLATFORM_PS3_REC_CACHE_H
#define SALSA64_PLATFORM_PS3_REC_CACHE_H

#include <stddef.h>
#include <sys/stat.h>

#include "platform/ps3/paths.h"

#define SALSA64_REC_CACHE_BYTES (16u << 20)
#define SALSA64_NOREC_FLAG SALSA64_DATA_DIR "/sin-recompilador"

void *salsa64_ps3_rec_cache(void);

static inline int salsa64_ps3_rec_wanted(void)
{
	struct stat st;
	return stat(SALSA64_NOREC_FLAG, &st) != 0;
}

#endif
