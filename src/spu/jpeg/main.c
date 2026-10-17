/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — programa SPU de la tarea JPEG (fondos prerrenderizados de
 * Zelda OoT) y de las tareas de video de Resident Evil 2, en uno de los SPEs
 * libres.
 *
 * Trabajador de lib/spukit: cada trabajo es una tarea JPEG completa (struct
 * jpeg_job: dónde está la RDRAM y la estructura de la tarea). Trae la
 * estructura y las tres tablas de cuantización, y los macrobloques con
 * doble búfer: mientras uno se decodifica llega el siguiente y sale el
 * anterior, a su mismo lugar de la RDRAM. Devuelve un struct
 * jpeg_job_result, igual al de salsa64_jpeg_run. La decodificación es la
 * versión en 4 carriles (src/rsp/jpeg/jpeg_v.c): lo mismo byte a byte que
 * la escalar del PPE y el host (prueba unitaria y prueba del arranque),
 * unas 15 veces menos operaciones.
 *
 * Video de Resident Evil 2 (src/rsp/jpeg/re2.c): tres tareas (RESIZE,
 * DECODE y FILL) con el mismo patrón: el segmento de datos de 128 bytes con
 * los parámetros y, fila a fila (o par de filas), doble búfer de entrada, el
 * núcleo de re2.c (el mismo código del PPE y el host: el resultado es igual
 * byte a byte) y salida por DMA. Las direcciones de la RDRAM vienen alineadas
 * a 8 pero no a 16: la entrada se trae con el tramo alineado que la contiene
 * y la salida se arma en el búfer con el mismo resto módulo 16 que su
 * dirección, para ponerla con DMA de 8 bytes en los extremos y de 16 · n en
 * el centro. Si la HLE del SPE no puede con una tarea (parámetros fuera de
 * lo medido), devuelve error y la hace el PPE.
 *
 * Etiquetas DMA: 0 y 1 entrada de cada búfer, 2 y 3 su salida, 4 la
 * estructura y las tablas. La 31 es de spukit.
 *
 * Presupuesto de local store (256 KB); los números exactos los imprime el
 * build (spu-size):
 *   código (jpeg.c, jpeg_v.c, re2.c, bucle) ~16 KB
 *   constantes de la IDCT (jpeg_v.c)      1,3 KB
 *   macrobloques 2 × 768 + 2 × 512        2,5 KB
 *   estructura y tablas (3 × 160 + 64)   < 1 KB
 *   spukit (descriptor, resultado…)      < 1 KB
 *   tablas de cuantización preparadas    0,75 KB
 *   video: DMEM de RESIZE 4 KB, filas de entrada 2 × 1,9 KB y de salida
 *     2 × 2 KB; DECODE: Y 2 × 0,5 KB, Cb y Cr 2 × 2 × 0,27 KB y salida
 *     2 × 2 × 1 KB; FILL: 2 × 2 × 2 KB                   ~26 KB
 *   pila                                 el resto (un macrobloque usa
 *                                        ~2 KB: 6 bloques de 16 v4 y dos
 *                                        de trabajo)
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "rsp/jpeg/jpeg.h"
#include "rsp/jpeg/jpeg_v.h"
#include "rsp/jpeg/job.h"
#include "rsp/jpeg/re2.h"

#define TAG_IN(b)  (b)
#define TAG_OUT(b) (2u + (b))
#define TAG_PARAM  4u

_Static_assert(sizeof(struct jpeg_job) <= SPUKIT_DATA_SIZE, "jpeg_job no cabe en el descriptor de spukit");
_Static_assert(sizeof(struct jpeg_job_result) <= SPUKIT_RESULT_SIZE, "jpeg_job_result no cabe en el resultado");

static uint8_t in_buf[2][SALSA64_JPEG_MB_IN(2)] __attribute__((aligned(128)));
static uint8_t out_buf[2][SALSA64_JPEG_MB_OUT(2)] __attribute__((aligned(128)));
static uint8_t task_buf[SALSA64_JPEG_TASK_BYTES + 32] __attribute__((aligned(16)));
static uint8_t q_buf[3][128 + 32] __attribute__((aligned(16)));
static struct salsa64_jpeg_qv qv;

