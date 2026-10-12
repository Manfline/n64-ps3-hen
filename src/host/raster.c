/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — rasterizador de referencia en software para la salida de la
 * HLE gráfica (src/rsp/gfx/gfx.h), solo para el host.
 *
 * Hace lo mismo que debe hacer el RSX con esa salida (src/rdp y
 * lib/rsxkit), a la resolución de la N64 y sin buscar velocidad: sirve para
 * ver y verificar en el host lo que produce la HLE (tools/test-roms.sh con
 * gfxtest) y como referencia cuando el RSX muestre otra cosa.
 *
 * Los destinos (qué color image tiene imagen, cuál se recicla) son la misma
 * tabla que usa la PS3, src/rdp/targets.c; el campo de la tabla es aquí el
 * número de tarea. Como en la PS3, una textura cuyo origen (gfx_tex.src_*)
 * cae en un destino dibujado hace poco se lee del destino (render a
 * textura), y un destino se copia a la RDRAM (RGBA5551) al terminar la
 * tarea cuando la política de rdp_targets lo pide: así el host modela la
 * misma conducta antes de medirla en la consola.
 *
 * Simplificaciones: sin recorte contra los planos (se descartan triángulos
 * con algún w ≤ 0), muestreo en el centro del píxel, regla de cobertura
 * por arriba a la izquierda aproximada.
 */
#include "raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/endian.h"
#include "core/rcp.h"
#include "rdp/targets.h"

#define MAX_DEPTHS 4
/* G_IM_SIZ_16b (gbi): color images y texturas de 16 bits. */
#define IM_SIZ_16b 2u

struct depth {
	uint32_t addr;
	bool used;
	float z[RASTER_W * RASTER_H];
};

static struct rdp_targets tg;
static uint32_t rgb[RDP_TARGETS_MAX][RASTER_W * RASTER_H];
static uint32_t scratch[RASTER_W * RASTER_H];  /* copia del ligado cuando un dibujo lo lee */
static int scratch_slot;
static struct depth depths[MAX_DEPTHS];
static unsigned next_depth;
static uint32_t task_no;
static uint32_t vi_field;  /* campos del VI con imagen (rdp_targets_present) */

bool raster_trace_fb;
uint32_t raster_field;
int raster_fbfx = 2;
static struct rdp_vi_ring vi_ring;
static uint32_t primes, readbacks, mirrors, frames;
static uint32_t rb_task[RDP_TARGETS_MAX];  /* última tarea con copia a la RDRAM por ranura */
#define RB_KEEP_TASKS 32
#define MIRROR_BYTES 65536  /* como rdp.c: tope de una copia espejo (imagen chica) */
static struct gfx_job last_job;  /* la RDRAM, para el espejo de cada campo */

static void prime(const struct gfx_job *job, int slot);

static int get_target(const struct gfx_job *job, uint32_t addr, uint32_t width, uint32_t fmt, uint32_t siz,
		      uint32_t min_lh)
{
	bool fresh;
	/* Como rdp.c: una color image imposible no toca la tabla. */
	if (!width || width > 640 || min_lh > 480) {
		if (raster_trace_fb)
			fprintf(stderr, "fb: campo %u tarea %u: color image imposible %06x/%u (alto %u): se ignora\n", raster_field,
				task_no, addr, width, min_lh);
		return -1;
	}
	int i = rdp_targets_get(&tg, addr, width, siz, min_lh, &fresh);
	if (fresh) {
		memset(rgb[i], 0, sizeof rgb[i]);
		tg.t[i].fmt = (uint8_t)fmt;
	}
	if (fresh || tg.task - tg.t[i].last_task > RDP_TARGETS_FRESH)
		prime(job, i);
	return i;
}

static struct depth *get_depth(uint32_t addr)
{
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (depths[i].used && depths[i].addr == addr)
			return &depths[i];
	struct depth *d = &depths[next_depth++ % MAX_DEPTHS];
	d->used = true;
	d->addr = addr;
	for (int i = 0; i < RASTER_W * RASTER_H; i++)
		d->z[i] = 1.0f;
	return d;
}

