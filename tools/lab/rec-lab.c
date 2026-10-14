/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — laboratorio del recompilador (docs/lab-recompilador.md).
 *
 * Traduce con el emisor de verdad (src/cpu/rec_emit.c) los bloques de un
 * perfil sobre una imagen de la RDRAM, en la Mac (o cualquier host que no es
 * ppc64): el emisor es C puro y escribe los bytes de PowerPC en un arreglo,
 * sin ejecutarlos. Lo que sale es un archivo de texto con, por bloque, las
 * palabras MIPS, las palabras PPC (calientes y frías) y dónde empieza el
 * código de cada instrucción MIPS (tools/lab/lab-analiza.py lo lee).
 *
 * Se incluye rec.c (en lab-emit.h) para ver sus funciones internas (preámbulo,
 * tramos): no se enlaza aparte. Con SALSA64_REC_LAB el emisor anota además las
 * marcas.
 *
 *   rec-lab --rdram volcado.bin --prf juego.prf -o salida.lab
 *   rec-lab --rdram volcado.bin --pcs perfil.tsv -o salida.lab
 *   rec-lab --rom juego.z64 --state escena.state --prf juego.prf -o salida.lab
 *
 * Con --state (de salsa64-run --state-save) se carga la máquina entera: la
 * RDRAM y la TLB del CPU, que hacen falta para el código mapeado (GoldenEye,
 * Perfect Dark…). Con --rdram solo se traduce lo que está en KSEG0.
 *
 * Ni el volcado ni la salida se versionan: llevan el código del juego.
 */
#include "lab-emit.h"

#include "core/n64.h"
#include "core/state.h"

static bool state_fread(void *ctx, void *data, uint32_t len)
{
	return fread(data, 1, len, (FILE *)ctx) == len;
}

struct lab_args {
	const char *rdram, *prf, *pcs, *out, *rom, *state, *romwin;
	uint32_t opts;
	int fr;               /* Status.FR para los bloques sin dato propio (-1: el del estado cargado) */
	unsigned max_blocks;
	bool verbose, far_calls;
	unsigned scan_rom;
	unsigned nmaps;
	struct { uint32_t va, pa, len; } maps[8];   /* --map: páginas mapeadas a mano (sin TLB que leer) */
};

struct lab_job {
	uint32_t vpc;
	uint32_t count;       /* entradas al bloque en el perfil */
	uint16_t n_expect;    /* instrucciones según el perfil (0: sin dato) */
	uint64_t hash;        /* huella según el perfil (0: sin dato) */
	int fr;               /* -1: el de la línea de órdenes */
	unsigned prf_entry;   /* índice + 1 en prf_table.entries, 0 = sin perfil */
};

static struct salsa64_rec_profile prf_table;  /* el perfil de --prf: sus sitios de salida salen como líneas T */

static unsigned load_prf(const char *path, struct lab_job **jobs)
{
	struct salsa64_rec_profile *pp = &prf_table;
#define p (*pp)
	if (!salsa64_rec_profile_load(&p, path, NULL))
		die("perfil ilegible", path);
	struct lab_job *j = calloc(p.count, sizeof *j);
	for (unsigned k = 0; k < p.count; k++) {
		const struct salsa64_rec_profile_entry *e = &p.entries[k];
		j[k] = (struct lab_job){ e->vpc, e->score, e->n, e->hash, -1, k + 1 };
		/* Un bloque visto solo con FR = 1 se compila con FR = 1; si dependió de FR en
		 * las dos, el de FR = 0. */
		if ((e->flags & SALSA64_RPF_FR_SEEN1) && !(e->flags & SALSA64_RPF_FR_SEEN0))
			j[k].fr = 1;
		else if (e->flags & SALSA64_RPF_FR_SEEN0)
			j[k].fr = 0;
	}
	*jobs = j;
	return p.count;
#undef p
}

/* "vpc cuenta" por línea (hex o decimal; # comenta). */
static unsigned load_pcs(const char *path, struct lab_job **jobs)
{
	FILE *f = fopen(path, "r");
	if (!f)
		die("no se puede abrir", path);
	unsigned cap = 1024, n = 0;
	struct lab_job *j = calloc(cap, sizeof *j);
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		char *e;
		unsigned long long pc = strtoull(line, &e, 16);
		unsigned long long cnt = strtoull(e, &e, 10);
		if (n == cap)
			j = realloc(j, (cap *= 2) * sizeof *j);
		j[n++] = (struct lab_job){ (uint32_t)pc, (uint32_t)(cnt > 0xFFFFFFFFu ? 0xFFFFFFFFu : cnt), 0, 0, -1, 0 };
	}
	fclose(f);
	*jobs = j;
	return n;
}

