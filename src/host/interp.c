/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — prototipo de interpolación de cuadros en el host, para comparar
 * los dos caminos de docs/interpolacion.md con las mismas escenas (solo
 * host; nada de esto corre en la PS3):
 *
 *   por geometría  la tarea va con GFX_JOB_INTERP: la HLE deja un registro
 *                  GFX_OP_VGROUP por cada G_VTX y la identidad de cada
 *                  vértice de salida en su pad; src/rdp/vmatch.c (lo mismo
 *                  que hará el PPE al traducir) empareja los grupos con los
 *                  del cuadro anterior del juego y da la posición anterior de
 *                  cada vértice. El cuadro intermedio es la lista del cuadro
 *                  nuevo dibujada otra vez con las posiciones interpoladas
 *                  (o extrapoladas); lo que no se emparejó (rectángulos,
 *                  HUD, vértices nuevos) queda donde está.
 *   por imagen     búsqueda por bloques entre las dos imágenes a resolución
 *                  nativa, en luma, con una pirámide de tres niveles
 *                  (búsqueda completa en el más grueso y candidatos más un
 *                  diamante en los otros), y deformación de las imágenes con
 *                  esos vectores; un píxel igual en los dos cuadros queda
 *                  tal cual (el HUD).
 *
 * Con `decimate`, una de cada k tareas hace de cuadro del juego y las de en
 * medio son la verdad: cada intermedio se compara con lo que el juego dibujó
 * de verdad en ese momento (GoldenEye en nuestro emulador da un cuadro por
 * campo). Sin `decimate` (Zelda, que va a 20 cuadros), cada tarea es un
 * cuadro y los intermedios se inventan entre una y la siguiente, sin verdad.
 *
 * Salida en DIR: o-T.ppm (lo que dibujó la tarea T), gi-N-j / ge-N-j (por
 * geometría, interpolado entre la tarea N−k (o N−1) y N en j/k, o
 * extrapolado más allá de N), ii-N-j / ie-N-j (por imagen), eg-N-j / ei-N-j
 * (dónde falla cada uno contra la verdad, en rojo), metricas.csv y
 * resumen.txt.
 */
#include "interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "raster.h"
#include "rdp/vmatch.h"
#include "rsp/gfx/hle.h"

#define W RASTER_W
#define H RASTER_H
#define NTABLES 8   /* tablas de cuadros guardadas (k ≤ 4) */
#define NIMG 8      /* imágenes de tareas guardadas */
#define MAX_K 6
#define MAX_OUT (1u << 16)
#define VM_GROUPS 2048  /* como la PS3 (src/rdp/rdp.c) */
#define VM_CLIP   32768

/* La tabla de grupos de un cuadro (vmatch.h). */
struct frame_table {
	uint32_t task;
	bool valid;
	struct vmatch_table t;
};

/* Una predicción guardada hasta que llegue su verdad (extrapolación). */
struct pending {
	bool used;
	uint32_t truth_task;
	unsigned j;
	char name[8];
	uint32_t img[W * H];
};

static struct {
	struct interp_opts o;
	bool on;
	uint32_t task;         /* número de la tarea en curso (desde 1) */
	struct frame_table tables[NTABLES];
	uint32_t prev_task;    /* tarea del cuadro anterior del juego (0: ninguna) */
	bool key;              /* la tarea en curso es un cuadro del juego (con decimate, 1 de cada k) */
	struct vmatch_stats vst;  /* emparejamiento de la tarea en curso */
	/* Posiciones de cada vértice de salida en el cuadro anterior (vmatch_prev). */
	float prev_v[MAX_OUT][4];
	struct gfx_vertex mod[MAX_OUT];
	uint8_t *rdram_copy;
	void *before, *after;  /* estado del rasterizador antes y después de la tarea */
	struct {
		uint32_t task;
		bool valid;
		uint32_t img[W * H];
	} imgs[NIMG];
	struct pending pend[16];
	FILE *csv, *tcsv;
	uint64_t cuts_geo, cuts_img;
	double last_sad;       /* SAD medio por píxel de la última búsqueda (nivel 0) */
	bool quiet;            /* sin imágenes (SALSA64_INTERP_SIN_IMAGENES) */
	bool recording;        /* la tarea en curso está en la ventana */
	/* Cuentas (desde from_field). */
	uint64_t v_loaded, v_found, v_matched, groups, groups_found, groups_moved, by_content;
	uint64_t ns_build, ns_pair, ns_prev, vm_tasks;  /* tiempo de vmatch (host) */
	uint64_t jumped, deformed, reordered, flat, atomic, why[VMATCH_WHY_COUNT];
	bool base_final, like_rsx;
	uint64_t out_vtx, out_matched, out_tasks;
	uint64_t sad_evals, sad_blocks, searches;
	double sum_psnr[8], sum_bad[8], sum_psnr_low[8], sum_bad_low[8];
	uint64_t n_metric[8];
	double fail_edge, fail_total;
} st;

enum { M_HOLD, M_BLEND, M_GEO_I, M_IMG_I, M_HOLD_E, M_GEO_E, M_IMG_E, M_COUNT };
static const char *const mname[M_COUNT] = { "repetir", "fundido", "geometria-i", "imagen-i", "repetir-e",
					    "geometria-e", "imagen-e" };

