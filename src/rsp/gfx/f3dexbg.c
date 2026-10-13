/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE del microcódigo gráfico de Conker's Bad Fur Day
 * («F3DEXBG.NoN fifo 2.08», variante de Rare de F3DEX2; GLideN64 la llama
 * F3DEX2CBFD).
 *
 * Es F3DEX2 (f3dex2.c) salvo:
 *
 *   0x10–0x1F  G_TRI4: cuatro triángulos con índices de 5 bits; los 4 bits
 *              bajos del opcode son parte del primer índice
 *   G_MOVEWORD G_MW_NUMLIGHT cuenta luces de 48 bytes; G_MW_COORD_MOD
 *              (0x10) da desplazamiento y escala de la posición de los
 *              vértices para las luces puntuales; no hay G_MW_LIGHTCOL
 *   G_MOVEMEM  G_MV_LIGHT con luces de 48 bytes (lookat X e Y en 0 y 48,
 *              luces desde 96) que traen posición e intensidad de luz
 *              puntual; el índice 14 da la tabla de normales
 *   G_VTX      vértices de 16 bytes con el color siempre en los bytes
 *              12–15; con G_LIGHTING, la normal es X e Y de la tabla de
 *              normales (2 bytes por lugar del búfer) y Z el byte 7, y la
 *              luz multiplica el color del vértice (no lo reemplaza); un
 *              vértice con el bit 15 de sus banderas no se ilumina
 *   G_LOAD_UCODE no cambia de GBI: carga la iluminación avanzada (la luz
 *              direccional y, con G_POINT_LIGHTING, el ángulo en las
 *              puntuales) y la lista sigue
 *
 * Luz de un vértice (como GLideN64): ambiental + Σ luces puntuales
 * l = 0 … n−2, cada una con intensidad min(1, ca / (2·d²/65536)) (d: de la
 * posición del vértice, con G_MW_COORD_MOD, a la de la luz) y, en el modo
 * avanzado, + la direccional n−1 por N·L; el color del vértice se
 * multiplica por min(1, suma).
 *
 * En el SPE 0: 4,4 KB de código; su estado (luces puntuales, tabla de
 * normales, COORD_MOD) en gfx_state.rare (~250 bytes); presupuesto en
 * src/spu/gfx/main.c.
 *
 * Fuentes (docs/THIRD_PARTY.md): GLideN64 src/uCodes/F3DEX2CBFD.cpp y
 * gSPCBFDVertex, gSPLightCBFD y gSPLightVertexCBFD_* en src/gSP.cpp (GPLv2,
 * solo consulta, sin copiar código); la codificación se verificó con las
 * listas del juego en el host (índices de G_TRI4 de una malla, offsets de
 * las luces y su G_LOAD_UCODE antes del G_RDPFULLSYNC).
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

enum {
	G_VTX = 0x01, G_TRI4 = 0x10, G_TRI4_LAST = 0x1F,
	G_MOVEWORD = 0xDB, G_MOVEMEM = 0xDC, G_LOAD_UCODE = 0xDD,
};

/* G_MOVEWORD */
enum { G_MW_NUMLIGHT = 0x02, G_MW_CLIP = 0x04, G_MW_SEGMENT = 0x06, G_MW_FOG = 0x08, G_MW_COORD_MOD = 0x10 };

/* G_MOVEMEM */
enum { G_MV_VIEWPORT = 8, G_MV_LIGHT = 10, G_MV_NORMALS = 14 };

#define LIGHT_BYTES 48u

/* G_MW_COORD_MOD: el offset (w0, múltiplo de 4) elige qué se escribe: con
 * el bit 3 nada; el bit 2, el par (x, y) o (z, w); los bits 4–5, la parte
 * entera de la escala (0), su fracción (0x10; la escala queda en 12–15) o
 * el desplazamiento (0x20). */
