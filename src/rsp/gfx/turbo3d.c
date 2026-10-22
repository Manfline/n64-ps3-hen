/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — listas de objetos Turbo3D.
 * Adaptación del formato y recorrido de Glide64 (turbo3D.h, 2008):
 * Copyright (c) 2002 Dave2001
 * Copyright (c) 2003-2009 Sergey 'Gonetz' Lipski
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details (LICENSE).
 *
 * RDRAM big-endian, sin estructuras superpuestas ni inversión de bytes.
 * SPE: imagen 5; 11264 bytes de vértices y 256 de validez exclusivos de
 * esta familia. No cambia gfx_state ni su estado RAW entre imágenes.
 */
#include <string.h>
#include "core/endian.h"
#include "rsp/gfx/hle.h"

#if GFX_HAS(TURBO3D)
static struct gfx_vtx vertices[256];
static uint8_t valid[256];

static bool bad(struct gfx_state *g)
{
	g->res->dl_errors++;
	g->res->flags |= GFX_RES_FOREIGN_END;
	return false;
}

static bool budget(struct gfx_state *g)
{
	if (g->res->flags & GFX_RES_CUT)
		return false;
	if (g->res->commands >= GFX_MAX_COMMANDS) {
		g->res->flags |= GFX_RES_LOOP;
		return false;
	}
	g->res->commands++;
	return true;
}

/* El terminador es un par de ceros; el primer comando también puede ser
 * TEXRECT. Sus dos palabras adicionales no son otro opcode. */
static bool rdp_list(struct gfx_state *g, uint32_t seg)
{
	if (!seg)
		return true;
	uint32_t pc = gfx_addr(g, seg), hi, lo;
	if (pc & 7)
		return bad(g);
	for (;;) {
		if (!budget(g))
			return false;
		if (!gfx_io_dl(g, pc, &hi, &lo))
			return bad(g);
		if (!(hi | lo))
			return true;
		hi |= 0xC0000000u;
		unsigned n = gfx_rdp_raw_words(hi >> 24);
		/* Validar el comando entero antes de modificar el estado. */
		for (unsigned k = 1; k < n; k++) {
			uint32_t a, b;
			if (!gfx_io_dl(g, pc + 8 * k, &a, &b))
				return bad(g);
		}
		gfx_dl_rdp_word(g, hi, lo);
		for (unsigned k = 1; k < n; k++) {
			if (!gfx_io_dl(g, pc + 8 * k, &hi, &lo))
				return bad(g);
			gfx_dl_rdp_word(g, hi, lo);
		}
		pc += 8 * n;
	}
}

static bool global_state(struct gfx_state *g, uint32_t seg)
{
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, seg), 100);
	if (!p)
		return bad(g);
	uint8_t state[100];
	memcpy(state, p, sizeof state);
	gfx_rdp_command(g, 0xEF000000u | (salsa64_rd32(state + 8) & 0xFFFFFF), salsa64_rd32(state + 12));
	for (unsigned i = 0; i < 16; i++)
		g->segments[i] = salsa64_rd32(state + 16 + 4 * i);
	gfx_set_viewport(g, state + 80);
	return rdp_list(g, salsa64_rd32(state + 96));
}

static uint32_t geometry(uint32_t m)
{
	uint32_t r = m & (G_ZBUFFER | G_SHADE | G_FOG | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_LOD | G_CLIPPING);
	if (m & 0x1000)
		r |= G_CULL_FRONT;
	if (m & 0x2000)
		r |= G_CULL_BACK;
	return r | G_SHADING_SMOOTH;
}

static bool object(struct gfx_state *g, uint32_t seg, uint32_t pvtx, uint32_t ptri)
{
	uint32_t addr = gfx_addr(g, seg);
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, addr, 24);
	if (!p)
		return bad(g);
	uint8_t state[24];
	memcpy(state, p, sizeof state);
	unsigned count = state[8], first = state[9], tris = state[10];
	if (first + count > 256)
		return bad(g);
	g->tex_tile = salsa64_rd32(state + 4) & 7;
	g->tex_on = 1;
	g->tex_s = g->tex_t = 0xFFFF;
	g->dirty = true;
	gfx_rdp_command(g, 0xEF000000u | (salsa64_rd32(state + 16) & 0xFFFFFF), salsa64_rd32(state + 20));
	gfx_sp_geometry(g, (g->geom | geometry(salsa64_rd32(state))) & ~G_LIGHTING);
	if (!(state[11] & 1)) {
		if (!gfx_io_read(g, GFX_IO_MTX, addr + 24, 64))
			return bad(g);
		gfx_force_matrix(g, addr + 24);
	}
	if (pvtx) {
		uint32_t va = gfx_addr(g, pvtx);
		for (unsigned i = 0; i < count;) {
			unsigned n = count - i;
			if (n > GFX_MAX_VTX)
				n = GFX_MAX_VTX;
			p = gfx_io_read(g, GFX_IO_VTX, va + 16 * i, 16 * n);
			if (!p)
				return bad(g);
			gfx_vertices(g, p, 0, n);
			/* Turbo3D usa s10.5 directamente, sin el truncamiento de una
			 * escala G_TEXTURE 0.16 que no existe en este formato. */
			for (unsigned k = 0; k < n; k++) {
				g->vtx[k].s = (float)(int16_t)salsa64_rd16(p + 16 * k + 8) / 32.0f;
				g->vtx[k].t = (float)(int16_t)salsa64_rd16(p + 16 * k + 10) / 32.0f;
			}
			memcpy(vertices + first + i, g->vtx, n * sizeof *vertices);
			memset(valid + first + i, 1, n);
			i += n;
		}
	}
	if (!rdp_list(g, salsa64_rd32(state + 12)))
		return false;
	if (ptri) {
		uint32_t ta = gfx_addr(g, ptri);
		for (unsigned i = 0; i < tris; i++) {
			if (!budget(g))
				return false;
			p = gfx_io_read(g, GFX_IO_SMALL, ta + 4 * i, 4);
			if (!p)
				return bad(g);
			unsigned a = p[0], b = p[1], c = p[2];
			if (!valid[a] || !valid[b] || !valid[c])
				return bad(g);
			g->vtx[0] = vertices[a];
			g->vtx[1] = vertices[b];
			g->vtx[2] = vertices[c];
			gfx_triangle(g, 0, 1, 2);
		}
	}
	return true;
}

void gfx_dl_turbo3d(struct gfx_state *g)
{
	uint32_t pc = g->dl_pc;
	memset(valid, 0, sizeof valid);
	if (pc & 7) {
		bad(g);
		return;
	}
	while (budget(g)) {
		const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, pc, 16);
		if (!p) {
			bad(g);
			return;
		}
		uint32_t pg = salsa64_rd32(p), po = salsa64_rd32(p + 4);
		uint32_t pv = salsa64_rd32(p + 8), pt = salsa64_rd32(p + 12);
		if (!po)
			return;
		if ((pg && !global_state(g, pg)) || !object(g, po, pv, pt))
			return;
		pc += 16;
	}
}
#endif
