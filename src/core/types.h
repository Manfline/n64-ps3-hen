/*
 * Salsa64 — tipos y macros comunes del núcleo.
 *
 * Todo lo de src/core y src/cpu compila igual para la PS3 (ppu-gcc,
 * big-endian) y para el host del contenedor (gcc nativo, arm64
 * little-endian): nada de este código depende de PSL1GHT.
 */
#ifndef SALSA64_CORE_TYPES_H
#define SALSA64_CORE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SALSA64_LIKELY(x)   __builtin_expect(!!(x), 1)
#define SALSA64_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Extensión de signo de 32 a 64 bits: así guarda el VR4300 todo resultado
 * de 32 bits en sus registros de 64. */
static inline uint64_t salsa64_sx32(uint32_t v)
{
	return (uint64_t)(int64_t)(int32_t)v;
}

static inline uint64_t salsa64_sx16(uint16_t v)
{
	return (uint64_t)(int64_t)(int16_t)v;
}

static inline uint64_t salsa64_sx8(uint8_t v)
{
	return (uint64_t)(int64_t)(int8_t)v;
}

#endif
