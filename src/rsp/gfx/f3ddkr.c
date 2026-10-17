/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE del microcódigo gráfico de Diddy Kong Racing (variante de
 * Rare de Fast3D; los emuladores la llaman F3DDKR).
 *
 * Es Fast3D (f3d.c: viewport, G_DL, estado del RDP, geometry mode, fog,
 * G_TEXTURE, G_SETOTHERMODE_*, comandos del RDP) con la geometría cambiada
 * por completo, pensada para no gastar DMEM ni ciclos del RSP:
 *
 *   G_MTX (0x01)      una matriz de 4 × 4 (ya con la proyección) por DMA a
 *                     una de cuatro ranuras: la ranura sale de los bits 22 y
 *                     23 de w0 (o, en Jet Force Gemini, de los 4 bajos del
 *                     byte 2 con la multiplicación en el bit 23); la
 *                     dirección es la de G_DMA_OFFSETS más w1
 *   G_VTX (0x04)      vértices de 10 bytes por DMA (x, y, z de 16 bits y el
 *                     color RGBA en 4 bytes; sin luces: los carga ya
 *                     transformados por la matriz elegida). n − 1 en los
 *                     bits 19 a 23 de w0, el primer lugar del búfer en los
 *                     bits 9 a 13 y, con el bit 16, que siga detrás de la
 *                     carga anterior (con carteles, desde el lugar 1)
 *   G_DMATRI (0x05)   triángulos de 16 bytes por DMA: banderas (0x40: sin
 *                     recorte por caras), los tres índices y las
 *                     coordenadas de textura de cada esquina (s, t en 10.5)
 *                     dentro del triángulo, no del vértice; n − 1 en los
 *                     bits 20 a 23 de w0 y el bit 16, textura encendida
 *   G_DL_IN_MEM (0x07) llama a una lista de n comandos (n en el byte 2 de
 *                     w0, que el largo en bytes de los 16 bajos repite ×8)
 *                     que vuelve sola, sin G_ENDDL
 *   G_MOVEWORD        el índice 2 enciende los carteles (billboards: los
 *                     vértices se suman al 0, ya transformado) y el 0x0A
 *                     elige la matriz de los vértices siguientes
 *   G_DMA_OFFSETS (0xBF)  bases de las direcciones de matrices (w0) y
 *                     vértices (w1)
 *
 * El microcódigo no trae texto de identificación (su tarea tampoco declara
 * el tamaño del código): se reconoce por el CRC-32 de sus 4 KB
 * (src/core/rsp.c).
 *
 * En el SPE 0, con -Os (src/frontend/Makefile): corre solo con este juego.
 * Sin estado propio fuera de gfx_state.rare (una unión con el de Conker) ni
 * de las ranuras de matrices, que son las primeras de gfx_state.mv;
 * presupuesto de local store en src/spu/gfx/main.c.
 *
 * Fuentes (docs/THIRD_PARTY.md): mupen64plus-video-glide64mk2 (ucode05.h) y
 * mupen64plus-video-rice (RSP_GBI_Others.h), GPLv2 o posterior, y GLideN64
 * (F3DDKR.cpp, GPLv2 solo: consulta, sin copiar código) para la codificación
 * de los comandos; la codificación se verificó con las listas del juego y
 * el microcódigo real (--lle) en el host.
 */
#include <string.h>

#include "core/endian.h"
#include "core/rcp.h"
#include "rsp/gfx/hle.h"

enum {
	G_MTX = 0x01, G_VTX = 0x04, G_DMATRI = 0x05, G_DL_IN_MEM = 0x07,
	G_MOVEWORD = 0xBC, G_DMA_OFFSETS = 0xBF,
};
enum { MW_BILLBOARD = 0x02, MW_MATRIX_SELECT = 0x0A };

#define DKR_VTX_BYTES 10u
#define DKR_TRI_BYTES 16u

#define DKR(g) ((g)->rare.dkr)

static void matrix(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	/* Matrices de 64 bytes (el largo va en los 16 bajos de w0). */
	if ((w0 & 0xFFFF) != 64)
		return;
	uint32_t n = (w0 >> 16) & 0xF;
	bool mul = false;
	if (n == 0)
		n = (w0 >> 22) & 3;
	else
		mul = (w0 >> 23) & 1;
	n &= 3;
	const uint8_t *src = gfx_io_read(g, GFX_IO_MTX, (DKR(g).dma_mtx + gfx_addr(g, w1)) & 0x00FFFFFF, 64);
	if (!src) {
		g->res->dl_errors++;
		return;
	}
	float m[16];
	gfx_load_matrix(m, src);
	if (mul)
		gfx_mtx_mul(g->mv[n], m, g->mv[0]);
	else
		memcpy(g->mv[n], m, sizeof m);
	DKR(g).cur_mtx = n;
}

