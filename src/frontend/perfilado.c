/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — perfilado: archivo de la sesión y su escritura (perfilado.h). */
#include "perfilado.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cpu/rec_sample.h"
#include "cpu/rec_trace.h"

/* Lo escrito en el anillo, antes de publicar el nuevo head (y lo leído,
 * antes de soltar el tail). */
static inline void barrier(void)
{
#if defined(__powerpc64__) || defined(__powerpc__)
	__asm__ volatile("lwsync" ::: "memory");
#else
	__sync_synchronize();
#endif
}

/* ---- Configuración -------------------------------------------------------------- */

void perf_cfg_defaults(struct perf_cfg *c)
{
	memset(c, 0, sizeof *c);
	c->on = c->trace = c->jr = c->blocks = c->samples = c->profile = c->tasks = true;
	c->dumps = 8;
	c->cap_mb = 200;
	c->stall_ms = 25;
	c->trace_s = 10;
	c->trace_len = 16384;
	c->dump_gap_s = 30;
}

static bool yes(const char *v)
{
	return !(v[0] == 'n' || v[0] == 'N' || v[0] == '0' || v[0] == 'f' || v[0] == 'F');
}

void perf_cfg_parse(struct perf_cfg *c, const char *text)
{
	char line[128];
	while (*text) {
		size_t n = strcspn(text, "\n");
		size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
		memcpy(line, text, k);
		line[k] = '\0';
		text += n + (text[n] == '\n');
		char *hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		char key[32], val[32];
		if (sscanf(line, " %31[a-z_] = %31s", key, val) != 2)
			continue;
		unsigned long u = strtoul(val, NULL, 10);
		if (!strcmp(key, "perfilado"))
			c->on = yes(val);
		else if (!strcmp(key, "traza"))
			c->trace = yes(val);
		else if (!strcmp(key, "jr"))
			c->jr = yes(val);
		else if (!strcmp(key, "bloques"))
			c->blocks = yes(val);
		else if (!strcmp(key, "muestreo"))
			c->samples = yes(val);
		else if (!strcmp(key, "perfil"))
			c->profile = yes(val);
		else if (!strcmp(key, "tareas"))
			c->tasks = yes(val);
		else if (!strcmp(key, "volcados"))
			c->dumps = (uint32_t)u;
		else if (!strcmp(key, "tope_mb") && u >= 1)
			c->cap_mb = (uint32_t)u;
		else if (!strcmp(key, "tiron_ms") && u >= 1)
			c->stall_ms = (uint32_t)u;
		else if (!strcmp(key, "traza_cada_s") && u >= 1)
			c->trace_s = (uint32_t)u;
		else if (!strcmp(key, "traza_largo") && u >= 64)
			c->trace_len = (uint32_t)(u > SALSA64_RT_WIN_MAX ? SALSA64_RT_WIN_MAX : u);
		else if (!strcmp(key, "volcados_cada_s"))
			c->dump_gap_s = (uint32_t)u;
	}
}

void perf_cfg_text(const struct perf_cfg *c, char *buf, size_t size)
{
	snprintf(buf, size,
		 "perfilado=%s traza=%s jr=%s bloques=%s muestreo=%s perfil=%s tareas=%s volcados=%u tope_mb=%u "
		 "tiron_ms=%u traza_cada_s=%u traza_largo=%u volcados_cada_s=%u",
		 c->on ? "si" : "no", c->trace ? "si" : "no", c->jr ? "si" : "no", c->blocks ? "si" : "no",
		 c->samples ? "si" : "no", c->profile ? "si" : "no", c->tasks ? "si" : "no", c->dumps, c->cap_mb,
		 c->stall_ms, c->trace_s, c->trace_len, c->dump_gap_s);
}

/* ---- Anillos y registros ---------------------------------------------------------- */

void perf_ring_init(struct perf_ring *r, uint8_t *buf, uint32_t size)
{
	memset(r, 0, sizeof *r);
	r->buf = buf;
	r->size = size;
}

