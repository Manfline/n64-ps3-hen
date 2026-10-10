/*
 * Salsa64 — caché de código del recompilador en un host Linux (ppc64 con
 * qemu; tools/test-rec.sh). Compartido por salsa64-run y salsa64-unit.
 *
 * Como en la PS3, la caché va pegada al programa para que el código generado
 * llame a C con `bl` directo: es un arreglo en .bss al que se le agrega
 * permiso de ejecución. Con SALSA64_REC_FAR=1 se usa un mmap lejano, que
 * obliga a llamar por mtctr/bctrl (el otro camino del emisor).
 */
#ifndef SALSA64_HOST_REC_HOST_H
#define SALSA64_HOST_REC_HOST_H

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#define SALSA64_HOST_REC_BYTES (16u << 20)

static uint8_t salsa64_host_rec_cache[SALSA64_HOST_REC_BYTES] __attribute__((aligned(65536)));

static inline void *salsa64_host_rec_buffer(void)
{
	if (!getenv("SALSA64_REC_FAR") &&
	    mprotect(salsa64_host_rec_cache, sizeof salsa64_host_rec_cache,
		     PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
		return salsa64_host_rec_cache;
	void *p = mmap(NULL, SALSA64_HOST_REC_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

#endif