void interp_init(const struct interp_opts *o)
{
	st.o = *o;
	if (!o->dir)
		return;
	if (st.o.k < 2)
		st.o.k = 2;
	if (st.o.k > MAX_K)
		st.o.k = MAX_K;
	if (!st.o.block)
		st.o.block = 8;
	if (!st.o.range)
		st.o.range = 8;
	for (int i = 0; i < NTABLES; i++)
		vmatch_table_init(&st.tables[i].t, aligned_alloc(16, vmatch_table_bytes(VM_GROUPS, VM_CLIP)), VM_GROUPS,
				  VM_CLIP);
	st.rdram_copy = malloc(8u << 20);
	st.before = malloc(raster_state_size());
	st.after = malloc(raster_state_size());
	char path[600];
	snprintf(path, sizeof path, "%s/metricas.csv", st.o.dir);
	st.csv = fopen(path, "w");
	if (!st.csv) {
		perror(path);
		exit(3);
	}
	fprintf(st.csv, "tarea,j,metodo,psnr_db,pixeles_malos_pct,psnr_4x4_db,bloques_malos_4x4_pct\n");
	snprintf(path, sizeof path, "%s/tareas.csv", st.o.dir);
	st.tcsv = fopen(path, "w");
	if (st.tcsv)
		fprintf(st.tcsv, "tarea,campo,cargados,con_pareja,interpolados,salida,salida_interpolada,salida_tri\n");
	st.quiet = getenv("SALSA64_INTERP_SIN_IMAGENES") != NULL;
	/* Como la PS3 (src/rdp/rdp.c, interp_draw): el intermedio se dibuja
	 * sobre el cuadro final, no sobre lo que había antes de la tarea. */
	st.base_final = getenv("SALSA64_INTERP_BASE_FINAL") != NULL;
	st.like_rsx = getenv("SALSA64_INTERP_COMO_RSX") != NULL;
	raster_clip_all = st.like_rsx;
	st.on = true;
}

/* ---- Por geometría: la tarea pide los registros de grupos a la HLE --------- */

void interp_task_begin(struct gfx_job *job, uint32_t field)
{
	if (!st.on)
		return;
	st.task++;
	st.recording = field >= st.o.from_field;
	job->flags |= GFX_JOB_INTERP;
	/* El cuadro anterior del juego: la tarea anterior o, con decimate, la de
	 * k tareas antes (solo las tareas que son cuadros del juego buscan). */
	uint32_t back = st.o.decimate ? st.o.k : 1;
	st.key = !st.o.decimate || st.task % st.o.k == 0;
	const struct frame_table *p = &st.tables[(st.task - back) % NTABLES];
	st.prev_task = st.key && st.task > back && p->valid && p->task == st.task - back ? p->task : 0;
}

/* ---- Imágenes ------------------------------------------------------------ */

/* La color image donde la tarea dibujó más triángulos (el framebuffer). */
static uint32_t main_target(const struct gfx_job *job, const struct gfx_result *res)
{
	const uint8_t *cmd = (const uint8_t *)(uintptr_t)job->cmd_ea;
	uint32_t addrs[32], tris[32], n = 0, cur = 0, off = 0;
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || !c->size)
			break;
		if (c->op == GFX_OP_TARGET && c->b >= 256) {
			for (cur = 0; cur < n && addrs[cur] != c->a; cur++)
				;
			if (cur == n && n < 32) {
				addrs[n] = c->a;
				tris[n++] = 0;
			}
		} else if (c->op == GFX_OP_DRAW && cur < n) {
			const struct gfx_draw *d = (const struct gfx_draw *)c;
			if (d->prim_kind == 0)
				tris[cur] += d->h.b / 3 + 1;
		}
		off += c->size;
	}
	uint32_t best = 0, bt = 0;
	for (uint32_t i = 0; i < n; i++)
		if (tris[i] >= bt) {
			bt = tris[i];
			best = addrs[i];
		}
	return best;
}

static void grab(uint32_t addr, uint32_t *out)
{
	uint32_t w = 0;
	const uint32_t *img = addr ? raster_image(addr, &w) : NULL;
	if (!img) {
		memset(out, 0, W * H * 4);
		return;
	}
	memcpy(out, img, W * H * 4);
}

static void write_ppm(const char *name, const uint32_t *img)
{
	if (st.quiet)
		return;
	char path[700];
	snprintf(path, sizeof path, "%s/%s.ppm", st.o.dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	static uint8_t row[W * 3];
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t c = img[y * W + x];
			row[x * 3] = (uint8_t)(c >> 16);
			row[x * 3 + 1] = (uint8_t)(c >> 8);
			row[x * 3 + 2] = (uint8_t)c;
		}
		fwrite(row, 1, sizeof row, f);
	}
	fclose(f);
}

static const uint32_t *task_img(uint32_t task)
{
	for (int i = 0; i < NIMG; i++)
		if (st.imgs[i].valid && st.imgs[i].task == task)
			return st.imgs[i].img;
	return NULL;
}

/* Lo mismo con las dos imágenes reducidas a bloques de 4 × 4 (promedio) y
 * un umbral de 12/255: no castiga una textura fina corrida un subpíxel (lo
 * que el ojo no ve en movimiento) y sí lo que está en otro lugar. */
static void compare_low(const uint32_t *a, const uint32_t *b, double *psnr, double *bad)
{
	double se = 0;
	long nbad = 0, n = 0;
	for (int by = 0; by + 4 <= H; by += 4)
		for (int bx = 0; bx + 4 <= W; bx += 4) {
			int worst = 0;
			for (int s = 0; s < 24; s += 8) {
				int sa = 0, sb = 0;
				for (int y = by; y < by + 4; y++)
					for (int x = bx; x < bx + 4; x++) {
						sa += (int)((a[y * W + x] >> s) & 0xFF);
						sb += (int)((b[y * W + x] >> s) & 0xFF);
					}
				double d = (sa - sb) / 16.0;
				se += d * d;
				worst = fabs(d) > worst ? (int)fabs(d) : worst;
			}
			nbad += worst > 12;
			n++;
		}
	double mse = se / (n * 3.0);
	*psnr = mse > 0 ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
	*bad = 100.0 * nbad / n;
}

/* PSNR (dB) y porcentaje de píxeles con algún canal a más de 24/255. */
static void compare(const uint32_t *a, const uint32_t *b, double *psnr, double *bad)
{
	double se = 0;
	long nbad = 0;
	for (int i = 0; i < W * H; i++) {
		int worst = 0;
		for (int s = 0; s < 24; s += 8) {
			int d = (int)((a[i] >> s) & 0xFF) - (int)((b[i] >> s) & 0xFF);
			se += d * d;
			worst = abs(d) > worst ? abs(d) : worst;
		}
		nbad += worst > 24;
	}
	double mse = se / (W * H * 3.0);
	*psnr = mse > 0 ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
	*bad = 100.0 * nbad / (W * H);
}

