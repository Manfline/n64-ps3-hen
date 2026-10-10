/*
 * Salsa64 — HLE de F3DEX2: matrices, viewport, luces y transformación de
 * vértices.
 *
 * Convenciones de la N64 (libultra, derivada de OpenGL): vectores fila,
 * clip = (x, y, z, 1) · MV · P; una matriz nueva multiplica por la
 * izquierda a la actual (M' = M_nueva · M). Pantalla: x = vtrans.x +
 * (x/w)·vscale.x, y = vtrans.y − (y/w)·vscale.y (la y crece hacia abajo),
 * z = vtrans.z + (z/w)·vscale.z en 0..0x3FF.
 *
 * Luces y lookat se llevan al espacio del modelo cuando cambia la
 * modelview (como hace el microcódigo) y se normalizan; la normal del
 * vértice (bytes con signo) se usa tal cual.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

static float inv_sqrt(float x)
{
	/* Newton desde una aproximación por bits: suficiente para normalizar. */
	union { float f; uint32_t u; } v = { x };
	v.u = 0x5F3759DFu - (v.u >> 1);
	float y = v.f;
	y = y * (1.5f - 0.5f * x * y * y);
	y = y * (1.5f - 0.5f * x * y * y);
	return y;
}

static void normalize3(float v[3])
{
	float d = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if (d > 1e-12f) {
		float k = inv_sqrt(d);
		v[0] *= k;
		v[1] *= k;
		v[2] *= k;
	}
}

static void identity(float m[16])
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void gfx_geom_reset(struct gfx_state *g)
{
	g->mv_top = 0;
	identity(g->mv[0]);
	identity(g->proj);
	identity(g->mvp);
	g->mvp_dirty = true;
	g->lights_dirty = true;
	memset(g->lights, 0, sizeof g->lights);
	g->num_lights = 0;
	g->lookat[0][0] = 1.0f, g->lookat[0][1] = 0.0f, g->lookat[0][2] = 0.0f;
	g->lookat[1][0] = 0.0f, g->lookat[1][1] = 1.0f, g->lookat[1][2] = 0.0f;
	g->vscale[0] = g->vtrans[0] = 160.0f;
	g->vscale[1] = g->vtrans[1] = 120.0f;
	g->vscale[2] = g->vtrans[2] = 511.0f;
	g->fog_mul = g->fog_off = 0.0f;
	memset(g->vtx, 0, sizeof g->vtx);
}

/* Mtx de libultra: 16 partes enteras (s16) y luego 16 fraccionarias (u16),
 * fila por fila; valor = (entero << 16 | fracción) / 65536. */
void gfx_load_matrix(float m[16], const uint8_t *src)
{
	for (int i = 0; i < 16; i++) {
		int32_t v = (int32_t)((uint32_t)salsa64_rd16(src + 2 * i) << 16 | salsa64_rd16(src + 32 + 2 * i));
		m[i] = (float)v * (1.0f / 65536.0f);
	}
}

void gfx_mtx_mul(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
				       a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
	memcpy(out, r, sizeof r);
}

/* Vp: vscale[4] y vtrans[4] (s16); x e y en cuartos de píxel. */
void gfx_set_viewport(struct gfx_state *g, const uint8_t *vp)
{
	for (int i = 0; i < 3; i++) {
		float k = i < 2 ? 0.25f : 1.0f;
		g->vscale[i] = (float)(int16_t)salsa64_rd16(vp + 2 * i) * k;
		g->vtrans[i] = (float)(int16_t)salsa64_rd16(vp + 8 + 2 * i) * k;
	}
}

/* Light: col[3], pad, colc[3], pad, dir[3] (s8), pad. */
void gfx_set_light(struct gfx_state *g, int index, const uint8_t *src)
{
	struct gfx_light *l = &g->lights[index];
	for (int i = 0; i < 3; i++) {
		l->col[i] = (float)src[i];
		l->dir[i] = (float)(int8_t)src[8 + i];
	}
	normalize3(l->dir);
	g->lights_dirty = true;
}

