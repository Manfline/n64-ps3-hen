/*
 * Salsa64 — pruebas del perfil persistente del recompilador
 * (cpu/rec_profile.h): la tabla por dirección y huella, la mezcla entre
 * sesiones, el marcado de calientes, el archivo (con corrupción) y, con el
 * recompilador, el contador de entradas y la zona caliente de la caché.
 */
#include "test.h"

#include <stdio.h>
#include <string.h>

#include "cpu/rec.h"
#include "cpu/rec_profile.h"

static struct salsa64_rec_profile tbl, tbl2;  /* un megabyte cada una: fuera de la pila */
static const uint32_t code_a[4] = { ADDIU(T0, T0, 1), ADDIU(T1, T1, 2), JR(RA), NOP };
static const uint32_t code_b[4] = { ADDIU(T0, T0, 1), ADDIU(T1, T1, 3), JR(RA), NOP };

static void lookup_by_address_and_fingerprint(void)
{
	salsa64_rec_profile_init(&tbl);
	uint64_t ha = salsa64_rec_profile_hash(code_a, 4), hb = salsa64_rec_profile_hash(code_b, 4);
	CHECK(ha != hb);
	CHECK(salsa64_rec_profile_hash(code_a, 3) != ha);  /* la cantidad también cuenta */

	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80001000, ha, 4, 100, false), 0);
	CHECK_EQ(tbl.count, 0);
	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80001000, ha, 4, 100, true), 1);
	CHECK_EQ(tbl.count, 1);
	CHECK_EQ(tbl.entries[0].words, 100);
	CHECK_EQ(tbl.entries[0].n, 4);
	CHECK(tbl.dirty);
	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80001000, ha, 4, 0, false), 1);
	/* Misma dirección, otra huella: otro bloque (overlay o código reescrito). */
	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80001000, hb, 4, 50, false), 0);
	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80001000, hb, 4, 50, true), 2);
	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80001000, ha, 3, 0, false), 0);
	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80001004, ha, 4, 0, false), 0);
	CHECK_EQ(tbl.count, 2);

	salsa64_rec_profile_count(&tbl, 1, 5);
	CHECK_EQ(tbl.entries[0].hits, 5);
	CHECK_EQ(tbl.session, 5);
	salsa64_rec_profile_count(&tbl, 1, UINT32_MAX);
	CHECK_EQ(tbl.entries[0].hits, UINT32_MAX);  /* satura */
	CHECK_EQ(tbl.session, 5ull + UINT32_MAX);
	salsa64_rec_profile_count(&tbl, 0, 9);  /* sin entrada: se descarta */
	salsa64_rec_profile_count(&tbl, 3, 9);
	CHECK_EQ(tbl.session, 5ull + UINT32_MAX);
	salsa64_rec_profile_set_words(&tbl, 2, 70000);
	CHECK_EQ(tbl.entries[1].words, 0xFFFF);

	/* Llena: lo que sigue no entra y se cuenta. */
	for (uint32_t k = 2; k < SALSA64_RPF_ENTRIES; k++)
		CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80010000 + 4 * k, ha ^ k, 4, 1, true), k + 1);
	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80FF0000, hb, 4, 1, true), 0);
	CHECK_EQ(tbl.full, 1);
	CHECK_EQ(salsa64_rec_profile_lookup(&tbl, 0x80010000 + 4 * 777, ha ^ 777, 4, 1, false), 778);
}

static void merge_decays_only_in_long_sessions(void)
{
	/* Sesión corta: solo sube. */
	CHECK_EQ(salsa64_rec_profile_merge(1000, 100, false), 1000);
	CHECK_EQ(salsa64_rec_profile_merge(1000, 2000, false), 2000);
	CHECK_EQ(salsa64_rec_profile_merge(0, 0, false), 0);
	/* Larga: lo que no se ejecutó decae a la mitad; lo que sí, al menos lo de ahora. */
	CHECK_EQ(salsa64_rec_profile_merge(1000, 100, true), 550);
	CHECK_EQ(salsa64_rec_profile_merge(1000, 0, true), 500);
	CHECK_EQ(salsa64_rec_profile_merge(1000, 2000, true), 2000);
	CHECK_EQ(salsa64_rec_profile_merge(0, 7, true), 7);
	CHECK_EQ(salsa64_rec_profile_merge(UINT32_MAX, UINT32_MAX, true), UINT32_MAX);
}

