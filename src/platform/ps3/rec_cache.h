/*
 * Salsa64 — caché de código del recompilador en la PS3 (src/cpu/rec.h).
 *
 * 24 MB en una cola NOBITS del segmento de texto (mk/rec-cache.ld): el
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

/* 32 MB es el techo: los saltos al preámbulo (±32 MB) tienen que alcanzarlo
 * desde el final (docs/rec-ppe.md, «Caché de código»). */
#define SALSA64_REC_CACHE_BYTES (32u << 20)
#define SALSA64_NOREC_FLAG SALSA64_DATA_DIR "/sin-recompilador"
/* Con este archivo, el perfil del recompilador por causa (src/cpu/rec_prof.h)
 * va al log con las estadísticas. */
#define SALSA64_RECPROF_FLAG SALSA64_DATA_DIR "/perfil"
/* Con este archivo, el recompilador se comporta como antes del 2026-10-10
 * (SALSA64_REC_OPT_LEGACY, rec.h): para medir A/B con el mismo paquete. */
#define SALSA64_RECLEGACY_FLAG SALSA64_DATA_DIR "/jit-viejo"
/* Con este archivo, la FPU como hasta el 2026-10-11 (SALSA64_REC_OPT_FPU_MEM,
 * rec.h): cada operación por cpu->fpr, sin registros residentes. */
#define SALSA64_RECFPUMEM_FLAG SALSA64_DATA_DIR "/jit-fpu-memoria"

void *salsa64_ps3_rec_cache(void);

static inline int salsa64_ps3_rec_wanted(void)
{
	struct stat st;
	return stat(SALSA64_NOREC_FLAG, &st) != 0;
}

static inline int salsa64_ps3_rec_prof_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECPROF_FLAG, &st) == 0;
}

static inline int salsa64_ps3_rec_legacy_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECLEGACY_FLAG, &st) == 0;
}

static inline int salsa64_ps3_rec_fpu_mem_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECFPUMEM_FLAG, &st) == 0;
}

/* Con este archivo, COP0 como hasta el 2026-10-11, todo por el intérprete
 * (SALSA64_REC_OPT_COP0_EXEC). */
#define SALSA64_RECCOP0EXEC_FLAG SALSA64_DATA_DIR "/jit-cop0-viejo"

static inline int salsa64_ps3_rec_cop0_exec_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECCOP0EXEC_FLAG, &st) == 0;
}

/* Con este archivo, la caché llena se vacía entera como hasta el 2026-10-11
 * en vez de desalojar el tramo más viejo (SALSA64_REC_OPT_NO_EVICT). */
#define SALSA64_RECNOEVICT_FLAG SALSA64_DATA_DIR "/jit-sin-tramos"

static inline int salsa64_ps3_rec_no_evict_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECNOEVICT_FLAG, &st) == 0;
}

#endif
