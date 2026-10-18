/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) los autores de GLideN64 (github.com/gonetz/GLideN64), de
 * cuyo src/uCodes/F5Rogue.cpp sale este archivo; adaptación a C y a la HLE
 * de Salsa64 (C) 2026 ricowp81.
 *
 * Salsa64 — HLE del microcódigo gráfico de Star Wars: Rogue Squadron
 * (Factor 5), traducida a C desde GLideN64.
 *
 * Basado en src/uCodes/F5Rogue.cpp de GLideN64 (https://github.com/gonetz/GLideN64),
 * «Star Wars Rogue Squadron ucode — initial implementation ported from
 * Lemmy's LemNemu plugin; microcode decoding: olivieryuyu». GLideN64 se
 * distribuye bajo la GNU General Public License versión 2 (solo, sin «o
 * posterior»): este archivo conserva esa licencia y el aviso de arriba; el
 * binario que lo enlaza se distribuye como GPLv2 (README.md,
 * docs/THIRD_PARTY.md).
 *
 * Qué es el microcódigo: una variante de Fast3D con lista de comandos de
 * longitud variable (los datos de un comando van en la propia lista, tras
 * su primera palabra) y lo propio de este juego:
 *
 *   0x01 G_MTX        el de F3D; recuerda la dirección de la proyección
 *   0x02              base de la tabla de colores de los vértices (w1)
 *   0x03 G_MOVEMEM    24 bytes: el viewport (+8, 16 bytes) o, con el
 *                     índice 0x82, la escala de s y t de los vértices
 *                     (palabras en +12 y +16)
 *   0x04 G_VTX        n vértices (bits 10–15 de w0) de 8 bytes (x, y, z
 *                     de 16 bits y una palabra que no se usa): se cargan
 *                     siempre desde el lugar 0 del búfer
 *   0x05 G_TRIGEN     40 bytes: genera mallas de relieve (rejillas de 5 × 5
 *                     o de 3 × 3 vértices con alturas de 8 bits, colores y
 *                     coordenadas de textura que el microcódigo calcula) y
 *                     un cuadrilátero; los triángulos salen ordenados por
 *                     profundidad media, de atrás adelante
 *   0x06/0x07 G_DL, G_BRANCHDL y 0xB5 (salto a la lista de software): cada
 *                     lista empieza con una palabra de 8 bytes cuyos 24
 *                     bits bajos dicen a dónde salta el 0xB5
 *   0xB4/0xBF G_TRI2, G_TRI1  índices de vértice ×5 (bytes de w1); los
 *                     colores salen de la tabla (un índice por vértice en
 *                     la palabra siguiente) y, con el bit 1 de w0, las
 *                     coordenadas de textura (4 bytes por vértice) en 16
 *                     bytes más; la lista de triángulos seguidos se dibuja
 *                     junta
 *   0xBC G_MOVEWORD   recorte, segmentos y niebla (la de este juego en
 *                     16.16)
 *   0xBD              rectángulo de textura generado desde un vértice
 *                     (carteles y brillos de los objetos): calcula el
 *                     tamaño con el viewport y la matriz de proyección
 *   0xB3 y 0xBE       othermode L y H con una máscara (la palabra de
 *                     8 bytes siguiente) y los bits nuevos (w1)
 *
 * Lo demás (G_TEXTURE, G_SETGEOMETRYMODE y sus pares, G_SETOTHERMODE_L/H,
 * G_ENDDL, el RDP) es el de Fast3D (f3d.c).
 *
 * Cambios respecto a GLideN64 (medidos contra el microcódigo real corriendo
 * en el RSP a bajo nivel, salsa64-run --lle=all; docs/estado.md,
 * 2026-10-18):
 *   - GLideN64 niega la y de los vértices tras transformarlos porque da vuelta
 *     el signo de la escala y del viewport; aquí el viewport conserva su
 *     signo, así que se invierte la y (y el recorte en y) de cada vértice.
 *   - La opción 0x02 de la rejilla de 3 × 3 mezcla en GLideN64 hacia la altura
 *     9, que esa rejilla no usa; se mezcla hacia la 7 (la 6 en índice
 *     «crudo»), como su ponderación y su color.
 *   - Las divisiones de 64 bits del rectángulo generado (0xBD) van en
 *     flotante: el SPE no trae la aritmética de 64 bits (libgcc, 5,3 KB) y la
 *     diferencia es de un cuarto de píxel en los bordes.
 *   - Tamaños acotados (el SPE no asigna memoria): las alturas y los colores de
 *     una malla se guardan en arreglos fijos y los triángulos en una lista de 40.
 *   - Las direcciones de la lista y de los datos se leen por la E/S de la HLE
 *     (gfx_io_read / gfx_io_dl), no de la RDRAM entera.
 *
 * En el SPE 0: una imagen propia («Factor 5», spe0.h, GFX_PART_F5ROGUE, con
 * lo común de F3D: f3d.c da G_TEXTURE, el modo geométrico, G_SETOTHERMODE y los
 * comandos del RDP); con -Os son ~14 KB de código y datos, y la imagen deja
 * 3,4 KB de holgura de pila (docs/spe0-familias.md). Su estado vive en
 * gfx_state.rare.f5 (68 bytes, en la unión de Conker y DKR, a cero en cada
 * tarea); presupuesto de local store en src/spu/gfx/main.c.
 */
#include <string.h>

#include "core/endian.h"
#include "core/rcp.h"
#include "rsp/gfx/hle.h"

enum {
	G_MTX = 0x01, G_VTXCOLOR = 0x02, G_MOVEMEM = 0x03, G_VTX = 0x04, G_TRIGEN = 0x05, G_DL = 0x06,
	G_BRANCHDL = 0x07, G_RDPSETOTHERMODE_L_EX = 0xB3, G_TRI2 = 0xB4, G_JUMPSWDL = 0xB5, G_MOVEWORD = 0xBC,
	G_TEXRECT_GEN = 0xBD, G_SETOTHERMODE_H_EX = 0xBE, G_TRI1 = 0xBF,
};
enum { MV_VIEWPORT = 0x80, MV_TEXSCALE = 0x82 };
enum { MW_CLIP = 0x04, MW_SEGMENT = 0x06, MW_FOG_MULTIPLIER = 0x08, MW_FOG_OFFSET = 0x0A, MW_PERSPNORM = 0x0E };

#define F5(g) ((g)->rare.f5)
#define F5_DL_DEPTH 10u  /* la pila de listas del microcódigo (GBI.PCStackSize) */

/* Triángulo de la malla generada: tres vértices y la z media de pantalla. */
struct f5_tri {
	uint8_t v[3];
	int16_t z;
};

/* ---- Vértices ---------------------------------------------------------- */

/* Un vértice de 8 bytes (x, y, z de 16 bits) transformado en el lugar `i`:
 * sin luces ni generación de s y t, y con el color en cero (después lo
 * pone el comando que lo usa). */
static void load_vertex(struct gfx_state *g, uint32_t i, int16_t x, int16_t y, int16_t z)
{
	uint8_t buf[16] __attribute__((aligned(16)));
	memset(buf, 0, sizeof buf);
	salsa64_wr16(buf + 0, (uint16_t)x);
	salsa64_wr16(buf + 2, (uint16_t)y);
	salsa64_wr16(buf + 4, (uint16_t)z);
	uint32_t geom = g->geom;
	g->geom = geom & ~(G_LIGHTING | G_TEXTURE_GEN);
	gfx_vertices(g, buf, i, 1);
	g->geom = geom;
	/* El microcódigo da la y de pantalla al revés de Fast3D (GLideN64 la
	 * niega tras transformar): se invierte y, con ella, el recorte en y. */
	struct gfx_vtx *v = &g->vtx[i];
	v->y = -v->y;
	v->sy = 2.0f * g->vtrans[1] - v->sy;
	uint32_t clip = v->clip, yc = clip & (GFX_CLIP_YN | GFX_CLIP_YP);
	if (yc == GFX_CLIP_YN || yc == GFX_CLIP_YP)
		v->clip = clip ^ (GFX_CLIP_YN | GFX_CLIP_YP);
}

static void cmd_vtx(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t n = (w0 >> 10) & 0x3F;
	if (n == 0)
		return;
	uint32_t t0 = gfx_io_clock();
	const uint8_t *src = gfx_io_read(g, GFX_IO_VTX, gfx_addr(g, w1), n * 8);
	if (!src) {
		g->res->dl_errors++;
		return;
	}
	for (uint32_t i = 0; i < n; i++, src += 8)
		load_vertex(g, i, (int16_t)salsa64_rd16(src), (int16_t)salsa64_rd16(src + 2), (int16_t)salsa64_rd16(src + 4));
	g->res->t_vtx += gfx_io_clock() - t0;
}

/* Color RGBA (palabra) y coordenadas de textura de un vértice. La escala
 * de las coordenadas (G_MOVEMEM 0x82) la hace el producto de 16 bits del
 * microcódigo: s·escala_baja_de_0 + s·escala_de_1, con sus acarreos. */
static void set_vertex_color(struct gfx_vtx *v, uint32_t c)
{
	v->c[0] = (uint8_t)(c >> 24);
	v->c[1] = (uint8_t)(c >> 16);
	v->c[2] = (uint8_t)(c >> 8);
	v->c[3] = (uint8_t)c;
}

static void set_vertex_st(struct gfx_state *g, struct gfx_vtx *v, uint32_t st, bool persp)
{
	const uint32_t sscale = F5(g).tex_scale[0] >> 16, tscale = F5(g).tex_scale[0] & 0xFFFF;
	const uint32_t sscale1 = F5(g).tex_scale[1] >> 16, tscale1 = F5(g).tex_scale[1] & 0xFFFF;
	const uint32_t s = (uint32_t)(int32_t)(int16_t)(st >> 16), t = (uint32_t)(int32_t)(int16_t)st;
	/* VMUDN y VMADH del microcódigo: la mitad alta del producto con la
	 * escala baja más la mitad baja del producto con la alta. */
	const int16_t scaled_s = (int16_t)(((s * sscale) >> 16) + (s * sscale1));
	const int16_t scaled_t = (int16_t)(((t * tscale) >> 16) + (t * tscale1));
	const float k = persp ? 1.0f / 32.0f : 1.0f / 16.0f;
	v->s = (float)scaled_s * k;
	v->t = (float)scaled_t * k;
}

/* La niebla del vértice (como gfx_vertices) con el z ya transformado: el
 * alfa del sombreado es el factor de niebla. */
static void apply_fog_alpha(struct gfx_vtx *v, uint8_t alpha)
{
	v->c[3] = alpha;
}

/* ---- G_TRI1 y G_TRI2 --------------------------------------------------- */

/* El mismo triángulo con el vértice que da el color del sombreado plano
 * (índice `flag` de la terna) primero. */
static void draw_tri(struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c, uint32_t flag)
{
	switch (flag % 3) {
	case 1:
		gfx_triangle(g, b, c, a);
		break;
	case 2:
		gfx_triangle(g, c, a, b);
		break;
	default:
		gfx_triangle(g, a, b, c);
		break;
	}
}

static void cmd_tri(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t ca, bool tri2)
{
	/* Índices de vértice: byte / 5 (el microcódigo los da como offsets de 8 bytes en una DMEM de 40 por vértice). */
	const uint32_t v[4] = { ((w1 >> 16) & 0xFF) / 5, ((w1 >> 8) & 0xFF) / 5, (w1 & 0xFF) / 5, (w1 >> 24) / 5 };
	const uint32_t n = tri2 ? 4 : 3;
	const bool tex = (w0 & 2) != 0;
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, ca + 8, tex ? 24 : 8);
	if (!p) {
		g->res->dl_errors++;
		g->dl_pc = ca + 16;
		return;
	}
	const uint32_t color_param = salsa64_rd32(p);
	/* Sus cuatro índices (de los bytes de la palabra) en la tabla de colores. */
	const uint32_t cidx[4] = { (color_param >> 16) & 0xFF, (color_param >> 8) & 0xFF, color_param & 0xFF,
				   color_param >> 24 };
	uint32_t st[4] = { 0, 0, 0, 0 };
	if (tex)
		for (uint32_t i = 0; i < n; i++)
			st[i] = salsa64_rd32(p + 8 + 4 * i);
	g->dl_pc = ca + (tex ? 32 : 16);

	/* Los colores: de la tabla, un tramo desde el índice más bajo hasta el más alto + 4. */
	uint32_t lo = 255, hi = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (cidx[i] < lo)
			lo = cidx[i];
		if (cidx[i] > hi)
			hi = cidx[i];
	}
	uint32_t col[4];
	const uint8_t *tab = gfx_io_read(g, GFX_IO_VTX, (F5(g).color_base + lo) & 0x00FFFFFF, hi - lo + 4);
	if (!tab) {
		g->res->dl_errors++;
		return;
	}
	for (uint32_t i = 0; i < n; i++)
		col[i] = salsa64_rd32(tab + cidx[i] - lo);

	const bool persp = ((g->oh >> G_MDSFT_TEXTPERSP) & 1) != 0;
	for (uint32_t i = 0; i < n; i++) {
		if (v[i] >= GFX_MAX_VTX)
			return;
		struct gfx_vtx *vt = &g->vtx[v[i]];
		uint8_t fog = vt->c[3];
		set_vertex_color(vt, col[i]);
		if (g->geom & G_FOG)
			apply_fog_alpha(vt, fog);
		if (tex)
			set_vertex_st(g, vt, st[i], persp);
	}

	/* Con culling de las dos caras, solo se descarta la de atrás. */
	const uint32_t geom = g->geom;
	if ((geom & (G_CULL_FRONT | G_CULL_BACK)) == (G_CULL_FRONT | G_CULL_BACK))
		gfx_sp_geometry(g, geom & ~G_CULL_FRONT);
	const uint32_t flag = w1 >> 24;
	draw_tri(g, v[0], v[1], v[2], flag);
	if (tri2) {
		/* El segundo triángulo es (v1, v3, v4). */
		draw_tri(g, v[0], v[2], v[3], flag);
	}
	if (g->geom != geom)
		gfx_sp_geometry(g, geom);
}