/* Algunos juegos (GoldenEye, Perfect Dark…) mapean su código por la TLB con un
 * pager: páginas de 4 KB de la ROM que se copian a marcos de la RDRAM cuando
 * hacen falta. De las entradas de la TLB del estado se deduce la correspondencia
 * lineal «dirección virtual → desplazamiento en la ROM» buscando el contenido
 * de cada marco residente en la ROM; las páginas que no están residentes se
 * traducen luego con ella a la física de la ROM (0x10000000 + desplazamiento). */
struct romwin {
	bool ok;
	uint32_t vlo, vhi;    /* ventana virtual [vlo, vhi) */
	int64_t delta;        /* desplazamiento en la ROM = va - vlo + rom_base */
	int64_t rom_base;
	unsigned votes, entries;
};

static void find_romwin(const struct salsa64_cpu *cpu, const uint8_t *rdram, uint32_t rdram_size,
			const uint8_t *rom, size_t romsize, struct romwin *w, bool verbose)
{
	enum { MAXV = 64 };
	int64_t bases[MAXV];
	unsigned counts[MAXV], nb = 0, total = 0;
	uint32_t vmin = 0xFFFFFFFFu, vmax = 0;
	memset(w, 0, sizeof *w);
	for (unsigned t = 0; t < SALSA64_TLB_ENTRIES; t++) {
		const struct salsa64_tlb_entry *e = &cpu->tlb[t];
		if (e->page_mask != 0)
			continue;  /* solo páginas de 4 KB */
		for (unsigned h = 0; h < 2; h++) {
			uint32_t lo = e->entry_lo[h];
			uint32_t va = ((uint32_t)e->entry_hi & 0xFFFFE000u) + h * 0x1000u;
			uint32_t pa = (lo >> 6) << 12;
			if (!(lo & 2) || va < 0x10000 || va >= 0x80000000u || pa + 4096 > rdram_size)
				continue;
			const uint8_t *fr = rdram + pa;
			/* búsqueda del marco en la ROM: 256 bytes de sonda con alineación de 4 */
			int64_t found = -1;
			const uint8_t *p = rom;
			size_t left = romsize;
			while (left >= 4096) {
				const uint8_t *q = memchr(p, fr[0], left - 4095);
				if (!q)
					break;
				if (memcmp(q, fr, 4096) == 0) {
					found = q - rom;
					break;
				}
				left -= (size_t)(q - p) + 1;
				p = q + 1;
			}
			if (found < 0)
				continue;
			int64_t base = found - (int64_t)va;
			unsigned i;
			for (i = 0; i < nb; i++)
				if (bases[i] == base)
					break;
			if (i == nb && nb < MAXV)
				bases[nb] = base, counts[nb++] = 0;
			if (i < MAXV)
				counts[i]++;
			total++;
			if (va < vmin) vmin = va;
			if (va > vmax) vmax = va;
		}
	}
	unsigned best = 0;
	for (unsigned i = 1; i < nb; i++)
		if (counts[i] > counts[best])
			best = i;
	if (!nb || counts[best] < 2)
		return;
	w->ok = true;
	w->rom_base = bases[best];
	w->votes = counts[best];
	w->entries = total;
	w->vlo = vmin & ~0xFFFu;
	w->vhi = (vmax + 0x1000u + 0xFFFFFu) & ~0xFFFFFu;  /* hasta el siguiente MB: la ventana del juego */
	if (w->vhi - w->vlo < 0x100000u)
		w->vhi = w->vlo + 0x100000u;
	if (verbose)
		fprintf(stderr, "rec-lab: ventana de ROM: virtual %08x–%08x ↔ ROM + %lld (%u de %u marcos residentes coinciden)\n", w->vlo,
			w->vhi, (long long)w->rom_base, w->votes, w->entries);
	(void)romsize;
}

/* Un marco libre de la RDRAM del laboratorio (ninguna entrada de la TLB lo usa) para
 * poner ahí la página de la ROM que se va a traducir: el desplazamiento en la ROM
 * no es múltiplo de 4 KB, y la traducción de ktlb[] solo guarda el marco. */
static uint32_t scratch_frame(const struct salsa64_cpu *cpu, uint32_t rdram_size)
{
	for (uint32_t f = rdram_size - 4096; f >= 0x10000; f -= 4096) {
		bool used = false;
		for (unsigned t = 0; t < SALSA64_TLB_ENTRIES && !used; t++)
			for (unsigned h = 0; h < 2; h++)
				if ((cpu->tlb[t].entry_lo[h] & 2) && (cpu->tlb[t].entry_lo[h] >> 6) == (f >> 12))
					used = true;
		if (!used)
			return f;
	}
	return 0;
}

