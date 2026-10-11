/*
 * Salsa64 — HLE del microcódigo gráfico de Perfect Dark (variante de Rare
 * de Fast3D; GLideN64 la llama F3DPD).
 *
 * Es Fast3D (f3d.c), con G_TRI4 en 0xB1 como el Fast3D 2.0G de GoldenEye,
 * salvo los vértices: G_VTX lee vértices de 12 bytes
 *
 *   x, y, z (s16), un byte de banderas, un byte de color, s, t (s16)
 *
 * y el color de cada uno sale de una tabla aparte: G_VTXCOLORBASE (0x07)
 * da su dirección (segmentada) y el byte de color es el offset en bytes
 * dentro de ella. Ahí hay 4 bytes R, G, B, A o, con G_LIGHTING, la normal
 * (X, Y, Z con signo) y A, igual que en el vértice de 16 bytes; así que
 * cada vértice se arma en ese formato y sigue el camino de siempre
 * (geom.c). El microcódigo no trae texto de identificación: se reconoce
 * por el CRC-32 de su código (src/core/rsp.c).
 *
 * En el SPE 0: 1,9 KB de código y nada de estado propio salvo la dirección
 * de la tabla (gfx_state.rare); presupuesto en src/spu/gfx/main.c.
 *
 * Fuentes (docs/THIRD_PARTY.md): GLideN64 src/uCodes/F3DPD.cpp y
 * gSPCIVertex en src/gSP.cpp (GPLv2, solo consulta, sin copiar código); la
 * codificación se verificó con las listas del juego en el host (largos de
 * G_VTX de 12 bytes por vértice, índices de G_TRI4 de un abanico).
 */
#include <string.h>

#include "rsp/gfx/hle.h"

enum { G_VTX = 0x04, G_VTXCOLORBASE = 0x07, G_QUAD = 0xB5 };

#define PD_VTX_BYTES 12u

static void vertices(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t v0 = (w0 >> 16) & 0xF, n = ((w0 >> 20) & 0xF) + 1;
	uint32_t t0 = gfx_io_clock();
	const uint8_t *src = gfx_io_read(g, GFX_IO_VTX, gfx_addr(g, w1), n * PD_VTX_BYTES);
	if (!src) {
		g->res->dl_errors++;
		return;
	}
	/* El tramo de la tabla que usan: menos de 260 bytes. */
	uint32_t lo = 255, hi = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t ci = src[PD_VTX_BYTES * i + 7];
		lo = ci < lo ? ci : lo;
		hi = ci > hi ? ci : hi;
	}
	const uint8_t *col = gfx_io_read(g, GFX_IO_SMALL, (g->rare.color_base + lo) & 0x00FFFFFF, hi - lo + 4);
	if (!col)
		g->res->dl_errors++;

	/* Vértices de 16 bytes: x, y, z, banderas, s, t, color (o normal). */
	uint8_t vtx[16 * 16] __attribute__((aligned(16)));
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *p = src + PD_VTX_BYTES * i;
		uint8_t *q = vtx + 16 * i;
		memcpy(q, p, 8);
		memcpy(q + 8, p + 8, 4);
		if (col)
			memcpy(q + 12, col + (p[7] - lo), 4);
		else
			memset(q + 12, 0, 4);
	}
	gfx_vertices(g, vtx, v0, n);
	g->res->t_vtx += gfx_io_clock() - t0;
}

/* Índice de vértice de un byte de comando (×10, como en F3D). */
static uint32_t vtx_index(uint32_t byte)
{
	return (byte * 205u) >> 11;
}

static bool pd_command(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	switch (w0 >> 24) {
	case G_VTX:
		vertices(g, w0, w1);
		return true;
	case G_VTXCOLORBASE:
		g->rare.color_base = gfx_addr(g, w1);
		return true;
	case G_QUAD: {
		/* En la familia de F3D de Rare el 0xB5 es un cuadrilátero (índices
		 * ×10 en los cuatro bytes de w1), no G_LINE3D. */
		uint32_t a = vtx_index(w1 >> 24), b = vtx_index((w1 >> 16) & 0xFF);
		uint32_t c = vtx_index((w1 >> 8) & 0xFF), d = vtx_index(w1 & 0xFF);
		gfx_triangle(g, a, b, c);
		gfx_triangle(g, a, c, d);
		return true;
	}
	default:
		return false;
	}
}

void gfx_dl_f3dpd(struct gfx_state *g)
{
	memset(&g->rare, 0, sizeof g->rare);
	gfx_dl_f3d(g, pd_command);
}