/* ---- G_TRIGEN ---------------------------------------------------------- */

/* El comando (40 bytes) genera una malla de relieve: una rejilla de 5 × 5 vértices
 * (modo 0, subcódigo 0: 25 alturas seguidas) o de 3 × 3 (subcódigo 1: tres alturas
 * cada 10 bytes), o un cuadrilátero (modo 2). Cada vértice trae una altura de 8 bits
 * (× 16), un color y las coordenadas de textura que genera el microcódigo; la
 * palabra 1 enciende «opciones» que reparten la malla en más triángulos y mezclan
 * vértices y colores, y las palabras 5 a 7 traen pesos de 16 bits con que se
 * mezclan (GLideN64 las llama ponderaciones). Los índices de las alturas son
 * «crudos»: la altura natural n vive en la posición n ^ 1 (el microcódigo las guarda
 * en pares de media palabra); los colores y los vértices van por su índice natural,
 * y el color de una mezcla es el de los mismos índices naturales. */

/* Suma de dos vectores de cuatro u16 (y, x, bandera, z) elemento a elemento. */
struct vec4 {
	uint16_t e[4];
};

static struct vec4 vadd(struct vec4 a, struct vec4 b)
{
	for (int i = 0; i < 4; i++)
		a.e[i] = (uint16_t)(a.e[i] + b.e[i]);
	return a;
}

