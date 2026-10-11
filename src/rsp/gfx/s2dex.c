/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tramo S2DEX2 dentro de una tarea de la familia F3DEX2: Zelda OoT
 * carga S2DEX con G_LOAD_UCODE para copiar el fondo de una sala
 * prerrenderizada (Room_DrawBackground2D: G_BG_COPY de una imagen RGBA16 de
 * 320 × 240) y vuelve a F3DZEX. Antes el tramo se saltaba entero (dl.c) y se
 * veía la malla de profundidad de la sala en colores planos.
 *
 * Se reconoce por el texto de los datos del microcódigo ("... S2DEX ... 2.0x"
 * en los bytes que carga el G_LOAD_UCODE). Del tramo se hace lo que comparte
 * con F3DEX2 (G_DL, G_ENDDL, G_MOVEWORD de segmentos, G_SETOTHERMODE_*,
 * G_RDPHALF_*, comandos del RDP) y los fondos:
 *
 * - G_BG_COPY (uObjBg): la imagen, desde (imageX, imageY) y repitiéndose en
 *   imageW × imageH, se copia sin escala al marco (frameX, frameY, frameW,
 *   frameH) en modo copia. Como el microcódigo: tiras de filas cargadas a la
 *   TMEM con LOADTILE (las que caben en 4 KB, 2 KB con paleta) y un
 *   TEXRECT por tira (dos si la tira da la vuelta en x). Todo pasa por
 *   rdp.c: las tiras son texturas normales (caché por contenido de la TMEM).
 * - G_BG_1CYC (uObjScaleBg): igual en 1 ciclo con escala (scaleW/scaleH en
 *   u5.10: texels por píxel); con G_OBJRM_BILERP cada tira lleva una fila
 *   más para el filtro.
 * - G_OBJ_RENDERMODE: se guarda (bilineal del fondo escalado).
 * Lo demás de S2DEX (sprites, G_OBJ_*) se cuenta como saltado.
 *
 * Campos de uObjBg/uObjScaleBg y su punto fijo (u10.5, u10.2, s10.2;
 * escala u5.10 = texels por píxel): GLideN64 `src/uCodes/S2DEX.cpp`
 * (consultado el 2026-10-11, solo el formato; docs/THIRD_PARTY.md). Opcodes
 * de S2DEX2 (los de la familia F3DEX2): G_BG_COPY = 0x0A como en la lista
 * de Zelda OoT. Código propio.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

enum {
	S2_BG_1CYC = 0x09, S2_BG_COPY = 0x0A, S2_OBJ_RENDERMODE = 0x0B,
	S2_MOVEWORD = 0xDB, S2_LOAD_UCODE = 0xDD, S2_DL = 0xDE, S2_ENDDL = 0xDF,
	S2_RDPHALF_1 = 0xE1, S2_SETOTHERMODE_L = 0xE2, S2_SETOTHERMODE_H = 0xE3,
	S2_RDPHALF_2 = 0xF1,
};

#define OBJRM_BILERP 0x08u
#define G_MW_SEGMENT 0x06u
#define BG_TILE      0       /* tile con que se dibuja */
#define BG_LOAD_TILE 7       /* G_TX_LOADTILE */

static bool has(const uint8_t *p, uint32_t n, const char *s, uint32_t *at)
{
	size_t k = strlen(s);
	for (uint32_t i = 0; i + k <= n; i++)
		if (memcmp(p + i, s, k) == 0) {
			*at = i;
			return true;
		}
	return false;
}

bool gfx_ucode_is_s2dex2(struct gfx_state *g, uint32_t data, uint32_t size)
{
	if (size == 0 || size > GFX_IO_MAX)
		size = GFX_IO_MAX;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, data, size);
	uint32_t at;
	if (!p || !has(p, size, "S2DEX", &at))
		return false;
	/* La versión: el primer "N." después del nombre. */
	for (uint32_t i = at + 5; i + 1 < size && p[i] >= 0x20 && p[i] < 0x7F; i++)
		if (p[i] >= '0' && p[i] <= '9' && p[i + 1] == '.')
			return p[i] == '2';
	return false;
}

struct bg {
	int32_t frame_x, frame_y;     /* píxeles (s10.2 → entero) */
	uint32_t frame_w, frame_h;    /* píxeles */
	uint32_t image_x, image_y;    /* texels (u10.5 → entero) */
	uint32_t image_w, image_h;    /* texels */
	uint32_t ptr;                 /* dirección segmentada de la imagen */
	unsigned fmt, siz, pal;
	uint32_t scale_w, scale_h;    /* u5.10 (1:1 = 1024) */
};