bool perf_begin(struct perf_rw *w, struct perf_ring *r, unsigned type, uint32_t len)
{
	uint32_t head = r->head, total = 4 + len;
	w->r = r;
	w->pos = w->end = head;
	if (len >= (1u << 24) || total > r->size - (head - r->tail)) {
		r->lost += total;
		return false;
	}
	w->end = head + total;
	perf_w32(w, type << 24 | len);
	return true;
}

void perf_wbytes(struct perf_rw *w, const void *p, uint32_t n)
{
	const uint8_t *b = p;
	for (uint32_t k = 0; k < n; k++)
		perf_w8(w, b[k]);
}

void perf_end(struct perf_rw *w)
{
	while (w->pos != w->end)
		perf_w8(w, 0);
	barrier();
	w->r->head = w->end;
}

bool perf_put(struct perf_ring *r, unsigned type, const void *payload, uint32_t len)
{
	struct perf_rw w;
	if (!perf_begin(&w, r, type, len))
		return false;
	perf_wbytes(&w, payload, len);
	perf_end(&w);
	return true;
}

bool perf_put_text(struct perf_ring *r, unsigned type, uint32_t ms, const char *text)
{
	struct perf_rw w;
	uint32_t n = (uint32_t)strlen(text);
	if (!perf_begin(&w, r, type, 4 + n))
		return false;
	perf_w32(&w, ms);
	perf_wbytes(&w, text, n);
	perf_end(&w);
	return true;
}

bool perf_put_values(struct perf_ring *r, unsigned type, uint32_t ms, const uint32_t *v, unsigned n)
{
	struct perf_rw w;
	if (!perf_begin(&w, r, type, 8 + 4 * n))
		return false;
	perf_w32(&w, ms);
	perf_w16(&w, n);
	perf_w16(&w, 0);
	for (unsigned k = 0; k < n; k++)
		perf_w32(&w, v[k]);
	perf_end(&w);
	return true;
}

#define PERF_NAME(n) #n ","
static const char window_names[] = PERF_WINDOW_FIELDS(PERF_NAME);
static const char stall_names[] = PERF_STALL_FIELDS(PERF_NAME);
#undef PERF_NAME

void perf_names_text(char *buf, size_t size)
{
	/* Sin la coma final de cada lista. */
	snprintf(buf, size, "ventana=%.*s\ntiron=%.*s\n", (int)sizeof window_names - 2, window_names,
		 (int)sizeof stall_names - 2, stall_names);
}

bool perf_put_task(struct perf_ring *r, const struct perf_task *t)
{
	struct perf_rw w;
	if (!perf_begin(&w, r, PERF_R_TASKS, 4 + PERF_TASK_BYTES))
		return false;
	perf_w16(&w, PERF_TASK_BYTES);
	perf_w16(&w, 1);
	perf_w32(&w, t->ms);
	perf_w32(&w, t->field);
	perf_w32(&w, t->task);
	perf_w32(&w, t->commands);
	perf_w32(&w, t->triangles);
	perf_w16(&w, t->draws);
	perf_w16(&w, t->textures);
	perf_w16(&w, t->unknown);
	perf_w8(&w, t->last_unknown);
	perf_w8(&w, t->flags);
	perf_w16(&w, t->approx_cc);
	perf_w16(&w, t->approx_draws);
	perf_w16(&w, t->skipped);
	perf_w16(&w, t->dl_errors);
	perf_w16(&w, t->spe_us);
	perf_w16(&w, t->translate_us);
	perf_w16(&w, t->set_wait_us);
	perf_w16(&w, t->fp_loads);
	perf_w8(&w, t->readbacks);
	perf_w8(&w, t->news);
	perf_end(&w);  /* 2 bytes de relleno */
	return true;
}