#ifdef SALSA64_GFX_TRACE
/* Depuración (build con -DSALSA64_GFX_TRACE, solo en las tareas que traza
 * --trace-gfx): SALSA64_GFX_MARK=n[,m…] pinta de magenta esos dibujos y
 * SALSA64_GFX_PIX=x,y muestra qué dibujos escriben ese píxel y con qué valores. */
extern int salsa64_gfx_trace_now;
static bool raster_mark;
static unsigned ndraw;
static int pix_x = -1, pix_y = -1;
#endif

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static int wrap(int i, int n, unsigned mode)
{
	if (mode == GFX_WRAP_REPEAT) {
		i %= n;
		return i < 0 ? i + n : i;
	}
	if (mode == GFX_WRAP_MIRROR) {
		int p = 2 * n;
		i %= p;
		if (i < 0)
			i += p;
		return i < n ? i : p - 1 - i;
	}
	return i < 0 ? 0 : i >= n ? n - 1 : i;
}

/* Memoria de las texturas: arena de la tarea y región de la caché. */
struct texmem {
	const uint8_t *base[2];  /* GFX_TEX_ARENA, GFX_TEX_CACHE */
};

/* De dónde se lee cada textura del dibujo: un destino (render a textura)
 * o su memoria. */
struct texsrc {
	int slot;            /* ranura del destino, o -1 */
	const uint32_t *pix; /* su imagen, o la copia (scratch) si es el ligado */
	uint32_t x0, y0;     /* texel (0, 0) de la textura dentro del destino */
};

static void fetch(const struct gfx_tex *t, const struct texmem *m, const struct texsrc *s, int x, int y,
		  float out[4])
{
	x = wrap(x, t->width, GFX_TEX_WRAP_S(t->flags));
	y = wrap(y, t->height, GFX_TEX_WRAP_T(t->flags));
	if (s->slot >= 0) {
		uint32_t px = s->x0 + (uint32_t)x, py = s->y0 + (uint32_t)y;
		if (px >= RASTER_W)
			px = RASTER_W - 1;
		if (py >= RASTER_H)
			py = RASTER_H - 1;
		uint32_t c = s->pix[py * RASTER_W + px];
		out[0] = (c >> 16 & 0xFF) / 255.0f;
		out[1] = (c >> 8 & 0xFF) / 255.0f;
		out[2] = (c & 0xFF) / 255.0f;
		out[3] = 1.0f;
		return;
	}
	const uint8_t *p = m->base[t->where & 1] + t->offset + (uint32_t)y * t->pitch + 4u * (uint32_t)x;
	for (int k = 0; k < 4; k++)
		out[k] = p[k] / 255.0f;
}

/* Muestra con semántica del RSX: u, v normalizados; centros de texel en
 * (i + 0,5) / tamaño. */