static bool read_bg(struct gfx_state *g, uint32_t seg, bool scaled, struct bg *b)
{
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, seg), 40);
	if (!p) {
		g->res->dl_errors++;
		return false;
	}
	b->image_x = salsa64_rd16(p + 0) >> 5;
	b->image_w = salsa64_rd16(p + 2) >> 2;
	b->frame_x = (int16_t)salsa64_rd16(p + 4) >> 2;
	b->frame_w = salsa64_rd16(p + 6) >> 2;
	b->image_y = salsa64_rd16(p + 8) >> 5;
	b->image_h = salsa64_rd16(p + 10) >> 2;
	b->frame_y = (int16_t)salsa64_rd16(p + 12) >> 2;
	b->frame_h = salsa64_rd16(p + 14) >> 2;
	b->ptr = salsa64_rd32(p + 16);
	b->fmt = p[22] & 7;
	b->siz = p[23] & 3;
	b->pal = salsa64_rd16(p + 24) & 15;
	b->scale_w = scaled ? salsa64_rd16(p + 28) : 1024;
	b->scale_h = scaled ? salsa64_rd16(p + 30) : 1024;
	if (!b->scale_w || !b->scale_h)
		return false;
	return b->image_w && b->image_h && b->frame_w && b->frame_h && b->image_w <= 1024 && b->image_h <= 1024;
}

/* Bytes de una fila en la TMEM (múltiplo de 8): en 32 bits, por mitad. */
static uint32_t row_bytes(uint32_t width, unsigned siz)
{
	uint32_t b = siz == G_IM_SIZ_4b ? (width + 1) / 2 : siz == G_IM_SIZ_8b ? width : 2 * width;
	return (b + 7) & ~7u;
}

/* Carga las filas [ty, ty + n) de la imagen a la TMEM y deja la tile de
 * dibujo apuntándolas (s de 0 a image_w − 1, t de ty a ty + n − 1). */
static void load_rows(struct gfx_state *g, const struct bg *b, uint32_t ty, uint32_t n)
{
	/* LOADTILE no carga 4 bits: se cargan como bytes, la mitad de ancho. */
	unsigned lsiz = b->siz == G_IM_SIZ_4b ? G_IM_SIZ_8b : b->siz;
	uint32_t lw = b->siz == G_IM_SIZ_4b ? (b->image_w + 1) / 2 : b->image_w;
	uint32_t line = row_bytes(b->image_w, b->siz) / 8;
	gfx_rdp_command(g, 0xFDu << 24 | b->fmt << 21 | lsiz << 19 | (lw - 1), b->ptr);
	gfx_rdp_command(g, 0xF5u << 24 | b->fmt << 21 | lsiz << 19 | line << 9, (uint32_t)BG_LOAD_TILE << 24);
	gfx_rdp_command(g, 0xF4u << 24 | (ty << 2), (uint32_t)BG_LOAD_TILE << 24 | (lw - 1) << 14 | (ty + n - 1) << 2);
	/* Dibujo: límites sin repetir (G_TX_CLAMP en s y t). */
	gfx_rdp_command(g, 0xF5u << 24 | b->fmt << 21 | b->siz << 19 | line << 9, (uint32_t)BG_TILE << 24 | b->pal << 20 |
								2u << 18 | 2u << 8);
	gfx_rdp_command(g, 0xF2u << 24 | (ty << 2), (uint32_t)BG_TILE << 24 | (b->image_w - 1) << 14 | (ty + n - 1) << 2);
}

/* TEXRECT de (x0, y0) a (x1, y1) en cuartos de píxel (borde inferior
 * derecho exclusivo) desde el texel (s, t), con dsdx/dtdy en s5.10. */
static void strip_rect(struct gfx_state *g, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t s,
		       uint32_t t, uint32_t dsdx, uint32_t dtdy, bool copy)
{
	if (x1 <= x0 || y1 <= y0)
		return;
	/* En modo copia los bordes son inclusivos y dsdx va ×4. */
	if (copy) {
		x1 -= 4;
		y1 -= 4;
		dsdx *= 4;
	}
	if (x1 > 0xFFF)
		x1 = 0xFFF;
	if (y1 > 0xFFF)
		y1 = 0xFFF;
	gfx_texrect(g, 0xE4u << 24 | x1 << 12 | y1, (uint32_t)BG_TILE << 24 | x0 << 12 | y0, s << 16 | (t & 0xFFFF),
		    (dsdx & 0xFFFF) << 16 | (dtdy & 0xFFFF), false);
}