#define TG_CAP 16u    /* alturas y colores que se guardan de la rejilla de 3 × 3 */
#define TG_TRIS 40u   /* 32 de la rejilla y 2 más por cada una de las cuatro opciones */

struct trigen {
	uint32_t p[10];            /* los 40 bytes del comando */
	uint16_t vd[26];           /* alturas × 16, en el orden «crudo» */
	uint32_t cd[25];           /* colores */
	struct f5_tri tri[TG_TRIS];
	uint32_t ntri;
};

static void add_tri(struct trigen *t, uint8_t a, uint8_t b, uint8_t c)
{
	if (t->ntri < TG_TRIS) {
		t->tri[t->ntri].v[0] = a;
		t->tri[t->ntri].v[1] = b;
		t->tri[t->ntri].v[2] = c;
		t->ntri++;
	}
}

/* Media de dos vértices (y de sus colores, byte a byte) en el lugar de un tercero. */
static void mix_avg(struct trigen *t, uint8_t s1, uint8_t s2, uint8_t d)
{
	t->vd[d] = (uint16_t)(((int16_t)t->vd[s1] + (int16_t)t->vd[s2]) / 2);
	uint32_t a = t->cd[s1 ^ 1], b = t->cd[s2 ^ 1], r = 0;
	for (int i = 0; i < 32; i += 8)
		r |= (((((a >> i) & 0xFF) + ((b >> i) & 0xFF)) >> 1) & 0xFF) << i;
	t->cd[d ^ 1] = r;
}

