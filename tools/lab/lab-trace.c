/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — laboratorio del recompilador: perfil exacto desde el intérprete
 * (docs/lab-recompilador.md).
 *
 * Se enlaza con salsa64-run (tools/lab/Makefile, salsa64-run-lab) y se activa
 * con SALSA64_LAB_TRACE=prefijo: el intérprete llama al gancho antes de cada
 * instrucción y aquí se simula el despachador del recompilador. Cada bloque
 * nuevo se traduce con salsa64_rec_compile (el emisor de verdad) y su extensión
 * dice cuándo se sale de él: las mismas reglas que el código generado (un salto
 * hacia adelante dentro del bloque no sale, la vuelta al inicio de un bucle
 * tampoco; todo lo demás es una entrada a otro bloque). Cada entrada suma una
 * al bloque y cada instrucción, una ejecución dentro de él. Al terminar escribe
 * <prefijo>.lab (el formato de rec-lab, con una línea H de ejecuciones por
 * instrucción) y <prefijo>.visits (una palabra de 32 bits por entrada a un
 * bloque: id en los 24 bits bajos, última instrucción ejecutada en los 8 altos)
 * que lab-cache.py usa para simular la caché de instrucciones.
 *
 *   SALSA64_LAB_TRACE=build/lab/ge/traza salsa64-run-lab -q --state-load e.state \
 *       --fields 5500 goldeneye.z64
 *
 * Variables: SALSA64_LAB_OPTS (opciones del emisor; por omisión PIN, como el
 * paquete de la PS3), SALSA64_LAB_VISITS (tope de entradas registradas, 40
 * millones), SALSA64_LAB_FROM (campo del VI desde el que se cuenta; por omisión
 * desde el primero). Ni el .lab ni el .visits se versionan: llevan el código del
 * juego.
 */
#include "lab-emit.h"

#include "core/n64.h"

extern void (*salsa64_lab_hook)(struct salsa64_cpu *cpu, uint64_t vpc, uint32_t pa);

struct tb {
	uint32_t vpc, start, end, id;
	uint8_t fr;
	bool frdep, loop;
	uint8_t nw;
	struct lab_emitted e;
	uint64_t entries;
	uint64_t icount[SALSA64_REC_MAX_INSNS];     /* ejecuciones de cada instrucción del bloque */
	uint32_t words[SALSA64_REC_MAX_INSNS];      /* las palabras MIPS con que se compiló */
};

static struct {
	struct salsa64_rec rec;
	uint32_t *code;
	bool ready;
	const char *prefix;
	uint32_t opts;
	struct tb **blocks;
	uint32_t nblocks, cap;
	int32_t *table;      /* (vpc, fr) → id + 1 */
	uint32_t tsize;
	struct tb *cur;
	uint32_t prev, prev_id;
	unsigned last_k;
	uint32_t *visits;
	uint64_t nvisits, max_visits;
	uint64_t insns, untracked, entries, new_blocks, recompiled;
	uint32_t from_field;
	bool counting;
	struct { uint64_t key; uint64_t count; } *edges;  /* (bloque anterior, bloque nuevo) → veces */
	uint32_t esize, ecount;
	uint32_t bad_pc[4096];     /* pcs que no compilan (un salto en la última palabra de una página…): se corren en el intérprete */
	uint64_t untracked_hist[1 << 12];   /* instrucciones sin seguir, por bits 31–20 del pc */
} L;

static uint32_t hash_key(uint32_t vpc, unsigned fr)
{
	return ((vpc >> 2) * 2654435761u) ^ (fr * 0x9E3779B1u);
}

static struct tb *lookup(uint32_t vpc, unsigned fr)
{
	for (uint32_t h = hash_key(vpc, fr) & (L.tsize - 1);; h = (h + 1) & (L.tsize - 1)) {
		int32_t v = L.table[h];
		if (!v)
			return NULL;
		struct tb *b = L.blocks[v - 1];
		if (b->vpc == vpc && b->fr == fr)
			return b;
	}
}

static void table_set(struct tb *b)
{
	for (uint32_t h = hash_key(b->vpc, b->fr) & (L.tsize - 1);; h = (h + 1) & (L.tsize - 1)) {
		int32_t v = L.table[h];
		if (!v || (L.blocks[v - 1]->vpc == b->vpc && L.blocks[v - 1]->fr == b->fr)) {
			L.table[h] = (int32_t)b->id + 1;  /* la versión nueva reemplaza a la vieja */
			return;
		}
	}
}

