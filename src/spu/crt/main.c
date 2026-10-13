/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — programa SPU del CRT con cable (filtro original del autor
 * portado de su versión de RetroMac, v1.1): la señal y la decodificación de
 * S-Video y compuesto de un grupo de líneas, la misma cuenta de
 * crt_cable_signal.fcg y crt_cable_decode.cgh (en float, sin la gamma ni la
 * saturación, que hace el RSX). Protocolo y estructuras en
 * src/frontend/filters/crt_spe.h.
 *
 * Trabajador de lib/spukit: cada trabajo es un cuadro. Espera la etiqueta
 * del RSX (la imagen reducida ya está en la memoria principal), trae la
 * configuración si cambió, y por cada grupo de 4 líneas: trae sus filas
 * (doble búfer), las pasa a Y, I, Q por píxel, arma la señal de cada
 * muestra y la decodifica con las 27 muestras del filtro de la tele, y
 * manda las filas de la tabla (doble búfer). Al final escribe su etiqueta.
 *
 * Las 4 líneas del grupo van en los 4 carriles de cada vector: así las
 * muestras de una columna se leen alineadas y el filtro es una FMA por
 * muestra, canal y toma, sin barajar (la fase de la subportadora de cada
 * línea va en su carril).
 *
 * Etiquetas DMA: 0 y 1 filas de entrada, 2 y 3 filas de salida, 4 la
 * configuración, 5 las etiquetas del RSX. La 31 es de spukit.
 *
 * Presupuesto de local store (256 KB); el build imprime los números exactos:
 *   configuración (struct crt_spe_config)        28 KB
 *   filas de entrada 2 × 4 × 2560 bytes         20 KB
 *   píxeles Y, I, Q (656 vectores cada uno)      31 KB
 *   señal L, I, Q (832 vectores cada uno)        39 KB
 *   filas de salida 2 × 4 × 6144 bytes           48 KB
 *   código, spukit y pila                        el resto (~90 KB)
 */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/spu_thread.h>

#include <spukit/spu.h>

#include "frontend/filters/crt_spe.h"

#define TAG_IN(b)  (b)
#define TAG_OUT(b) (2u + (b))
#define TAG_CFG    4u
#define TAG_LABEL  5u

#define SIG_N (CRT_SPE_MAX_COLS + 2 * CRT_SPE_PAD)
#define PX_N  (CRT_SPE_MAX_SW + 2 * CRT_SPE_PX_PAD)
#define ROW_IN  (CRT_SPE_MAX_SW * 4)
#define ROW_OUT (CRT_SPE_MAX_COLS * 8)

_Static_assert(sizeof(struct crt_spe_job) <= SPUKIT_DATA_SIZE, "crt_spe_job no cabe en el descriptor de spukit");
_Static_assert(sizeof(struct crt_spe_result) <= SPUKIT_RESULT_SIZE, "crt_spe_result no cabe en el resultado");

static struct crt_spe_config cfg;
static uint32_t cfg_id;
static uint8_t row_in[2][4][ROW_IN] __attribute__((aligned(128)));
static const uint8_t row_zero[ROW_IN] __attribute__((aligned(128)));
static vec_float4 pY[PX_N], pI[PX_N], pQ[PX_N];
static vec_float4 sL[SIG_N], sI[SIG_N], sQ[SIG_N];
static uint8_t row_out[2][4][ROW_OUT] __attribute__((aligned(128)));
static volatile uint32_t label_buf[4] __attribute__((aligned(16)));

/* Trasposición de 4 × 4 palabras: out[i] = (a[i], b[i], c[i], d[i]). */
static const vec_uchar16 T_LO = { 0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23 };
static const vec_uchar16 T_HI = { 8, 9, 10, 11, 24, 25, 26, 27, 12, 13, 14, 15, 28, 29, 30, 31 };
static const vec_uchar16 T_A = { 0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23 };
static const vec_uchar16 T_B = { 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31 };
/* (R, G) en una palabra: la mitad baja de cada palabra de cada uno. */
static const vec_uchar16 RG = { 2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31 };

static inline void transpose4(vec_uint4 a, vec_uint4 b, vec_uint4 c, vec_uint4 d, vec_uint4 out[4])
{
	vec_uint4 t0 = spu_shuffle(a, b, T_LO), t1 = spu_shuffle(c, d, T_LO);
	vec_uint4 t2 = spu_shuffle(a, b, T_HI), t3 = spu_shuffle(c, d, T_HI);
	out[0] = spu_shuffle(t0, t1, T_A);
	out[1] = spu_shuffle(t0, t1, T_B);
	out[2] = spu_shuffle(t2, t3, T_A);
	out[3] = spu_shuffle(t2, t3, T_B);
}