bool perf_put_block(struct perf_ring *r, uint32_t ms, uint32_t field, const struct salsa64_rec_trace_block *b)
{
	struct perf_rw w;
	if (!perf_begin(&w, r, PERF_R_BLOCKS, 4 + PERF_BLOCK_BYTES))
		return false;
	perf_w16(&w, PERF_BLOCK_BYTES);
	perf_w16(&w, 1);
	perf_w32(&w, ms);
	perf_w32(&w, field);
	perf_w32(&w, b->vpc);
	perf_w32(&w, b->pa);
	perf_w32(&w, b->code);
	perf_w32(&w, b->ticks);
	perf_w64(&w, b->hash);
	perf_w16(&w, b->id);
	perf_w16(&w, b->hot);
	perf_w16(&w, b->cold);
	perf_w8(&w, b->n);
	perf_w8(&w, b->end);
	perf_w8(&w, b->seg);
	perf_w8(&w, b->flags);
	for (unsigned k = 0; k < SALSA64_RT_CLASSES; k++)
		perf_w8(&w, b->ins[k]);
	perf_w8(&w, 0);
	for (unsigned k = 0; k < SALSA64_RT_CLASSES; k++)
		perf_w16(&w, b->whot[k]);
	for (unsigned k = 0; k < SALSA64_RT_CLASSES; k++)
		perf_w16(&w, b->wcold[k]);
	perf_w16(&w, 0);
	perf_w32(&w, b->target);
	perf_w8(&w, b->nbr);
	perf_end(&w);  /* 3 bytes de relleno */
	return true;
}

bool perf_put_evict(struct perf_ring *r, uint32_t ms, uint32_t field, unsigned kind, unsigned seg, unsigned killed)
{
	struct perf_rw w;
	if (!perf_begin(&w, r, PERF_R_EVICT, 16))
		return false;
	perf_w32(&w, ms);
	perf_w32(&w, field);
	perf_w8(&w, kind);
	perf_w8(&w, seg);
	perf_w16(&w, 0);
	perf_w32(&w, killed);
	perf_end(&w);
	return true;
}

bool perf_put_slow(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_trace_slow_stats *s)
{
	struct perf_rw w;
	if (!perf_begin(&w, r, PERF_R_SLOW, 8 + 12 * SALSA64_RT_SLOW_N + 16 * SALSA64_RT_TOP_PC))
		return false;
	perf_w32(&w, ms);
	perf_w16(&w, SALSA64_RT_SLOW_N);
	perf_w16(&w, SALSA64_RT_TOP_PC);
	for (unsigned k = 0; k < SALSA64_RT_SLOW_N; k++) {
		perf_w32(&w, s->calls[k]);
		perf_w64(&w, s->ticks[k]);
	}
	for (unsigned k = 0; k < SALSA64_RT_TOP_PC; k++) {
		perf_w32(&w, s->top[k].pc);
		perf_w64(&w, s->top[k].ticks);
		perf_w32(&w, s->top[k].calls);
	}
	perf_end(&w);
	return true;
}

/* COP0 y MI (desde la ventana anterior): ms, n entradas no vacías, relleno;
 * por entrada: índice u16 (enum salsa64_rt_cop0), relleno u16, llamadas u32,
 * ticks u64. */
static bool put_cop0(struct perf_ring *r, uint32_t ms, const uint32_t *calls, const uint64_t *ticks)
{
	struct perf_rw w;
	uint32_t n = 0;
	for (unsigned k = 0; k < SALSA64_RT_COP0_N; k++)
		n += calls[k] || ticks[k];
	if (!n)
		return true;
	if (!perf_begin(&w, r, PERF_R_COP0, 8 + 16 * n))
		return false;
	perf_w32(&w, ms);
	perf_w16(&w, n);
	perf_w16(&w, 0);
	for (unsigned k = 0; k < SALSA64_RT_COP0_N; k++) {
		if (!(calls[k] || ticks[k]))
			continue;
		perf_w16(&w, k);
		perf_w16(&w, 0);
		perf_w32(&w, calls[k]);
		perf_w64(&w, ticks[k]);
	}
	perf_end(&w);
	return true;
}