static bool in_rdram(const struct jpeg_job *j, uint32_t addr, uint32_t size)
{
	return addr <= j->rdram_size && size <= j->rdram_size - addr;
}

/* ---- Video de Resident Evil 2 ---- */

/* Trae `size` bytes desde `ea` (cualquier alineación) al búfer `buf` (alineado a 16, de al menos
 * size + 32 bytes) sin esperar; devuelve dónde quedó el primer byte (con el mismo resto módulo 16
 * que `ea`). */
static inline uint8_t *dma_get_any(uint8_t *buf, uint64_t ea, uint32_t size, uint32_t tag)
{
	uint64_t start = ea & ~15ull;
	uint32_t len = (uint32_t)(((ea + size + 15) & ~15ull) - start);
	spukit_dma_get(buf, start, len, tag);
	return buf + (ea - start);
}

/* Escribe `size` bytes (múltiplo de 8, desde un `ls` con el mismo resto módulo 16 que `ea`, que es
 * múltiplo de 8): 8 bytes al principio si ea no está alineada a 16, el centro de 16 en 16 y 8 al
 * final si sobran. No espera. */
static inline void dma_put8(const uint8_t *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	if (ea & 8) {
		mfc_put((volatile void *)ls, ea, 8, tag, 0, 0);
		ls += 8;
		ea += 8;
		size -= 8;
	}
	uint32_t body = size & ~15u;
	if (body)
		spukit_dma_put(ls, ea, body, tag);
	if (size & 8)
		mfc_put((volatile void *)(ls + body), ea + body, 8, tag, 0, 0);
}

static inline bool fits(const struct jpeg_job *j, uint64_t addr, uint64_t n)
{
	return addr <= j->rdram_size && n <= j->rdram_size - addr;
}

#define RE2_MAX_ROW 2048u  /* bytes de una fila de RESIZE (ancho 1024) o de FILL (filas · largo ≤ 2048) */

static uint8_t seg_buf[SALSA64_RE2_DATA_BYTES + 32] __attribute__((aligned(16)));
static uint8_t dm[SALSA64_RE2_DMEM] __attribute__((aligned(128)));
static uint8_t rz_in[2][2 * SALSA64_RE2_SRC_ROW + 32] __attribute__((aligned(128)));
static uint8_t rz_out[2][RE2_MAX_ROW + 32] __attribute__((aligned(128)));
static uint8_t dc_y[2][2 * 240 + 32] __attribute__((aligned(128)));
static uint8_t dc_cb[2][240 + 32] __attribute__((aligned(128)));
static uint8_t dc_cr[2][240 + 32] __attribute__((aligned(128)));
static uint8_t dc_out[2][2][4 * 240 + 32] __attribute__((aligned(128)));
static uint8_t fl_src[2][RE2_MAX_ROW + 32] __attribute__((aligned(128)));
static uint8_t fl_dst[2][RE2_MAX_ROW + 32] __attribute__((aligned(128)));

