/*
 * Salsa64 — rasterizador de referencia en software para la salida de la
 * HLE gráfica (src/rsp/gfx/gfx.h), solo para el host.
 *
 * Hace lo mismo que debe hacer el RSX con esa salida (src/rdp y
 * lib/rsxkit), a la resolución de la N64 y sin buscar velocidad: sirve para
 * ver y verificar en el host lo que produce la HLE (tools/test-roms.sh con
 * gfxtest) y como referencia cuando el RSX muestre otra cosa.
 *
 * Simplificaciones: sin recorte contra los planos (se descartan triángulos
 * con algún w ≤ 0), muestreo en el centro del píxel, regla de cobertura
 * por arriba a la izquierda aproximada.
 */
#include "raster.h"

#include <math.h>
#include <string.h>

#define MAX_TARGETS 4

struct target {
	uint32_t addr;
	uint32_t width;
	bool used;
	uint32_t rgb[RASTER_W * RASTER_H];
};

struct depth {
	uint32_t addr;
	bool used;
	float z[RASTER_W * RASTER_H];
};

static struct target targets[MAX_TARGETS];
static struct depth depths[MAX_TARGETS];
static unsigned next_target, next_depth;

static struct target *get_target(uint32_t addr, uint32_t width)
{
	for (int i = 0; i < MAX_TARGETS; i++)
		if (targets[i].used && targets[i].addr == addr)
			return &targets[i];
	struct target *t = &targets[next_target++ % MAX_TARGETS];
	t->used = true;
	t->addr = addr;
	t->width = width;
	memset(t->rgb, 0, sizeof t->rgb);
	return t;
}

static struct depth *get_depth(uint32_t addr)
{
	for (int i = 0; i < MAX_TARGETS; i++)
		if (depths[i].used && depths[i].addr == addr)
			return &depths[i];
	struct depth *d = &depths[next_depth++ % MAX_TARGETS];
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
#include <stdio.h>
#include <stdlib.h>
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

static void fetch(const struct gfx_tex *t, const struct texmem *m, int x, int y, float out[4])
{
	x = wrap(x, t->width, GFX_TEX_WRAP_S(t->flags));
	y = wrap(y, t->height, GFX_TEX_WRAP_T(t->flags));
	const uint8_t *p = m->base[t->where & 1] + t->offset + (uint32_t)y * t->pitch + 4u * (uint32_t)x;
	for (int k = 0; k < 4; k++)
		out[k] = p[k] / 255.0f;
}

/* Muestra con semántica del RSX: u, v normalizados; centros de texel en
 * (i + 0,5) / tamaño. */
static void sample(const struct gfx_tex *t, const struct texmem *m, float u, float v, float out[4])
{
	float x = u * t->width, y = v * t->height;
	if (!(t->flags & GFX_TEX_BILINEAR)) {
		fetch(t, m, (int)floorf(x), (int)floorf(y), out);
		return;
	}
	x -= 0.5f;
	y -= 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0, a[4], b[4], c[4], d[4];
	fetch(t, m, x0, y0, a);
	fetch(t, m, x0 + 1, y0, b);
	fetch(t, m, x0, y0 + 1, c);
	fetch(t, m, x0 + 1, y0 + 1, d);
	for (int k = 0; k < 4; k++)
		out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void draw_triangle(const struct gfx_draw *d, const struct gfx_vertex *v, const struct texmem *m,
			  struct target *tg, struct depth *zb)
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
				sample(te, m, s * te->xf[0] + te->xf[2], t * te->xf[1] + te->xf[3], tx[i]);
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
			uint32_t old = tg->rgb[idx];
			float dst[3] = { (old >> 16 & 0xFF) / 255.0f, (old >> 8 & 0xFF) / 255.0f, (old & 0xFF) / 255.0f };
			if (d->blend == GFX_BLEND_KEEP)
				continue;
			for (int l = 0; l < 3; l++) {
				if (d->blend == GFX_BLEND_ALPHA)
					out[l] = out[l] * out[3] + dst[l] * (1.0f - out[3]);
				else if (d->blend == GFX_BLEND_ADD)
					out[l] = clamp01(out[l] * out[3] + dst[l]);
			}
			tg->rgb[idx] = (uint32_t)(out[0] * 255.0f + 0.5f) << 16 | (uint32_t)(out[1] * 255.0f + 0.5f) << 8 |
				       (uint32_t)(out[2] * 255.0f + 0.5f);
			if ((d->depth & GFX_DEPTH_WRITE) && zb)
				zb->z[idx] = z;
		}
}

void raster_task(const struct gfx_job *job, const struct gfx_result *res)
{
	const uint8_t *cmd = (const uint8_t *)(uintptr_t)job->cmd_ea;
	const struct gfx_vertex *vtx = (const struct gfx_vertex *)(uintptr_t)job->vtx_ea;
	const struct texmem m = { { (const uint8_t *)(uintptr_t)job->tex_ea, (const uint8_t *)(uintptr_t)job->cache_ea } };
	struct target *tg = NULL;
	uint32_t off = 0;
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
			tg = get_target(c->a, c->b);
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
			if (!tg)
				break;
			struct depth *zb = d->depth ? get_depth(d->zimg) : NULL;
			for (uint32_t i = 0; i + 3 <= d->h.b; i += 3)
				draw_triangle(d, vtx + d->h.a + i, &m, tg, zb);
			break;
		}
		default:
			break;
		}
		off += c->size;
	}
}

const uint32_t *raster_image(uint32_t addr, uint32_t *width)
{
	for (int i = 0; i < MAX_TARGETS; i++)
		if (targets[i].used && addr >= targets[i].addr &&
		    addr < targets[i].addr + targets[i].width * RASTER_H * 4) {
			*width = targets[i].width;
			return targets[i].rgb;
		}
	return NULL;
}