static void metric(uint32_t task, unsigned j, int m, const uint32_t *pred, const uint32_t *truth)
{
	double p, b, pl, bl;
	compare(pred, truth, &p, &b);
	compare_low(pred, truth, &pl, &bl);
	fprintf(st.csv, "%u,%u,%s,%.2f,%.3f,%.2f,%.3f\n", task, j, mname[m], p, b, pl, bl);
	st.sum_psnr[m] += p;
	st.sum_bad[m] += b;
	st.sum_psnr_low[m] += pl;
	st.sum_bad_low[m] += bl;
	st.n_metric[m]++;
}

/* Verdad en rojo donde la predicción se aleja más de 24/255. */
static void error_map(const char *name, const uint32_t *pred, const uint32_t *truth)
{
	static uint32_t e[W * H];
	for (int i = 0; i < W * H; i++) {
		int worst = 0;
		for (int s = 0; s < 24; s += 8) {
			int d = abs((int)((pred[i] >> s) & 0xFF) - (int)((truth[i] >> s) & 0xFF));
			worst = d > worst ? d : worst;
		}
		uint32_t c = truth[i];
		uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
		uint32_t y = (r * 77 + g * 150 + b * 29) >> 8;
		if (worst > 24)
			e[i] = 0xFF0000u | (y / 3) << 8 | y / 3;
		else
			e[i] = (y / 2) << 16 | (y / 2) << 8 | y / 2;
	}
	write_ppm(name, e);
}

/* ---- Por geometría: el cuadro intermedio --------------------------------- */

/* Corte por cuadro (vmatch_pair): no se interpola nada del cuadro. */
static bool geo_cut(void)
{
	return st.vst.cut;
}

/* La búsqueda no encontró nada parecido: SAD medio de más de 10/255 por
 * píxel en luma (otro plano, destello que cubre la pantalla). */
#define IMG_CUT_SAD 10.0
static bool img_cut(void)
{
	return st.last_sad > IMG_CUT_SAD;
}

static void draw_variant(const struct gfx_job *job, const struct gfx_result *res, uint32_t target, float t, bool extrap,
			 uint32_t *out)
{
	uint32_t nv = res->vtx_bytes / sizeof(struct gfx_vertex);
	if (nv > MAX_OUT)
		nv = MAX_OUT;
	const struct gfx_vertex *cur = (const struct gfx_vertex *)(uintptr_t)job->vtx_ea;
	for (uint32_t i = 0; i < nv; i++) {
		/* m = c + (p − c)·k, con p la posición del cuadro anterior
		 * (vmatch_prev: sin pareja, p = c). */
		const float *p = st.prev_v[i];
		struct gfx_vertex *m = &st.mod[i];
		float k = extrap ? -t : 1.0f - t;
		*m = cur[i];
		/* Para raster_clip_w: se recortan los triángulos que en el cuadro de
		 * verdad estaban enteros delante; con SALSA64_INTERP_COMO_RSX todos,
		 * como el RSX en la PS3 (src/host/raster.c, draw_triangle_clip). */
		m->pad = st.like_rsx || cur[i].w > 1e-3f;
		m->x = cur[i].x + (p[0] - cur[i].x) * k;
		m->y = cur[i].y + (p[1] - cur[i].y) * k;
		m->w = cur[i].w + (p[3] - cur[i].w) * k;
		/* Como el vertex program de la PS3 (rsxkit_lincomb_lerp.vcg): las
		 * cuatro, también la z. */
		m->z = cur[i].z + (p[2] - cur[i].z) * k;
	}
	struct gfx_job j2 = *job;
	j2.vtx_ea = (uint64_t)(uintptr_t)st.mod;
	if (job->rdram_size <= (8u << 20)) {
		memcpy(st.rdram_copy, (const void *)(uintptr_t)job->rdram_ea, job->rdram_size);
		j2.rdram_ea = (uint64_t)(uintptr_t)st.rdram_copy;
	}
	raster_state_restore(st.base_final ? st.after : st.before);
	bool tr = raster_trace_fb;
	raster_trace_fb = false;
	raster_clip_w = true;
	raster_task(&j2, res);
	raster_clip_w = false;
	raster_trace_fb = tr;
	grab(target, out);
	raster_state_restore(st.after);
}

/* ---- Por imagen: búsqueda por bloques ------------------------------------ */

#define LEVELS 3

struct level {
	int w, h;
	uint8_t *ref, *cur;  /* luma */
	int bw, bh;          /* bloques */
	int16_t *vx, *vy;    /* desplazamiento de cada bloque de cur hacia ref */
};

static struct level lv[LEVELS];

