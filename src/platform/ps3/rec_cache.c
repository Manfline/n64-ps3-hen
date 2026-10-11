/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caché de código del recompilador en la PS3. Ver rec_cache.h.
 */
#include "platform/ps3/rec_cache.h"

/* "ax" + @nobits: ejecutable y sin contenido en el archivo; la ubica
 * mk/rec-cache.ld al final del segmento de texto. */
__asm__("	.section .salsa64_rec_cache,\"ax\",@nobits\n"
	"	.balign 65536\n"
	"	.globl salsa64_rec_cache_mem\n"
	"salsa64_rec_cache_mem:\n"
	"	.space 16777216\n"  /* SALSA64_REC_CACHE_BYTES */
	"	.previous\n");

extern unsigned char salsa64_rec_cache_mem[];
_Static_assert(SALSA64_REC_CACHE_BYTES == 16777216, "actualizar el .space de arriba");

void *salsa64_ps3_rec_cache(void)
{
	return salsa64_rec_cache_mem;
}