bool perf_put_slow_window(struct perf_ring *r, uint32_t ms, struct salsa64_rec_trace *t,
			  struct salsa64_rec_trace_prev *prev, struct salsa64_rec_trace_slow_stats *out)
{
	struct salsa64_rec_trace_slow_stats local, *delta = out ? out : &local;
	memset(delta, 0, sizeof *delta);
	for (unsigned k = 0; k < SALSA64_RT_SLOW_N; k++) {
		delta->calls[k] = t->slow.calls[k] - prev->slow.calls[k];
		delta->ticks[k] = t->slow.ticks[k] - prev->slow.ticks[k];
		prev->slow.calls[k] = t->slow.calls[k];
		prev->slow.ticks[k] = t->slow.ticks[k];
	}
	/* Si sigue pedido el vaciado de la ventana anterior, no hubo nada lento
	 * desde entonces: la tabla es la vieja. */
	if (!t->top_reset)
		memcpy(delta->top, t->slow_field.top, sizeof delta->top);
	t->top_reset = 1;
	uint32_t calls[SALSA64_RT_COP0_N];
	uint64_t ticks[SALSA64_RT_COP0_N];
	for (unsigned k = 0; k < SALSA64_RT_COP0_N; k++) {
		calls[k] = t->cop0.calls[k] - prev->cop0.calls[k];
		ticks[k] = t->cop0.ticks[k] - prev->cop0.ticks[k];
		prev->cop0.calls[k] = t->cop0.calls[k];
		prev->cop0.ticks[k] = t->cop0.ticks[k];
	}
	bool ok = perf_put_slow(r, ms, delta);
	return put_cop0(r, ms, calls, ticks) && ok;
}

/* Total de sesión: ms, n entradas, n razones, FR (2 × llamadas u32 + ticks u64),
 * PCs lentos acumulados (cuenta u32 y 16 × pc/ticks/llamadas) y por entrada
 * no vacía: índice u16, relleno u16, ticks u64, llamadas por razón u32. */
bool perf_put_fpu(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_trace *t)
{
	struct perf_rw w;
	const struct salsa64_rec_trace_fpu *f = &t->fpu;
	uint32_t n = 0;
	for (unsigned k = 0; k < SALSA64_RT_FPU_N; k++)
		n += f->ticks[k] != 0 || f->calls[k][0] || f->calls[k][1] || f->calls[k][2] || f->calls[k][3];
	if (!perf_begin(&w, r, PERF_R_FPU, 12 + 24 + 4 + 16 * SALSA64_RT_TOP_PC + (12 + 4 * SALSA64_RT_FPU_R_N) * n))
		return false;
	perf_w32(&w, ms);
	perf_w16(&w, n);
	perf_w16(&w, SALSA64_RT_FPU_R_N);
	perf_w32(&w, 0);
	for (unsigned k = 0; k < 2; k++) {
		perf_w32(&w, f->fr_calls[k]);
		perf_w64(&w, f->fr_ticks[k]);
	}
	perf_w32(&w, SALSA64_RT_TOP_PC);
	for (unsigned k = 0; k < SALSA64_RT_TOP_PC; k++) {
		perf_w32(&w, t->slow.top[k].pc);
		perf_w64(&w, t->slow.top[k].ticks);
		perf_w32(&w, t->slow.top[k].calls);
	}
	for (unsigned k = 0; k < SALSA64_RT_FPU_N; k++) {
		if (!(f->ticks[k] || f->calls[k][0] || f->calls[k][1] || f->calls[k][2] || f->calls[k][3]))
			continue;
		perf_w16(&w, k);
		perf_w16(&w, 0);
		perf_w64(&w, f->ticks[k]);
		for (unsigned j = 0; j < SALSA64_RT_FPU_R_N; j++)
			perf_w32(&w, f->calls[k][j]);
	}
	perf_end(&w);
	return true;
}