/* Copia la página de `va` de la ROM al marco `frame` y la anota en ktlb[], si cae en la
 * ventana. */
static bool romwin_fill(struct salsa64_rec *rec, const struct romwin *w, uint32_t va, const uint8_t *rom,
			size_t romsize, uint8_t *rdram, uint32_t frame)
{
	if (!w->ok || !frame || va < w->vlo || va >= w->vhi)
		return false;
	int64_t off = (int64_t)(va & ~0xFFFu) + w->rom_base;
	if (off < 0 || (size_t)off + 4096 > romsize)
		return false;
	memcpy(rdram + frame, rom + off, 4096);
	rec->x.ktlb[salsa64_rec_vt_slot(va)] = salsa64_rec_vt_value(va, frame, false);
	return true;
}

int main(int argc, char **argv)
{
	struct lab_args a = { .fr = -1, .opts = SALSA64_REC_OPT_PIN };  /* como el paquete de la PS3 */
	for (int i = 1; i < argc; i++) {
		const char *s = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(s, "--rdram") && v) a.rdram = v, i++;
		else if (!strcmp(s, "--rom") && v) a.rom = v, i++;
		else if (!strcmp(s, "--state") && v) a.state = v, i++;
		else if (!strcmp(s, "--romwin") && v) a.romwin = v, i++;
		else if (!strcmp(s, "--map") && v) {
			unsigned long long va, pa, len = 0x400000;
			if (sscanf(v, "%llx:%llx:%llx", &va, &pa, &len) < 2 || a.nmaps == 8)
				die("--map VA:PA[:LARGO] en hexadecimal (hasta 8)", v);
			a.maps[a.nmaps].va = (uint32_t)va, a.maps[a.nmaps].pa = (uint32_t)pa, a.maps[a.nmaps].len = (uint32_t)len;
			a.nmaps++;
			i++;
		}
		else if (!strcmp(s, "--prf") && v) a.prf = v, i++;
		else if (!strcmp(s, "--pcs") && v) a.pcs = v, i++;
		else if (!strcmp(s, "-o") && v) a.out = v, i++;
		else if (!strcmp(s, "--opts") && v) a.opts = (uint32_t)strtoul(v, NULL, 0), i++;
		else if (!strcmp(s, "--fr") && v) a.fr = (int)strtoul(v, NULL, 0), i++;
		else if (!strcmp(s, "--max-blocks") && v) a.max_blocks = (unsigned)strtoul(v, NULL, 0), i++;
		else if (!strcmp(s, "--scan-rom") && v) a.scan_rom = (unsigned)strtoul(v, NULL, 0), i++;
		else if (!strcmp(s, "-v")) a.verbose = true;
		else if (!strcmp(s, "--far")) a.far_calls = true;
		else die("opción desconocida", s);
	}
	if ((!a.rdram && !a.state) || !a.out || (!a.prf && !a.pcs) || (a.state && !a.rom) || (a.romwin && !a.rom))
		die("uso: rec-lab (--rdram volcado.bin | --rom juego.z64 --state escena.state) (--prf juego.prf | --pcs perfil.tsv)\n"
		    "               [--opts N] [--fr 0|1] [--max-blocks N] [--romwin VA:OFFSET[:LARGO]] [--far] -o salida.lab\n"
		    "  --map VA:PA[:LARGO]: páginas de código mapeadas por la TLB del juego (a mano, sin estado): la ventana lineal de Perfect Dark\n"
		    "            es 7f000000:220000 y 70000000:0\n"
		    "  --romwin: el código en [VA, VA+LARGO) (por omisión 16 MB) está en la ROM desde OFFSET (hex): el pager de GoldenEye\n"
		    "            y Perfect Dark; con --state se deduce de la TLB", NULL);

	static struct salsa64_n64 n64;
	uint8_t *romdata = NULL;
	size_t romsize = 0;
	size_t rsize = 8u << 20;
	uint8_t *rdram = aligned_alloc(4096, rsize);
	if (!rdram)
		die("sin memoria", NULL);
	memset(rdram, 0, rsize);
	salsa64_n64_init(&n64, rdram, (uint32_t)rsize);
	if (a.state) {
		romdata = read_file(a.rom, &romsize);
		salsa64_bus_set_rom(&n64.bus, romdata, (uint32_t)romsize);
		static uint8_t scratch[SALSA64_STATE_SCRATCH];
		char err[160] = "no se abre";
		bool broken = false;
		FILE *f = fopen(a.state, "rb");
		if (!f || !salsa64_rsp_quiesce(&n64.bus) ||
		    !salsa64_state_load(&n64, state_fread, f, scratch, err, sizeof err, &broken))
			die(err, a.state);
		fclose(f);
	} else {
		if (a.rom) {
			romdata = read_file(a.rom, &romsize);
			salsa64_bus_set_rom(&n64.bus, romdata, (uint32_t)romsize);
		}
		size_t dsize;
		uint8_t *d = read_file(a.rdram, &dsize);
		if (dsize != rsize)
			die("el volcado debe ser de 8 MB", a.rdram);
		memcpy(rdram, d, rsize);
		free(d);
	}

	struct salsa64_cpu *cpu_p = &n64.cpu;