static void sample(const struct gfx_tex *t, const struct texmem *m, const struct texsrc *s, float u, float v,
		   float out[4])
{
	float x = u * t->width, y = v * t->height;
	if (!(t->flags & GFX_TEX_BILINEAR)) {
		fetch(t, m, s, (int)floorf(x), (int)floorf(y), out);
		return;
	}
	x -= 0.5f;
	y -= 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0, a[4], b[4], c[4], d[4];
	fetch(t, m, s, x0, y0, a);
	fetch(t, m, s, x0 + 1, y0, b);
	fetch(t, m, s, x0, y0 + 1, c);
	fetch(t, m, s, x0 + 1, y0 + 1, d);
	for (int k = 0; k < 4; k++)
		out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void draw_triangle(const struct gfx_draw *d, const struct gfx_vertex *v, const struct texmem *m,
			  const struct texsrc src[2], uint32_t *tg_rgb, struct depth *zb)
{
	float sx[3], sy[3], sz[3], iw[3];
	for (int i = 0; i < 3; i++) {
		if (v[i].w <= 0.0f)
			return;
		iw[i] = 1.0f / v[i].w;
		sx[i] = v[i].x * iw[i];
		sy[i] = v[i].y * iw[i];
		sz[i] = v[i].z * iw[i];
	}
	float area = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
	if (area == 0.0f)
		return;
	/* Antihorario visto con y hacia arriba (convención de OpenGL y de
	 * libultra) es el frente; con la y de pantalla hacia abajo, área < 0. */
	if ((d->cull == GFX_CULL_BACK && area > 0.0f) || (d->cull == GFX_CULL_FRONT && area < 0.0f))
		return;

	float x0 = d->scissor[0] / 4.0f, y0 = d->scissor[1] / 4.0f;
	float x1 = d->scissor[2] / 4.0f, y1 = d->scissor[3] / 4.0f;
	float minx = fminf(fminf(sx[0], sx[1]), sx[2]), maxx = fmaxf(fmaxf(sx[0], sx[1]), sx[2]);
	float miny = fminf(fminf(sy[0], sy[1]), sy[2]), maxy = fmaxf(fmaxf(sy[0], sy[1]), sy[2]);
	int px0 = (int)floorf(fmaxf(minx, x0)), px1 = (int)ceilf(fminf(maxx, x1));
	int py0 = (int)floorf(fmaxf(miny, y0)), py1 = (int)ceilf(fminf(maxy, y1));
	if (px0 < 0) px0 = 0;
	if (py0 < 0) py0 = 0;
	if (px1 > RASTER_W) px1 = RASTER_W;
	if (py1 > RASTER_H) py1 = RASTER_H;

	for (int y = py0; y < py1; y++)
		for (int x = px0; x < px1; x++) {
			float cx = x + 0.5f, cy = y + 0.5f;
			if (cx < x0 || cx >= x1 || cy < y0 || cy >= y1)
				continue;
			float b0 = edge(sx[1], sy[1], sx[2], sy[2], cx, cy) / area;
			float b1 = edge(sx[2], sy[2], sx[0], sy[0], cx, cy) / area;
			float b2 = 1.0f - b0 - b1;
			if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f)
				continue;
			float z = b0 * sz[0] + b1 * sz[1] + b2 * sz[2];
			int idx = y * RASTER_W + x;
			if ((d->depth & GFX_DEPTH_TEST) && zb && z > zb->z[idx] + ((d->depth & GFX_DEPTH_DECAL) ? 1e-3f : 0.0f))
				continue;
			/* Interpolación con perspectiva. */
			float p0 = b0 * iw[0], p1 = b1 * iw[1], p2 = b2 * iw[2], pw = p0 + p1 + p2;
			p0 /= pw, p1 /= pw, p2 /= pw;
			float c[4], s = p0 * v[0].s + p1 * v[1].s + p2 * v[2].s, t = p0 * v[0].t + p1 * v[1].t + p2 * v[2].t;
			const uint8_t *c0 = &v[0].r, *c1 = &v[1].r, *c2 = &v[2].r;
			for (int k = 0; k < 4; k++)
				c[k] = (p0 * c0[k] + p1 * c1[k] + p2 * c2[k]) / 255.0f;

			float tx[2][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 } };
			for (int i = 0; i < d->ntex && i < 2; i++) {
				const struct gfx_tex *te = &d->tex[i];
				sample(te, m, &src[i], s * te->xf[0] + te->xf[2], t * te->xf[1] + te->xf[3], tx[i]);
			}
			float X[GFX_K_TERMS][4];
			for (int k = 0; k < GFX_K_TERMS; k++)
				for (int l = 0; l < 4; l++)
					X[k][l] = d->k[k][0][l] + d->k[k][1][l] * c[l] + d->k[k][2][l] * c[3];
			if (d->fog) {
				/* Como el vertex program de rsxkit: todo por (1 − f) y el
				 * color de niebla al término constante (el primero sin
				 * factores, que combine.c garantiza). */
				float f = c[3];
				int fog_term = GFX_K_ONE;
				if (d->nterms) {
					fog_term = -1;
					for (int k = 0; k < d->nterms && fog_term < 0; k++)
						if (!d->factors[k][0])
							fog_term = k;
				}
				for (int k = 0; k < GFX_K_TERMS; k++)
					for (int l = 0; l < 3; l++)
						X[k][l] *= 1.0f - f;
				for (int l = 0; fog_term >= 0 && l < 3; l++)
					X[fog_term][l] += f * ((d->fog_color >> (24 - 8 * l)) & 0xFF) / 255.0f;
			}
			float out[4];
			if (d->nterms) {
				/* Términos del generador de fragment programs. */
				for (int l = 0; l < 4; l++) {
					float sum = 0.0f;
					for (int k = 0; k < d->nterms; k++) {
						float p = X[k][l];
						for (int f = 0; f < GFX_K_FACTORS && d->factors[k][f]; f++)
							switch (d->factors[k][f]) {
							case GFX_F_T0: p *= tx[0][l]; break;
							case GFX_F_T0A: p *= tx[0][3]; break;
							case GFX_F_T1: p *= tx[1][l]; break;
							case GFX_F_T1A: p *= tx[1][3]; break;
							case GFX_F_C: p *= c[l]; break;
							default: p *= c[3]; break;
							}
						sum += p;
					}
					out[l] = clamp01(sum);
				}
			} else {
				for (int l = 0; l < 4; l++)
					out[l] = clamp01(tx[0][l] * X[GFX_K_T0][l] + tx[0][3] * X[GFX_K_T0A][l] +
							 tx[1][l] * X[GFX_K_T1][l] + tx[1][3] * X[GFX_K_T1A][l] +
							 X[GFX_K_ONE][l]);
			}
			if (d->atest == GFX_ATEST_GEQUAL && out[3] * 255.0f + 0.5f < d->aref)
				continue;
#ifdef SALSA64_GFX_TRACE
			if (raster_mark)
				out[0] = 1.0f, out[1] = 0.0f, out[2] = 1.0f, out[3] = 1.0f;
#endif
#ifdef SALSA64_GFX_TRACE
			if (pix_x == x && pix_y == y)
				fprintf(stderr, "    PIX draw %u ntex %u blend %u atest %u c %.2f %.2f %.2f %.2f tx0 %.2f %.2f %.2f %.2f "
					"X0 %.2f %.2f %.2f X4 %.2f %.2f %.2f out %.2f %.2f %.2f %.2f s %.1f t %.1f z %.3f\n",
					ndraw, d->ntex, d->blend, d->atest, c[0], c[1], c[2], c[3], tx[0][0], tx[0][1], tx[0][2], tx[0][3],
					X[GFX_K_T0][0], X[GFX_K_T0][1], X[GFX_K_T0][2], X[GFX_K_ONE][0], X[GFX_K_ONE][1], X[GFX_K_ONE][2],
					out[0], out[1], out[2], out[3], s, t, z);
#endif
			uint32_t old = tg_rgb[idx];
			float dst[3] = { (old >> 16 & 0xFF) / 255.0f, (old >> 8 & 0xFF) / 255.0f, (old & 0xFF) / 255.0f };
			if (d->blend == GFX_BLEND_KEEP)
				continue;
			for (int l = 0; l < 3; l++) {
				if (d->blend == GFX_BLEND_ALPHA)
					out[l] = out[l] * out[3] + dst[l] * (1.0f - out[3]);
				else if (d->blend == GFX_BLEND_ADD)
					out[l] = clamp01(out[l] * out[3] + dst[l]);
			}
			tg_rgb[idx] = (uint32_t)(out[0] * 255.0f + 0.5f) << 16 | (uint32_t)(out[1] * 255.0f + 0.5f) << 8 |
				      (uint32_t)(out[2] * 255.0f + 0.5f);
			if ((d->depth & GFX_DEPTH_WRITE) && zb)
				zb->z[idx] = z;
		}
}

