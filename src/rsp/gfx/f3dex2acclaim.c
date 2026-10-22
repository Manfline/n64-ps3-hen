/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) los autores de GLideN64 (github.com/gonetz/GLideN64), de
 * cuyos F3DEX2ACCLAIM.cpp y gSP.cpp sale este archivo; adaptación a C y a
 * la HLE de Salsa64 (C) 2026 ricowp81.
 *
 * Salsa64 — HLE de F3DEX2ACCLAIM, traducida de F3DEX2ACCLAIM.cpp,
 * gSPLightAcclaim y gSPPointLightVertexAcclaim de GLideN64.
 *
 * GLideN64 se distribuye bajo la GNU General Public License versión 2
 * (solo, sin «o posterior»). Este archivo conserva esa licencia y el aviso
 * de origen; el binario que lo enlaza se distribuye como GPLv2 (README.md,
 * docs/THIRD_PARTY.md).
 *
 * Es F3DEX2 salvo por G_MOVEMEM y por el bit geométrico 0x80. Las cuatro
 * primeras ranuras de luz ocupan 24 bytes: lookat X/Y y dos luces normales.
 * Desde el offset 96 hay ocho ranuras compactas de 16 bytes, con posición
 * s16, RGB, radio s16 y atenuación lineal u16. El bit puede sumar esas luces
 * con o sin G_LIGHTING; el RGB se satura una sola vez, después de todos los
 * aportes, y el alfa no cambia.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

enum { G_MOVEMEM = 0xDC };
enum { G_MV_VIEWPORT = 8, G_MV_LIGHT = 10, G_MV_MATRIX = 14 };

static void set_compact_light(struct gfx_state *g, unsigned slot, const uint8_t *src)
{
	unsigned i = slot - 2;
	for (unsigned k = 0; k < 3; k++) {
		g->rare.acclaim.pos[i][k] = (float)(int16_t)salsa64_rd16(src + 2 * k);
		g->lights[slot].col[k] = (float)src[6 + k];
	}
	g->rare.acclaim.ca[i] = (float)(int16_t)salsa64_rd16(src + 10);
	g->rare.acclaim.la[i] = (float)salsa64_rd16(src + 12) * (1.0f / 65536.0f);
	g->rare.acclaim.qa[i] = (float)salsa64_rd16(src + 14);
}

static void movemem(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t addr = gfx_addr(g, w1);
	const uint8_t *src;
	switch (w0 & 0xFF) {
	case G_MV_VIEWPORT:
		if ((src = gfx_io_read(g, GFX_IO_SMALL, addr, 16)) != NULL)
			gfx_set_viewport(g, src);
		break;
	case G_MV_LIGHT: {
		uint32_t offset = (w0 >> 5) & 0x7F8;
		if (offset <= 72) {
			unsigned n = offset / 24;
			if ((src = gfx_io_read(g, GFX_IO_SMALL, addr, 24)) == NULL)
				break;
			if (n < 2)
				gfx_set_lookat(g, (int)n, src);
			else
				gfx_set_light(g, (int)n - 2, src);
		} else if (offset >= 96) {
			unsigned slot = 2 + (offset - 96) / 16;
			if (slot < 10 && (src = gfx_io_read(g, GFX_IO_SMALL, addr, 16)) != NULL)
				set_compact_light(g, slot, src);
		}
		break;
	}
	case G_MV_MATRIX:
		gfx_force_matrix(g, addr);
		/* En esta variante gSPForceMatrix ocupa dos comandos. */
		g->dl_pc += 8;
		break;
	default:
		break;
	}
}

static bool acclaim_command(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	if ((w0 >> 24) != G_MOVEMEM)
		return false;
	movemem(g, w0, w1);
	return true;
}

static float absf(float x)
{
	return x < 0.0f ? -x : x;
}

void gfx_acclaim_light(const struct gfx_state *g, const struct gfx_vtx *v, float rgb[3])
{
	for (unsigned i = 0; i < 8; i++) {
		if (g->rare.acclaim.ca[i] < 0.0f)
			continue;
		float d = absf(g->rare.acclaim.pos[i][0] - v->x) +
			  absf(g->rare.acclaim.pos[i][1] - v->y) +
			  absf(g->rare.acclaim.pos[i][2] - v->z) - g->rare.acclaim.ca[i];
		if (d >= 0.0f)
			continue;
		float intensity = -d * g->rare.acclaim.la[i];
		for (unsigned k = 0; k < 3; k++)
			rgb[k] += g->lights[i + 2].col[k] * intensity;
	}
}

void gfx_dl_f3dex2acclaim(struct gfx_state *g)
{
	memset(&g->rare.acclaim, 0, sizeof g->rare.acclaim);
	gfx_dl_f3dex2(g, acclaim_command);
}
