/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas del registro del recompilador para el perfilado
 * (cpu/rec_trace.h): la ventana y su tabla de bloques, las retraducciones,
 * los JR plegados por dirección y, con el recompilador, la secuencia de
 * entradas que deja el código generado, los JR contados por bloque y las
 * palabras por clase de cada compilación.
 */
#include "test.h"

#include <string.h>

#include "cpu/rec.h"
#include "cpu/rec_trace.h"

static struct salsa64_rec fake;             /* unos 8 MB: fuera de la pila; solo blocks, x.xb, next_block y cpu */
static struct salsa64_rec_trace tr;
static struct salsa64_cpu fake_cpu;
static struct salsa64_rec_trace_block got[16];
static unsigned ngot;

static void take_block(void *ctx, const struct salsa64_rec_trace_block *b)
{
	(void)ctx;
	if (ngot < 16)
		got[ngot] = *b;
	ngot++;
}

static void setup(void)
{
	memset(&fake, 0, sizeof fake);
	memset(&fake_cpu, 0, sizeof fake_cpu);
	tr.seq = tr.jr = false;
	tr.win_len = 8;
	tr.on_block = take_block;
	tr.on_evict = NULL;
	tr.ctx = NULL;
	salsa64_rec_trace_init(&tr);
	fake.trace = &tr;
	fake.cpu = &fake_cpu;
	fake.next_block = 4;
	for (unsigned id = 1; id < 4; id++) {
		fake.blocks[id].n = (uint8_t)(4 * id);
		fake.blocks[id].code = 1024 + 64 * id;
		fake.x.xb[id].vpc = 0x80001000u + 0x100 * id;
	}
	ngot = 0;
}

static void compiled(unsigned id, uint64_t hash, unsigned end, unsigned hot, unsigned cold)
{
	tr.cur.hash = hash;
	tr.cur.end = (uint8_t)end;
	tr.cur.hot = (uint16_t)hot;
	tr.cur.cold = (uint16_t)cold;
	salsa64_rec_trace_compiled(&fake, id, (uint64_t)(int64_t)(int32_t)fake.x.xb[id].vpc, 0x1000, 0,
				   fake.blocks[id].code, fake.blocks[id].n, 77);
}

/* La ventana: lo que escriben el código generado y las marcas de C, en
 * orden; al llenarse se cierra sola y la tabla dice qué es cada id. */
static void window_fills_and_lists_blocks(void)
{
	setup();
	compiled(1, 0x1111, SALSA64_RT_END_JR_RA, 40, 12);
	compiled(2, 0x2222, SALSA64_RT_END_JAL | SALSA64_RT_END_LOOP, 30, 10);
	CHECK_EQ(salsa64_rec_trace_take(&fake, true), 0);  /* sin ventana */
	tr.arm_at = 100;
	fake_cpu.cycles = 120;
	salsa64_rec_trace_arm(&fake);
	CHECK_EQ(tr.arm_at, 0);
	CHECK_EQ(fake_cpu.rec_tron, 1);
	CHECK_EQ(tr.win_cycles, 120);
	/* Como trace_rtn: el id y el puntero. */
	*fake_cpu.rec_tpos++ = 2;
	*fake_cpu.rec_tpos++ = 1;
	salsa64_rec_trace_mark(&fake_cpu, SALSA64_RT_M_EXIT | SALSA64_REC_EXIT_JUMP);
	salsa64_rec_trace_mark(&fake_cpu, SALSA64_RT_M_COMPILE | 3);
	CHECK_EQ(salsa64_rec_trace_take(&fake, false), 0);  /* sigue abierta */
	for (unsigned k = 0; k < 6; k++)
		salsa64_rec_trace_mark(&fake_cpu, SALSA64_RT_M_INTERP | k);
	CHECK_EQ(fake_cpu.rec_tron, 0);  /* se llenó con 8 */
	salsa64_rec_trace_mark(&fake_cpu, SALSA64_RT_M_EVENT);  /* ya no entra */
	CHECK_EQ(salsa64_rec_trace_take(&fake, false), 8);
	CHECK_EQ(tr.win[0], 2);
	CHECK_EQ(tr.win[2], SALSA64_RT_M_EXIT | SALSA64_REC_EXIT_JUMP);
	CHECK_EQ(tr.win[7], SALSA64_RT_M_INTERP | 3);
	CHECK_EQ(tr.ntab, 3);  /* 2, 1 y el 3 de la marca de compilación */
	CHECK_EQ(tr.tab[0].id, 2);
	CHECK_EQ(tr.tab[0].vpc, 0x80001200u);
	CHECK_EQ(tr.tab[0].code, 1024 + 128);
	CHECK_EQ(tr.tab[0].n, 8);
	CHECK_EQ(tr.tab[0].end, SALSA64_RT_END_JAL | SALSA64_RT_END_LOOP);
	CHECK_EQ(tr.tab[0].hot, 30);
	CHECK_EQ(tr.tab[1].id, 1);
	CHECK_EQ(tr.tab[1].cold, 12);
	CHECK_EQ(tr.tab[2].id, 3);
	CHECK_EQ(salsa64_rec_trace_take(&fake, true), 0);  /* ya recogida */
}