void gfx_set_lookat(struct gfx_state *g, int axis, const uint8_t *src)
{
	for (int i = 0; i < 3; i++)
		g->lookat[axis][i] = (float)(int8_t)src[8 + i];
	normalize3(g->lookat[axis]);
	g->lights_dirty = true;
}

/* Dirección en espacio de la cámara → espacio del modelo: d_m = M3 · d (la
 * transpuesta de cómo se transforma una normal con vectores fila). */
static void to_model(const float m[16], const float d[3], float out[3])
{
	for (int i = 0; i < 3; i++)
		out[i] = m[i * 4 + 0] * d[0] + m[i * 4 + 1] * d[1] + m[i * 4 + 2] * d[2];
	normalize3(out);
}

static void update_lights(struct gfx_state *g)
{
	const float *m = g->mv[g->mv_top];
	for (int i = 0; i < g->num_lights; i++)
		to_model(m, g->lights[i].dir, g->light_model[i]);
	to_model(m, g->lookat[0], g->lookat_model[0]);
	to_model(m, g->lookat[1], g->lookat_model[1]);
	g->lights_dirty = false;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* acos aproximado (error < 1e-4 rad) para G_TEXTURE_GEN_LINEAR. */
static float acos_approx(float x)
{
	x = clampf(x, -1.0f, 1.0f);
	float ax = x < 0.0f ? -x : x;
	float r = 1.5707288f + ax * (-0.2121144f + ax * (0.0742610f + ax * -0.0187293f));
	float s = 1.0f - ax;
	r *= s > 0.0f ? s * inv_sqrt(s) : 0.0f;  /* sqrt(1 − |x|) */
	return x < 0.0f ? 3.14159265f - r : r;
}

void gfx_load_vertices(struct gfx_state *g, uint32_t addr, uint32_t v0, uint32_t n)
{
	if (v0 >= GFX_MAX_VTX)
		return;
	if (n > GFX_MAX_VTX - v0)
		n = GFX_MAX_VTX - v0;
	const uint8_t *src = gfx_io_read(g, GFX_IO_VTX, addr, n * 16);
	if (!src) {
		g->res->dl_errors++;
		return;
	}
	if (g->mvp_dirty) {
		gfx_mtx_mul(g->mvp, g->mv[g->mv_top], g->proj);
		g->mvp_dirty = false;
	}
	bool lighting = (g->geom & G_LIGHTING) != 0;
	if (lighting && g->lights_dirty)
		update_lights(g);
	const float *m = g->mvp;
	/* s, t del vértice (s10.5) × escala de G_TEXTURE (0.16) → texels. */
	float ks = (float)g->tex_s / (65536.0f * 32.0f), kt = (float)g->tex_t / (65536.0f * 32.0f);

	for (uint32_t i = 0; i < n; i++, src += 16) {
		struct gfx_vtx *v = &g->vtx[v0 + i];
		float x = (float)(int16_t)salsa64_rd16(src + 0);
		float y = (float)(int16_t)salsa64_rd16(src + 2);
		float z = (float)(int16_t)salsa64_rd16(src + 4);
		v->x = x * m[0] + y * m[4] + z * m[8] + m[12];
		v->y = x * m[1] + y * m[5] + z * m[9] + m[13];
		v->z = x * m[2] + y * m[6] + z * m[10] + m[14];
		v->w = x * m[3] + y * m[7] + z * m[11] + m[15];

		uint32_t clip = 0;
		if (v->x < -v->w) clip |= GFX_CLIP_XN;
		if (v->x > v->w) clip |= GFX_CLIP_XP;
		if (v->y < -v->w) clip |= GFX_CLIP_YN;
		if (v->y > v->w) clip |= GFX_CLIP_YP;
		if (v->z > v->w) clip |= GFX_CLIP_ZF;
		if (v->w < 0.0f) clip |= GFX_CLIP_W;
		v->clip = clip;

		float iw = v->w != 0.0f ? 1.0f / v->w : 0.0f;
		v->sx = g->vtrans[0] + v->x * iw * g->vscale[0];
		v->sy = g->vtrans[1] - v->y * iw * g->vscale[1];
		v->sz = g->vtrans[2] + v->z * iw * g->vscale[2];

		float s = (float)(int16_t)salsa64_rd16(src + 8);
		float t = (float)(int16_t)salsa64_rd16(src + 10);
		const uint8_t *c = src + 12;

		if (lighting) {
			float nrm[3] = { (float)(int8_t)c[0], (float)(int8_t)c[1], (float)(int8_t)c[2] };
			normalize3(nrm);
			const struct gfx_light *amb = &g->lights[g->num_lights];
			float rgb[3] = { amb->col[0], amb->col[1], amb->col[2] };
			for (int l = 0; l < g->num_lights; l++) {
				const float *d = g->light_model[l];
				float k = nrm[0] * d[0] + nrm[1] * d[1] + nrm[2] * d[2];
				if (k > 0.0f) {
					rgb[0] += k * g->lights[l].col[0];
					rgb[1] += k * g->lights[l].col[1];
					rgb[2] += k * g->lights[l].col[2];
				}
			}
			for (int k = 0; k < 3; k++)
				v->c[k] = (uint8_t)clampf(rgb[k] + 0.5f, 0.0f, 255.0f);
			v->c[3] = c[3];

			if (g->geom & G_TEXTURE_GEN) {
				const float *lx = g->lookat_model[0], *ly = g->lookat_model[1];
				float dx = nrm[0] * lx[0] + nrm[1] * lx[1] + nrm[2] * lx[2];
				float dy = nrm[0] * ly[0] + nrm[1] * ly[1] + nrm[2] * ly[2];
				/* En unidades s10.5, antes de la escala de G_TEXTURE:
				 * (d + 1)/2 en 0..1 llega a 1024 texels; con la escala
				 * típica de mapeo de entorno (0x07C0) quedan ~31. */
				if (g->geom & G_TEXTURE_GEN_LINEAR) {
					s = acos_approx(-dx) * (1024.0f * 32.0f / 3.14159265f);
					t = acos_approx(-dy) * (1024.0f * 32.0f / 3.14159265f);
				} else {
					s = (dx + 1.0f) * (512.0f * 32.0f);
					t = (dy + 1.0f) * (512.0f * 32.0f);
				}
			}
		} else {
			v->c[0] = c[0];
			v->c[1] = c[1];
			v->c[2] = c[2];
			v->c[3] = c[3];
		}
		v->s = s * ks;
		v->t = t * kt;

		if (g->geom & G_FOG) {
			/* Alfa = factor de niebla según la profundidad (z/w se fija
			 * en −1 delante del plano cercano o detrás de la cámara). */
			float zw = v->w > 0.0f && v->z >= -v->w ? v->z * iw : -1.0f;
			float f = zw * g->fog_mul + g->fog_off;
			v->c[3] = (uint8_t)clampf(f + 0.5f, 0.0f, 255.0f);
		}
	}
}

void gfx_vtx_out(const struct gfx_state *g, const struct gfx_vtx *v, struct gfx_vertex *o)
{
	o->x = g->vtrans[0] * v->w + g->vscale[0] * v->x;
	o->y = g->vtrans[1] * v->w - g->vscale[1] * v->y;
	if (g->ol & G_ZS_PRIM)
		o->z = (float)g->prim_z / 32767.0f * v->w;
	else
		o->z = (g->vtrans[2] * v->w + g->vscale[2] * v->z) * (1.0f / 1023.0f);
	o->w = v->w;
	o->s = v->s;
	o->t = v->t;
	o->r = v->c[0];
	o->g = v->c[1];
	o->b = v->c[2];
	o->a = v->c[3];
	o->pad = 0;
}