static void lab_ready(struct salsa64_cpu *cpu)
{
	L.code = aligned_alloc(65536, LAB_CACHE_BYTES);
	memset(L.code, 0, LAB_CACHE_BYTES);
	if (!lab_init(&L.rec, cpu, L.code, LAB_CACHE_BYTES, L.opts, false, false))
		die("el preámbulo no cabe", NULL);
	L.cap = 1u << 16;
	L.blocks = calloc(L.cap, sizeof *L.blocks);
	L.tsize = 1u << 20;
	L.table = calloc(L.tsize, sizeof *L.table);
	L.esize = 1u << 21;
	L.edges = calloc(L.esize, sizeof *L.edges);
	L.visits = malloc((size_t)(L.max_visits ? L.max_visits : 1) * sizeof *L.visits);
	L.ready = true;
}

/* Traduce el bloque que empieza en vpc con el Status.FR de ahora. NULL si el despachador no lo
 * compilaría (sin traducción, ROM, modo usuario) o no compila. */
static struct tb *make_block(struct salsa64_cpu *cpu, uint32_t vpc, uint32_t pa, unsigned fr)
{
	struct salsa64_rec *rec = &L.rec;
	if (cpu->tlb_writes != rec->x.tlb_writes)
		salsa64_rec_x_sync(rec);
	uint32_t pa2;
	unsigned seg = salsa64_rec_seg(rec, (uint64_t)(int64_t)(int32_t)vpc, &pa2);
	if (seg == SALSA64_REC_SEG_NONE || pa2 >= rec->rdram_size) {
		L.bad_pc[(vpc >> 2) & 4095] = vpc;
		return NULL;
	}
	struct tb *b = calloc(1, sizeof *b);
	if (!lab_emit(rec, (uint64_t)(int64_t)(int32_t)vpc, pa2, seg, fr, &b->e)) {
		free(b);
		L.bad_pc[(vpc >> 2) & 4095] = vpc;
		return NULL;
	}
	(void)pa;
	b->vpc = b->start = vpc;
	b->end = vpc + b->e.bytes;
	b->fr = (uint8_t)fr;
	b->frdep = b->e.frdep;
	b->loop = b->e.loop;
	b->nw = (uint8_t)(b->e.bytes / 4);
	memcpy(b->words, b->e.mips, b->nw * sizeof b->words[0]);
	if (L.nblocks >= (1u << 24) - 1)
		die("demasiados bloques", NULL);
	if (L.nblocks == L.cap) {
		L.cap *= 2;
		L.blocks = realloc(L.blocks, L.cap * sizeof *L.blocks);
	}
	b->id = L.nblocks;
	L.blocks[L.nblocks++] = b;
	table_set(b);
	L.new_blocks++;
	return b;
}

static struct tb *find_block(struct salsa64_cpu *cpu, uint32_t vpc, uint32_t pa)
{
	unsigned fr = (cpu->status & SALSA64_ST_FR) ? 1 : 0;
	struct tb *b = lookup(vpc, fr);
	if (!b) {
		struct tb *o = lookup(vpc, fr ^ 1);
		if (o && !o->frdep)
			b = o;  /* no depende de FR: el mismo código vale con los dos */
	}
	if (b) {  /* ¿sigue el mismo código en la física de ahora? (overlays; las páginas del pager vuelven en otro marco) */
		bool same = pa + 4u * b->nw <= L.rec.rdram_size;
		for (unsigned q = 0; q < b->nw && same; q++)
			same = salsa64_rec_fetch(&L.rec, pa + 4 * q) == b->words[q];
		if (same)
			return b;
		L.recompiled++;
		b = NULL;
	}
	return make_block(cpu, vpc, pa, fr);
}

static void edge(uint32_t from, uint32_t to)
{
	uint64_t key = ((uint64_t)from + 1) << 32 | to;
	for (uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (L.esize - 1);; h = (h + 1) & (L.esize - 1)) {
		if (L.edges[h].key == key) {
			L.edges[h].count++;
			return;
		}
		if (!L.edges[h].key) {
			if (L.ecount * 2 < L.esize) {
				L.edges[h].key = key;
				L.edges[h].count = 1;
				L.ecount++;
			}
			return;
		}
	}
}

static void log_visit(struct tb *c)
{
	L.prev_id = c->id;
	if (L.nvisits < L.max_visits)
		L.visits[L.nvisits++] = c->id | (uint32_t)(L.last_k > 255 ? 255 : L.last_k) << 24;
}