#define cpu (*cpu_p)
	static struct salsa64_rec rec;
	uint32_t *code = aligned_alloc(65536, LAB_CACHE_BYTES);
	if (!code)
		die("sin memoria para la caché", NULL);
	memset(code, 0, LAB_CACHE_BYTES);
	if (!lab_init(&rec, &cpu, code, LAB_CACHE_BYTES, a.opts, a.far_calls, true))
		die("el preámbulo no cabe", NULL);

	bool state_fr = (cpu.status & SALSA64_ST_FR) != 0;
	struct romwin rw;
	uint32_t sframe = 0;
	if (a.romwin) {  /* VA:OFFSET[:LARGO] a mano (sin estado no hay TLB de donde deducirla) */
		unsigned long long va = 0, off = 0, len = 0x1000000;
		if (sscanf(a.romwin, "%llx:%llx:%llx", &va, &off, &len) < 2)
			die("--romwin VA:OFFSET[:LARGO] en hexadecimal", a.romwin);
		memset(&rw, 0, sizeof rw);
		rw.ok = true;
		rw.vlo = (uint32_t)va;
		rw.vhi = (uint32_t)(va + len);
		rw.rom_base = (int64_t)off - (int64_t)va;
	} else if (romdata) {
		find_romwin(&cpu, rdram, rec.rdram_size, romdata, romsize, &rw, true);
	} else {
		memset(&rw, 0, sizeof rw);
	}
	sframe = scratch_frame(&cpu, rec.rdram_size);
	if (a.verbose)
		for (unsigned t = 0; t < SALSA64_TLB_ENTRIES; t++) {
			const struct salsa64_tlb_entry *e = &cpu.tlb[t];
			fprintf(stderr, "tlb[%2u] hi=%016llx lo0=%08x lo1=%08x mask=%08x\n", t, (unsigned long long)e->entry_hi,
				e->entry_lo[0], e->entry_lo[1], e->page_mask);
		}

	struct lab_job *jobs;
	unsigned njobs = a.prf ? load_prf(a.prf, &jobs) : load_pcs(a.pcs, &jobs);
	if (a.max_blocks && njobs > a.max_blocks)
		njobs = a.max_blocks;

	FILE *out = fopen(a.out, "w");
	if (!out)
		die("no se puede escribir", a.out);
	lab_print_prologue(out, &rec, code, a.opts, a.fr, a.rom ? a.rom : a.rdram);

	unsigned done = 0, skipped = 0, mismatch = 0, failed = 0, scanned = 0;
	uint64_t w_done = 0, w_skipped = 0, w_mismatch = 0, w_total = 0;
	for (unsigned k = 0; k < njobs; k++) {
		const struct lab_job *jb = &jobs[k];
		uint32_t vpc = jb->vpc;
		w_total += jb->count;
		uint32_t pa;
		/* Las páginas que la TLB del estado no tiene residentes salen de la ROM, cada vez al
		 * marco de trabajo (la traducción de ktlb[] de la anterior ya no vale). */
		uint32_t pa0;
		bool dirty0;
		if (!salsa64_vr4300_kernel_probe(&cpu, (uint64_t)(int64_t)(int32_t)vpc, &pa0, &dirty0)) {
			bool mapped = false;
			for (unsigned q = 0; q < a.nmaps && !mapped; q++)
				if (vpc >= a.maps[q].va && vpc - a.maps[q].va < a.maps[q].len) {
					uint32_t pg = a.maps[q].pa + ((vpc - a.maps[q].va) & ~0xFFFu);
					rec.x.ktlb[salsa64_rec_vt_slot(vpc)] = salsa64_rec_vt_value(vpc, pg, false);
					mapped = true;
				}
			if (!mapped)
				romwin_fill(&rec, &rw, vpc, romdata, romsize, rdram, sframe);
		}
		unsigned seg = salsa64_rec_seg(&rec, (uint64_t)(int64_t)(int32_t)vpc, &pa);
		if (seg == SALSA64_REC_SEG_NONE) {  /* sin traducción, otro modo */
			skipped++;
			w_skipped += jb->count;
			fprintf(out, "S %08x %u\n", vpc, jb->count);
			continue;
		}
		if (jb->hash && jb->n_expect) {  /* ¿el volcado tiene el mismo código que el perfil? */
			uint32_t w[SALSA64_REC_MAX_INSNS];
			unsigned n = jb->n_expect < SALSA64_REC_MAX_INSNS ? jb->n_expect : SALSA64_REC_MAX_INSNS;
			uint32_t end = salsa64_rec_code_end(&rec, pa, seg);
			if (pa + 4 * n > end) {
				mismatch++;
				w_mismatch += jb->count;
				fprintf(out, "X %08x %u\n", vpc, jb->count);
				if (a.verbose && (mismatch <= 3 || jb->count > 5000000))
					fprintf(stderr, "rec-lab: X %08x seg=%u pa=%08x n=%u palabras %08x %08x %08x\n", vpc, seg, pa, n, w[0], w[1],
						w[2]);
				continue;
			}
			for (unsigned q = 0; q < n; q++)
				w[q] = salsa64_rec_fetch(&rec, pa + 4 * q);
			if (salsa64_rec_profile_hash(w, n) != jb->hash) {
				mismatch++;
				w_mismatch += jb->count;
				fprintf(out, "X %08x %u\n", vpc, jb->count);
				if (a.verbose && (mismatch <= 3 || jb->count > 5000000))
					fprintf(stderr, "rec-lab: X hash %08x seg=%u pa=%08x n=%u palabras %08x %08x %08x\n", vpc, seg, pa, n, w[0], w[1],
						w[2]);
				if (a.scan_rom && scanned < a.scan_rom && romdata) {  /* ¿dónde está en la ROM ese código? */
					scanned++;
					for (size_t o = 0; o + 4 * n <= romsize; o += 4) {
						for (unsigned q = 0; q < n; q++)
							w[q] = salsa64_rd32(romdata + o + 4 * q);
						if (salsa64_rec_profile_hash(w, n) == jb->hash) {
							fprintf(stderr, "rec-lab: %08x (n=%u, %u entradas) está en la ROM en %zx (va - rom = %lld)\n", vpc, n, jb->count, o,
								(long long)((int64_t)o - (int64_t)(vpc & 0xFFFFFF)));
							break;
						}
					}
				}
				continue;
			}
		}
		unsigned fr = jb->fr >= 0 ? (unsigned)jb->fr : a.fr >= 0 ? (unsigned)a.fr : (state_fr ? 1u : 0u);
		struct lab_emitted e;
		if (!lab_emit(&rec, (uint64_t)(int64_t)(int32_t)vpc, pa, seg, fr, &e)) {
			failed++;
			continue;
		}
		lab_print_block(out, vpc, jb->count, &e);
		free(e.words);
		if (jb->prf_entry) {  /* sitios de salida del perfil (solo bloques calientes): k, clase, destino, tomado, no tomado */
			const struct salsa64_rec_profile_entry *pe = &prf_table.entries[jb->prf_entry - 1];
			for (unsigned si = pe->site; si; si = prf_table.sites[si - 1].next) {
				const struct salsa64_rec_profile_site *st = &prf_table.sites[si - 1];
				fprintf(out, "T %u %u %08x %u %u\n", st->k, st->kind, st->target, st->taken0, st->not_taken0);
			}
		}
		done++;
		w_done += jb->count;
	}
	fprintf(out, "# bloques=%u saltados=%u huella_distinta=%u sin_compilar=%u\n", done, skipped, mismatch, failed);
	fprintf(out, "# entradas: total=%llu traducidas=%llu saltadas=%llu huella_distinta=%llu\n", (unsigned long long)w_total,
		(unsigned long long)w_done, (unsigned long long)w_skipped, (unsigned long long)w_mismatch);
	fclose(out);
	fprintf(stderr, "rec-lab: %u bloques traducidos, %u sin traducción a la RDRAM, %u con otra huella, %u que no compilaron\n",
		done, skipped, mismatch, failed);
	if (w_total)
		fprintf(stderr, "rec-lab: de las entradas del perfil: %.1f %% traducidas, %.1f %% sin traducción, %.1f %% con otra huella\n",
			100.0 * w_done / w_total, 100.0 * w_skipped / w_total, 100.0 * w_mismatch / w_total);
	return 0;
}