/* La misma vpc, huella y largo otra vez es una retraducción; las cuentas
 * por campo y totales suben y el aviso lleva lo del emisor. */
static void compile_records_and_retranslations(void)
{
	setup();
	compiled(1, 0xABCD, SALSA64_RT_END_JR, 50, 20);
	CHECK_EQ(ngot, 1);
	CHECK_EQ(got[0].flags & SALSA64_RT_F_RETRANS, 0);
	CHECK_EQ(got[0].id, 1);
	CHECK_EQ(got[0].vpc, 0x80001100u);
	CHECK_EQ(got[0].ticks, 77);
	CHECK_EQ(got[0].hot, 50);
	compiled(2, 0xABCD, SALSA64_RT_END_JR, 50, 20);   /* otra vpc: no */
	compiled(1, 0xABCE, SALSA64_RT_END_JR, 50, 20);   /* otra huella: no */
	compiled(1, 0xABCD, SALSA64_RT_END_JR, 52, 20);   /* lo mismo: sí */
	CHECK_EQ(ngot, 4);
	CHECK_EQ(got[1].flags & SALSA64_RT_F_RETRANS, 0);
	CHECK_EQ(got[2].flags & SALSA64_RT_F_RETRANS, 0);
	CHECK_EQ(got[3].flags & SALSA64_RT_F_RETRANS, SALSA64_RT_F_RETRANS);
	CHECK_EQ(tr.retrans, 1);
	CHECK_EQ(tr.compiled, 4);
	CHECK_EQ(tr.field.compiled, 4);
	CHECK_EQ(tr.field.compile_ticks, 4 * 77);
	CHECK_EQ(tr.field.words, 3 * 70 + 72);
	CHECK_EQ(tr.cur.hash, 0);  /* el emisor empieza de cero */
	salsa64_rec_trace_evicted(&fake, 0, 3, 120);
	salsa64_rec_trace_evicted(&fake, 2, SALSA64_REC_FLUSH_STATE, 40);
	CHECK_EQ(tr.evictions, 1);
	CHECK_EQ(tr.flushes, 1);
	CHECK_EQ(tr.field.killed, 160);
}

/* Los JR de un bloque pasan a su dirección al morir (y el id reutilizado
 * empieza de cero). */
