/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE gráfica: lo que la interpolación de cuadros por geometría
 * pide a cada tarea con GFX_JOB_INTERP (gfx.h, docs/interpolacion.md): un
 * registro GFX_OP_VGROUP por cada G_VTX con la identidad y las coordenadas
 * de recorte de sus vértices (la de los que agrega el recorte cercano la
 * pone gfx_triangle, rdp.c). En el SPE 0 va con -Os (src/frontend/Makefile): sin la
 * bandera no hace nada, y con ella usa la pila fuera de la cadena más honda
 * (el registro se arma aquí, en G_VTX) y nada de local store aparte. El
 * contenido de cada grupo lo mira el PPE (vmatch.c), no el SPE.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

_Static_assert(offsetof(struct gfx_vtx, w) == offsetof(struct gfx_vtx, x) + 12, "x, y, z, w seguidos en gfx_vtx");

void gfx_vgroup_begin(struct gfx_state *g)
{
	g->vgroups = 0;
}

void gfx_vgroup(struct gfx_state *g, uint32_t addr, uint32_t stride, uint32_t v0, uint32_t n, const uint8_t *src)
{
	if (!(g->job->flags & GFX_JOB_INTERP) || v0 >= GFX_MAX_VTX)
		return;
	if (n > GFX_MAX_VTX - v0)
		n = GFX_MAX_VTX - v0;
	struct {
		struct gfx_vgroup r;
		float c[GFX_MAX_VTX][4];
	} rec __attribute__((aligned(16)));
	rec.r.h.op = GFX_OP_VGROUP;
	rec.r.h.size = (uint16_t)(sizeof rec.r + 16u * n);
	rec.r.h.a = addr;
	rec.r.h.b = n | v0 << 8 | stride << 16;
	/* El contenido (los 16 bytes de cada vértice que se acaban de leer): de
	 * respaldo de la dirección para el consumidor, sin que tenga que volver a
	 * leer la RDRAM (cuando traduce, el juego ya puede haberla cambiado). */
	uint32_t h = 0x811C9DC5u;
	for (uint32_t i = 0; i < 4 * n; i++)
		h = (h ^ salsa64_rd32(src + 4 * i)) * 0x01000193u;
	rec.r.h.c = h ? h : 1;
	rec.r.vp[0] = g->vscale[0], rec.r.vp[1] = g->vscale[1];
	rec.r.vp[2] = g->vtrans[0], rec.r.vp[3] = g->vtrans[1];
	uint32_t id = GFX_VGID(g->vgroups + 1, 0u) << GFX_CLIP_ID_SHIFT;
	for (uint32_t i = 0; i < n; i++, id += 1u << GFX_CLIP_ID_SHIFT) {
		struct gfx_vtx *v = &g->vtx[v0 + i];
		memcpy(rec.c[i], &v->x, sizeof rec.c[i]);  /* x, y, z, w seguidos */
		v->clip = (v->clip & GFX_CLIP_MASK) | id;
	}
	if (gfx_io_cmd(g, &rec, rec.r.h.size))  /* si no cabe, la marca de búfer lleno la pone gfx_io_cmd */
		g->vgroups++;
}