static void hot_marking_fills_capacity_by_score(void)
{
	salsa64_rec_profile_init(&tbl);
	for (uint32_t k = 0; k < 10; k++) {
		unsigned i = salsa64_rec_profile_lookup(&tbl, 0x80002000 + 4 * k, 0x1234 + k, 2, 100, true);
		tbl.entries[i - 1].score = 10 - k;
	}
	unsigned zero = salsa64_rec_profile_lookup(&tbl, 0x80003000, 0x99, 2, 1, true);  /* puntaje 0: nunca */

	salsa64_rec_profile_mark_hot(&tbl, 350);
	CHECK_EQ(tbl.hot_entries, 3);
	CHECK_EQ(tbl.hot_words, 300);
	CHECK_EQ(tbl.hot_threshold, 8);
	for (uint32_t k = 0; k < 10; k++)
		CHECK_EQ(tbl.entries[k].hot, k < 3);
	CHECK_EQ(tbl.entries[zero - 1].hot, 0);

	salsa64_rec_profile_mark_hot(&tbl, 100000);
	CHECK_EQ(tbl.hot_entries, 10);
	CHECK_EQ(tbl.hot_threshold, 1);
	CHECK_EQ(tbl.entries[zero - 1].hot, 0);

	salsa64_rec_profile_mark_hot(&tbl, 0);
	CHECK_EQ(tbl.hot_entries, 0);
	for (uint32_t k = 0; k < tbl.count; k++)
		CHECK_EQ(tbl.entries[k].hot, 0);

	/* Empates en el umbral: entran los que caben, en orden de la tabla. */
	for (uint32_t k = 0; k < 10; k++)
		tbl.entries[k].score = 5;
	salsa64_rec_profile_mark_hot(&tbl, 250);
	CHECK_EQ(tbl.hot_entries, 2);
	CHECK_EQ(tbl.entries[0].hot, 1);
	CHECK_EQ(tbl.entries[1].hot, 1);
	CHECK_EQ(tbl.entries[2].hot, 0);
}

#define N_FILE 3000u
#define SITES_ENTRY 10u   /* la entrada con sitios: n = 10 */
#define ENT_LEN 24u
#define HDR_LEN 32u

static const struct salsa64_rec_profile_game game_a = { { 'N', 'S', 'M', 'E' }, 0x635A2BFFu, 0x8B022326u };
static const struct salsa64_rec_profile_game game_b = { { 'N', 'Z', 'L', 'E' }, 0x11111111u, 0x22222222u };

static void fill(struct salsa64_rec_profile *p)
{
	salsa64_rec_profile_init(p);
	p->game = game_a;
	for (uint32_t k = 0; k < N_FILE; k++) {
		unsigned i = salsa64_rec_profile_lookup(p, 0x80000000 + 4 * k, 0x9E3779B97F4A7C15ull * (k + 1),
							1 + k % 64, k % 500, true);
		CHECK_EQ(i, k + 1);
		p->entries[i - 1].score = 3 * k;
		p->entries[i - 1].hits = k;
		p->entries[i - 1].flags = (uint8_t)(k & 7);
	}
	/* Sitios de la entrada 10 (vpc 0x80000024, n = 10, hits = 9): dos
	 * condicionales internos, y el salto final con su no tomado. */
	unsigned a = salsa64_rec_profile_site(p, SITES_ENTRY, 2, SALSA64_RPF_SITE_COND, 0x80001000);
	unsigned b = salsa64_rec_profile_site(p, SITES_ENTRY, 5, SALSA64_RPF_SITE_COND, 0x80002000);
	unsigned c = salsa64_rec_profile_site(p, SITES_ENTRY, 9, SALSA64_RPF_SITE_FALL, 0x8000004C);
	unsigned d = salsa64_rec_profile_site(p, SITES_ENTRY, 9, SALSA64_RPF_SITE_COND, 0x80003000);
	CHECK_EQ(a, 1);
	CHECK_EQ(b, 2);
	CHECK_EQ(c, 3);
	CHECK_EQ(d, 4);
	CHECK_EQ(salsa64_rec_profile_site(p, SITES_ENTRY, 5, SALSA64_RPF_SITE_COND, 0x80002000), 2);  /* repetido */
	CHECK_EQ(p->nsites, 4);
	CHECK_EQ(p->entries[SITES_ENTRY - 1].nsites, 4);
	p->sites[0].taken = 5, p->sites[0].taken0 = 7, p->sites[0].not_taken0 = 1, p->sites[0].succ_hash = 0xAAA;
	p->sites[1].taken = 3, p->sites[1].succ_hash = 0xCCC;
	p->sites[2].taken = 1;  /* no tomado del salto final: sin huella (no es sucesor) */
	p->sites[3].taken = 4, p->sites[3].succ_hash = 0xBBB;
}