static int run_resize(const struct jpeg_job *j, const uint8_t *seg, struct jpeg_job_result *res)
{
	struct salsa64_re2_resize t;
	struct salsa64_re2_dither st;
	if (!salsa64_re2_resize_read(seg, &t))
		return JPEG_ERR_KIND;
	const uint32_t row = 2 * t.width;
	res->out_addr = t.dst;
	res->mb_stride = t.stride;
	res->mb_out = row;
	/* Las filas que caben en la RDRAM (como run_resize de re2.c). */
	uint32_t n = 0;
	while (n < t.rows && fits(j, salsa64_re2_resize_src(&t, n), 2 * SALSA64_RE2_SRC_ROW) &&
	       fits(j, (uint64_t)t.dst + (uint64_t)n * t.stride, row))
		n++;
	res->bad = n < t.rows;
	salsa64_re2_resize_begin(&st, j->dram_stack, j->dram_stack_size);
	memset(dm, 0, sizeof dm);
	const uint8_t *src[2] = { 0, 0 };
	if (n)
		src[0] = dma_get_any(rz_in[0], j->rdram_ea + salsa64_re2_resize_src(&t, 0), 2 * SALSA64_RE2_SRC_ROW,
				     TAG_IN(0));
	for (uint32_t m = 0; m < n; m++) {
		unsigned b = m & 1;
		if (m + 1 < n)
			src[b ^ 1] = dma_get_any(rz_in[b ^ 1], j->rdram_ea + salsa64_re2_resize_src(&t, m + 1),
						 2 * SALSA64_RE2_SRC_ROW, TAG_IN(b ^ 1));
		spukit_dma_wait(1u << TAG_IN(b) | 1u << TAG_OUT(b));  /* la entrada llegó y la salida de m − 2 salió */
		memcpy(dm, src[b], 2 * SALSA64_RE2_SRC_ROW);
		salsa64_re2_resize_row(dm, &t, &st, m);
		uint64_t o = (uint64_t)t.dst + (uint64_t)m * t.stride;
		uint8_t *ls = rz_out[b] + (o & 15);
		memcpy(ls, dm + SALSA64_RE2_OUT_BASE, row);
		dma_put8(ls, j->rdram_ea + o, row, TAG_OUT(b));
	}
	spukit_dma_wait(1u << TAG_OUT(0) | 1u << TAG_OUT(1));
	res->macroblocks = n;
	res->out_bytes = n ? (n - 1) * t.stride + row : 0;
	return 0;
}

static int run_decode(const struct jpeg_job *j, const uint8_t *seg, struct jpeg_job_result *res)
{
	struct salsa64_re2_decode t;
	if (!salsa64_re2_decode_read(seg, &t))
		return JPEG_ERR_KIND;
	const uint32_t w = t.width, half = t.step / 2;
	res->out_addr = t.dst;
	res->mb_stride = t.step;
	res->mb_out = half + 4 * w;
	uint32_t n = 0;
	while (n < t.pairs) {
		uint64_t cc = (uint64_t)n * (w / 2), o = (uint64_t)t.dst + (uint64_t)n * t.step;
		if (!fits(j, (uint64_t)t.y + (uint64_t)n * 2 * w, 2 * w) || !fits(j, t.cb + cc, w) ||
		    !fits(j, t.cr + cc, w) || !fits(j, o, 4 * w) || !fits(j, o + half, 4 * w))
			break;
		n++;
	}
	res->bad = n < t.pairs;
	const uint8_t *y[2] = { 0, 0 }, *cb[2] = { 0, 0 }, *cr[2] = { 0, 0 };
	if (n) {
		y[0] = dma_get_any(dc_y[0], j->rdram_ea + t.y, 2 * w, TAG_IN(0));
		cb[0] = dma_get_any(dc_cb[0], j->rdram_ea + t.cb, w, TAG_IN(0));
		cr[0] = dma_get_any(dc_cr[0], j->rdram_ea + t.cr, w, TAG_IN(0));
	}
	for (uint32_t m = 0; m < n; m++) {
		unsigned b = m & 1;
		if (m + 1 < n) {
			uint64_t cc = (uint64_t)(m + 1) * (w / 2);
			y[b ^ 1] = dma_get_any(dc_y[b ^ 1], j->rdram_ea + t.y + (uint64_t)(m + 1) * 2 * w, 2 * w,
					       TAG_IN(b ^ 1));
			cb[b ^ 1] = dma_get_any(dc_cb[b ^ 1], j->rdram_ea + t.cb + cc, w, TAG_IN(b ^ 1));
			cr[b ^ 1] = dma_get_any(dc_cr[b ^ 1], j->rdram_ea + t.cr + cc, w, TAG_IN(b ^ 1));
		}
		spukit_dma_wait(1u << TAG_IN(b) | 1u << TAG_OUT(b));
		uint64_t o0 = (uint64_t)t.dst + (uint64_t)m * t.step, o1 = o0 + half;
		uint8_t *r0 = dc_out[b][0] + (o0 & 15), *r1 = dc_out[b][1] + (o1 & 15);
		salsa64_re2_decode_pair(&t, y[b], cb[b], cr[b], r0, r1);
		dma_put8(r0, j->rdram_ea + o0, 4 * w, TAG_OUT(b));
		dma_put8(r1, j->rdram_ea + o1, 4 * w, TAG_OUT(b));
	}
	spukit_dma_wait(1u << TAG_OUT(0) | 1u << TAG_OUT(1));
	res->macroblocks = n;
	res->out_bytes = n ? (n - 1) * t.step + half + 4 * w : 0;
	return 0;
}