static void bg_draw(struct gfx_state *g, uint32_t seg, bool scaled)
{
	struct bg b;
	if (!read_bg(g, seg, scaled, &b))
		return;
	unsigned cycle = (g->oh >> G_MDSFT_CYCLETYPE) & 3;
	bool copy = cycle == G_CYC_COPY;
	/* Con paleta, la mitad alta de la TMEM es de la paleta. */
	bool tlut = ((g->oh >> G_MDSFT_TEXTLUT) & 3) >= G_TT_RGBA16 && b.siz <= G_IM_SIZ_8b;
	uint32_t cap = (tlut || b.siz == G_IM_SIZ_32b) ? 2048 : 4096;
	uint32_t per_load = cap / row_bytes(b.image_w, b.siz);
	bool bilerp = scaled && (g->obj_rendermode & OBJRM_BILERP);
	if (per_load < (bilerp ? 2u : 1u))
		return;  /* una fila no cabe en la TMEM */

	/* Marco recortado a la izquierda y arriba de la pantalla: se corre la
	 * imagen lo que se recorta. Todo en texels ×1024 (u5.10) para la
	 * escala. */
	uint64_t sx0 = (uint64_t)b.image_x << 10, ty0 = (uint64_t)b.image_y << 10;
	uint32_t fx = 0, fy = 0;
	if (b.frame_x < 0) {
		fx = (uint32_t)-b.frame_x;
		sx0 += (uint64_t)fx * b.scale_w;
	}
	if (b.frame_y < 0) {
		fy = (uint32_t)-b.frame_y;
		ty0 += (uint64_t)fy * b.scale_h;
	}
	if (fx >= b.frame_w || fy >= b.frame_h)
		return;
	uint32_t x_left = (uint32_t)(b.frame_x + (int32_t)fx), y = (uint32_t)(b.frame_y + (int32_t)fy);
	uint32_t y_end = (uint32_t)(b.frame_y + (int32_t)b.frame_h);
	uint32_t x_end = (uint32_t)(b.frame_x + (int32_t)b.frame_w);
	uint64_t wrap_w = (uint64_t)b.image_w << 10, wrap_h = (uint64_t)b.image_h << 10;
	uint64_t t = ty0 % wrap_h;

	while (y < y_end) {
		/* Filas de imagen de esta tira: desde la de t, hasta per_load (una
		 * menos con bilineal, que lee la siguiente) o el fin de la imagen. */
		uint32_t ty = (uint32_t)(t >> 10);
		uint32_t n = per_load - (bilerp ? 1 : 0);
		if (n > b.image_h - ty)
			n = b.image_h - ty;
		/* Píxeles de pantalla que caen en esas filas. */
		uint64_t t_end = (uint64_t)(ty + n) << 10;
		uint32_t rows = (uint32_t)((t_end - t + b.scale_h - 1) / b.scale_h);
		if (rows == 0)
			rows = 1;
		if (rows > y_end - y)
			rows = y_end - y;
		uint32_t load_n = n + (bilerp && ty + n < b.image_h ? 1 : 0);
		load_rows(g, &b, ty, load_n);

		/* En x, tramos hasta la vuelta de la imagen. */
		uint64_t s = sx0 % wrap_w;
		uint32_t x = x_left;
		while (x < x_end) {
			uint32_t cols = (uint32_t)((wrap_w - s + b.scale_w - 1) / b.scale_w);
			if (cols > x_end - x)
				cols = x_end - x;
			strip_rect(g, x << 2, y << 2, (x + cols) << 2, (y + rows) << 2, (uint32_t)(s >> 5),
				   (uint32_t)(t >> 5), b.scale_w, b.scale_h, copy);
			x += cols;
			s = (s + (uint64_t)cols * b.scale_w) % wrap_w;
		}
		y += rows;
		t = (t + (uint64_t)rows * b.scale_h) % wrap_h;
	}
}

bool gfx_dl_s2dex2(struct gfx_state *g)
{
	struct gfx_result *res = g->res;
	uint32_t w0, w1;
	res->flags |= GFX_RES_LOAD_UCODE;
	while (gfx_dl_next(g, &w0, &w1)) {
		uint32_t op = w0 >> 24;
		switch (op) {
		case S2_LOAD_UCODE:
			if (g->job->ucode && gfx_addr(g, w1) == g->job->ucode)
				return true;
			/* Otro más: S2DEX otra vez sigue aquí; si no, se salta. */
			if (!gfx_ucode_is_s2dex2(g, gfx_addr(g, g->rdphalf1), (w0 & 0xFFFF) + 1))
				return gfx_dl_skip_foreign(g, S2_LOAD_UCODE, S2_DL, S2_ENDDL);
			break;
		case S2_DL:
			if (!gfx_dl_call(g, w1, (w0 >> 16) & 1))
				return false;
			break;
		case S2_ENDDL:
			if (!gfx_dl_end(g)) {
				res->flags |= GFX_RES_FOREIGN_END;
				return false;
			}
			break;
		case S2_MOVEWORD:
			if (((w0 >> 16) & 0xFF) == G_MW_SEGMENT)
				g->segments[((w0 & 0xFFFF) >> 2) & 15] = w1 & 0x00FFFFFF;
			break;
		case S2_RDPHALF_1:
			g->rdphalf1 = w1;
			break;
		case S2_RDPHALF_2:
			g->rdphalf2 = w1;
			break;
		case S2_SETOTHERMODE_L:
		case S2_SETOTHERMODE_H: {
			int len = (int)(w0 & 0xFF) + 1;
			gfx_sp_othermode(g, op == S2_SETOTHERMODE_H, 32 - (int)((w0 >> 8) & 0xFF) - len, len, w1);
			break;
		}
		case S2_BG_COPY:
		case S2_BG_1CYC:
			bg_draw(g, w1, op == S2_BG_1CYC);
			res->bg++;
			break;
		case S2_OBJ_RENDERMODE:
			g->obj_rendermode = w1;
			break;
		default:
			/* Comandos del RDP (0xE4 es G_RDPHALF_0 de S2DEX2). */
			if (op >= 0xE6)
				gfx_rdp_command(g, w0, w1);
			else
				res->skipped++;
			break;
		}
	}
	return false;
}