static void jr_fold_by_address(void)
{
	setup();
	compiled(1, 1, SALSA64_RT_END_JR_RA, 10, 4);
	tr.jr_id[1].count = 10;
	tr.jr_id[1].repeat = 7;
	salsa64_rec_trace_fold(&fake, 1);
	CHECK_EQ(tr.jr_id[1].count, 0);
	CHECK_EQ(tr.jr_nsites, 1);
	compiled(1, 1, SALSA64_RT_END_JR_RA, 10, 4);  /* el mismo bloque, otra vez */
	tr.jr_id[1].count = 5;
	tr.jr_id[1].repeat = 5;
	fake.x.xb[3].vpc = fake.x.xb[1].vpc;  /* otro id con la misma dirección */
	compiled(3, 2, SALSA64_RT_END_JR, 10, 4);
	tr.jr_id[3].count = 1;
	salsa64_rec_trace_fold_all(&fake);
	CHECK_EQ(tr.jr_nsites, 1);
	const struct salsa64_rec_trace_jrsite *e = NULL;
	for (unsigned k = 0; k < SALSA64_RT_JR_SITES; k++)
		if (tr.jr_sites[k].vpc == 0x80001100u)
			e = &tr.jr_sites[k];
	CHECK(e != NULL);
	if (e) {
		CHECK_EQ(e->count, 16);
		CHECK_EQ(e->repeat, 12);
		CHECK_EQ(e->end, SALSA64_RT_END_JR);
		CHECK_EQ(e->n, 12);
	}
	compiled(1, 1, SALSA64_RT_END_JR_RA, 10, 4);
	CHECK_EQ(tr.jr_id[1].count, 0);
}

static void classes_of_instructions(void)
{
	CHECK_EQ(salsa64_rec_trace_class(ADDIU(T0, T0, 1)), SALSA64_RT_C_ALU);
	CHECK_EQ(salsa64_rec_trace_class(NOP), SALSA64_RT_C_ALU);
	CHECK_EQ(salsa64_rec_trace_class(LW(T0, 0, T1)), SALSA64_RT_C_MEM);
	CHECK_EQ(salsa64_rec_trace_class(SDC1(2, 8, T1)), SALSA64_RT_C_MEM);
	CHECK_EQ(salsa64_rec_trace_class(ADD_F(16, 0, 2, 4)), SALSA64_RT_C_FPU);
	CHECK_EQ(salsa64_rec_trace_class(MTC0(T0, 12)), SALSA64_RT_C_OTHER);
}

/* ---- Con el recompilador ---------------------------------------------------- */

#define F_OFF  0x40u
#define G_OFF  0x80u
#define END_OFF 0x100u

static void load_calls(void)
{
	static const uint32_t main_code[6] = {
		ADDIU(T0, T0, 1), JAL(T_CODE + F_OFF), NOP, ADDIU(T1, T1, 1), J(T_CODE + G_OFF), NOP,
	};
	t_load(main_code, 6);
	static const uint32_t f[3] = { ADDIU(T2, T2, 1), JR(RA), NOP };
	static const uint32_t g[3] = { ADDIU(T0, T0, 0x10), JR(T3), NOP };
	for (unsigned k = 0; k < 3; k++) {
		t_wr32(T_CODE_PA + F_OFF + 4 * k, f[k]);
		t_wr32(T_CODE_PA + G_OFF + 4 * k, g[k]);
		t_wr32(T_CODE_PA + END_OFF + 4 * k, NOP);
	}
}

static void run_calls(void)
{
	R[T3] = T_CODE + END_OFF;
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(12);
	CHECK_EQ(CPU->pc, T_CODE + END_OFF);
}

/* Las entradas a bloques en orden (también por enlace y por exit_jr), los
 * JR por bloque y lo que se informa de cada compilación. */