bool perf_put_compile(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_trace_compile_stats *s)
{
	struct perf_rw w;
	if (!perf_begin(&w, r, PERF_R_COMPILE, 16 + 16 * SALSA64_RT_COMPILE_N))
		return false;
	perf_w32(&w, ms);
	perf_w16(&w, SALSA64_RT_COMPILE_N);
	perf_w16(&w, 0);
	perf_w32(&w, s->max_field_compiles);
	perf_w32(&w, s->max_field);
	for (unsigned k = 0; k < SALSA64_RT_COMPILE_N; k++) {
		perf_w32(&w, s->calls[k]);
		perf_w64(&w, s->ticks[k]);
		perf_w32(&w, s->words[k]);
	}
	perf_end(&w);
	return true;
}

bool perf_put_trace(struct perf_ring *r, uint32_t ms, uint32_t field, uint64_t cycles, const struct salsa64_rec_trace *t,
		    unsigned len)
{
	struct perf_rw w;
	if (!perf_begin(&w, r, PERF_R_TRACE, 28 + 4 * len + 24 * t->ntab))
		return false;
	perf_w32(&w, ms);
	perf_w32(&w, field);
	perf_w64(&w, cycles);
	perf_w32(&w, len);
	perf_w32(&w, t->ntab);
	perf_w32(&w, t->tab_full);
	for (unsigned k = 0; k < len; k++)
		perf_w32(&w, t->win[k]);
	for (unsigned k = 0; k < t->ntab; k++) {
		const struct salsa64_rec_trace_id *e = &t->tab[k];
		perf_w16(&w, e->id);
		perf_w8(&w, e->n);
		perf_w8(&w, e->end);
		perf_w32(&w, e->vpc);
		perf_w32(&w, e->code);
		perf_w16(&w, e->hot);
		perf_w16(&w, e->cold);
		perf_w32(&w, e->target);
		perf_w8(&w, e->nbr);
		perf_w8(&w, 0);
		perf_w16(&w, 0);
	}
	perf_end(&w);
	return true;
}

bool perf_put_jr(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_trace *t)
{
	struct perf_rw w;
	uint32_t n = 0;
	for (unsigned k = 0; k < SALSA64_RT_JR_SITES; k++)
		n += t->jr_sites[k].vpc && t->jr_sites[k].count;
	if (!perf_begin(&w, r, PERF_R_JR, 12 + 16 * n))
		return false;
	perf_w32(&w, ms);
	perf_w32(&w, n);
	perf_w32(&w, t->jr_full);
	for (unsigned k = 0; k < SALSA64_RT_JR_SITES; k++) {
		const struct salsa64_rec_trace_jrsite *e = &t->jr_sites[k];
		if (!e->vpc || !e->count)
			continue;
		perf_w32(&w, e->vpc);
		perf_w32(&w, e->count);
		perf_w32(&w, e->repeat);
		perf_w8(&w, e->end);
		perf_w8(&w, e->n);
		perf_w16(&w, 0);
	}
	perf_end(&w);
	return true;
}

bool perf_put_samples(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_sample *s)
{
	struct perf_rw w;
	uint32_t n = 0;
	for (unsigned k = 0; k < SALSA64_SMP_TABLE; k++)
		n += s->table[k].pc && s->table[k].samples;
	if (!perf_begin(&w, r, PERF_R_SAMPLES, 4 + 8 + 4 + 4 * SALSA64_SMP_N + 12 + 16 * n))
		return false;
	perf_w32(&w, ms);
	perf_w64(&w, s->ticks);
	perf_w32(&w, SALSA64_SMP_N);
	for (unsigned m = 0; m < SALSA64_SMP_N; m++)
		perf_w32(&w, s->marks[m]);
	perf_w32(&w, s->unknown);
	perf_w32(&w, s->full);
	perf_w32(&w, n);
	for (unsigned k = 0; k < SALSA64_SMP_TABLE; k++) {
		const struct salsa64_rec_sample_entry *e = &s->table[k];
		if (!e->pc || !e->samples)
			continue;
		perf_w32(&w, e->pc);
		perf_w32(&w, e->samples);
		perf_w16(&w, e->n);
		perf_w16(&w, e->words);
		perf_w16(&w, e->hot);
		perf_w16(&w, 0);
	}
	perf_end(&w);
	return true;
}