static void check_loaded(struct salsa64_rec_profile *p, bool long_session)
{
	CHECK_EQ(p->count, N_FILE);
	CHECK_EQ(p->loaded, N_FILE);
	CHECK(!p->dirty);
	CHECK_EQ(p->session, 0);
	CHECK(memcmp(p->game.code, game_a.code, 4) == 0 && p->game.crc1 == game_a.crc1 && p->game.crc2 == game_a.crc2);
	for (uint32_t k = 0; k < N_FILE; k += 97) {
		unsigned i = salsa64_rec_profile_lookup(p, 0x80000000 + 4 * k, 0x9E3779B97F4A7C15ull * (k + 1), 1 + k % 64,
							0, false);
		CHECK(i != 0);
		if (!i)
			continue;
		CHECK_EQ(p->entries[i - 1].score, salsa64_rec_profile_merge(3 * k, k, long_session));
		CHECK_EQ(p->entries[i - 1].hits, 0);
		CHECK_EQ(p->entries[i - 1].words, k % 500);
		CHECK_EQ(p->entries[i - 1].flags, k & 7);
		CHECK_EQ(p->entries[i - 1].hot, 0);
	}
	/* Los sitios, con lo tomado sumado y lo no tomado derivado: el de k = 2
	 * alcanza las 9 entradas (9 − 5 = 4, más 1 del archivo); el de k = 5,
	 * 9 − 5 = 4 (4 − 3 = 1); el final tiene su no tomado explícito (1). */
	CHECK_EQ(p->nsites, 4);
	unsigned a = salsa64_rec_profile_site(p, SITES_ENTRY, 2, SALSA64_RPF_SITE_COND, 0x80001000);
	unsigned b = salsa64_rec_profile_site(p, SITES_ENTRY, 5, SALSA64_RPF_SITE_COND, 0x80002000);
	unsigned c = salsa64_rec_profile_site(p, SITES_ENTRY, 9, SALSA64_RPF_SITE_FALL, 0x8000004C);
	unsigned d = salsa64_rec_profile_site(p, SITES_ENTRY, 9, SALSA64_RPF_SITE_COND, 0x80003000);
	CHECK_EQ(p->nsites, 4);
	CHECK(a && b && c && d);
	if (a && b && c && d) {
		CHECK_EQ(p->sites[a - 1].taken0, 12);
		CHECK_EQ(p->sites[a - 1].not_taken0, 5);
		CHECK_EQ(p->sites[a - 1].taken, 0);
		CHECK_EQ(p->sites[b - 1].taken0, 3);
		CHECK_EQ(p->sites[b - 1].not_taken0, 1);
		CHECK_EQ(p->sites[c - 1].taken0, 1);
		CHECK_EQ(p->sites[c - 1].not_taken0, 0);
		CHECK_EQ(p->sites[d - 1].taken0, 4);
		CHECK_EQ(p->sites[d - 1].not_taken0, 1);
		CHECK_EQ(p->sites[a - 1].succ_hash, 0);  /* se resuelve en cada sesión */
	}
}

static long file_size(const char *path, uint8_t *buf, size_t size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	long n = (long)fread(buf, 1, size, f);
	fclose(f);
	return n;
}