/* Mezcla ponderada: lo que había en el destino por (1 − peso) más la media de los dos por el peso. */
static void mix_weight(struct trigen *t, uint8_t s1, uint8_t s2, uint8_t d, uint16_t w)
{
	uint32_t v = (uint32_t)((int32_t)(int16_t)t->vd[d] * (int32_t)(0x10000u - w));
	uint32_t sum = (uint32_t)((int32_t)(int16_t)t->vd[s1] + (int32_t)(int16_t)t->vd[s2]) * w;
	t->vd[d] = (uint16_t)((v + (uint32_t)((int32_t)sum >> 1)) >> 16);
	uint32_t a = t->cd[s1 ^ 1], b = t->cd[s2 ^ 1], dd = t->cd[d ^ 1], r = 0;
	for (int i = 0; i < 32; i += 8) {
		uint32_t c = ((dd >> i) & 0xFF) * (0x10000u - w) + (((((a >> i) & 0xFF) + ((b >> i) & 0xFF)) * w) >> 1);
		r |= ((c >> 16) & 0xFF) << i;
	}
	t->cd[d ^ 1] = r;
}

/* Una opción y su ponderación comparten los vértices a mezclar (origen 1, origen 2, destino:
 * índices crudos de las alturas; los de los colores son los mismos con el bit 0 invertido).
 * La opción, si la palabra 1 tiene `val` bajo `mask`, los promedia y añade triángulos. */
struct tg_group {
	uint32_t mask, val;
	uint8_t nv, nt;
	uint8_t v[8][3];
	uint8_t tri[2][3];
};

/* De dónde sale el peso de cada grupo (palabra del comando y desplazamiento). */
static const uint8_t tg_pw_word[5] = { 6, 7, 5, 6, 5 };
static const uint8_t tg_pw_shift[5] = { 0, 16, 0, 16, 16 };

static const struct tg_group tg0_groups[5] = {
	{ 0x0000FF00u, 0x00000100u, 2, 2, { { 1, 3, 0 }, { 3, 5, 2 } }, { { 0, 10, 5 }, { 10, 20, 15 } } },
	{ 0x000000FFu, 0x00000001u, 2, 2, { { 21, 23, 20 }, { 23, 25, 22 } }, { { 4, 9, 14 }, { 14, 19, 24 } } },
	{ 0xFF000000u, 0x01000000u, 2, 2, { { 1, 11, 4 }, { 11, 21, 14 } }, { { 0, 1, 2 }, { 2, 3, 4 } } },
	{ 0x00FF0000u, 0x00010000u, 2, 2, { { 5, 15, 8 }, { 15, 25, 18 } }, { { 22, 24, 23 }, { 20, 22, 21 } } },
	{ 0, 0, 8, 0,
	  { { 1, 13, 7 }, { 3, 13, 6 }, { 3, 15, 9 }, { 11, 13, 10 }, { 13, 15, 12 }, { 11, 23, 17 }, { 13, 23, 16 },
	    { 13, 25, 19 } }, { { 0, 0, 0 } } },
};

/* GLideN64 pone en la opción 0x02 de la rejilla de 3 × 3 el destino 8 (la altura 9, que no
 * existe en ella): aquí el 6, que es el de su ponderación y el que cuadra con el color. */
static const struct tg_group tg1_groups[5] = {
	{ 0x0000FF00u, 0x00000200u, 1, 1, { { 1, 3, 0 } }, { { 0, 6, 3 } } },
	{ 0x000000FFu, 0x00000002u, 1, 1, { { 7, 9, 6 } }, { { 2, 5, 8 } } },
	{ 0xFF000000u, 0x02000000u, 1, 1, { { 1, 7, 2 } }, { { 0, 1, 2 } } },
	{ 0x00FF0000u, 0x00020000u, 1, 1, { { 3, 9, 4 } }, { { 6, 8, 7 } } },
	{ 0, 0, 1, 0, { { 1, 9, 5 } }, { { 0, 0, 0 } } },
};

static void tg_options(struct trigen *t, const struct tg_group *grp)
{
	for (unsigned k = 0; k < 5; k++) {
		const struct tg_group *g = &grp[k];
		if (g->nt && (t->p[1] & g->mask) == g->val) {
			for (unsigned i = 0; i < g->nv; i++)
				mix_avg(t, g->v[i][0], g->v[i][1], g->v[i][2]);
			for (unsigned i = 0; i < g->nt; i++)
				add_tri(t, g->tri[i][0], g->tri[i][1], g->tri[i][2]);
		}
		uint16_t w = (uint16_t)(t->p[tg_pw_word[k]] >> tg_pw_shift[k]);
		if (w)
			for (unsigned i = 0; i < g->nv; i++)
				mix_weight(t, g->v[i][0], g->v[i][1], g->v[i][2], w);
	}
}

/* Las alturas: bytes con signo × 16 desde la dirección de la palabra 2 (con el desplazamiento de
 * sus 3 bits bajos): 25 seguidos, o (rejilla de 3 × 3) tres por cada 10 bytes. */