/* Hasta qué línea llegan las tijeras de los dibujos que siguen a un TARGET
 * (como rdp.c en la PS3): el alto mínimo de su imagen. */
static uint32_t scissor_bottom(const uint8_t *cmd, uint32_t off, uint32_t cmd_bytes)
{
	uint32_t bottom = 0;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->op == GFX_OP_TARGET || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW) {
			uint32_t y1 = (((const struct gfx_draw *)c)->scissor[3] + 3) / 4;
			if (y1 > bottom)
				bottom = y1;
		}
		off += c->size;
	}
	return bottom;
}

/* El origen de una textura del dibujo, si es un destino dibujado hace
 * poco (raster_fresh tareas o menos). */
static int texture_source(const struct gfx_tex *t, int cur, struct texsrc *s)
{
	s->slot = -1;
	if (!t->src_addr || t->src_siz != IM_SIZ_16b)
		return -1;
	uint32_t x0, y0;
	int i;
	enum rdp_texsrc src = rdp_targets_texture(&tg, t->src_addr, t->src_pitch, 2, cur, &i, &x0, &y0);
	if ((src != RDP_TEXSRC_RSX && src != RDP_TEXSRC_SELF) || !raster_fbfx)
		return i;
	if (src == RDP_TEXSRC_SELF) {
		if (scratch_slot != i) {
			memcpy(scratch, rgb[i], sizeof scratch);
			scratch_slot = i;
		}
		s->pix = scratch;
	} else {
		s->pix = rgb[i];
	}
	s->slot = i;
	s->x0 = x0;
	s->y0 = y0;
	return i;
}