/* float → fp16 (redondeo hacia arriba en el medio, sin subnormales), en la
 * mitad baja de cada palabra. */
static inline vec_uint4 to_half(vec_float4 f)
{
	vec_uint4 b = (vec_uint4)f;
	vec_uint4 sign = spu_and(spu_rlmask(b, -16), 0x8000u);
	vec_uint4 mag = spu_and(b, 0x7FFFFFFFu);
	vec_uint4 h = spu_rlmask(spu_add(spu_sub(mag, spu_splats(0x38000000u)), spu_splats(0x1000u)), -13);
	h = spu_sel(h, spu_splats(0u), spu_cmpgt(spu_splats(0x38800000u), mag));  /* < 2^-14: 0 */
	h = spu_sel(h, spu_splats(0x7BFFu), spu_cmpgt(mag, spu_splats(0x477FE000u)));  /* > 65504 */
	return spu_or(h, sign);
}

static uint32_t read_label(uint64_t ea)
{
	mfc_get(label_buf, ea & ~15ull, 16, TAG_LABEL, 0, 0);
	spukit_dma_wait(1u << TAG_LABEL);
	return label_buf[(ea & 15) >> 2];
}

static void write_label(uint64_t ea, uint32_t v)
{
	unsigned i = (unsigned)(ea & 15) >> 2;
	label_buf[i] = v;
	mfc_put(&label_buf[i], ea, 4, TAG_LABEL, 0, 0);
	spukit_dma_wait(1u << TAG_LABEL);
}

/* Las filas y0 .. y0 + n − 1 a los vectores de píxeles (carril = línea),
 * en Y, I, Q de la FCC sobre los valores codificados. */
static void pixels(uint8_t (*rows)[ROW_IN], unsigned n)
{
	const vec_float4 k = spu_splats(1.0f / 255.0f);
	const vec_float4 yr = spu_splats(cfg.yiq[0][0]), yg = spu_splats(cfg.yiq[0][1]), yb = spu_splats(cfg.yiq[0][2]);
	const vec_float4 ir = spu_splats(cfg.yiq[1][0]), ig = spu_splats(cfg.yiq[1][1]), ib = spu_splats(cfg.yiq[1][2]);
	const vec_float4 qr = spu_splats(cfg.yiq[2][0]), qg = spu_splats(cfg.yiq[2][1]), qb = spu_splats(cfg.yiq[2][2]);
	const vec_uint4 *r[4];
	for (unsigned l = 0; l < 4; l++)
		r[l] = (const vec_uint4 *)(l < n ? rows[l] : row_zero);
	unsigned quads = (cfg.sw + 3) / 4;
	for (unsigned q = 0; q < quads; q++) {
		vec_uint4 t[4];
		transpose4(r[0][q], r[1][q], r[2][q], r[3][q], t);
		for (unsigned p = 0; p < 4; p++) {
			unsigned i = 4 * q + p;
			vec_float4 R = spu_mul(spu_convtf(spu_and(spu_rlmask(t[p], -16), 0xFFu), 0), k);
			vec_float4 G = spu_mul(spu_convtf(spu_and(spu_rlmask(t[p], -8), 0xFFu), 0), k);
			vec_float4 B = spu_mul(spu_convtf(spu_and(t[p], 0xFFu), 0), k);
			vec_float4 z = spu_splats(0.0f);
			bool in = i < cfg.sw;
			pY[CRT_SPE_PX_PAD + i] = in ? spu_madd(yb, B, spu_madd(yg, G, spu_mul(yr, R))) : z;
			pI[CRT_SPE_PX_PAD + i] = in ? spu_madd(ib, B, spu_madd(ig, G, spu_mul(ir, R))) : z;
			pQ[CRT_SPE_PX_PAD + i] = in ? spu_madd(qb, B, spu_madd(qg, G, spu_mul(qr, R))) : z;
		}
	}
}

/* La señal de cada muestra de la tabla `g` en las 4 líneas desde y0:
 * luma = Y + kc·C, color = 2 (C + kc·Y) (cos θ, sin θ), con C = I cos θ +
 * Q sin θ y la fase de cada línea en su carril. */