static bool tg_read_heights(struct gfx_state *g, struct trigen *t, bool strided)
{
	const uint32_t base = t->p[2] & 0x00FFFFF8u, off = t->p[2] & 7u;
	if (!strided) {
		const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, base + off, 25);
		if (!s)
			return false;
		for (uint32_t i = 0; i < 25; i++)
			t->vd[i ^ 1] = (uint16_t)((int16_t)((int8_t)s[i]) * 16);
		return true;
	}
	const uint32_t size = ((t->p[4] >> 16) + 0x16) & 0xFF0u;
	const uint32_t need = 8 + 12 * 10;
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, base, need);
	if (!s)
		return false;
	uint32_t idx8 = off, di = 0;
	do {
		for (uint32_t i = 0; i < 3; i++, di++, idx8 += 2)
			if (di < TG_CAP && idx8 < need)
				t->vd[di ^ 1] = (uint16_t)((int16_t)((int8_t)s[idx8]) * 16);
		idx8 += 4;
	} while (size > idx8 && idx8 < need);
	return true;
}

/* Los colores: 25 palabras seguidas, o tres de cada 10 (índice de palabra). */
static bool tg_read_colors(struct gfx_state *g, struct trigen *t, bool strided)
{
	if (!strided) {
		const uint8_t *s = gfx_io_read(g, GFX_IO_VTX, t->p[3] & 0x00FFFFFFu, 100);
		if (!s)
			return false;
		for (uint32_t i = 0; i < 25; i++)
			t->cd[i] = salsa64_rd32(s + 4 * i);
		return true;
	}
	const uint32_t end = ((t->p[4] & 0xFFFF) + 0x13) & 0xFF0u, off = t->p[3] & 7u;
	if (end < off)
		return false;
	const uint32_t size = (end - off) / 4;
	const uint8_t *s = gfx_io_read(g, GFX_IO_VTX, (t->p[3] & 0x00FFFFF8u) + off, 4 * 40);
	if (!s)
		return false;
	uint32_t idx = 0, n = 0;
	do {
		for (int k = 0; k < 3; k++, n++, idx += 2)
			if (n < TG_CAP && idx < 40)
				t->cd[n] = salsa64_rd32(s + 4 * idx);
		idx += 4;
	} while (size > idx && idx + 5 <= 40);
	return true;
}

/* Z media de un triángulo, como la del microcódigo (z de pantalla × 32768 / 1024). */
static int16_t tri_z(const struct gfx_state *g, const struct f5_tri *t)
{
	int32_t sum = 0;
	for (int i = 0; i < 3; i++)
		sum += (int32_t)(g->vtx[t->v[i]].sz * 32.0f);
	return (int16_t)(sum / 3);
}

/* Dibuja los triángulos (los de la opción y los de la rejilla) de atrás adelante, en
 * orden estable. */
static void tg_draw(struct gfx_state *g, struct trigen *t, const uint8_t (*def)[3], uint32_t ndef)
{
	for (uint32_t i = 0; i < ndef; i++)
		add_tri(t, def[i][0], def[i][1], def[i][2]);
	for (uint32_t i = 0; i < t->ntri; i++)
		t->tri[i].z = tri_z(g, &t->tri[i]);
	for (uint32_t i = 1; i < t->ntri; i++) {
		struct f5_tri x = t->tri[i];
		uint32_t j = i;
		while (j > 0 && t->tri[j - 1].z < x.z) {
			t->tri[j] = t->tri[j - 1];
			j--;
		}
		t->tri[j] = x;
	}
	const uint32_t geom = g->geom;
	if ((geom & (G_CULL_FRONT | G_CULL_BACK)) == (G_CULL_FRONT | G_CULL_BACK))
		gfx_sp_geometry(g, geom & ~G_CULL_FRONT);
	for (uint32_t i = 0; i < t->ntri; i++)
		gfx_triangle(g, t->tri[i].v[0], t->tri[i].v[1], t->tri[i].v[2]);
	if (g->geom != geom)
		gfx_sp_geometry(g, geom);
}

static const uint8_t tg0_order[32][3] = {
	{ 0, 6, 1 },   { 0, 5, 6 },   { 1, 7, 2 },   { 1, 6, 7 },   { 2, 8, 3 },   { 5, 11, 6 },  { 2, 7, 8 },
	{ 5, 10, 11 }, { 3, 9, 4 },   { 3, 8, 9 },   { 6, 12, 7 },  { 6, 11, 12 }, { 10, 16, 11 }, { 7, 13, 8 },
	{ 10, 15, 16 }, { 7, 12, 13 }, { 8, 14, 9 },  { 11, 17, 12 }, { 8, 13, 14 }, { 11, 16, 17 }, { 15, 21, 16 },
	{ 12, 18, 13 }, { 15, 20, 21 }, { 12, 17, 18 }, { 13, 19, 14 }, { 16, 22, 17 }, { 16, 21, 22 }, { 13, 18, 19 },
	{ 17, 23, 18 }, { 17, 22, 23 }, { 18, 24, 19 }, { 18, 23, 24 },
};

static const uint8_t tg1_order[8][3] = {
	{ 0, 3, 4 }, { 0, 4, 1 }, { 1, 4, 5 }, { 1, 5, 2 }, { 3, 6, 7 }, { 3, 7, 4 }, { 4, 7, 8 }, { 4, 8, 5 },
};

/* Los step × step vértices: x, y, z de la esquina más pasos de la palabra 9 (y la altura), con
 * sus colores y las coordenadas de textura que genera el microcódigo (s en la columna, t al revés
 * en la fila). */