static int run_fill(const struct jpeg_job *j, const uint8_t *seg, struct jpeg_job_result *res)
{
	struct salsa64_re2_fill t;
	if (!salsa64_re2_fill_read(seg, &t))
		return JPEG_ERR_KIND;
	const uint32_t pitch = t.len + t.skip;
	res->out_addr = t.dst;
	res->mb_stride = t.advance;
	res->mb_out = (t.rows - 1) * pitch + t.len;
	/* Las pasadas completas que caben en la RDRAM (una pasada es todo o nada). */
	uint32_t n = 0;
	while (n < t.iters && fits(j, (uint64_t)t.src + (uint64_t)n * t.advance, res->mb_out) &&
	       fits(j, (uint64_t)t.dst + (uint64_t)n * t.advance, res->mb_out))
		n++;
	res->bad = n < t.iters;
	const uint32_t total = n * t.rows;  /* iters ≤ 65536 y filas · largo ≤ 2048: cabe */
	const uint8_t *sp[2] = { 0, 0 }, *dp[2] = { 0, 0 };
	if (total) {
		sp[0] = dma_get_any(fl_src[0], j->rdram_ea + t.src, t.len, TAG_IN(0));
		dp[0] = dma_get_any(fl_dst[0], j->rdram_ea + t.dst, t.len, TAG_IN(0));
	}
	/* La fila k es la r-ésima de la pasada it; se lleva la cuenta en vez de dividir. */
	uint32_t it = 0, r = 0, nit = 0, nr = 0;
	for (uint32_t k = 0; k < total; k++) {
		unsigned b = k & 1;
		if (k + 1 < total) {
			/* El búfer del destino es también el de salida: su DMA de la fila anterior tiene
			 * que haber salido antes de pisarlo con la siguiente entrada. */
			spukit_dma_wait(1u << TAG_OUT(b ^ 1));
			nr = r + 1;
			nit = it;
			if (nr == t.rows) {
				nr = 0;
				nit++;
			}
			uint64_t off = (uint64_t)nit * t.advance + (uint64_t)nr * pitch;
			sp[b ^ 1] = dma_get_any(fl_src[b ^ 1], j->rdram_ea + t.src + off, t.len, TAG_IN(b ^ 1));
			dp[b ^ 1] = dma_get_any(fl_dst[b ^ 1], j->rdram_ea + t.dst + off, t.len, TAG_IN(b ^ 1));
		}
		spukit_dma_wait(1u << TAG_IN(b) | 1u << TAG_OUT(b));
		uint64_t off = (uint64_t)it * t.advance + (uint64_t)r * pitch;
		salsa64_re2_fill_row(sp[b], (uint8_t *)dp[b], t.len);
		dma_put8(dp[b], j->rdram_ea + t.dst + off, t.len, TAG_OUT(b));
		it = nit;
		r = nr;
	}
	spukit_dma_wait(1u << TAG_OUT(0) | 1u << TAG_OUT(1));
	res->macroblocks = n;
	res->out_bytes = n ? (n - 1) * t.advance + res->mb_out : 0;
	return 0;
}