/* G_RM_VISCVG: cobertura plena en la RDRAM en vez de dibujar (como rdp.c). */
static void viscvg(const struct gfx_job *job, int cur, const struct gfx_draw *d)
{
	const struct rdp_target *t = &tg.t[cur];
	if (raster_fbfx < 2 || t->bpp != 2 || !job->rdram_ea)
		return;
	rb_task[cur] = tg.task;
	uint8_t *rdram = (uint8_t *)(uintptr_t)job->rdram_ea;
	uint32_t y0 = d->scissor[1] / 4, y1 = (d->scissor[3] + 3) / 4, x0 = d->scissor[0] / 4, x1 = (d->scissor[2] + 3) / 4;
	if (y1 > t->lh)
		y1 = t->lh;
	if (x1 > t->width)
		x1 = t->width;
	for (uint32_t y = y0; y < y1 && x1 > x0; y++) {
		uint32_t a = t->addr + (y * t->width + x0) * 2;
		if (a + (x1 - x0) * 2 > job->rdram_size)
			break;
		memset(rdram + a, 0xFF, (x1 - x0) * 2);
	}
}

/* Ceba la imagen de la ranura con la RDRAM (RGBA5551), como hace la PS3
 * con una superficie nueva o sin dibujar desde hace tareas. */
static void prime(const struct gfx_job *job, int slot)
{
	const struct rdp_target *t = &tg.t[slot];
	if (!raster_fbfx || t->bpp != 2 || !job->rdram_ea)
		return;
	const uint8_t *rdram = (const uint8_t *)(uintptr_t)job->rdram_ea;
	uint32_t w = t->width < RASTER_W ? t->width : RASTER_W, h = t->lh < RASTER_H ? t->lh : RASTER_H;
	for (uint32_t y = 0; y < h; y++) {
		uint32_t a = t->addr + y * t->width * 2;
		if (a + w * 2 > job->rdram_size)
			break;
		for (uint32_t x = 0; x < w; x++) {
			uint32_t p = salsa64_rd16(rdram + a + 2 * x);
			uint32_t r = p >> 11, g = (p >> 6) & 31, b = (p >> 1) & 31;
			rgb[slot][y * RASTER_W + x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
		}
	}
	primes++;
}

/* Ranura cuyo rango de direcciones contiene `addr`, sin mirar el paso ni
 * el tamaño de texel (traza: texturas que vienen de un destino pero no
 * como se esperaba). */
static int locate_any(uint32_t addr)
{
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &tg.t[i];
		if (t->used && addr >= t->addr && addr < t->addr + t->width * t->bpp * t->lh)
			return i;
	}
	return -1;
}

/* Copia a la RDRAM como RGBA5551 (sin cobertura: alfa 1) las `rows`
 * primeras filas del destino. */
static void readback(const struct gfx_job *job, int slot, uint32_t rows)
{
	const struct rdp_target *t = &tg.t[slot];
	if ((t->bpp != 2 && t->bpp != 1) || !job->rdram_ea)
		return;
	uint32_t w = t->width < RASTER_W ? t->width : RASTER_W, h = rows < RASTER_H ? rows : RASTER_H;
	uint8_t *rdram = (uint8_t *)(uintptr_t)job->rdram_ea;
	for (uint32_t y = 0; y < h; y++) {
		uint32_t a = t->addr + y * t->width * t->bpp;
		if (a + w * t->bpp > job->rdram_size)
			break;
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c = rgb[slot][y * RASTER_W + x];
			if (t->bpp == 1) {
				/* Imagen de 8 bits: la intensidad es el rojo (como rdp.c). */
				rdram[a + x] = (uint8_t)(c >> 16);
				continue;
			}
			uint32_t p = ((c >> 19) & 31) << 11 | ((c >> 11) & 31) << 6 | ((c >> 3) & 31) << 1 | 1;
			salsa64_wr16(rdram + a + 2 * x, (uint16_t)p);
		}
	}
}

