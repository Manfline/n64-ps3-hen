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
#include "rsp/gfx/hle.h"

void gfx_vgroup_begin(struct gfx_state *g)
{
	g->vgroups = 0;
	g->vgroup_bytes = 0;
}

/* Sin memcpy: con -Os en el SPE cada memcpy chico es una llamada, y eran
 * una por palabra del hash y otra por vértice (en Super Odyssey 64, con
 * miles de vértices por tarea, el SPE 0 tardaba 65 % más en los vértices
 * con la interpolación encendida y la emulación lo esperaba). */
static uint32_t rd32_be(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
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
	uint32_t size = (uint32_t)sizeof rec.r + 16u * n;
	/* Hasta un tercio del búfer de comandos (gfx.h): lo que no cabe se queda
	 * sin registro y sin identidad (pad = 0, como sin la bandera) y la
	 * tarea no se interpola, pero no se corta. */
	if ((g->res->flags & GFX_RES_VGROUP_FULL) || size > g->job->cmd_cap / GFX_VGROUP_SHARE - g->vgroup_bytes)
		goto drop;
	rec.r.h.op = GFX_OP_VGROUP;
	rec.r.h.size = (uint16_t)size;
	rec.r.h.a = addr;
	rec.r.h.b = n | v0 << 8 | stride << 16;
	/* El contenido (los 16 bytes de cada vértice que se acaban de leer): de
	 * respaldo de la dirección para el consumidor, sin que tenga que volver a
	 * leer la RDRAM (cuando traduce, el juego ya puede haberla cambiado). */
	uint32_t h = 0x811C9DC5u;
	for (uint32_t i = 0; i < 4 * n; i++)
		h = (h ^ rd32_be(src + 4 * i)) * 0x01000193u;
	rec.r.h.c = h ? h : 1;
	rec.r.vp[0] = g->vscale[0], rec.r.vp[1] = g->vscale[1];
	rec.r.vp[2] = g->vtrans[0], rec.r.vp[3] = g->vtrans[1];
	/* La z de salida, como gfx_vtx_out: para que el consumidor saque la del
	 * cuadro anterior (con la de ahora, el agua y la orilla, casi a la misma
	 * profundidad, se cruzaban en los intermedios). */
	if (g->ol & G_ZS_PRIM)
		rec.r.vz[0] = 0.0f, rec.r.vz[1] = (float)g->prim_z / 32767.0f;
	else
		rec.r.vz[0] = g->vscale[2] * (1.0f / 1023.0f), rec.r.vz[1] = g->vtrans[2] * (1.0f / 1023.0f);
	rec.r.vz[2] = rec.r.vz[3] = 0.0f;
	uint32_t id = GFX_VGID(g->vgroups + 1, 0u) << GFX_CLIP_ID_SHIFT;
	for (uint32_t i = 0; i < n; i++, id += 1u << GFX_CLIP_ID_SHIFT) {
		struct gfx_vtx *v = &g->vtx[v0 + i];
		rec.c[i][0] = v->x, rec.c[i][1] = v->y, rec.c[i][2] = v->z, rec.c[i][3] = v->w;
		v->clip = (v->clip & GFX_CLIP_MASK) | id;
	}
	if (gfx_io_cmd(g, &rec, size)) {
		g->vgroups++;
		g->vgroup_bytes += size;
		return;
	}
	/* Sin lugar en el búfer: sin identidad, también para que el grupo
	 * siguiente no repita la de este. */
drop:
	g->res->flags |= GFX_RES_VGROUP_FULL;
	for (uint32_t i = 0; i < n; i++)
		g->vtx[v0 + i].clip &= GFX_CLIP_MASK;
}