static void tg_grid(struct gfx_state *g, struct trigen *t, uint32_t step)
{
	const uint32_t *p = t->p;
	const uint16_t V0 = (uint16_t)p[9];
	const struct vec4 v0 = { { (uint16_t)((p[8] & 0xFFFF) << 4), (uint16_t)(p[8] >> 16), 0, (uint16_t)(p[9] >> 16) } };
	const struct vec4 v2 = { { 0, V0, 0, 0 } }, v3 = { { 0, 0, 0, V0 } };
	struct vec4 v1 = { { 0, 0, 0, 0 } };
	const uint32_t bound = step * (step - 1) + 1;
	uint32_t n = 0;
	for (uint32_t start = 0; start < step; start++) {
		for (uint32_t idx = 0; idx < bound; idx += step) {
			v1.e[0] = t->vd[(start + idx) ^ 1];
			struct vec4 v4 = vadd(v1, v0);
			load_vertex(g, n++, (int16_t)v4.e[1], (int16_t)v4.e[0], (int16_t)v4.e[3]);
			v1 = vadd(v1, v3);
		}
		v1 = vadd(v1, v2);
		v1.e[3] = 0;
	}
	const uint16_t tex = (uint16_t)p[7];
	for (uint32_t i = 0; i < step; i++)
		for (uint32_t j = 0; j < step; j++) {
			struct gfx_vtx *vt = &g->vtx[i * step + j];
			uint8_t fog = vt->c[3];
			set_vertex_color(vt, t->cd[i + j * step]);
			if (g->geom & G_FOG)
				apply_fog_alpha(vt, fog);
			set_vertex_st(g, vt, ((uint32_t)(uint16_t)(tex * i) << 16) | (uint16_t)(tex * (step - 1 - j)), false);
		}
}

/* Rejilla de 5 × 5 o de 3 × 3. */
static __attribute__((noinline)) void tg_mesh(struct gfx_state *g, struct trigen *t, bool three)
{
	const uint32_t step = three ? 3 : 5;
	if (!tg_read_colors(g, t, three) || !tg_read_heights(g, t, three))
		return;
	tg_options(t, three ? tg1_groups : tg0_groups);
	tg_grid(g, t, step);
	if (three)
		tg_draw(g, t, tg1_order, 8);
	else
		tg_draw(g, t, tg0_order, 32);
}

/* Cuadrilátero (modo 2): cuatro alturas y cuatro colores en el propio comando; dos triángulos. */
static __attribute__((noinline)) void tg_quad(struct gfx_state *g, const uint32_t *p)
{
	const uint16_t V0 = (uint16_t)p[9];
	const uint16_t V1 = (uint16_t)((p[8] & 0xFFFF) << 4);
	const uint16_t ox = (uint16_t)(p[8] >> 16), oz = (uint16_t)(p[9] >> 16);
	const uint16_t hy[4] = { (uint16_t)(p[1] >> 16), (uint16_t)p[1], (uint16_t)(p[2] >> 16), (uint16_t)p[2] };
	const uint16_t px[4] = { 0, V0, 0, V0 }, pz[4] = { 0, 0, V0, V0 };
	const uint16_t tex = (uint16_t)p[7];
	const uint32_t st[4] = { tex, tex | ((uint32_t)tex << 16), 0, (uint32_t)tex << 16 };
	for (uint32_t i = 0; i < 4; i++) {
		load_vertex(g, i, (int16_t)(ox + px[i]), (int16_t)(uint16_t)(hy[i] + V1), (int16_t)(oz + pz[i]));
		struct gfx_vtx *vt = &g->vtx[i];
		uint8_t fog = vt->c[3];
		set_vertex_color(vt, p[3 + i]);
		if (g->geom & G_FOG)
			apply_fog_alpha(vt, fog);
		set_vertex_st(g, vt, st[i], false);
	}
	const uint32_t geom = g->geom;
	if ((geom & (G_CULL_FRONT | G_CULL_BACK)) == (G_CULL_FRONT | G_CULL_BACK))
		gfx_sp_geometry(g, geom & ~G_CULL_FRONT);
	if (g->vtx[2].z / g->vtx[2].w > g->vtx[1].z / g->vtx[1].w) {
		gfx_triangle(g, 0, 1, 3);
		gfx_triangle(g, 0, 3, 2);
	} else {
		gfx_triangle(g, 0, 3, 2);
		gfx_triangle(g, 0, 1, 3);
	}
	if (g->geom != geom)
		gfx_sp_geometry(g, geom);
}

static void cmd_trigen(struct gfx_state *g, uint32_t w0, uint32_t ca)
{
	struct trigen t;
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca, 40);
	g->dl_pc = ca + 40;
	if (!s) {
		g->res->dl_errors++;
		return;
	}
	for (uint32_t i = 0; i < 10; i++)
		t.p[i] = salsa64_rd32(s + 4 * i);
	memset(t.vd, 0, sizeof t.vd);
	memset(t.cd, 0, sizeof t.cd);
	t.ntri = 0;
	const uint32_t mode = (w0 >> 8) & 0xFF;
	if (mode == 0x00 && (w0 & 0xFF) <= 1)
		tg_mesh(g, &t, (w0 & 0xFF) == 1);
	else if (mode == 0x02)
		tg_quad(g, t.p);
}

/* ---- G_TEXRECT_GEN ----------------------------------------------------- */

/* Flotante no negativo → entero sin pasarse de 2^32. */
static uint32_t ftou(float f)
{
	return f >= 4294967040.0f ? 0xFFFFFF00u : (uint32_t)f;
}