/* Espejo del framebuffer (como frame_mirror en rdp.c): las `rows` primeras
 * filas de la ranura a la RDRAM, una vez por dibujo, sin pisar lo que una
 * copia reciente de una imagen que se solape dejó en esa memoria. */
static void frame_mirror(const struct gfx_job *job, int slot, uint32_t rows, const char *when)
{
	const struct rdp_target *t = &tg.t[slot];
	if (rows > t->lh)
		rows = t->lh;
	if (t->bpp != 2 || !rows || t->addr + t->width * 2 * rows > job->rdram_size ||
	    !rdp_targets_mirror_wanted(&tg, slot))
		return;
	/* Como rdp.c: no pisar una copia reciente que se solape (Link en los
	 * primeros bytes del framebuffer de Zelda; la cobertura de VISCVG). */
	uint32_t lo = t->addr, hi = t->addr + t->width * 2 * rows;
	for (int k = 0; k < RDP_TARGETS_MAX; k++) {
		const struct rdp_target *a = &tg.t[k];
		if (a->used && tg.task - rb_task[k] <= RB_KEEP_TASKS && a->addr < hi &&
		    a->addr + a->width * a->bpp * a->lh > lo)
			return;
	}
	readback(job, slot, rows);
	rdp_targets_mirrored(&tg, slot);
	frames++;
	if (raster_trace_fb)
		fprintf(stderr, "fb: campo %u tarea %u: espejo del framebuffer %06x/%u %s: %u filas\n", raster_field,
			task_no, t->addr, t->width, when, rows);
}