static void lab_hook(struct salsa64_cpu *cpu, uint64_t vpc64, uint32_t pa)
{
	uint32_t v = (uint32_t)vpc64;
	if (SALSA64_UNLIKELY(!L.counting)) {
		if (cpu->bus->vi.fields < L.from_field)
			return;
		L.counting = true;
		L.cur = NULL;
	}
	if (SALSA64_UNLIKELY(!L.ready))
		lab_ready(cpu);
	struct tb *c = L.cur;
	if (c) {
		if (v == L.prev + 4 ? v < c->end : (v > L.prev && v < c->end && v >= c->start)) {
			/* sigue dentro del bloque: secuencial, o un salto hacia adelante que el bloque contiene */
		} else if (v == c->start && c->loop && v != L.prev + 4) {
			/* la vuelta de un bucle de un solo bloque */
		} else {
			log_visit(c);
			c = NULL;
		}
	}
	if (!c) {
		if (L.bad_pc[(v >> 2) & 4095] == v) {
			L.cur = NULL;
			L.untracked++;
			L.untracked_hist[v >> 20]++;
			L.prev = v;
			return;
		}
		c = find_block(cpu, v, pa);
		L.cur = c;
		if (!c) {
			L.untracked++;
			L.untracked_hist[v >> 20]++;
			L.prev = v;
			return;
		}
		c->entries++;
		L.entries++;
		L.last_k = 0;
		edge(L.prev_id, c->id);
	}
	unsigned k = (v - c->start) >> 2;
	c->icount[k]++;
	if (k > L.last_k)
		L.last_k = k;
	L.prev = v;
	L.insns++;
}

static void lab_finish(void)
{
	if (!L.ready || !L.prefix)
		return;
	if (L.cur)
		log_visit(L.cur);
	char path[1024];
	snprintf(path, sizeof path, "%s.lab", L.prefix);
	FILE *out = fopen(path, "w");
	if (!out) {
		perror(path);
		return;
	}
	lab_print_prologue(out, &L.rec, L.code, L.opts, -1, "lab-trace");
	for (uint32_t i = 0; i < L.nblocks; i++) {
		struct tb *b = L.blocks[i];
		lab_print_block(out, b->vpc, b->entries, &b->e);
		fprintf(out, "H");
		for (unsigned q = 0; q < b->nw; q++)
			fprintf(out, " %llu", (unsigned long long)b->icount[q]);
		fprintf(out, "\n");
	}
	for (uint32_t h = 0; h < L.esize; h++)
		if (L.edges[h].key)
			fprintf(out, "E %llu %llu %llu\n", (unsigned long long)(L.edges[h].key >> 32) - 1, (unsigned long long)(L.edges[h].key & 0xFFFFFFFFu),
				(unsigned long long)L.edges[h].count);
	fprintf(out, "# bloques=%u entradas=%llu instrucciones=%llu sin_seguir=%llu recompilados=%llu\n", L.nblocks,
		(unsigned long long)L.entries, (unsigned long long)L.insns, (unsigned long long)L.untracked,
		(unsigned long long)L.recompiled);
	fclose(out);
	snprintf(path, sizeof path, "%s.visits", L.prefix);
	out = fopen(path, "wb");
	if (out) {
		fwrite(L.visits, sizeof *L.visits, L.nvisits, out);
		fclose(out);
	}
	for (unsigned i = 0; i < 4096; i++)
		if (L.untracked_hist[i] * 50 > L.untracked)
			fprintf(stderr, "lab-trace: sin seguir: %08x_____ %llu instrucciones (%.1f %%)\n", i << 20,
				(unsigned long long)L.untracked_hist[i], 100.0 * L.untracked_hist[i] / L.untracked);
	fprintf(stderr, "lab-trace: %u bloques, %llu entradas, %llu instrucciones (%.1f por entrada), %llu sin seguir, %llu entradas registradas → %s.lab\n",
		L.nblocks, (unsigned long long)L.entries, (unsigned long long)L.insns, L.entries ? (double)L.insns / L.entries : 0.0,
		(unsigned long long)L.untracked, (unsigned long long)L.nvisits, L.prefix);
}

__attribute__((constructor)) static void lab_trace_init(void)
{
	L.prefix = getenv("SALSA64_LAB_TRACE");
	if (!L.prefix || !*L.prefix)
		return;
	const char *o = getenv("SALSA64_LAB_OPTS"), *v = getenv("SALSA64_LAB_VISITS"), *f = getenv("SALSA64_LAB_FROM");
	L.opts = o ? (uint32_t)strtoul(o, NULL, 0) : SALSA64_REC_OPT_PIN;
	L.max_visits = v ? strtoull(v, NULL, 0) : 40000000ull;
	L.from_field = f ? (uint32_t)strtoul(f, NULL, 0) : 0;
	salsa64_lab_hook = lab_hook;
	atexit(lab_finish);
}