/* Una tarea de video: trae el segmento de datos y la hace. */
static int run_video(const struct jpeg_job *j, struct jpeg_job_result *res)
{
	uint32_t data = j->data_ptr & 0x1FFFFFFF;
	if (!fits(j, data, SALSA64_RE2_DATA_BYTES)) {
		res->bad = 1;
		return 0;
	}
	const uint8_t *seg = spukit_get_unaligned(seg_buf, j->rdram_ea + data, SALSA64_RE2_DATA_BYTES, TAG_PARAM);
	switch ((enum salsa64_jpeg_kind)j->kind) {
	case SALSA64_JPEG_RE2_RESIZE: return run_resize(j, seg, res);
	case SALSA64_JPEG_RE2_DECODE: return run_decode(j, seg, res);
	case SALSA64_JPEG_RE2_FILL: return run_fill(j, seg, res);
	default: return JPEG_ERR_KIND;
	}
}

static int run(const struct jpeg_job *j, struct jpeg_job_result *res)
{
	struct salsa64_jpeg_task t;
	uint32_t data = j->data_ptr & 0x1FFFFFFF;
	if (salsa64_jpeg_is_re2((enum salsa64_jpeg_kind)j->kind))
		return run_video(j, res);
	if (j->kind != SALSA64_JPEG_OOT)
		return JPEG_ERR_KIND;
	if (!in_rdram(j, data, SALSA64_JPEG_TASK_BYTES)) {
		res->bad = 1;
		return 0;
	}
	if (!salsa64_jpeg_task_read(spukit_get_unaligned(task_buf, j->rdram_ea + data, SALSA64_JPEG_TASK_BYTES,
							 TAG_PARAM),
				    &t)) {
		res->bad = 1;
		return 0;
	}
	int16_t q[3][64];
	for (int i = 0; i < 3; i++) {
		if (!in_rdram(j, t.qaddr[i], 128)) {
			res->bad = 1;
			return 0;
		}
		salsa64_jpeg_qtable(spukit_get_unaligned(q_buf[i], j->rdram_ea + t.qaddr[i], 128, TAG_PARAM), q[i]);
	}
	salsa64_jpeg_qv_init(&qv, q);
	if ((t.addr & 15) || (j->rdram_ea & 15))
		return JPEG_ERR_ALIGN;  /* que la haga el PPE */
	uint32_t in_bytes = SALSA64_JPEG_MB_IN(t.mode), out_bytes = SALSA64_JPEG_MB_OUT(t.mode);
	res->out_addr = t.addr;
	res->mb_stride = in_bytes;
	res->mb_out = out_bytes;
	/* Los que caben en la RDRAM, como salsa64_jpeg_run: se para en el
	 * primero que se sale. */
	uint32_t n = 0;
	while (n < t.count && in_rdram(j, t.addr + n * in_bytes, in_bytes))
		n++;
	if (n < t.count)
		res->bad = 1;
	if (n)
		spukit_dma_get(in_buf[0], j->rdram_ea + t.addr, in_bytes, TAG_IN(0));
	for (uint32_t m = 0; m < n; m++) {
		unsigned b = m & 1;
		uint64_t ea = j->rdram_ea + t.addr + m * in_bytes;
		if (m + 1 < n)
			spukit_dma_get(in_buf[b ^ 1], ea + in_bytes, in_bytes, TAG_IN(b ^ 1));
		spukit_dma_wait(1u << TAG_IN(b) | 1u << TAG_OUT(b));  /* el suyo llegó y la salida de m − 2 salió */
		salsa64_jpeg_mb_v(t.mode, &qv, in_buf[b], out_buf[b]);
		spukit_dma_put(out_buf[b], ea, out_bytes, TAG_OUT(b));
	}
	spukit_dma_wait(1u << TAG_OUT(0) | 1u << TAG_OUT(1));
	res->macroblocks = n;
	res->out_bytes = n ? (n - 1) * in_bytes + out_bytes : 0;
	return 0;
}

static int handler(void *ctx, const spukit_job *j, spukit_result *res)
{
	(void)ctx;
	struct jpeg_job job;
	struct jpeg_job_result r;
	if (j->kind != JPEG_JOB_TASK)
		return JPEG_ERR_KIND;
	memcpy(&job, j->data, sizeof job);
	memset(&r, 0, sizeof r);
	int err = run(&job, &r);
	memcpy(res->data, &r, sizeof r);
	return err;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg, (void)a2, (void)a3;
	salsa64_jpeg_v_init();
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