static bool write_file(const char *path, const uint8_t *buf, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	bool ok = fwrite(buf, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static uint32_t rd32(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void file_round_trip_rejects_corruption(void)
{
	static uint8_t buf[90000];
	char path[512];
	if (!t_tmp_dir)
		return;
	snprintf(path, sizeof path, "%s/juego.prf", t_tmp_dir);

	/* Sin archivo: vacío y false, con la identidad pedida. */
	CHECK(!salsa64_rec_profile_load(&tbl2, path, &game_a));
	CHECK_EQ(tbl2.count, 0);
	CHECK_EQ(tbl2.game.crc1, game_a.crc1);

	fill(&tbl);
	CHECK(salsa64_rec_profile_save(&tbl, path));
	CHECK(salsa64_rec_profile_load(&tbl2, path, &game_a));
	check_loaded(&tbl2, false);
	/* Sin identidad: toma la del archivo. De otro juego: se rechaza. */
	CHECK(salsa64_rec_profile_load(&tbl2, path, NULL));
	check_loaded(&tbl2, false);
	CHECK(!salsa64_rec_profile_load(&tbl2, path, &game_b));
	CHECK_EQ(tbl2.count, 0);
	CHECK_EQ(tbl2.nsites, 0);

	/* Lo escrito: cabecera, entradas (la 10 con dos sucesores, por cuenta
	 * total: 0xAAA con 12 y 0xBBB con 4; el no tomado no es sucesor), sitios
	 * y suma. */
	long n = file_size(path, buf, sizeof buf);
	CHECK_EQ(n, HDR_LEN + N_FILE * ENT_LEN + 2 * 16 + 4 * 16 + 8);
	CHECK(memcmp(buf, "SPRF", 4) == 0);
	CHECK_EQ(rd32(buf + 4), 2);
	CHECK_EQ(rd32(buf + 8), N_FILE);
	CHECK_EQ(rd32(buf + 12), 4);
	CHECK(memcmp(buf + 16, "NSME", 4) == 0);
	CHECK_EQ(rd32(buf + 20), game_a.crc1);
	CHECK_EQ(rd32(buf + 24), game_a.crc2);
	const uint8_t *e10 = buf + HDR_LEN + (SITES_ENTRY - 1) * ENT_LEN;
	CHECK_EQ(rd32(e10), 0x80000000 + 4 * (SITES_ENTRY - 1));
	CHECK_EQ(e10[20], (SITES_ENTRY - 1) & 7);
	CHECK_EQ(e10[21], 2);
	CHECK_EQ(rd32(e10 + ENT_LEN), 0x80001000);
	CHECK_EQ(rd32(e10 + ENT_LEN + 8), 0xAAA);
	CHECK_EQ(rd32(e10 + ENT_LEN + 12), 12);
	CHECK_EQ(rd32(e10 + ENT_LEN + 16), 0x80003000);
	CHECK_EQ(rd32(e10 + ENT_LEN + 24), 0xBBB);
	CHECK_EQ(rd32(e10 + ENT_LEN + 28), 4);
	const uint8_t *s0 = buf + HDR_LEN + N_FILE * ENT_LEN + 2 * 16;
	CHECK_EQ(rd32(s0), (uint32_t)SITES_ENTRY << 16 | 2 << 8 | SALSA64_RPF_SITE_COND);
	CHECK_EQ(rd32(s0 + 4), 0x80001000);
	CHECK_EQ(rd32(s0 + 8), 12);
	CHECK_EQ(rd32(s0 + 12), 5);

	/* Sesión larga: con decaimiento. Y por trozos, con la tabla cambiando
	 * entre trozos (las cuentas nuevas pueden entrar o no). */
	tbl.session = SALSA64_RPF_LONG_SESSION;
	struct salsa64_rec_profile_writer w;
	memset(&w, 0, sizeof w);
	CHECK(salsa64_rec_profile_save_begin(&tbl, &w, path));
	int r = salsa64_rec_profile_save_step(&tbl, &w, 1000);
	CHECK_EQ(r, 0);
	salsa64_rec_profile_lookup(&tbl, 0x80100000, 0x77, 3, 9, true);  /* después de count: no entra en este archivo */
	salsa64_rec_profile_site(&tbl, 5, 0, SALSA64_RPF_SITE_END, 0x80000100);  /* después de nsites: tampoco */
	while ((r = salsa64_rec_profile_save_step(&tbl, &w, 700)) == 0)
		;
	CHECK_EQ(r, 1);
	CHECK(!w.f);
	CHECK(salsa64_rec_profile_load(&tbl2, path, &game_a));
	check_loaded(&tbl2, true);

	/* Truncado: se descarta entero. */
	n = file_size(path, buf, sizeof buf);
	CHECK_EQ(n, HDR_LEN + N_FILE * ENT_LEN + 2 * 16 + 4 * 16 + 8);
	CHECK(write_file(path, buf, (size_t)n - 5));
	CHECK(!salsa64_rec_profile_load(&tbl2, path, &game_a));
	CHECK_EQ(tbl2.count, 0);
	/* Un byte cambiado en medio: la suma no cuadra. */
	buf[HDR_LEN + ENT_LEN * 1234 + 13] ^= 0x40;
	CHECK(write_file(path, buf, (size_t)n));
	CHECK(!salsa64_rec_profile_load(&tbl2, path, &game_a));
	CHECK_EQ(tbl2.count, 0);
	buf[HDR_LEN + ENT_LEN * 1234 + 13] ^= 0x40;
	/* Con algo de más al final, tampoco. */
	buf[n] = 0;
	CHECK(write_file(path, buf, (size_t)n + 1));
	CHECK(!salsa64_rec_profile_load(&tbl2, path, &game_a));
	/* Intacto otra vez: carga. */
	CHECK(write_file(path, buf, (size_t)n));
	CHECK(salsa64_rec_profile_load(&tbl2, path, &game_a));
	CHECK_EQ(tbl2.count, N_FILE);
	CHECK_EQ(tbl2.nsites, 4);
	/* Tabla vacía: archivo válido de cero entradas. */
	salsa64_rec_profile_init(&tbl);
	CHECK(salsa64_rec_profile_save(&tbl, path));
	CHECK(salsa64_rec_profile_load(&tbl2, path, NULL));
	CHECK_EQ(tbl2.count, 0);
	remove(path);
}

static void game_identity_from_rom_header(void)
{
	uint8_t rom[64] = { 0 };
	struct salsa64_rec_profile_game g;
	rom[0x10] = 0x63, rom[0x11] = 0x5A, rom[0x12] = 0x2B, rom[0x13] = 0xFF;
	rom[0x14] = 0x8B, rom[0x15] = 0x02, rom[0x16] = 0x23, rom[0x17] = 0x26;
	memcpy(rom + 0x3B, "NSME", 4);
	salsa64_rec_profile_game_of(&g, rom);
	CHECK(memcmp(g.code, "NSME", 4) == 0);
	CHECK_EQ(g.crc1, 0x635A2BFFu);
	CHECK_EQ(g.crc2, 0x8B022326u);
}

/* Con el recompilador: un bloque caliente registra sus sitios de salida
 * (aquí el condicional interno que sale del bloque; el JR no da sitio), el
 * código generado los cuenta solo cuando pasa por ellos, el sucesor se
 * resuelve a la huella del bloque que hay en el destino, y con `saltos = no`
 * no se registra nada. */
static void rec_records_exit_sites_of_hot_blocks(void)
{
	struct salsa64_rec *rec = CPU->rec;
	if (!rec)
		return;
	static const uint32_t code[6] = { ADDIU(T0, T0, -1), BNE(T0, ZERO, 4), NOP, ADDIU(T1, T1, 1), JR(RA), NOP };
	static const uint32_t dest[2] = { JR(RA), NOP };
	salsa64_rec_profile_init(&tbl);
	unsigned i = salsa64_rec_profile_lookup(&tbl, (uint32_t)T_CODE, salsa64_rec_profile_hash(code, 6), 6, 0, true);
	tbl.entries[i - 1].score = 10;
	salsa64_rec_profile_mark_hot(&tbl, 1000);
	rec->profile = &tbl;
	rec->profile_sites = true;

	t_load(code, 6);
	t_wr32(T_CODE_PA + 24, dest[0]);  /* el destino del BNE: T_CODE + 24, fuera del bloque */
	t_wr32(T_CODE_PA + 28, dest[1]);
	R[RA] = T_CODE + 0x100;
	R[T0] = 1;
	R[T1] = 0;
	t_step(6);  /* no tomado */
	CHECK_EQ(R[T1], 1);
	unsigned id = rec->map[T_CODE_PA >> 2];
	CHECK(id != 0);
	CHECK_EQ(rec->pent[id], i);
	CHECK_EQ(tbl.nsites, 1);
	CHECK_EQ(tbl.entries[i - 1].nsites, 1);
	CHECK_EQ(tbl.sites[0].k, 1);
	CHECK_EQ(tbl.sites[0].kind, SALSA64_RPF_SITE_COND);
	CHECK_EQ(tbl.sites[0].target, (uint32_t)T_CODE + 24);
	CHECK_EQ(tbl.sites[0].taken, 0);
	CHECK_EQ(tbl.entries[i - 1].flags, SALSA64_RPF_FR_SEEN1);  /* T_STATUS tiene FR; no usa la FPU */

	/* Tomado: ADDIU, BNE y su delay slot, el bloque del destino (JR y su
	 * delay slot, que así queda compilado y con entrada) y una más en RA. El
	 * paso tiene que dar para el bloque entero: si no cabe antes del evento,
	 * el modo estricto compila uno recortado, sin contadores. */
	R[T0] = 2;
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(6);
	CHECK_EQ(R[T0], 1);
	CHECK_EQ(CPU->pc, T_CODE + 0x104);
	CHECK_EQ(tbl.sites[0].taken, 1);
	CHECK_EQ(rec->hits[id], 2);
	CHECK_EQ(tbl.count, 3);  /* el destino y el bloque de RA (que corrió recortado, pero se compiló entero) */

	/* El sucesor se resuelve a la huella del bloque del destino. */
	salsa64_rec_profile_fold(rec);
	CHECK_EQ(tbl.entries[i - 1].hits, 2);
	CHECK_EQ(tbl.sites[0].succ_hash, salsa64_rec_profile_hash(dest, 2));

	/* Guardado: tomado 1, no tomado 2 − 1 = 1. */
	if (t_tmp_dir) {
		char path[512];
		snprintf(path, sizeof path, "%s/sitios.prf", t_tmp_dir);
		CHECK(salsa64_rec_profile_save(&tbl, path));
		CHECK(salsa64_rec_profile_load(&tbl2, path, NULL));
		CHECK_EQ(tbl2.nsites, 1);
		CHECK_EQ(tbl2.sites[0].taken0, 1);
		CHECK_EQ(tbl2.sites[0].not_taken0, 1);
		remove(path);
	}

	/* Sin registro de sitios: el bloque recompilado no agrega ninguno. */
	rec->profile_sites = false;
	t_wr32(T_CODE_PA + 12, ADDIU(T1, T1, 2));  /* otra huella: otra entrada, fría */
	R[T0] = 1;
	R[T1] = 0;
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(6);
	CHECK_EQ(R[T1], 2);
	CHECK_EQ(tbl.count, 4);
	CHECK_EQ(tbl.nsites, 1);
	rec->profile_sites = true;

	salsa64_rec_profile_detach(rec);
	salsa64_rec_flush(rec);
}

/* Con el recompilador: cada bloque cuenta sus entradas (no las vueltas de
 * un bucle), los marcados van a la zona caliente, un bloque reescrito en la
 * misma dirección es otra entrada (fría), y al morir un bloque su cuenta se
 * suma al perfil. */
static void rec_counts_entries_and_places_hot_blocks(void)
{
	struct salsa64_rec *rec = CPU->rec;
	if (!rec)
		return;
	uint32_t hot_lo = 1024, hot_hi = hot_lo + salsa64_rec_hot_capacity(rec);
	uint64_t placed = rec->stats.hot_placed, evictions = rec->stats.hot_evictions;
	CHECK(rec->hot_words != 0);

	salsa64_rec_profile_init(&tbl);
	unsigned i = salsa64_rec_profile_lookup(&tbl, (uint32_t)T_CODE, salsa64_rec_profile_hash(code_a, 4), 4, 0, true);
	tbl.entries[i - 1].score = 10;
	salsa64_rec_profile_mark_hot(&tbl, 1000);
	CHECK_EQ(tbl.entries[i - 1].hot, 1);
	rec->profile = &tbl;

	t_load(code_a, 4);
	R[T0] = R[T1] = 0;
	R[RA] = T_CODE + 0x200;
	t_step(4);
	CHECK_EQ(R[T0], 1);
	CHECK_EQ(R[T1], 2);
	unsigned id = rec->map[T_CODE_PA >> 2];
	CHECK(id != 0);
	CHECK(rec->blocks[id].code >= hot_lo && rec->blocks[id].code < hot_hi);
	CHECK_EQ(rec->pent[id], i);
	CHECK_EQ(rec->hits[id], 1);
	CHECK_EQ(rec->stats.hot_placed, placed + 1);
	CHECK(tbl.entries[i - 1].words > 0);

	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(4);
	CHECK_EQ(rec->hits[id], 2);
	salsa64_rec_profile_fold(rec);
	CHECK_EQ(rec->hits[id], 0);
	CHECK_EQ(tbl.entries[i - 1].hits, 2);
	CHECK_EQ(tbl.session, 2);
	CHECK_EQ(rec->stats.prof_folded >= 2, 1);

	/* Una entrada más y el código reescrito: el bloque muere y su cuenta se
	 * suma; el nuevo tiene otra huella, otra entrada y va a la zona fría. */
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(4);
	CHECK_EQ(rec->hits[id], 1);
	t_wr32(T_CODE_PA + 4, code_b[1]);
	CHECK_EQ(tbl.entries[i - 1].hits, 3);
	CHECK_EQ(rec->pent[id], 0);
	salsa64_cpu_set_pc(CPU, T_CODE);
	R[T1] = 0;
	t_step(4);
	CHECK_EQ(R[T1], 3);
	unsigned id2 = rec->map[T_CODE_PA >> 2];
	CHECK(id2 != 0);
	CHECK(rec->blocks[id2].code >= hot_hi);
	CHECK_EQ(tbl.count, 2);
	CHECK_EQ(rec->pent[id2], 2);
	CHECK_EQ(tbl.entries[1].hot, 0);
	CHECK_EQ(tbl.entries[1].vpc, (uint32_t)T_CODE);
	CHECK_EQ(tbl.entries[1].hash, salsa64_rec_profile_hash(code_b, 4));
	CHECK_EQ(rec->stats.hot_placed, placed + 1);

	/* Un bucle dentro de un bloque: una entrada, cinco vueltas. */
	T_LOAD(ADDIU(T0, T0, -1), BNE(T0, ZERO, -2), NOP);
	R[T0] = 5;
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(15);
	CHECK_EQ(R[T0], 0);
	unsigned id3 = rec->map[T_CODE_PA >> 2];
	CHECK(id3 != 0);
	CHECK_EQ(rec->hits[id3], 1);
	CHECK_EQ(tbl.count, 3);

	/* Un caliente que no cupo (hot_full): la próxima compilación pasa a la
	 * otra mitad, y lo que había en la primera sigue. */
	uint32_t pa4 = T_CODE_PA + 0x100;
	for (unsigned k = 0; k < 4; k++)
		t_wr32(pa4 + 4 * k, code_a[k]);
	unsigned i4 = salsa64_rec_profile_lookup(&tbl, (uint32_t)T_CODE + 0x100, salsa64_rec_profile_hash(code_a, 4), 4,
						 0, true);
	tbl.entries[i4 - 1].hot = 1;
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(4);  /* el bloque en T_CODE otra vez, para que siga vivo */
	rec->hot_full = true;
	salsa64_cpu_set_pc(CPU, T_CODE + 0x100);
	R[T0] = R[T1] = 0;
	t_step(4);
	CHECK_EQ(R[T1], 2);
	unsigned id4 = rec->map[pa4 >> 2];
	CHECK(id4 != 0);
	CHECK_EQ(rec->cur_hot, 1);
	CHECK_EQ(rec->stats.hot_evictions, evictions + 1);
	CHECK(rec->blocks[id4].code >= hot_lo + rec->hot_words && rec->blocks[id4].code < hot_hi);
	CHECK_EQ(rec->stats.hot_placed, placed + 2);
	CHECK(!rec->hot_full);
	CHECK(rec->map[T_CODE_PA >> 2] != 0);  /* la mitad 0 no se tocó */

	salsa64_rec_profile_detach(rec);
	CHECK(!rec->profile);
	CHECK_EQ(rec->pent[id4], 0);
	CHECK_EQ(tbl.entries[i4 - 1].hits, 1);
	salsa64_rec_flush(rec);
}

void t_suite_rec_profile(void)
{
	RUN(lookup_by_address_and_fingerprint);
	RUN(merge_decays_only_in_long_sessions);
	RUN(hot_marking_fills_capacity_by_score);
	RUN(file_round_trip_rejects_corruption);
	RUN(game_identity_from_rom_header);
	RUN(rec_counts_entries_and_places_hot_blocks);
	RUN(rec_records_exit_sites_of_hot_blocks);
}
