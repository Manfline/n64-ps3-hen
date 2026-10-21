/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Solo para el final de un hilo auxiliar: ninguna llamada a libc después.
 * newlib conserva buffers de stdio y de conversión decimal en su _reent
 * TLS; sysThreadExit no los recoge. Esta variante de newlib usa __sf[3]
 * embebidos por hilo (sin _REENT_SMALL). */
#ifndef SALSA64_PS3_THREAD_CLEANUP_H
#define SALSA64_PS3_THREAD_CLEANUP_H
#include <stddef.h>
#include <reent.h>

static inline void salsa64_thread_reclaim(void)
{
	struct _reent *r = _REENT;
	/* No alterar el contexto global ni un FILE compartido que otro hilo
	 * haya puesto en stdin/out/err. Solo los tres FILE locales embebidos.
	 * fclose libera sus buffers y locks, pero no debe cerrar los
	 * descriptores 0..2 del proceso (TTY y entrada compartidas). */
	if (r == _GLOBAL_REENT)
		return;
	for (unsigned i = 0; i < 3; i++)
		if (r->__sf[i]._file >= 0 && r->__sf[i]._file <= 2)
			r->__sf[i]._close = NULL;
	_reclaim_reent(r);
}
#endif