static void vertices(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	/* Jet Force Gemini y Mickey (F3DJFG) no suman el 1 (sin respaldo para
	 * comprobarlo: la codificación es la de GLideN64 y Glide64). */
	uint32_t n = ((w0 >> 19) & 0x1F) + (g->job->gbi == SALSA64_GBI_F3DJFG ? 0u : 1u);
	if (w0 & 0x10000) {
		if (DKR(g).billboard)
			DKR(g).vtx_last = 1;
	} else {
		DKR(g).vtx_last = 0;
	}
	uint32_t v0 = ((w0 >> 9) & 0x1F) + DKR(g).vtx_last;
	DKR(g).vtx_last += n;
	if (v0 >= GFX_MAX_VTX)
		return;
	if (n > GFX_MAX_VTX - v0)
		n = GFX_MAX_VTX - v0;
	uint32_t t0 = gfx_io_clock();
	const uint8_t *src = gfx_io_read(g, GFX_IO_VTX, (DKR(g).dma_vtx + gfx_addr(g, w1)) & 0x00FFFFFF, n * DKR_VTX_BYTES);
	if (!src) {
		g->res->dl_errors++;
		return;
	}
	/* La matriz elegida lleva ya la proyección; los carteles le suman el
	 * vértice 0 (una traslación más), y sin luces ni generación de s y t
	 * (los de DKR son del triángulo). */
	float *m = g->mvp;
	memcpy(m, g->mv[DKR(g).cur_mtx], 16 * sizeof(float));
	if (DKR(g).billboard) {
		m[12] += g->vtx[0].x;
		m[13] += g->vtx[0].y;
		m[14] += g->vtx[0].z;
		m[15] += g->vtx[0].w;
	}
	g->mvp_dirty = false;
	uint32_t geom = g->geom;
	g->geom = geom & ~(G_LIGHTING | G_TEXTURE_GEN);
	uint8_t buf[16] __attribute__((aligned(16)));
	memset(buf + 6, 0, 6);
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *p = src + DKR_VTX_BYTES * i;
		memcpy(buf, p, 6);
		memcpy(buf + 12, p + 6, 4);
		gfx_vertices(g, buf, v0 + i, 1);
	}
	g->geom = geom;
	g->res->t_vtx += gfx_io_clock() - t0;
}

static void triangles(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t n = ((w0 >> 20) & 0xF) + 1;
	bool tex = ((w0 >> 16) & 0xF) != 0;
	DKR(g).vtx_last = 0;
	if (tex != (g->tex_on != 0)) {
		g->tex_on = tex;
		g->dirty = true;
	}
	/* Las coordenadas de textura (s10.5) van tal cual al RDP: la escala de
	 * G_TEXTURE, que el juego deja en cero, no cuenta. */
	const float ts = 1.0f / 32.0f;
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, w1), n * DKR_TRI_BYTES);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	for (uint32_t i = 0; i < n; i++, p += DKR_TRI_BYTES) {
		/* Sin el 0x40 se descartan las caras de atrás: la de atrás es la
		 * "back" de siempre salvo con un viewport invertido en x. */
		uint32_t cull = (p[0] & 0x40) ? 0 : g->vscale[0] > 0.0f ? G_CULL_BACK : G_CULL_FRONT;
		uint32_t geom = (g->geom & ~(G_CULL_FRONT | G_CULL_BACK)) | G_SHADING_SMOOTH | cull;
		if (geom != g->geom)
			gfx_sp_geometry(g, geom);
		for (uint32_t k = 0; k < 3; k++) {
			uint32_t v = p[1 + k];
			if (v < GFX_MAX_VTX) {
				g->vtx[v].s = (float)(int16_t)salsa64_rd16(p + 4 + 4 * k) * ts;
				g->vtx[v].t = (float)(int16_t)salsa64_rd16(p + 6 + 4 * k) * ts;
			}
		}
		gfx_triangle(g, p[1], p[2], p[3]);
	}
}

static bool dkr_command(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	/* Una lista contada que ya dio todos sus comandos: el que se acaba de
	 * leer es de la lista que llamó (que sigue) y se vuelve a leer. */
	if (DKR(g).dl_pop) {
		DKR(g).dl_pop = 0;
		if (g->dl_sp == (int)DKR(g).dl_depth)
			g->dl_pc = g->dl_stack[--g->dl_sp];
		return true;
	}
	if (DKR(g).dl_left && --DKR(g).dl_left == 0)
		DKR(g).dl_pop = 1;
	switch (w0 >> 24) {
	case G_MTX:
		matrix(g, w0, w1);
		return true;
	case G_VTX:
		vertices(g, w0, w1);
		return true;
	case G_DMATRI:
		triangles(g, w0, w1);
		return true;
	case G_DL_IN_MEM:
		/* El conteo de una lista dentro de otra contada no se anida. */
		if (!DKR(g).dl_left && ((w0 >> 16) & 0xFF) && gfx_dl_call(g, w1, false)) {
			DKR(g).dl_left = (w0 >> 16) & 0xFF;
			DKR(g).dl_depth = (uint32_t)g->dl_sp;
		}
		return true;
	case G_DMA_OFFSETS:
		DKR(g).dma_mtx = w0 & 0x00FFFFFF;
		DKR(g).dma_vtx = w1 & 0x00FFFFFF;
		DKR(g).vtx_last = 0;
		return true;
	case G_MOVEWORD:
		if ((w0 & 0xFF) == MW_BILLBOARD) {
			DKR(g).billboard = w1 & 1;
			return true;
		}
		if ((w0 & 0xFF) == MW_MATRIX_SELECT) {
			DKR(g).cur_mtx = (w1 >> 6) & 3;
			return true;
		}
		return false;
	default:
		return false;
	}
}

void gfx_dl_f3ddkr(struct gfx_state *g)
{
	memset(&g->rare, 0, sizeof g->rare);
	gfx_dl_f3d(g, dkr_command);
}