void raster_task(const struct gfx_job *job, const struct gfx_result *res)
{
	const uint8_t *cmd = (const uint8_t *)(uintptr_t)job->cmd_ea;
	const struct gfx_vertex *vtx = (const struct gfx_vertex *)(uintptr_t)job->vtx_ea;
	const struct texmem m = { { (const uint8_t *)(uintptr_t)job->tex_ea, (const uint8_t *)(uintptr_t)job->cache_ea } };
	int cur = -1;
	uint32_t off = 0;
	bool drawn[RDP_TARGETS_MAX] = { false };
	uint32_t drawn_rows[RDP_TARGETS_MAX] = { 0 };  /* hasta dónde llegaron las tijeras */
	/* Dibujos que leen cada destino en esta tarea, por destino escrito (traza). */
	uint32_t reads[RDP_TARGETS_MAX][RDP_TARGETS_MAX] = { { 0 } }, read_stale[RDP_TARGETS_MAX][RDP_TARGETS_MAX] = { { 0 } };
	uint32_t read_age[RDP_TARGETS_MAX] = { 0 };
	uint32_t suspect[RDP_TARGETS_MAX] = { 0 };
	++task_no;  /* tg.field lo lleva el VI (raster_vi), como en la PS3 */
	rdp_targets_task(&tg);
	scratch_slot = -1;
	last_job = *job;
#ifdef SALSA64_GFX_TRACE
	ndraw = 0;
	pix_x = pix_y = -1;
	if (salsa64_gfx_trace_now && getenv("SALSA64_GFX_PIX"))
		sscanf(getenv("SALSA64_GFX_PIX"), "%d,%d", &pix_x, &pix_y);
#endif
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size == 0)
			break;
		switch (c->op) {
		case GFX_OP_TARGET:
			cur = get_target(job, c->a, c->b, (c->c >> 8) & 0xFF, c->c & 0xFF,
					 scissor_bottom(cmd, off + c->size, res->cmd_bytes));
			break;
		case GFX_OP_ZCLEAR: {
			const struct gfx_zclear *z = (const struct gfx_zclear *)c;
			struct depth *zb = get_depth(c->a);
			for (int y = z->rect[1]; y < z->rect[3] && y < RASTER_H; y++)
				for (int x = z->rect[0]; x < z->rect[2] && x < RASTER_W; x++)
					zb->z[y * RASTER_W + x] = 1.0f;
			break;
		}
		case GFX_OP_DRAW: {
			const struct gfx_draw *d = (const struct gfx_draw *)c;
#ifdef SALSA64_GFX_TRACE
			{
				ndraw++;
				const char *mk = salsa64_gfx_trace_now ? getenv("SALSA64_GFX_MARK") : NULL;
				raster_mark = false;
				for (const char *q = mk; q && *q;) {
					char *e;
					unsigned long n = strtoul(q, &e, 10);
					if (e == q)
						break;
					if (n == ndraw)
						raster_mark = true;
					q = *e ? e + 1 : e;
				}
			}
#endif
			if (cur < 0)
				break;
			struct gfx_draw white;
			if (d->viscvg) {
				/* Cobertura plena en la RDRAM y blanco en la imagen (como rdp.c). */
				viscvg(job, cur, d);
				white = *d;
				white.ntex = 0;
				white.nterms = 0;
				white.blend = GFX_BLEND_OFF;
				white.atest = 0;
				white.fog = 0;
				white.depth = 0;
				memset(white.k, 0, sizeof white.k);
				for (int lane = 0; lane < 4; lane++)
					white.k[GFX_K_ONE][0][lane] = 1.0f;
				d = &white;
			}
			struct texsrc src[2];
			for (int i = 0; i < 2; i++) {
				src[i].slot = -1;
				if (i < d->ntex) {
					int k = texture_source(&d->tex[i], cur, &src[i]);
					if (k < 0 && raster_trace_fb && d->tex[i].src_addr) {
						int j = locate_any(d->tex[i].src_addr);
						if (j >= 0 && suspect[j]++ == 0)
							fprintf(stderr, "fb: campo %u tarea %u: textura %ux%u con origen %06x en el "
								"destino %06x/%u pero paso %u, siz %u, fmt %u\n", raster_field, task_no,
								d->tex[i].width, d->tex[i].height, d->tex[i].src_addr, tg.t[j].addr,
								tg.t[j].width, d->tex[i].src_pitch, d->tex[i].src_siz, d->tex[i].src_fmt);
					}
					if (k >= 0) {
						reads[k][cur]++;
						read_age[k] = tg.field - tg.t[k].last_field;
						if (src[i].slot < 0)
							read_stale[k][cur]++;
					}
				}
			}
			struct depth *zb = d->depth ? get_depth(d->zimg) : NULL;
			for (uint32_t i = 0; i + 3 <= d->h.b; i += 3)
				draw_triangle(d, vtx + d->h.a + i, &m, src, rgb[cur], zb);
			if (!drawn[cur])
				rdp_targets_drawn(&tg, cur);
			drawn[cur] = true;
			{
				/* Como rdp.c: hasta la fila más baja de los vértices. */
				float ymax = 0.0f;
				for (uint32_t k = 0; k < d->h.b; k++) {
					const struct gfx_vertex *v = vtx + d->h.a + k;
					if (v->w > 0.0f && v->y / v->w > ymax)
						ymax = v->y / v->w;
				}
				uint32_t rows = (uint32_t)(ymax + 0.999f), bottom = (d->scissor[3] + 3) / 4;
				if (rows > bottom)
					rows = bottom;
				if (rows > drawn_rows[cur])
					drawn_rows[cur] = rows;
			}
			break;
		}
		default:
			break;
		}
		off += c->size;
	}
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		for (int j = 0; raster_trace_fb && j < RDP_TARGETS_MAX; j++)
			if (reads[i][j])
				fprintf(stderr, "fb: campo %u tarea %u: %u dibujos leen el destino %06x/%u (dibujado hace %u "
					"tareas; %u de la RDRAM por viejo o ser el mismo) hacia %06x/%u\n", raster_field,
					task_no, reads[i][j], tg.t[i].addr, tg.t[i].width, read_age[i], read_stale[i][j],
					tg.t[j].addr, tg.t[j].width);
		if (!drawn[i])
			continue;
		/* Como en la PS3: lo decide el ancho del VI y sus orígenes. */
		const struct rdp_target *t = &tg.t[i];
		uint32_t rows = drawn_rows[i] < t->lh ? drawn_rows[i] : t->lh;
		if (raster_fbfx >= 2 && rdp_readback_wanted(&vi_ring, t->addr, t->width, t->bpp == 2 ? 2 : 0) &&
		    rdp_targets_readback(&tg, i, true)) {
			readback(job, i, rows);
			rb_task[i] = tg.task;
			readbacks++;
			if (raster_trace_fb)
				fprintf(stderr, "fb: campo %u tarea %u: copia a la RDRAM del destino %06x/%u: %u filas (alto conjeturado %u)\n",
					raster_field, task_no, t->addr, t->width, rows, t->lh);
		} else if (raster_fbfx >= 2 && rows && !t->presented && (t->bpp == 2 || (t->bpp == 1 && t->fmt == 4)) &&
			   t->width != tg.vi_width && t->width * t->bpp * rows <= MIRROR_BYTES) {
			/* Espejo de una imagen auxiliar chica (en la PS3, sin esperar). */
			readback(job, i, rows);
			rb_task[i] = tg.task;
			mirrors++;
			if (raster_trace_fb)
				fprintf(stderr, "fb: campo %u tarea %u: copia espejo del destino %06x/%u: %u filas (alto conjeturado %u)\n",
					raster_field, task_no, t->addr, t->width, rows, t->lh);
		} else if (raster_fbfx >= 2 && rdp_targets_mirror_at_task_end(&tg, i)) {
			/* Framebuffer: a la RDRAM al terminar la tarea que lo dibujó,
			 * como el RDP (en la PS3, sin esperar). */
			frame_mirror(job, i, rows, "al terminar la tarea");
		}
	}
}