static __attribute__((noinline)) void cmd_texrect_gen(struct gfx_state *g, uint32_t w0, uint32_t ca)
{
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca, 24);
	g->dl_pc = ca + 24;
	if (!s)
		return;
	const uint32_t vi = ((w0 >> 8) & 0xFF) / 5;
	if (vi >= GFX_MAX_VTX)
		return;
	const uint32_t prim = salsa64_rd32(s + 4), p3 = salsa64_rd32(s + 12), p4 = salsa64_rd32(s + 16);
	const struct gfx_vtx *v = &g->vtx[vi];
	if (v->clip & (GFX_CLIP_XN | GFX_CLIP_XP | GFX_CLIP_YN | GFX_CLIP_YP | GFX_CLIP_W))
		return;
	const bool flip = (w0 & 1) != 0;

	const float screen_x = v->sx, screen_y = v->sy;
	const uint32_t w_i = v->w >= 1.0f ? (uint32_t)v->w : 1u;
	{
		const uint8_t *vp = gfx_io_read(g, GFX_IO_MTX, F5(g).viewport & 0x00FFFFFF, 4);
		const uint8_t *pm = gfx_io_read(g, GFX_IO_VTX, F5(g).persp & 0x00FFFFFF, 64);
		if (!vp || !pm)
			return;
		const uint32_t vpw = salsa64_rd32(vp);
		const uint32_t viewport_x = (vpw >> 17) & 0x7FFF, viewport_y = (vpw >> 1) & 0x7FFF;
		const uint32_t persp_x = (salsa64_rd32(pm) & 0xFFFF0000u) | (salsa64_rd32(pm + 32) >> 16);
		const uint32_t persp_y = (salsa64_rd32(pm + 8) << 16) | (salsa64_rd32(pm + 40) & 0xFFFF);
		uint32_t p3x = p3 >> 16, p3y = p3 & 0xFFFF;
		if (flip) {
			uint32_t x = p3x;
			p3x = p3y;
			p3y = x;
		}
		/* (a · b · c / w) >> 16 en 64 bits en GLideN64; aquí en flotante (el SPE no trae
		 * la aritmética de 64 bits): a lo más un cuarto de píxel de diferencia en
		 * los bordes, por debajo de lo que ya difiere de la w del microcódigo. */
		const float wf = (float)w_i * 65536.0f;
		uint32_t off_x = ftou((float)(p3x * viewport_x) * (float)persp_x / wf);
		uint32_t off_y = ftou((float)(p3y * viewport_y) * (float)persp_y / wf);
		const float off_xf = (float)off_x * 0.25f, off_yf = (float)off_y * 0.25f;
		float ulx = screen_x - off_xf, lrx = screen_x + off_xf;
		if (lrx - ulx <= 0.0f)
			return;
		float uly = screen_y - off_yf, lry = screen_y + off_yf;
		if (lry - uly <= 0.0f)
			return;

		uint32_t p4x = p4 & 0xFFFF0000u, p4y = p4 << 16;
		const float *m = g->mvp;
		float fx = m[0] < 0 ? -m[0] : m[0], fy = m[1] < 0 ? -m[1] : m[1];
		fx -= (float)(int32_t)fx;
		fy -= (float)(int32_t)fy;
		p4x |= (uint32_t)(fx * 65536.0f);
		p4y |= (uint32_t)(fy * 65536.0f);
		if (flip) {
			uint32_t x = off_x;
			off_x = off_y;
			off_y = x;
		}
		if (off_x == 0 || off_y == 0)
			return;
		uint16_t dsdx_i = (uint16_t)(ftou((float)p4x / (float)off_x) >> 10);
		uint16_t dtdy_i = (uint16_t)(ftou((float)p4y / (float)off_y) >> 10);
		uint16_t E = 0, F = 0;
		if (w0 & 2) {
			dsdx_i = (uint16_t)-dsdx_i;
			E = (uint16_t)(p4 >> 16);
		}
		if (w0 & 4) {
			dtdy_i = (uint16_t)-dtdy_i;
			F = (uint16_t)p4;
		}
		/* Lo que el RDP recibe: dsdx y dtdy en 5.10; con flip, intercambiadas. */
		uint16_t rdsdx = dsdx_i, rdtdy = dtdy_i;
		if (flip) {
			uint16_t x = rdsdx;
			rdsdx = rdtdy;
			rdtdy = x;
		}
		uint16_t S, T;
		if (ulx > 0.0f) {
			S = (uint16_t)(E + 0xFFF0);
		} else {
			const int ulx_i = (int)(ulx * 4.0f);
			S = (uint16_t)(((uint32_t)(0 - (int)((uint32_t)rdsdx << 6) * ulx_i) << 3) + E + 0xFFF0u);
		}
		if (uly > 0.0f) {
			T = (uint16_t)(F + 0xFFF0);
		} else {
			const int uly_i = (int)(uly * 4.0f);
			T = (uint16_t)(((uint32_t)(0 - (int)((uint32_t)rdtdy << 6) * uly_i) << 3) + F + 0xFFF0u);
		}
		/* z del primitivo: la de pantalla del vértice (0..1023 → 0..0x7FFF). */
		float z15 = v->sz * (32767.0f / 1023.0f);
		z15 = z15 < 0.0f ? 0.0f : z15 > 32767.0f ? 32767.0f : z15;
		gfx_rdp_command(g, 0xEE000000u, (uint32_t)(uint16_t)(int)z15 << 16);
		gfx_rdp_command(g, 0xFA000000u, prim);
		if (g->geom & G_FOG)
			gfx_rdp_command(g, 0xF8000000u, (prim & 0xFFFFFF00u) | v->c[3]);

		/* Las coordenadas se mandan al RDP en cuartos de píxel (sin negativos: el
		 * microcódigo corrige s y t en lo que sale de la pantalla). */
		int ix0 = ulx > 0.0f ? (int)(ulx * 4.0f) : 0, iy0 = uly > 0.0f ? (int)(uly * 4.0f) : 0;
		int ix1 = (int)(lrx * 4.0f), iy1 = (int)(lry * 4.0f);
		uint32_t tile = g->tex_tile;
		uint32_t r0 = 0xE4000000u | ((uint32_t)(ix1 & 0xFFF) << 12) | (uint32_t)(iy1 & 0xFFF);
		uint32_t r1 = (tile << 24) | ((uint32_t)(ix0 & 0xFFF) << 12) | (uint32_t)(iy0 & 0xFFF);
		uint32_t r2 = ((uint32_t)S << 16) | T;
		uint32_t r3 = ((uint32_t)dsdx_i << 16) | dtdy_i;
		gfx_texrect(g, r0, r1, r2, r3, flip);
	}
}

