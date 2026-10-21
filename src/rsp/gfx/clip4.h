/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSA64_RSP_GFX_CLIP4_H
#define SALSA64_RSP_GFX_CLIP4_H

#include "rsp/gfx/hle.h"

typedef float gfx_clip4f __attribute__((vector_size(16)));
typedef uint32_t gfx_clip4u __attribute__((vector_size(16)));

#ifdef __SPU__
#include <spu_intrinsics.h>
#define GFX_CLIP4_GT(a, b) spu_cmpgt((a), (b))
#else
#define GFX_CLIP4_GT(a, b) ((gfx_clip4u)((a) > (b)))
#endif
#define GFX_CLIP4_BIT(b) ((gfx_clip4u){ (b), (b), (b), (b) })

/* Los seis bits de cada vértice; las igualdades con el plano no recortan.
 * Las máscaras SIMD evitan convertir cada comparación escalar en una rama.
 * No se cambian ni las coordenadas ni su redondeo. */
static inline gfx_clip4u gfx_clip4_mask(gfx_clip4f x, gfx_clip4f y, gfx_clip4f z, gfx_clip4f w)
{
	gfx_clip4f nw = -w, zero = { 0.0f, 0.0f, 0.0f, 0.0f };
	return (GFX_CLIP4_GT(nw, x) & GFX_CLIP4_BIT(GFX_CLIP_XN)) |
	       (GFX_CLIP4_GT(x, w) & GFX_CLIP4_BIT(GFX_CLIP_XP)) |
	       (GFX_CLIP4_GT(nw, y) & GFX_CLIP4_BIT(GFX_CLIP_YN)) |
	       (GFX_CLIP4_GT(y, w) & GFX_CLIP4_BIT(GFX_CLIP_YP)) |
	       (GFX_CLIP4_GT(z, w) & GFX_CLIP4_BIT(GFX_CLIP_ZF)) |
	       (GFX_CLIP4_GT(zero, w) & GFX_CLIP4_BIT(GFX_CLIP_W));
}

#undef GFX_CLIP4_GT
#undef GFX_CLIP4_BIT
#endif