static void signal(unsigned g, unsigned y0)
{
	vec_float4 cl, sl;
	float c4[4], s4[4];
	for (unsigned l = 0; l < 4; l++) {
		unsigned y = y0 + l < CRT_SPE_MAX_LINES ? y0 + l : 0;
		c4[l] = cfg.line[y][0];
		s4[l] = cfg.line[y][1];
	}
	cl = (vec_float4){ c4[0], c4[1], c4[2], c4[3] };
	sl = (vec_float4){ s4[0], s4[1], s4[2], s4[3] };
	const vec_float4 kc = spu_splats(cfg.kc), two = spu_splats(2.0f);
	const struct crt_spe_col *col = cfg.col[g];
	for (unsigned m = 0; m < cfg.cols; m++) {
		int i = CRT_SPE_PX_PAD + col[m].n;
		vec_float4 t = spu_splats(col[m].t), c0 = spu_splats(col[m].c), s0 = spu_splats(col[m].s);
		vec_float4 ys = spu_madd(t, spu_sub(pY[i], pY[i - 1]), pY[i - 1]);
		vec_float4 is = spu_madd(t, spu_sub(pI[i], pI[i - 1]), pI[i - 1]);
		vec_float4 qs = spu_madd(t, spu_sub(pQ[i], pQ[i - 1]), pQ[i - 1]);
		vec_float4 c = spu_nmsub(s0, sl, spu_mul(c0, cl));  /* cos(θ + φ) */
		vec_float4 s = spu_madd(c0, sl, spu_mul(s0, cl));   /* sin(θ + φ) */
		vec_float4 C = spu_madd(qs, s, spu_mul(is, c));
		vec_float4 cc = spu_mul(two, spu_madd(kc, ys, C));
		sL[CRT_SPE_PAD + m] = spu_madd(kc, C, ys);
		sI[CRT_SPE_PAD + m] = spu_mul(cc, c);
		sQ[CRT_SPE_PAD + m] = spu_mul(cc, s);
	}
}

/* Los pesos de las 27 tomas como variables (en registros: el filtro es una
 * FMA por toma y canal, sin cargas de los pesos). */
#define TAPS(X)                                                                                 \
	X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) \
	X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26)