/* ---- Despacho ---------------------------------------------------------- */

static void other_mode_ex(struct gfx_state *g, uint32_t ca, uint32_t w1, bool high)
{
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca + 8, 4);
	g->dl_pc = ca + 16;
	if (!s)
		return;
	uint32_t *mode = high ? &g->oh : &g->ol;
	uint32_t v = (*mode & salsa64_rd32(s)) | w1;
	if (v != *mode)
		g->dirty = true;
	*mode = v;
}

static void list_entry(struct gfx_state *g)
{
	/* La lista nueva empieza con la palabra de salto de software; el
	 * microcódigo la guarda y sigue desde el segundo comando. */
	uint32_t w0, w1;
	if (g->dl_sp >= (int)F5_DL_DEPTH || !gfx_io_dl(g, g->dl_pc, &w0, &w1)) {
		F5(g).dl[g->dl_sp & 15] = 0;
		return;
	}
	F5(g).dl[g->dl_sp] = w0 & 0x00FFFFFFu;
	g->dl_pc += 8;
}

static bool f5_command(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint32_t ca = g->dl_pc - 8;  /* dirección del comando: sus datos siguen */
	switch (w0 >> 24) {
	case G_MTX:
		if ((w0 >> 16) & 1)
			F5(g).persp = w1 & 0x00FFFFFFu;
		return false;
	case G_VTXCOLOR:
		F5(g).color_base = gfx_addr(g, w1);
		return true;
	case G_MOVEMEM: {
		uint32_t index = (w0 >> 16) & 0xFF;
		const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca + 8, 16);
		g->dl_pc = ca + 24;
		if (!s)
			return true;
		if (index == MV_VIEWPORT) {
			F5(g).viewport = ca + 8;
			gfx_set_viewport(g, s);
		} else if (index == MV_TEXSCALE) {
			F5(g).tex_scale[0] = salsa64_rd32(s + 8);
			F5(g).tex_scale[1] = salsa64_rd32(s + 4);
		}
		return true;
	}
	case G_VTX:
		cmd_vtx(g, w0, w1);
		return true;
	case G_TRIGEN:
		cmd_trigen(g, w0, ca);
		return true;
	case G_DL:
		if (g->dl_sp + 1 >= (int)F5_DL_DEPTH || !gfx_dl_call(g, w1, false)) {
			g->res->dl_errors++;
			g->dl_pc = 0xFFFFFFFFu;
			return true;
		}
		list_entry(g);
		return true;
	case G_BRANCHDL:
		gfx_dl_call(g, w1, true);
		list_entry(g);
		return true;
	case G_JUMPSWDL:
		g->dl_pc = F5(g).dl[g->dl_sp];
		list_entry(g);
		return true;
	case G_RDPSETOTHERMODE_L_EX:
		other_mode_ex(g, ca, w1, false);
		return true;
	case G_SETOTHERMODE_H_EX:
		other_mode_ex(g, ca, w1, true);
		return true;
	case G_TRI1:
	case G_TRI2:
		cmd_tri(g, w0, w1, ca, (w0 >> 24) == G_TRI2);
		return true;
	case G_MOVEWORD:
		switch (w0 & 0xFF) {
		case MW_CLIP:
			gfx_clip_ratio(g, (w0 >> 8) & 0xFFFF, w1);
			break;
		case MW_SEGMENT:
			g->segments[((w0 >> 8) & 0xFFFF) >> 2 & 15] = w1 & 0x00FFFFFF;
			break;
		case MW_FOG_MULTIPLIER:
			g->fog_mul = (float)(int32_t)w1 * (1.0f / 256.0f);
			break;
		case MW_FOG_OFFSET:
			g->fog_off = (float)(int32_t)w1 * (1.0f / 256.0f);
			break;
		default:
			break;
		}
		return true;
	case G_TEXRECT_GEN:
		cmd_texrect_gen(g, w0, ca);
		return true;
	case 0xE4:
	case 0xE5: {
		/* G_TEXRECT: 16 bytes (los otros dos comandos de Fast3D no existen aquí): sus
		 * palabras tercera y cuarta (s y t, dsdx y dtdy) siguen a las dos primeras. */
		uint32_t w2, w3;
		if (!gfx_io_dl(g, g->dl_pc, &w2, &w3))
			return true;
		g->dl_pc += 8;
		gfx_texrect(g, w0, w1, w2, w3, (w0 >> 24) == 0xE5);
		return true;
	}
	default:
		return false;
	}
}

void gfx_dl_f5rogue(struct gfx_state *g)
{
	memset(&g->rare, 0, sizeof g->rare);
	/* Como la primera generación: el recorte con que arranca es 1 (gfx_run la pone en 2). */
	g->clip_ratio[0] = g->clip_ratio[1] = 1.0f;
	/* La primera palabra de la lista principal es también la de salto. */
	list_entry(g);
	gfx_dl_f3d(g, f5_command);
}