static void coord_mod(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	float *m = g->rare.coord_mod;
	if (w0 & 8)
		return;
	uint32_t i = (w0 >> 1) & 2;
	switch (w0 & 0x30) {
	case 0x00:
		m[i] = (float)(int16_t)(w1 >> 16);
		m[i + 1] = (float)(int16_t)w1;
		break;
	case 0x10:
		m[4 + i] = (float)(w1 >> 16) * (1.0f / 65536.0f);
		m[5 + i] = (float)(w1 & 0xFFFF) * (1.0f / 65536.0f);
		m[12 + i] = m[i] + m[4 + i];
		m[13 + i] = m[i + 1] + m[5 + i];
		break;
	case 0x20:
		m[8 + i] = (float)(int16_t)(w1 >> 16);
		m[9 + i] = (float)(int16_t)w1;
		break;
	default:
		break;
	}
}

static void moveword(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	switch ((w0 >> 16) & 0xFF) {
	case G_MW_NUMLIGHT:
		gfx_num_lights(g, (int)(w1 / LIGHT_BYTES));
		break;
	case G_MW_SEGMENT:
		g->segments[((w0 & 0xFFFF) >> 2) & 15] = w1 & 0x00FFFFFF;
		break;
	case G_MW_FOG:
		g->fog_mul = (float)(int16_t)(w1 >> 16);
		g->fog_off = (float)(int16_t)(w1 & 0xFFFF);
		break;
	case G_MW_COORD_MOD:
		coord_mod(g, w0, w1);
		break;
	case G_MW_CLIP:
		gfx_clip_ratio(g, w0 & 0xFFFF, w1);
		break;
	default:
		break;  /* G_MW_PERSPNORM… */
	}
}

/* Luz de 48 bytes: color (0–2), dirección (8–10, con signo), intensidad de
 * la puntual (12, en dieciseisavos) y posición (32–37, s16). */
static void set_light(struct gfx_state *g, int index, const uint8_t *src)
{
	if (index > GFX_MAX_LIGHTS)
		return;
	gfx_set_light(g, index, src);
	g->rare.light_ca[index] = (float)src[12] * (1.0f / 16.0f);
	for (int k = 0; k < 3; k++)
		g->rare.light_pos[index][k] = (float)(int16_t)salsa64_rd16(src + 32 + 2 * k);
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
		/* Offset en la DMEM de las luces: (w0 >> 5) & 0x3FFF. */
		uint32_t slot = ((w0 >> 5) & 0x3FFF) / LIGHT_BYTES;
		uint32_t len = ((w0 >> 19) & 0x1F) * 8 + 8;
		uint32_t n = len / LIGHT_BYTES ? len / LIGHT_BYTES : 1;
		for (uint32_t i = 0; i < n; i++, slot++, addr += LIGHT_BYTES) {
			if (slot < 2) {
				if ((src = gfx_io_read(g, GFX_IO_SMALL, addr, 16)) != NULL)
					gfx_set_lookat(g, (int)slot, src);
			} else if ((src = gfx_io_read(g, GFX_IO_SMALL, addr, 40)) != NULL) {
				set_light(g, (int)slot - 2, src);
			}
		}
		break;
	}
	case G_MV_NORMALS:
		g->rare.normal_base = addr;
		break;
	default:
		break;
	}
}

static float min1(float x)
{
	return x < 1.0f ? x : 1.0f;
}

static float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* La luz de los vértices v0..v0+n−1 (ya transformados, con el color del
 * vértice) desde sus datos `src` y la tabla de normales. */