static void rec_window_jr_and_classes(void)
{
	struct salsa64_rec *rec = CPU->rec;
	struct salsa64_rec_trace *t = rec ? rec->trace : NULL;
	if (!t)
		return;
	t->seq = t->jr = true;
	t->on_block = take_block;
	ngot = 0;
	salsa64_rec_flush(rec);
	load_calls();
	R[T0] = R[T1] = R[T2] = 0;
	uint64_t jr0 = CPU->rec_jr_count;
	run_calls();  /* compila y enlaza */
	CHECK_EQ(R[T0], 0x11);
	CHECK_EQ(R[T2], 1);
	CHECK(ngot >= 4);

	t->win_len = 64;
	t->arm_at = CPU->cycles ? CPU->cycles : 1;
	run_calls();
	unsigned len = salsa64_rec_trace_take(rec, true);
	CHECK(len >= 4);
	static const uint32_t want[4] = { 0, F_OFF, 0xC, G_OFF };
	unsigned seen = 0;
	for (unsigned k = 0; k < len && seen < 4; k++) {
		uint32_t v = t->win[k];
		if (!v || v >= SALSA64_RT_IDS)
			continue;
		CHECK_EQ(rec->x.xb[v].vpc, (uint32_t)T_CODE + want[seen]);
		seen++;
	}
	CHECK_EQ(seen, 4);
	CHECK(t->ntab >= 4);

	/* JR: el de f fue dos veces a T_CODE + 0xC (la segunda, repetido); el
	 * de g, dos veces a END. */
	unsigned idf = rec->map[(T_CODE_PA + F_OFF) >> 2], idg = rec->map[(T_CODE_PA + G_OFF) >> 2];
	CHECK(idf && idg);
	CHECK_EQ(t->jr_id[idf].count, 2);
	CHECK_EQ(t->jr_id[idf].repeat, 1);
	CHECK_EQ(t->jr_id[idf].last, (uint32_t)T_CODE + 0xC);
	CHECK_EQ(t->jr_id[idg].count, 2);
	CHECK_EQ(CPU->rec_jr_count - jr0, 4);
	CHECK_EQ(t->end[idf], SALSA64_RT_END_JR_RA);
	CHECK_EQ(t->end[rec->map[T_CODE_PA >> 2]], SALSA64_RT_END_JAL);
	CHECK_EQ(t->target[rec->map[T_CODE_PA >> 2]], (uint32_t)T_CODE + F_OFF);
	CHECK_EQ(t->nbr[rec->map[T_CODE_PA >> 2]], 1);
	CHECK_EQ(t->target[idf], 0);  /* JR: sin destino fijo */

	/* El aviso de f: clases y palabras que suman lo que ocupa. */
	const struct salsa64_rec_trace_block *bf = NULL;
	for (unsigned k = 0; k < ngot && k < 16; k++)
		if (got[k].vpc == (uint32_t)T_CODE + F_OFF)
			bf = &got[k];
	CHECK(bf != NULL);
	if (bf) {
		unsigned hot = 0, cold = 0;
		for (unsigned k = 0; k < SALSA64_RT_CLASSES; k++) {
			hot += bf->whot[k];
			cold += bf->wcold[k];
		}
		CHECK_EQ(bf->n, 3);
		CHECK_EQ(bf->end, SALSA64_RT_END_JR_RA);
		CHECK_EQ(hot, bf->hot);
		CHECK(cold <= bf->cold);
		CHECK_EQ(bf->ins[SALSA64_RT_C_BRANCH], 1);
		CHECK_EQ(bf->ins[SALSA64_RT_C_ALU], 2);
		CHECK(bf->whot[SALSA64_RT_C_ENTRY] > 0);
		CHECK(bf->code != 0);
	}

	/* Otra vez lo mismo tras vaciar: retraducciones. */
	uint64_t re = t->retrans;
	salsa64_rec_flush(rec);
	run_calls();
	CHECK(t->retrans >= re + 4);

	t->seq = t->jr = false;
	t->on_block = NULL;
	salsa64_rec_flush(rec);
}

void t_suite_rec_trace(void)
{
	RUN(window_fills_and_lists_blocks);
	RUN(compile_records_and_retranslations);
	RUN(jr_fold_by_address);
	RUN(classes_of_instructions);
	RUN(rec_window_jr_and_classes);
}
