/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — acceso a memoria en el orden de bytes de la N64 (big-endian).
 *
 * La RDRAM emulada, la memoria del RSP, la PIF RAM y la ROM del cartucho se
 * guardan tal como están en la N64. En la PS3 (big-endian) estos accesores
 * son cargas y almacenamientos directos; en un host little-endian agregan el
 * byte-swap. Nunca se byte-swapea un bloque de memoria completo.
 *
 * memcpy de tamaño fijo compila a una sola instrucción de carga o
 * almacenamiento en PPC64 y en arm64 (ambos permiten accesos no alineados;
 * el CPU emulado de todos modos solo hace accesos alineados).
 */
#ifndef SALSA64_CORE_ENDIAN_H
#define SALSA64_CORE_ENDIAN_H

#include <string.h>

#include "core/types.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SALSA64_HOST_BIG_ENDIAN 1
#define SALSA64_BE16(x) (x)
#define SALSA64_BE32(x) (x)
#define SALSA64_BE64(x) (x)
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SALSA64_HOST_BIG_ENDIAN 0
#define SALSA64_BE16(x) __builtin_bswap16(x)
#define SALSA64_BE32(x) __builtin_bswap32(x)
#define SALSA64_BE64(x) __builtin_bswap64(x)
#else
#error "orden de bytes del host desconocido"
#endif

static inline uint8_t salsa64_rd8(const uint8_t *p)
{
	return *p;
}

static inline uint16_t salsa64_rd16(const uint8_t *p)
{
	uint16_t v;
	memcpy(&v, p, sizeof v);
	return SALSA64_BE16(v);
}

static inline uint32_t salsa64_rd32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, sizeof v);
	return SALSA64_BE32(v);
}

static inline uint64_t salsa64_rd64(const uint8_t *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof v);
	return SALSA64_BE64(v);
}

static inline void salsa64_wr8(uint8_t *p, uint8_t v)
{
	*p = v;
}

static inline void salsa64_wr16(uint8_t *p, uint16_t v)
{
	v = SALSA64_BE16(v);
	memcpy(p, &v, sizeof v);
}

static inline void salsa64_wr32(uint8_t *p, uint32_t v)
{
	v = SALSA64_BE32(v);
	memcpy(p, &v, sizeof v);
}

static inline void salsa64_wr64(uint8_t *p, uint64_t v)
{
	v = SALSA64_BE64(v);
	memcpy(p, &v, sizeof v);
}

#endif