void raster_vi(const struct salsa64_vi_frame *f)
{
	rdp_vi_ring_push(&vi_ring, f->origin, f->width);
	/* Como la PS3: qué destino se presenta (marca `presented`, el ancho del
	 * VI y la ranura protegida del reciclado). Sin esto el host copiaba a
	 * la RDRAM todos los destinos de cada tarea, framebuffers incluidos, y
	 * no reproducía la política de la consola. */
	struct rdp_present out;
	rdp_targets_present(&tg, f, ++vi_field, &out);
	/* Como la PS3: un framebuffer dibujado que aún no se copió a la RDRAM
	 * (no se sabía que lo era al terminar su tarea) se copia al
	 * presentarlo, solo las filas que muestra el VI. */
	if (raster_fbfx >= 2 && last_job.rdram_ea && out.slot >= 0)
		frame_mirror(&last_job, out.slot, out.y0 + out.h, "al presentarlo");
}

void raster_reset(void)
{
	memset(&tg, 0, sizeof tg);
	memset(rgb, 0, sizeof rgb);
	scratch_slot = 0;
	memset(depths, 0, sizeof depths);
	next_depth = 0;
	task_no = vi_field = 0;
	memset(&vi_ring, 0, sizeof vi_ring);
	primes = readbacks = mirrors = frames = 0;
	memset(rb_task, 0, sizeof rb_task);
	memset(&last_job, 0, sizeof last_job);
}

void raster_fb_stats(uint32_t *n_readbacks, uint32_t *n_primes, uint32_t *tex_rsx, uint32_t *tex_rdram)
{
	*n_readbacks = readbacks;
	*n_primes = primes;
	*tex_rsx = tg.tex_rsx;
	*tex_rdram = tg.tex_rdram;
}

const uint32_t *raster_image(uint32_t addr, uint32_t *width)
{
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &tg.t[i];
		if (t->used && addr >= t->addr && addr < t->addr + t->width * t->bpp * t->lh) {
			*width = t->width;
			return rgb[i];
		}
	}
	return NULL;
}

bool raster_target_info(int slot, uint32_t *addr, uint32_t *width, uint32_t *bpp, uint32_t *lh)
{
	if (slot < 0 || slot >= RDP_TARGETS_MAX || !tg.t[slot].used)
		return false;
	*addr = tg.t[slot].addr;
	*width = tg.t[slot].width;
	*bpp = tg.t[slot].bpp;
	*lh = tg.t[slot].lh;
	return true;
}