/* ---- Archivos ------------------------------------------------------------------------ */

/* a/b en `out`; false si no cabe. */
static bool join(char *out, size_t size, const char *a, const char *b)
{
	int n = snprintf(out, size, "%s/%s", a, b);
	return n > 0 && (size_t)n < size;
}

static bool is_session(const char *name, unsigned *n)
{
	char *end;
	unsigned long v = strtoul(name, &end, 10);
	if (end == name || strcmp(end, ".sesion"))
		return false;
	*n = (unsigned)v;
	return true;
}

/* Una entrada de la raíz: archivo con su tamaño y fecha. */
struct item {
	char path[PERF_PATH_MAX];
	uint64_t bytes;
	long long mtime;
	unsigned n;     /* número de sesión (o el prefijo de un volcado) */
};

/* Recorre <raíz>/<juego>/ y <raíz>/<juego>/volcados/. Con `out`, anota
 * hasta `max` de los que se pueden borrar (no juego.prf ni `keep`). */
static uint64_t walk(const char *root, unsigned *sessions, struct item *out, unsigned max, unsigned *count,
		     const char *keep)
{
	uint64_t total = 0;
	DIR *d = opendir(root);
	if (!d)
		return 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		char game[PERF_PATH_MAX];
		if (!join(game, sizeof game, root, de->d_name))
			continue;
		for (int sub = 0; sub < 2; sub++) {
			char dir[PERF_PATH_MAX];
			if (!sub)
				memcpy(dir, game, sizeof dir);
			else if (!join(dir, sizeof dir, game, "volcados"))
				break;
			DIR *g = opendir(dir);
			if (!g)
				break;
			struct dirent *fe;
			while ((fe = readdir(g)) != NULL) {
				if (fe->d_name[0] == '.')
					continue;
				char path[PERF_PATH_MAX];
				struct stat st;
				if (!join(path, sizeof path, dir, fe->d_name) || stat(path, &st) != 0 || S_ISDIR(st.st_mode))
					continue;
				total += (uint64_t)st.st_size;
				unsigned n = 0;
				bool ses = !sub && is_session(fe->d_name, &n);
				if (ses && sessions)
					(*sessions)++;
				if (sub)
					n = (unsigned)strtoul(fe->d_name, NULL, 10);
				bool keep_it = (keep && !strcmp(path, keep)) || (!sub && !ses);
				if (out && !keep_it && *count < max) {
					struct item *it = &out[(*count)++];
					memcpy(it->path, path, sizeof it->path);
					it->bytes = (uint64_t)st.st_size;
					it->mtime = (long long)st.st_mtime;
					it->n = n;
				}
			}
			closedir(g);
		}
	}
	closedir(d);
	return total;
}

uint64_t perf_dir_bytes(const char *root, unsigned *sessions)
{
	if (sessions)
		*sessions = 0;
	return walk(root, sessions, NULL, 0, NULL, NULL);
}

static int item_cmp(const void *a, const void *b)
{
	const struct item *x = a, *y = b;
	if (x->mtime != y->mtime)
		return x->mtime < y->mtime ? -1 : 1;
	if (x->n != y->n)
		return x->n < y->n ? -1 : 1;
	return strcmp(x->path, y->path);
}

#define ROTATE_MAX 4096u

