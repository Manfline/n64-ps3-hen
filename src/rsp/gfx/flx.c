/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE gráfica: F3DFLX, la variante de F3DEX2 de F-Zero X (hle.h,
 * gfx_ucode_variant).
 *
 * Todo fuera de línea y, en el SPE 0, con -Os (src/frontend/Makefile): solo
 * corre con F-Zero X y así no suma código ni marco a la cadena más honda de
 * la pila (gfx_dl_f3dex2, begin_prim).
 */
#include "rsp/gfx/hle.h"

static bool has(const uint8_t *p, uint32_t size, const char *s)
{
	for (uint32_t i = 0; i < size; i++) {
		uint32_t k = 0;
		while (s[k] && i + k < size && p[i + k] == (uint8_t)s[k])
			k++;
		if (!s[k])
			return true;
	}
	return false;
}

unsigned gfx_ucode_variant(struct gfx_state *g, uint32_t data, uint32_t size)
{
	if (size == 0 || size > GFX_IO_MAX)
		size = GFX_IO_MAX;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, data, size);
	if (!p)
		return 0;
	/* En el nombre: "RSP Gfx ucode F3DFLX.Rej fifo 2.03F …". */
	return has(p, size, "F3DFLX") ? GFX_UCODE_FLX : 0u;
}

void gfx_flx_dma_io(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	/* gSPDma_io: bit 23 escribir a la RDRAM, DMEM en 22:13 × 8, bytes − 1
	 * en 11:0. Solo cuenta la tabla del alfa: los 256 bytes que F-Zero X
	 * sube detrás de los vértices (0x8B0); se lee de la RDRAM al cargar
	 * vértices (gfx_flx_table), sin copia en el local store. */
	if ((g->ucode_var & GFX_UCODE_FLX) && !(w0 & 0x800000u) && (w0 & 0xFFF) + 1 >= 256)
		g->flx_table = w1;  /* F-Zero X: 0x800CE3D0 */
}

const uint8_t *gfx_flx_table(struct gfx_state *g)
{
	bool on = (g->ucode_var & GFX_UCODE_FLX) && (g->geom & G_TEXTURE_GEN) && g->flx_table;
	return on ? gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, g->flx_table), 256) : NULL;
}

bool gfx_flx_shade_alpha(const struct gfx_state *g)
{
	/* F-Zero X mezcla el alfa de la tabla con el color de niebla (casi
	 * blanco) en el primer ciclo del blender, sin G_FOG: los brillos. */
	return (g->ucode_var & GFX_UCODE_FLX) && (g->geom & (G_LIGHTING | G_TEXTURE_GEN)) == (G_LIGHTING | G_TEXTURE_GEN);
}

uint8_t gfx_flx_alpha(const struct gfx_state *g, const uint8_t *table, const uint8_t *nrm)
{
	/* El producto con la luz del alfa, × 128 hacia abajo desde el centro
	 * de la tabla. Como el microcódigo: la normal tal cual (s8, sin
	 * normalizar: las del auto miden ~120) y la luz con el módulo que le
	 * deja su normalización (1,0055, medido en la DMEM con la LLE). */
	const float *d = g->lookat_model[1];
	float dot = ((float)(int8_t)nrm[0] * d[0] + (float)(int8_t)nrm[1] * d[1] + (float)(int8_t)nrm[2] * d[2]) * 1.0055f;
	int i = (int)(dot + 129.0f) - 1;  /* 128 + floor(dot), sin floorf */
	return table[i < 0 ? 0 : i > 255 ? 255 : i];
}