static void luma(const uint32_t *img, uint8_t *y)
{
	for (int i = 0; i < W * H; i++) {
		uint32_t c = img[i];
		y[i] = (uint8_t)((((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29) >> 8);
	}
}

static void down(const uint8_t *s, int w, int h, uint8_t *d)
{
	for (int y = 0; y < h / 2; y++)
		for (int x = 0; x < w / 2; x++)
			d[y * (w / 2) + x] = (uint8_t)((s[2 * y * w + 2 * x] + s[2 * y * w + 2 * x + 1] + s[(2 * y + 1) * w + 2 * x] +
							s[(2 * y + 1) * w + 2 * x + 1] + 2) >> 2);
}

static inline int px(const struct level *l, const uint8_t *p, int x, int y)
{
	x = x < 0 ? 0 : x >= l->w ? l->w - 1 : x;
	y = y < 0 ? 0 : y >= l->h ? l->h - 1 : y;
	return p[y * l->w + x];
}

/* SAD del bloque (bx, by) de cur contra ref desplazado (dx, dy). */
static unsigned sad(const struct level *l, int bx, int by, int dx, int dy)
{
	unsigned B = st.o.block, s = 0;
	int x0 = bx * (int)B, y0 = by * (int)B;
	st.sad_evals++;
	bool inside = x0 + dx >= 0 && y0 + dy >= 0 && x0 + dx + (int)B <= l->w && y0 + dy + (int)B <= l->h &&
		      x0 + (int)B <= l->w && y0 + (int)B <= l->h;
	for (unsigned y = 0; y < B; y++)
		for (unsigned x = 0; x < B; x++) {
			int a, b;
			if (inside) {
				a = l->cur[(y0 + (int)y) * l->w + x0 + (int)x];
				b = l->ref[(y0 + (int)y + dy) * l->w + x0 + (int)x + dx];
			} else {
				a = px(l, l->cur, x0 + (int)x, y0 + (int)y);
				b = px(l, l->ref, x0 + (int)x + dx, y0 + (int)y + dy);
			}
			s += (unsigned)abs(a - b);
		}
	return s;
}

static int med3(int a, int b, int c)
{
	return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
}

#define LAMBDA 4u  /* costo por píxel de distancia al predictor */

static unsigned cost(const struct level *l, int bx, int by, int dx, int dy, int px_, int py_)
{
	return sad(l, bx, by, dx, dy) + LAMBDA * (unsigned)(abs(dx - px_) + abs(dy - py_));
}

static void search_level(int li)
{
	struct level *l = &lv[li];
	const struct level *up = li + 1 < LEVELS ? &lv[li + 1] : NULL;
	int R = (int)st.o.range;
	for (int by = 0; by < l->bh; by++)
		for (int bx = 0; bx < l->bw; bx++) {
			int i = by * l->bw + bx;
			st.sad_blocks++;
			/* Predictor: mediana de izquierda, arriba y arriba a la derecha. */
			int lx = bx ? l->vx[i - 1] : 0, ly = bx ? l->vy[i - 1] : 0;
			int tx = by ? l->vx[i - l->bw] : 0, ty = by ? l->vy[i - l->bw] : 0;
			int rx = by && bx + 1 < l->bw ? l->vx[i - l->bw + 1] : tx, ry = by && bx + 1 < l->bw ? l->vy[i - l->bw + 1] : ty;
			int pxv = med3(lx, tx, rx), pyv = med3(ly, ty, ry);
			int bestx = 0, besty = 0;
			unsigned best = ~0u;
			if (!up) {
				/* Nivel más grueso: búsqueda completa. */
				for (int dy = -R; dy <= R; dy++)
					for (int dx = -R; dx <= R; dx++) {
						unsigned c = cost(l, bx, by, dx, dy, pxv, pyv);
						if (c < best)
							best = c, bestx = dx, besty = dy;
					}
			} else {
				/* Candidatos: el padre (×2) y sus vecinos, el predictor y cero. */
				int cand[8][2], nc = 0;
				int ux = bx / 2 < up->bw ? bx / 2 : up->bw - 1, uy = by / 2 < up->bh ? by / 2 : up->bh - 1;
				static const int nb[5][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
				for (int k = 0; k < 5; k++) {
					int qx = ux + nb[k][0], qy = uy + nb[k][1];
					if (qx < 0 || qy < 0 || qx >= up->bw || qy >= up->bh)
						continue;
					cand[nc][0] = 2 * up->vx[qy * up->bw + qx];
					cand[nc][1] = 2 * up->vy[qy * up->bw + qx];
					nc++;
				}
				cand[nc][0] = pxv, cand[nc][1] = pyv, nc++;
				cand[nc][0] = 0, cand[nc][1] = 0, nc++;
				for (int k = 0; k < nc; k++) {
					unsigned c = cost(l, bx, by, cand[k][0], cand[k][1], pxv, pyv);
					if (c < best)
						best = c, bestx = cand[k][0], besty = cand[k][1];
				}
				/* Diamante chico hasta que no mejora. */
				for (int step = 0; step < 8; step++) {
					static const int d4[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
					int nx = bestx, ny = besty;
					for (int k = 0; k < 4; k++) {
						unsigned c = cost(l, bx, by, bestx + d4[k][0], besty + d4[k][1], pxv, pyv);
						if (c < best)
							best = c, nx = bestx + d4[k][0], ny = besty + d4[k][1];
					}
					if (nx == bestx && ny == besty)
						break;
					bestx = nx, besty = ny;
				}
			}
			l->vx[i] = (int16_t)bestx;
			l->vy[i] = (int16_t)besty;
			if (li == 0)
				st.last_sad += sad(l, bx, by, bestx, besty);
		}
}

/* Campo de desplazamientos de cada bloque de `cur` hacia `ref` (en píxeles
 * del nivel 0): el contenido del bloque estaba en ref en p + d. */
static void motion(const uint32_t *ref, const uint32_t *cur)
{
	static uint8_t buf[2][LEVELS][W * H];
	static int16_t vbuf[2][LEVELS][(W / 4) * (H / 4)];
	int w = W, h = H;
	for (int i = 0; i < LEVELS; i++) {
		struct level *l = &lv[i];
		l->w = w, l->h = h;
		l->ref = buf[0][i], l->cur = buf[1][i];
		l->bw = (w + (int)st.o.block - 1) / (int)st.o.block;
		l->bh = (h + (int)st.o.block - 1) / (int)st.o.block;
		l->vx = vbuf[0][i], l->vy = vbuf[1][i];
		if (i == 0) {
			luma(ref, l->ref);
			luma(cur, l->cur);
		} else {
			down(lv[i - 1].ref, lv[i - 1].w, lv[i - 1].h, l->ref);
			down(lv[i - 1].cur, lv[i - 1].w, lv[i - 1].h, l->cur);
		}
		w /= 2, h /= 2;
	}
	st.last_sad = 0;
	for (int i = LEVELS - 1; i >= 0; i--)
		search_level(i);
	st.last_sad /= (double)lv[0].bw * lv[0].bh * st.o.block * st.o.block;
	st.searches++;
}

/* Desplazamiento en el píxel (x, y): bilineal entre centros de bloque. */
static void field_at(float x, float y, float *dx, float *dy)
{
	const struct level *l = &lv[0];
	float B = (float)st.o.block;
	float fx = x / B - 0.5f, fy = y / B - 0.5f;
	int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
	float ax = fx - (float)x0, ay = fy - (float)y0;
	float sx = 0, sy = 0;
	for (int k = 0; k < 4; k++) {
		int qx = x0 + (k & 1), qy = y0 + (k >> 1);
		qx = qx < 0 ? 0 : qx >= l->bw ? l->bw - 1 : qx;
		qy = qy < 0 ? 0 : qy >= l->bh ? l->bh - 1 : qy;
		float wgt = ((k & 1) ? ax : 1.0f - ax) * ((k >> 1) ? ay : 1.0f - ay);
		sx += wgt * (float)l->vx[qy * l->bw + qx];
		sy += wgt * (float)l->vy[qy * l->bw + qx];
	}
	*dx = sx, *dy = sy;
}

static void sample(const uint32_t *img, float x, float y, float out[3])
{
	x = x < 0 ? 0 : x > W - 1 ? (float)(W - 1) : x;
	y = y < 0 ? 0 : y > H - 1 ? (float)(H - 1) : y;
	int x0 = (int)x, y0 = (int)y;
	int x1 = x0 + 1 < W ? x0 + 1 : x0, y1 = y0 + 1 < H ? y0 + 1 : y0;
	float ax = x - (float)x0, ay = y - (float)y0;
	uint32_t c[4] = { img[y0 * W + x0], img[y0 * W + x1], img[y1 * W + x0], img[y1 * W + x1] };
	for (int s = 0; s < 3; s++) {
		int sh = 16 - 8 * s;
		float a = (float)((c[0] >> sh) & 0xFF) * (1 - ax) + (float)((c[1] >> sh) & 0xFF) * ax;
		float b = (float)((c[2] >> sh) & 0xFF) * (1 - ax) + (float)((c[3] >> sh) & 0xFF) * ax;
		out[s] = a * (1 - ay) + b * ay;
	}
}

static uint32_t pack(const float c[3])
{
	uint32_t r = 0;
	for (int s = 0; s < 3; s++) {
		float v = c[s] + 0.5f;
		uint32_t q = v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
		r |= q << (16 - 8 * s);
	}
	return r;
}

/* Interpolado en t entre ref (t = 0) y cur (t = 1): lo que en cur está en
 * p estaba en ref en p + d, así que en t está en p + d·(1 − t). Se toma el
 * campo en el píxel de salida (aproximación de campo suave). */
static void warp_interp(const uint32_t *ref, const uint32_t *cur, float t, uint32_t *out)
{
	if (img_cut()) {
		memcpy(out, t < 0.5f ? ref : cur, W * H * 4);
		return;
	}
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			int i = y * W + x;
			if (ref[i] == cur[i]) {
				out[i] = cur[i];  /* quieto (HUD, cielo) */
				continue;
			}
			float dx, dy, a[3], b[3], c[3];
			field_at((float)x, (float)y, &dx, &dy);
			sample(ref, (float)x + dx * t, (float)y + dy * t, a);
			sample(cur, (float)x - dx * (1.0f - t), (float)y - dy * (1.0f - t), b);
			/* Si las dos muestras no concuerdan (oclusión, vector malo), la
			 * del cuadro más cercano en el tiempo: una imagen nítida en vez
			 * de un fantasma doble. */
			float worst = 0;
			for (int s = 0; s < 3; s++)
				worst = fabsf(a[s] - b[s]) > worst ? fabsf(a[s] - b[s]) : worst;
			float wb = worst > 48.0f ? (t < 0.5f ? 0.0f : 1.0f) : t;
			for (int s = 0; s < 3; s++)
				c[s] = a[s] * (1.0f - wb) + b[s] * wb;
			out[i] = pack(c);
		}
}

/* Extrapolado s cuadros del juego más allá de cur: lo que va de ref a cur
 * sigue igual (p − d·s). */
static void warp_extrap(const uint32_t *cur, float s, uint32_t *out)
{
	if (img_cut()) {
		memcpy(out, cur, W * H * 4);
		return;
	}
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			float dx, dy, c[3];
			field_at((float)x, (float)y, &dx, &dy);
			sample(cur, (float)x + dx * s, (float)y + dy * s, c);
			out[y * W + x] = pack(c);
		}
}

/* Bloques con error en el intermedio: cuántos están en un borde del campo
 * (un vecino con otro desplazamiento: oclusión, objeto contra fondo). */
static void fail_stats(const uint32_t *pred, const uint32_t *truth)
{
	const struct level *l = &lv[0];
	int B = (int)st.o.block;
	for (int by = 0; by < l->bh; by++)
		for (int bx = 0; bx < l->bw; bx++) {
			long bad = 0;
			for (int y = by * B; y < (by + 1) * B && y < H; y++)
				for (int x = bx * B; x < (bx + 1) * B && x < W; x++) {
					uint32_t a = pred[y * W + x], b = truth[y * W + x];
					int worst = 0;
					for (int s = 0; s < 24; s += 8) {
						int d = abs((int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF));
						worst = d > worst ? d : worst;
					}
					bad += worst > 24;
				}
			if (bad * 4 < B * B)
				continue;  /* menos de un cuarto del bloque mal */
			st.fail_total++;
			int i = by * l->bw + bx, edge = 0;
			static const int nb[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
			for (int k = 0; k < 4; k++) {
				int qx = bx + nb[k][0], qy = by + nb[k][1];
				if (qx < 0 || qy < 0 || qx >= l->bw || qy >= l->bh)
					continue;
				int j = qy * l->bw + qx;
				if (abs(l->vx[i] - l->vx[j]) + abs(l->vy[i] - l->vy[j]) > 2)
					edge = 1;
			}
			st.fail_edge += edge;
		}
}

/* ---- Cada tarea ------------------------------------------------------------ */

void interp_before_raster(void)
{
	if (st.on)
		raster_state_save(st.before);
}

static void settle_pending(uint32_t task, const uint32_t *truth)
{
	for (unsigned i = 0; i < sizeof st.pend / sizeof st.pend[0]; i++) {
		struct pending *p = &st.pend[i];
		if (!p->used || p->truth_task != task)
			continue;
		int m = !strcmp(p->name, "ge") ? M_GEO_E : !strcmp(p->name, "ie") ? M_IMG_E : M_HOLD_E;
		metric(task, p->j, m, p->img, truth);
		if (m != M_HOLD_E) {
			char name[64];
			snprintf(name, sizeof name, "%s-%u-%u", m == M_GEO_E ? "eg-e" : "ei-e", task - p->j, p->j);
			error_map(name, p->img, truth);
		}
		p->used = false;
	}
}

static void keep_pending(const char *name, uint32_t truth_task, unsigned j, const uint32_t *img)
{
	for (unsigned i = 0; i < sizeof st.pend / sizeof st.pend[0]; i++)
		if (!st.pend[i].used) {
			struct pending *p = &st.pend[i];
			p->used = true;
			p->truth_task = truth_task;
			p->j = j;
			snprintf(p->name, sizeof p->name, "%s", name);
			memcpy(p->img, img, sizeof p->img);
			return;
		}
}

void interp_after_raster(const struct gfx_job *job, const struct gfx_result *res, uint32_t field)
{
	if (!st.on || !st.recording)
		return;
	uint32_t target = main_target(job, res);
	raster_state_save(st.after);
	int slot = (int)(st.task % NIMG);
	st.imgs[slot].task = st.task;
	st.imgs[slot].valid = true;
	grab(target, st.imgs[slot].img);
	const uint32_t *now = st.imgs[slot].img;
	char name[64];
	snprintf(name, sizeof name, "o-%u", st.task);
	write_ppm(name, now);

	/* Verdades que llegan: las intermedias de la interpolación (decimate)
	 * se comparan aquí mismo; las extrapolaciones esperan a su tarea. */
	settle_pending(st.task, now);

	/* La tabla de grupos de este cuadro, su emparejamiento con el anterior y
	 * la posición anterior de cada vértice de salida: lo mismo que hará el
	 * PPE al traducir (src/rdp/vmatch.c). */
	struct frame_table *ft = &st.tables[st.task % NTABLES];
	uint32_t nv = res->vtx_bytes / sizeof(struct gfx_vertex);
	if (nv > MAX_OUT)
		nv = MAX_OUT;
	if (getenv("SALSA64_INTERP_VOLCAR") && (st.task == 20 || st.task == 21)) {
		/* Para tools/vmatch-bench.c: comandos, vértices y RDRAM de la tarea. */
		char vp[700];
		const void *parts[3] = { (const void *)(uintptr_t)job->cmd_ea, (const void *)(uintptr_t)job->vtx_ea,
					 (const void *)(uintptr_t)job->rdram_ea };
		const size_t lens[3] = { res->cmd_bytes, res->vtx_bytes, job->rdram_size };
		static const char *const ext[3] = { "cmd", "vtx", "ram" };
		for (int q = 0; q < 3; q++) {
			snprintf(vp, sizeof vp, "%s/vm-%u.%s", st.o.dir, st.task, ext[q]);
			FILE *vf = fopen(vp, "wb");
			if (vf) {
				fwrite(parts[q], 1, lens[q], vf);
				fclose(vf);
			}
		}
	}
	struct timespec t0, t1, t2, t3;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	const struct vmatch_table *pt = st.prev_task ? &st.tables[st.prev_task % NTABLES].t : NULL;
	vmatch_build(&ft->t, (const uint8_t *)(uintptr_t)job->cmd_ea, res->cmd_bytes);
	ft->task = st.task;
	ft->valid = true;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	vmatch_pair(pt, &ft->t, st.o.max_disp, &st.vst);
	if (!getenv("SALSA64_INTERP_SIN_MALLA"))
		vmatch_atomic(&ft->t, (const struct gfx_vertex *)(uintptr_t)job->vtx_ea, nv, &st.vst);
	clock_gettime(CLOCK_MONOTONIC, &t2);
	uint32_t om = vmatch_prev(pt, &ft->t, &st.vst, (const struct gfx_vertex *)(uintptr_t)job->vtx_ea, nv, st.prev_v);
	clock_gettime(CLOCK_MONOTONIC, &t3);
#define NS(a, b) ((uint64_t)((b).tv_sec - (a).tv_sec) * 1000000000u + (uint64_t)((b).tv_nsec - (a).tv_nsec))
	st.ns_build += NS(t0, t1);
	st.ns_pair += NS(t1, t2);
	st.ns_prev += NS(t2, t3);
	st.vm_tasks++;
#undef NS
	if (pt && getenv("SALSA64_INTERP_GRUPOS")) {
		/* Diagnóstico: cada grupo de la tarea, su pareja y por qué no. */
		char gp[700];
		snprintf(gp, sizeof gp, "%s/grupos-%u.txt", st.o.dir, st.task);
		FILE *gf = fopen(gp, "w");
		for (uint32_t i = 0; gf && i < ft->t.ngroups; i++) {
			const struct vmatch_group *g = &ft->t.g[i];
			int byaddr = -1, byhash = -1, addrn = 0;
			for (uint32_t q = 0; q < pt->ngroups; q++) {
				const struct vmatch_group *h = &pt->g[q];
				if (h->addr == g->addr && h->occ == g->occ)
					byaddr = (int)q;
				if (h->addr == g->addr)
					addrn++;
				if (g->hash && h->hash == g->hash && h->hocc == g->hocc)
					byhash = (int)q;
			}
			float cx = 0, cy = 0, px = 0, py = 0;
			int nvis = 0;
			for (uint32_t k = 0; k < g->n; k++) {
				const float *c = ft->t.clip[g->clip + k];
				if (c[3] <= 0.05f || fabsf(c[0]) > 1.5f * c[3] || fabsf(c[1]) > 1.5f * c[3])
					continue;
				cx += c[0] / c[3] * g->vp[0], cy += c[1] / c[3] * g->vp[1];
				if (g->pair >= 0) {
					const float *d = pt->clip[pt->g[g->pair].clip + k];
					if (d[3] > 0.0f)
						px += d[0] / d[3] * g->vp[0], py += d[1] / d[3] * g->vp[1];
				}
				nvis++;
			}
			float w0 = 1e30f, w1 = -1e30f;
			for (uint32_t k = 0; k < g->n; k++) {
				float w = ft->t.clip[g->clip + k][3];
				w0 = w < w0 ? w : w0, w1 = w > w1 ? w : w1;
			}
			static const char *const why[VMATCH_WHY_COUNT] = { "ok",  "nuevo",    "movido", "saltó", "deformado",
									  "tamaño", "HUD", "aplastado", "objeto", "malla" };
			fprintf(gf, "%3u addr %06x n %2u v0 %2u occ %3u/%d hash %08x hocc %u pair %4d %-9s dir %4d cont %4d vis %2d w %g..%g",
				i, g->addr, g->n, g->v0, g->occ, addrn, g->hash, g->hocc, g->pair, g->why < VMATCH_WHY_COUNT ? why[g->why] : "?",
				byaddr, byhash, nvis, w0, w1);
			if (nvis)
				fprintf(gf, " c (%.0f, %.0f)", cx / nvis, -cy / nvis);
			if (g->pair >= 0 && nvis)
				fprintf(gf, " d %.1f", sqrtf((cx - px) * (cx - px) + (cy - py) * (cy - py)) / nvis);
			/* Cuánto se aplasta a media interpolación (con la pareja o la
			 * candidata por dirección): radio del grupo en t = 0,5 entre el
			 * menor de los dos cuadros. */
			int cand = g->pair >= 0 ? g->pair : byaddr;
			if (cand >= 0 && pt->g[cand].n == g->n) {
				float m[3][2] = { { 0 } }, r[3] = { 0 };
				int nk = 0;
				for (int pass = 0; pass < 2; pass++)
					for (uint32_t k = 0; k < g->n; k++) {
						const float *c = ft->t.clip[g->clip + k], *d = pt->clip[pt->g[cand].clip + k];
						if (c[3] <= 0.05f || d[3] <= 0.05f)
							continue;
						float p1[2] = { c[0] / c[3] * g->vp[0], c[1] / c[3] * g->vp[1] };
						float p0[2] = { d[0] / d[3] * g->vp[0], d[1] / d[3] * g->vp[1] };
						float q[3][2] = { { p0[0], p0[1] }, { (p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2 }, { p1[0], p1[1] } };
						for (int j = 0; j < 3; j++) {
							if (!pass) {
								m[j][0] += q[j][0], m[j][1] += q[j][1];
							} else {
								float dx = q[j][0] - m[j][0] / nk, dy = q[j][1] - m[j][1] / nk;
								r[j] += dx * dx + dy * dy;
							}
						}
						if (!pass)
							nk++;
					}
				float lo = r[0] < r[2] ? r[0] : r[2];
				if (nk > 1 && lo > 0.0f)
					fprintf(gf, " col %.2f r %.1f", sqrtf(r[1] / lo), sqrtf(lo / nk));
			}
			fputc('\n', gf);
		}
		if (gf)
			fclose(gf);
	}
	if (st.prev_task) {
		uint32_t ot = 0;
		const struct gfx_vertex *vv = (const struct gfx_vertex *)(uintptr_t)job->vtx_ea;
		for (uint32_t i = 0; i < nv; i++)
			ot += vv[i].pad != 0;
		st.out_tasks++;
		st.out_vtx += nv;
		st.out_matched += om;
		st.v_loaded += st.vst.vertices;
		st.v_found += st.vst.found_vertices;
		st.v_matched += st.vst.paired_vertices;
		st.groups += st.vst.groups;
		st.groups_found += st.vst.found;
		st.groups_moved += st.vst.moved;
		st.by_content += st.vst.by_content;
		st.jumped += st.vst.jumped;
		st.deformed += st.vst.deformed;
		st.reordered += st.vst.reordered;
		st.flat += st.vst.flat;
		st.atomic += st.vst.atomic;
		for (uint32_t i = 0; i < ft->t.ngroups; i++) {
			const struct vmatch_group *g = &ft->t.g[i];
			if (g->front && g->why < VMATCH_WHY_COUNT)
				st.why[g->why]++;
		}
		if (st.tcsv)
			fprintf(st.tcsv, "%u,%u,%u,%u,%u,%u,%u,%u\n", st.task, field, st.vst.vertices, st.vst.found_vertices,
				st.vst.paired_vertices, nv, om, ot);
	}
	if (!st.prev_task)
		return;
	const uint32_t *before = task_img(st.prev_task);
	if (!before)
		return;
	unsigned k = st.o.k;
	static uint32_t pred[W * H];
	motion(before, now);
	st.cuts_geo += geo_cut();
	st.cuts_img += img_cut();
	if (st.tcsv)
		fprintf(st.tcsv, "# tarea %u: corte geometría %d, imagen %d (SAD medio %.1f)\n", st.task, geo_cut(), img_cut(),
			st.last_sad);
	if (getenv("SALSA64_INTERP_T0")) {
		/* Prueba del emparejamiento: todo en las posiciones de antes (t = 0)
		 * debería dar el cuadro anterior; lo que no, son grupos sin pareja
		 * o mal emparejados. */
		draw_variant(job, res, target, 0.0f, false, pred);
		snprintf(name, sizeof name, "g0-%u", st.task);
		write_ppm(name, pred);
	}
	for (unsigned j = 1; j < k; j++) {
		float t = (float)j / (float)k;
		const uint32_t *truth = st.o.decimate ? task_img(st.task - k + j) : NULL;
		/* Por geometría, interpolado. */
		draw_variant(job, res, target, t, false, pred);
		snprintf(name, sizeof name, "gi-%u-%u", st.task, j);
		write_ppm(name, pred);
		if (truth) {
			metric(st.task - k + j, j, M_GEO_I, pred, truth);
			snprintf(name, sizeof name, "eg-i-%u-%u", st.task, j);
			error_map(name, pred, truth);
		}
		/* Por imagen, interpolado. */
		warp_interp(before, now, t, pred);
		snprintf(name, sizeof name, "ii-%u-%u", st.task, j);
		write_ppm(name, pred);
		if (truth) {
			metric(st.task - k + j, j, M_IMG_I, pred, truth);
			snprintf(name, sizeof name, "ei-i-%u-%u", st.task, j);
			error_map(name, pred, truth);
			fail_stats(pred, truth);
			/* Referencias: repetir el cuadro viejo y el fundido. */
			metric(st.task - k + j, j, M_HOLD, before, truth);
			for (int i = 0; i < W * H; i++) {
				float a[3], b[3], c[3];
				for (int s = 0; s < 3; s++) {
					a[s] = (float)((before[i] >> (16 - 8 * s)) & 0xFF);
					b[s] = (float)((now[i] >> (16 - 8 * s)) & 0xFF);
					c[s] = a[s] * (1 - t) + b[s] * t;
				}
				pred[i] = pack(c);
			}
			metric(st.task - k + j, j, M_BLEND, pred, truth);
		}
		/* Extrapolados: s = j/k cuadros del juego más allá. */
		draw_variant(job, res, target, t, true, pred);
		snprintf(name, sizeof name, "ge-%u-%u", st.task, j);
		write_ppm(name, pred);
		if (st.o.decimate)
			keep_pending("ge", st.task + j, j, pred);
		warp_extrap(now, t, pred);
		snprintf(name, sizeof name, "ie-%u-%u", st.task, j);
		write_ppm(name, pred);
		if (st.o.decimate) {
			keep_pending("ie", st.task + j, j, pred);
			keep_pending("he", st.task + j, j, now);
		}
	}
}

void interp_report(void)
{
	if (!st.on)
		return;
	char path[600];
	snprintf(path, sizeof path, "%s/resumen.txt", st.o.dir);
	FILE *out[2] = { stderr, fopen(path, "w") };
	for (int f = 0; f < 2; f++) {
		FILE *o = out[f];
		if (!o)
			continue;
		fprintf(o, "interp: k = %u%s, max_disp %.0f px, bloque %u, rango ±%u en el nivel %d\n", st.o.k,
			st.o.decimate ? " (1 de cada k tareas; las demás son la verdad)" : "", st.o.max_disp, st.o.block,
			st.o.range, LEVELS - 1);
		if (st.v_loaded)
			fprintf(o,
				"interp: geometría: %llu vértices cargados en cuadros con anterior; con pareja %.1f %%, "
				"interpolados %.1f %% (grupos con pareja %llu de %llu, rechazados por desplazamiento %llu)\n",
				(unsigned long long)st.v_loaded, 100.0 * st.v_found / st.v_loaded,
				100.0 * st.v_matched / st.v_loaded, (unsigned long long)st.groups_found,
				(unsigned long long)st.groups, (unsigned long long)st.groups_moved);
		if (st.out_vtx)
			fprintf(o, "interp: geometría: vértices de salida interpolados %.1f %% (%llu tareas, %.0f por tarea)\n",
				100.0 * st.out_matched / st.out_vtx, (unsigned long long)st.out_tasks,
				(double)st.out_vtx / st.out_tasks);
		fprintf(o, "interp: cortes detectados: geometría %llu, imagen %llu (de %llu cuadros con anterior)\n",
			(unsigned long long)st.cuts_geo, (unsigned long long)st.cuts_img, (unsigned long long)st.searches);
		if (st.groups)
			fprintf(o, "interp: geometría: %.1f %% de los grupos se encontraron por contenido (la dirección no estaba)\n",
				100.0 * st.by_content / st.groups);
		if (st.vm_tasks)
			fprintf(o, "interp: vmatch por tarea (host): armar %.1f µs, emparejar %.1f µs, posiciones %.1f µs; %.0f grupos; "
				"%.2f saltaron, %.2f se deformaron, %.2f con otra instancia más cerca, %.1f del HUD (ortográficos)\n",
				st.ns_build / 1e3 / st.vm_tasks, st.ns_pair / 1e3 / st.vm_tasks, st.ns_prev / 1e3 / st.vm_tasks,
				(double)st.groups / st.vm_tasks, (double)st.jumped / st.vm_tasks, (double)st.deformed / st.vm_tasks,
				(double)st.reordered / st.vm_tasks, (double)st.flat / st.vm_tasks);
		if (st.vm_tasks) {
			static const char *const why[VMATCH_WHY_COUNT] = {
				"con pareja", "nuevos", "se movieron (corte)", "saltaron", "se deformaron", "otro tamaño",
				"HUD", "se aplastarían", "por su objeto", "por su malla"
			};
			fprintf(o, "interp: grupos delante de la cámara por tarea:");
			for (int q = 0; q < VMATCH_WHY_COUNT; q++)
				fprintf(o, "%s %.1f %s", q ? "," : "", (double)st.why[q] / st.vm_tasks, why[q]);
			fputc('\n', o);
		}
		if (st.searches)
			fprintf(o,
				"interp: imagen: %llu búsquedas, %.0f bloques y %.0f SAD de %ux%u por búsqueda (%.1f por bloque)\n",
				(unsigned long long)st.searches, (double)st.sad_blocks / st.searches,
				(double)st.sad_evals / st.searches, st.o.block, st.o.block,
				(double)st.sad_evals / st.sad_blocks);
		if (st.fail_total)
			fprintf(o, "interp: imagen: bloques mal interpolados (≥ 1/4 de sus píxeles): %.1f por intermedio, %.0f %% en un "
				   "borde del campo de vectores\n",
				st.fail_total / (double)(st.n_metric[M_IMG_I] ? st.n_metric[M_IMG_I] : 1),
				100.0 * st.fail_edge / st.fail_total);
		for (int m = 0; m < M_COUNT; m++)
			if (st.n_metric[m])
				fprintf(o, "interp: %-12s PSNR %.2f dB, píxeles malos %.2f %%; en 4×4: PSNR %.2f dB, bloques malos "
					   "%.2f %% (%llu intermedios)\n", mname[m],
					st.sum_psnr[m] / st.n_metric[m], st.sum_bad[m] / st.n_metric[m],
					st.sum_psnr_low[m] / st.n_metric[m], st.sum_bad_low[m] / st.n_metric[m],
					(unsigned long long)st.n_metric[m]);
	}
	if (out[1])
		fclose(out[1]);
	if (st.csv)
		fclose(st.csv);
	if (st.tcsv)
		fclose(st.tcsv);
}