unsigned perf_rotate(const char *root, uint64_t limit, const char *keep)
{
	struct item *items = malloc(ROTATE_MAX * sizeof *items);
	if (!items)
		return 0;
	unsigned count = 0, removed = 0;
	uint64_t total = walk(root, NULL, items, ROTATE_MAX, &count, keep);
	if (total > limit) {
		qsort(items, count, sizeof *items, item_cmp);
		for (unsigned k = 0; k < count && total > limit; k++) {
			if (unlink(items[k].path) == 0) {
				total -= items[k].bytes;
				removed++;
			}
		}
	}
	free(items);
	return removed;
}

bool perf_session_open(struct perf_session *s, const char *root, const char *game, uint64_t cap, uint64_t reserve)
{
	struct perf_ring *rings[PERF_RINGS];
	memcpy(rings, s->rings, sizeof rings);
	memset(s, 0, sizeof *s);
	memcpy(s->rings, rings, sizeof rings);
	s->cap = cap;
	mkdir(root, 0777);
	if (!join(s->dir, sizeof s->dir, root, game))
		return false;
	mkdir(s->dir, 0777);
	s->removed = perf_rotate(root, cap > reserve ? cap - reserve : 0, NULL);
	unsigned top = 0;
	DIR *d = opendir(s->dir);
	if (d) {
		struct dirent *de;
		unsigned n;
		while ((de = readdir(d)) != NULL)
			if (is_session(de->d_name, &n) && n > top)
				top = n;
		closedir(d);
	}
	s->n = top + 1;
	char name[24];
	snprintf(name, sizeof name, "%u.sesion", s->n);
	if (!join(s->path, sizeof s->path, s->dir, name))
		return false;
	s->others = perf_dir_bytes(root, NULL);
	s->f = fopen(s->path, "wb");
	if (!s->f)
		return false;
	uint8_t h[PERF_HEADER] = { 'S', '6', '4', 'P', 0, PERF_VERSION, 0, PERF_HEADER };
	if (fwrite(h, 1, sizeof h, s->f) != sizeof h) {
		fclose(s->f);
		s->f = NULL;
		return false;
	}
	s->written = sizeof h;
	return true;
}

static size_t write_out(struct perf_session *s, const uint8_t *p, size_t n)
{
	if (!n)
		return 0;
	if (fwrite(p, 1, n, s->f) != n)
		s->write_errors++;
	s->written += n;
	return n;
}

size_t perf_drain(struct perf_session *s)
{
	size_t total = 0;
	if (!s->f)
		return 0;
	for (unsigned i = 0; i < PERF_RINGS; i++) {
		struct perf_ring *r = s->rings[i];
		if (!r)
			continue;
		uint32_t head = r->head, tail = r->tail;
		barrier();  /* el contenido, después de ver head */
		uint32_t mask = r->size - 1, a = tail & mask, n = head - tail;
		if (n) {
			uint32_t first = n < r->size - a ? n : r->size - a;
			total += write_out(s, r->buf + a, first);
			total += write_out(s, r->buf, n - first);
			barrier();
			r->tail = head;
		}
		uint32_t lost = r->lost;
		if (lost != r->lost_seen) {
			uint8_t rec[12] = { PERF_R_LOST, 0, 0, 8, 0, 0, 0, (uint8_t)i };
			uint32_t d = lost - r->lost_seen;
			rec[8] = (uint8_t)(d >> 24);
			rec[9] = (uint8_t)(d >> 16);
			rec[10] = (uint8_t)(d >> 8);
			rec[11] = (uint8_t)d;
			total += write_out(s, rec, sizeof rec);
			r->lost_seen = lost;
		}
	}
	if (total)
		fflush(s->f);
	if (s->others + s->written > s->cap)
		s->over = true;
	return total;
}

void perf_session_close(struct perf_session *s)
{
	if (!s->f)
		return;
	perf_drain(s);
	if (fclose(s->f) != 0)
		s->write_errors++;
	s->f = NULL;
}