#define WEIGHT(j) const vec_float4 wl##j = spu_splats(cfg.wl[j]), wc##j = spu_splats(cfg.wc[j]);
#define TAP(j)                                      \
	aL = spu_madd(wl##j, sL[b + (j)], aL);      \
	aI = spu_madd(wc##j, sI[b + (j)], aI);      \
	aQ = spu_madd(wc##j, sQ[b + (j)], aQ);
/* La columna m (las 4 líneas): R, G, B antes de la gamma, en fp16 en la
 * mitad baja de cada palabra. */
#define DECODE_COL(m, R, G, B)                                                              \
	do {                                                                                \
		int b = CRT_SPE_PAD + (int)(m) + cfg.jlo;                                   \
		vec_float4 aL = spu_splats(0.0f), aI = aL, aQ = aL;                         \
		TAPS(TAP)                                                                   \
		R = to_half(spu_madd(m02, aQ, spu_madd(m01, aI, spu_mul(m00, aL))));        \
		G = to_half(spu_madd(m12, aQ, spu_madd(m11, aI, spu_mul(m10, aL))));        \
		B = to_half(spu_madd(m22, aQ, spu_madd(m21, aI, spu_mul(m20, aL))));        \
	} while (0)

/* Las filas de la tabla de las 4 líneas (quien llama manda las que valen). */
static void decode(uint8_t (*out)[ROW_OUT])
{
	TAPS(WEIGHT)
	const vec_float4 m00 = spu_splats(cfg.rgb[0][0]), m01 = spu_splats(cfg.rgb[0][1]), m02 = spu_splats(cfg.rgb[0][2]);
	const vec_float4 m10 = spu_splats(cfg.rgb[1][0]), m11 = spu_splats(cfg.rgb[1][1]), m12 = spu_splats(cfg.rgb[1][2]);
	const vec_float4 m20 = spu_splats(cfg.rgb[2][0]), m21 = spu_splats(cfg.rgb[2][1]), m22 = spu_splats(cfg.rgb[2][2]);
	for (unsigned m = 0; m < cfg.cols; m += 2) {
		vec_uint4 r0, g0, b0, r1, g1, b1, o[4];
		DECODE_COL(m, r0, g0, b0);
		DECODE_COL(m + 1, r1, g1, b1);
		/* Cada muestra: (R, G) y (B, 0) en dos palabras; por línea, dos
		 * muestras seguidas en un vector. */
		transpose4(spu_shuffle(r0, g0, RG), spu_sl(b0, 16), spu_shuffle(r1, g1, RG), spu_sl(b1, 16), o);
		for (unsigned l = 0; l < 4; l++)
			*(vec_uint4 *)(out[l] + m * 8) = o[l];
	}
}

static void get_rows(const struct crt_spe_job *j, unsigned buf, unsigned y, unsigned n)
{
	uint32_t len = (cfg.sw * 4 + 15) & ~15u;
	for (unsigned l = 0; l < n; l++)
		spukit_dma_get(row_in[buf][l], j->src_ea + (uint64_t)(y + l) * j->src_pitch, len, TAG_IN(buf));
}

static int run(const struct crt_spe_job *j, struct crt_spe_result *res)
{
	uint32_t t0 = spukit_ticks();
	if (j->config_id != cfg_id) {
		if (j->config_bytes > sizeof cfg)
			return -1;
		spukit_dma_get(&cfg, j->config_ea, (j->config_bytes + 15) & ~15u, TAG_CFG);
		spukit_dma_wait(1u << TAG_CFG);
		cfg_id = j->config_id;
	}
	if (cfg.sw > CRT_SPE_MAX_SW || cfg.cols > CRT_SPE_MAX_COLS || cfg.sh > CRT_SPE_MAX_LINES || cfg.grids < 1 ||
	    cfg.grids > 2 || cfg.jlo < -(int32_t)CRT_SPE_PAD || cfg.jlo + CRT_SPE_TAPS + 1 > (int32_t)CRT_SPE_PAD ||
	    j->y1 > cfg.sh || (j->y0 & 3))
		return -2;
	/* Lo de fuera de la imagen vale 0 (el modo BORDER del RSX). */
	memset(pY, 0, sizeof pY);
	memset(pI, 0, sizeof pI);
	memset(pQ, 0, sizeof pQ);
	memset(sL, 0, sizeof sL);
	memset(sI, 0, sizeof sI);
	memset(sQ, 0, sizeof sQ);
	/* La imagen está cuando el RSX escribió su etiqueta (2 s como mucho). */
	uint32_t limit = 2u * 79800000u;
	while ((int32_t)(read_label(j->label_in_ea) - j->seq) < 0) {
		if (t0 - spukit_ticks() > limit)
			return -3;
		for (volatile int k = 0; k < 64; k++)
			;
	}
	uint32_t t1 = spukit_ticks();
	res->wait_ticks = t0 - t1;
	uint32_t out_len = (cfg.cols * 8 + 15) & ~15u;
	unsigned bi = 0, bo = 0;
	if (j->y0 < j->y1)
		get_rows(j, 0, j->y0, j->y1 - j->y0 < 4 ? j->y1 - j->y0 : 4);
	for (unsigned y = j->y0; y < j->y1; y += 4) {
		unsigned n = j->y1 - y < 4 ? j->y1 - y : 4;
		if (y + 4 < j->y1)
			get_rows(j, bi ^ 1, y + 4, j->y1 - (y + 4) < 4 ? j->y1 - (y + 4) : 4);
		spukit_dma_wait(1u << TAG_IN(bi));
		pixels(row_in[bi], n);
		for (unsigned g = 0; g < cfg.grids; g++) {
			signal(g, y);
			spukit_dma_wait(1u << TAG_OUT(bo));  /* lo que salió de este búfer dos vueltas antes */
			decode(row_out[bo]);
			for (unsigned l = 0; l < n; l++)
				spukit_dma_put(row_out[bo][l], j->out_ea[g] + (uint64_t)(y + l) * j->out_pitch, out_len,
					       TAG_OUT(bo));
			bo ^= 1;
		}
		bi ^= 1;
	}
	spukit_dma_wait(1u << TAG_OUT(0) | 1u << TAG_OUT(1));
	mfc_sync(TAG_LABEL);
	write_label(j->label_out_ea, j->seq);
	res->work_ticks = t1 - spukit_ticks();
	res->lines = j->y1 - j->y0;
	return 0;
}

static int handler(void *ctx, const spukit_job *j, spukit_result *res)
{
	(void)ctx;
	struct crt_spe_job job;
	struct crt_spe_result r;
	if (j->kind != CRT_SPE_JOB)
		return -1;
	memcpy(&job, j->data, sizeof job);
	memset(&r, 0, sizeof r);
	r.err = run(&job, &r);
	memcpy(res->data, &r, sizeof r);
	return r.err;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg, (void)a2, (void)a3;
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