static void light_vertices(struct gfx_state *g, const uint8_t *src, uint32_t v0, uint32_t n)
{
	const struct gfx_rare *r = &g->rare;
	const float *cm = r->coord_mod;
	const uint8_t *nb = gfx_io_read(g, GFX_IO_SMALL, (r->normal_base + 2 * v0) & 0x00FFFFFF, 2 * n);
	int nl = g->num_lights;
	bool point_angle = r->advanced && (g->geom & G_POINT_LIGHTING);
	gfx_light_vectors(g);

	for (uint32_t i = 0; i < n; i++, src += 16) {
		struct gfx_vtx *v = &g->vtx[v0 + i];
		uint16_t flags = salsa64_rd16(src + 6);
		float nrm[3] = {
			(nb ? (float)(int8_t)nb[2 * i] : 0.0f) * (1.0f / 127.0f),
			(nb ? (float)(int8_t)nb[2 * i + 1] : 0.0f) * (1.0f / 127.0f),
			(float)(int8_t)flags * (1.0f / 127.0f),
		};
		if (!(flags & 0x8000)) {
			const float *amb = g->lights[nl].col;
			float rgb[3] = { amb[0], amb[1], amb[2] };
			if (r->advanced && nl > 0) {
				float k = min1(dot3(nrm, g->light_model[nl - 1]));
				for (int c = 0; c < 3 && k > 0.0f; c++)
					rgb[c] += k * g->lights[nl - 1].col[c];
			}
			float pos[3];
			for (int c = 0; c < 3; c++)
				pos[c] = ((float)(int16_t)salsa64_rd16(src + 2 * c) + cm[8 + c]) * cm[12 + c];
			for (int l = nl - 2; l >= 0; l--) {
				float d[3] = { pos[0] - r->light_pos[l][0], pos[1] - r->light_pos[l][1],
					       pos[2] - r->light_pos[l][2] };
				float len = 2.0f * dot3(d, d) * (1.0f / 65536.0f);
				float k = len > 1e-6f ? min1(r->light_ca[l] / len) : (r->light_ca[l] > 0.0f ? 1.0f : 0.0f);
				if (point_angle)
					k *= min1(dot3(nrm, g->light_model[l]));
				for (int c = 0; c < 3 && k > 0.0f; c++)
					rgb[c] += k * g->lights[l].col[c];
			}
			for (int c = 0; c < 3; c++) {
				float f = min1(rgb[c] * (1.0f / 255.0f));
				v->c[c] = (uint8_t)((float)v->c[c] * f + 0.5f);
			}
		}
		if (g->geom & G_TEXTURE_GEN)
			gfx_texgen_vertex(g, nrm, v);
	}
}

static void vertices(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
	if (n > end)
		return;
	uint32_t v0 = end - n;
	if (v0 >= GFX_MAX_VTX)
		return;
	if (n > GFX_MAX_VTX - v0)
		n = GFX_MAX_VTX - v0;
	uint32_t t0 = gfx_io_clock();
	const uint8_t *src = gfx_io_read(g, GFX_IO_VTX, gfx_addr(g, w1), n * 16);
	if (!src) {
		g->res->dl_errors++;
		return;
	}
	/* Posición, color, s, t y niebla como siempre; la luz, aparte. */
	uint32_t geom = g->geom;
	g->geom = geom & ~G_LIGHTING;
	gfx_vertices(g, src, v0, n);
#ifdef SALSA64_GFX_INTERP
	if (g->interp)
		gfx_interp_vertices(g, gfx_addr(g, w1), 16, v0, n);
#endif
	g->geom = geom;
	if (geom & G_LIGHTING)
		light_vertices(g, src, v0, n);
	g->res->t_vtx += gfx_io_clock() - t0;
}

/* Cuatro triángulos con índices de 5 bits: el primero toma los 4 bits bajos
 * del opcode, el tercero 3 bits de w0 y 2 de w1. */
static void tri4(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint32_t v[12] = {
		(w0 >> 23) & 31, (w0 >> 18) & 31, ((w0 >> 15) & 7) << 2 | w1 >> 30,
		(w0 >> 10) & 31, (w0 >> 5) & 31, w0 & 31,
		(w1 >> 25) & 31, (w1 >> 20) & 31, (w1 >> 15) & 31,
		(w1 >> 10) & 31, (w1 >> 5) & 31, w1 & 31,
	};
	for (int k = 0; k < 12; k += 3)
		if (v[k] != v[k + 1] && v[k + 1] != v[k + 2] && v[k] != v[k + 2])
			gfx_triangle(g, v[k], v[k + 1], v[k + 2]);
}

static bool cbfd_command(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t op = w0 >> 24;
	if (op >= G_TRI4 && op <= G_TRI4_LAST) {
		tri4(g, w0, w1);
		return true;
	}
	switch (op) {
	case G_VTX:
		vertices(g, w0, w1);
		return true;
	case G_MOVEWORD:
		moveword(g, w0, w1);
		return true;
	case G_MOVEMEM:
		movemem(g, w0, w1);
		return true;
	case G_LOAD_UCODE:
		g->rare.advanced = true;
		return true;
	default:
		return false;
	}
}

void gfx_dl_f3dexbg(struct gfx_state *g)
{
	memset(&g->rare, 0, sizeof g->rare);
	gfx_dl_f3dex2(g, cbfd_command);
}
