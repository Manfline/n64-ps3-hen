/*
 * Salsa64 — micro-benchmark del intérprete: bucles de 10 instrucciones
 * corriendo desde KSEG0 sobre la RDRAM. Dan millones de instrucciones
 * emuladas por segundo (MIPS) y nanosegundos por instrucción.
 *
 * "mixto" es la mezcla típica de un juego (cargas, almacenamientos, ALU,
 * salto, FPU) y es la cifra que se compara entre versiones. Las demás
 * variantes aíslan el costo de cada clase de instrucción: "nop" mide el
 * bucle del intérprete (búsqueda, decodificación y despacho), y la
 * diferencia de cada variante contra "nop" es lo que cuesta su clase.
 * Todas llevan la misma cola ADDIU + BNE + delay slot.
 *
 * Solo vale como referencia en la PS3 real (docs/perf-log.md). Como guía:
 * el VR4300 corre a 93,75 MHz y un juego ejecuta del orden de 30–60 millones
 * de instrucciones por segundo.
 */
#include <stdio.h>

#include "core/endian.h"
#include "cpu/rec.h"
#include "test.h"

#define LOOP_TAIL(delay) ADDIU(T5, T5, -1), BNE(T5, ZERO, -9), delay

static const uint32_t loop_mixed[] = {
	LW(T1, 0, T0),
	ADDIU(T1, T1, 1),
	SW(T1, 0, T0),
	ADDU(T2, T2, T1),
	SLL(T3, T2, 3),
	XOR(T4, T3, T2),
	ADD_F(F_S, 0, 0, 1),
	LOOP_TAIL(NOP),
};

static const uint32_t loop_nop[] = {
	NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	LOOP_TAIL(NOP),
};

static const uint32_t loop_int[] = {
	ADDU(T2, T2, T1),
	SLL(T3, T2, 3),
	XOR(T4, T3, T2),
	OR(T6, T4, T1),
	SUBU(T7, T6, T2),
	SRL(T8, T7, 2),
	AND(T9, T8, T4),
	LOOP_TAIL(NOP),
};

static const uint32_t loop_mem[] = {
	LW(T1, 0, T0),
	SW(T1, 4, T0),
	LW(T2, 8, T0),
	SW(T2, 12, T0),
	LW(T3, 16, T0),
	SW(T3, 20, T0),
	LW(T4, 24, T0),
	LOOP_TAIL(SW(T4, 28, T0)),
};

/* MOV.S: pasa por la decodificación de COP1 sin aritmética. */
static const uint32_t loop_fmov[] = {
	MOV_F(F_S, 2, 0), MOV_F(F_S, 3, 1), MOV_F(F_S, 4, 0), MOV_F(F_S, 5, 1),
	MOV_F(F_S, 6, 0), MOV_F(F_S, 7, 1), MOV_F(F_S, 8, 0),
	LOOP_TAIL(NOP),
};

/* ADD.S con resultado inexacto (1,5 + 0,1): el camino completo de redondeo. */
static const uint32_t loop_fadd[] = {
	ADD_F(F_S, 2, 0, 1), ADD_F(F_S, 3, 0, 1), ADD_F(F_S, 4, 0, 1), ADD_F(F_S, 5, 0, 1),
	ADD_F(F_S, 6, 0, 1), ADD_F(F_S, 7, 0, 1), ADD_F(F_S, 8, 0, 1),
	LOOP_TAIL(NOP),
};

/* Conversiones de la FPU (entero a float, truncado, float ↔ double,
 * raíz, a entero) y CFC1: el recompilador las mandaba al intérprete hasta
 * el 2026-10-10 (SALSA64_REC_OPT_RARE_EXEC lo repite). f1 como entero
 * (0x3DCCCCCD) tiene 31 bits: CVT.S.W redondea. */
static const uint32_t loop_cvt[] = {
	CVT_S(F_W, 2, 1),
	TRUNC_W(F_S, 3, 0),
	CVT_D(F_S, 4, 1),
	CVT_S(F_D, 6, 4),
	SQRT_F(F_S, 7, 0),
	CFC1(T6, 31),
	CVT_W(F_S, 8, 1),
	LOOP_TAIL(NOP),
};

/* El molde de «(int)x» de los compiladores de N64: CVT.W con el redondeo a
 * cero puesto en FCR31 (ctc1 antes y después), así que corre con RM = 1; el
 * recompilador los mandaba al intérprete hasta el 2026-10-16. */
static const uint32_t loop_cvtw_rm[] = {
	CVT_W(F_S, 2, 0), CVT_W(F_S, 3, 0), CVT_W(F_S, 4, 0), CVT_W(F_S, 5, 0),
	CVT_W(F_S, 6, 0), CVT_W(F_S, 7, 0), CVT_W(F_S, 8, 0),
	LOOP_TAIL(NOP),
};

/* MUL.S con un operando minúsculo (1e-25, fuera de la franja de ±2^60 de
 * antes) y un producto normal (3e-25). */
static const uint32_t loop_fmul_tiny[] = {
	MUL_F(F_S, 2, 0, 1), MUL_F(F_S, 3, 0, 1), MUL_F(F_S, 4, 0, 1), MUL_F(F_S, 5, 0, 1),
	MUL_F(F_S, 6, 0, 1), MUL_F(F_S, 7, 0, 1), MUL_F(F_S, 8, 0, 1),
	LOOP_TAIL(NOP),
};

/* Copia no alineada como la escribe el compilador: pares LWL/LWR y
 * SWL/SWR. */
static const uint32_t loop_unaligned[] = {
	LWL(T1, 1, T0), LWR(T1, 4, T0), SWL(T1, 0x21, T0), SWR(T1, 0x24, T0),
	LWL(T2, 6, T0), LWR(T2, 9, T0), ADDU(T3, T3, T2),
	LOOP_TAIL(NOP),
};

/* MFC0 (Count, Status, Cause, EPC): antes cada uno terminaba el bloque. */
static const uint32_t loop_mfc0[] = {
	MFC0(T1, SALSA64_CP0_COUNT), ADDU(T2, T2, T1), MFC0(T3, SALSA64_CP0_STATUS), OR(T4, T4, T3),
	MFC0(T6, SALSA64_CP0_CAUSE), XOR(T7, T7, T6), MFC0(T8, SALSA64_CP0_EPC),
	LOOP_TAIL(NOP),
};

/* Espera por tiempo sondeando Count (como la ROM gfxtest): un bucle corto
 * que no termina durante el bench (T5 = 0x7FFFFFFF ciclos de espera). Con
 * MFC0 terminando el bloque, el recompilador volvía a C en cada vuelta. */
static const uint32_t loop_count_wait[] = {
	MFC0(T1, SALSA64_CP0_COUNT), SUBU(T2, T1, T3), SLTU(T2, T2, T5), BNE(T2, ZERO, -4), NOP,
};

struct bench_case {
	const char *name;
	const uint32_t *code;
	unsigned n;
	uint64_t instructions;
	float f0, f1;
	uint32_t fcr31;
};

#define CASE(name, loop, m, f0, f1) { name, loop, sizeof loop / sizeof loop[0], (m) * 1000000ull, f0, f1, 0 }
#define CASE_FCR(name, loop, m, f0, f1, fcr31) \
	{ name, loop, sizeof loop / sizeof loop[0], (m) * 1000000ull, f0, f1, fcr31 }

static const struct bench_case cases[] = {
	CASE("mixto", loop_mixed, 20, 0, 1.0f),  /* igual que la línea base: comparable */
	CASE("nop", loop_nop, 10, 0, 0),
	CASE("enteros", loop_int, 10, 0, 0),
	CASE("memoria", loop_mem, 10, 0, 0),
	CASE("mov.s", loop_fmov, 10, 1.5f, 0.1f),
	CASE("add.s", loop_fadd, 10, 1.5f, 0.1f),
	CASE("cvt", loop_cvt, 10, 1.5f, 0.1f),
	CASE_FCR("cvt.w rm1", loop_cvtw_rm, 10, 1234.5f, 0, 1),
	CASE_FCR("cvt.w rm2", loop_cvtw_rm, 10, 1234.5f, 0, 2),
	CASE("mul.s mín", loop_fmul_tiny, 10, 1e-25f, 3.0f),
	CASE("no alin.", loop_unaligned, 10, 0, 0),
	CASE("mfc0", loop_mfc0, 10, 0, 0),
	CASE("espera", loop_count_wait, 10, 0, 0),
};

void salsa64_unit_bench(void (*print)(const char *line), uint64_t (*now_us)(void))
{
	char line[224];
	struct salsa64_rec *rec = CPU->rec;
	if (rec)
		rec->strict = false;  /* como en un juego: lo que no cabe antes de un evento, al intérprete */

	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		const struct bench_case *c = &cases[k];

		salsa64_n64_reset(&t_n64);
		t_set_status(T_STATUS);
		t_load(c->code, c->n);
		salsa64_cpu_set_pc(CPU, T_CODE);
		R[T0] = T_DATA;
		R[T1] = 0x12345;
		R[T5] = 0x7FFFFFFF;
		CPU->fpr[0] = t_fbits(c->f0);
		CPU->fpr[1] = t_fbits(c->f1);
		CPU->fcr31 = c->fcr31;

		uint64_t entries0 = rec ? rec->stats.entries : 0, calls0 = rec ? rec->stats.exec_calls : 0;
		uint64_t t0 = now_us();
		salsa64_n64_run(&t_n64, c->instructions);  /* 1 instrucción = 1 ciclo por ahora */
		uint64_t us = now_us() - t0;
		if (!us)
			us = 1;

		int len = snprintf(line, sizeof line,
				   "bench%s: %-8s %3llu M instr. en %6llu ms = %4llu.%01llu MIPS, %5llu.%01llu ns/instr.",
				   rec ? " rec" : "", c->name, (unsigned long long)(c->instructions / 1000000),
				   (unsigned long long)(us / 1000),
				   (unsigned long long)(c->instructions / us),
				   (unsigned long long)(c->instructions * 10 / us % 10),
				   (unsigned long long)(us * 1000 / c->instructions),
				   (unsigned long long)(us * 10000 / c->instructions % 10));
		if (rec && len > 0 && (size_t)len < sizeof line)  /* salidas a C y llamadas al intérprete */
			snprintf(line + len, sizeof line - (size_t)len, ", %llu entradas desde C, %llu llamadas al intérprete",
				 (unsigned long long)(rec->stats.entries - entries0),
				 (unsigned long long)(rec->stats.exec_calls - calls0));
		print(line);
	}
}

/*
 * Diagnóstico (docs/perf-log.md): el bench del intérprete bajó de 21 a
 * 14,7 MIPS en la PS3 al traer la Fase 2 sin cambiar interp.c, y en la Mac
 * no. Las mismas variantes, con el intérprete, en una máquina aparte con la
 * estructura y la RDRAM alineadas a 64 KB: si aquí vuelve a ~21, es la
 * ubicación de t_n64/t_rdram en memoria, no el código.
 */
static struct salsa64_n64 bench_n64 __attribute__((aligned(65536)));
static uint8_t bench_rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(65536)));

void salsa64_unit_bench_aligned(void (*print)(const char *line), uint64_t (*now_us)(void))
{
	char line[200];
	struct salsa64_n64 *n = &bench_n64;
	salsa64_n64_init(n, bench_rdram, sizeof bench_rdram);
	snprintf(line, sizeof line, "bench alineado: máquina en %p, RDRAM en %p (t_n64 en %p, t_rdram en %p)",
		 (void *)n, (void *)bench_rdram, (void *)&t_n64, (void *)t_n64.cpu.rdram);
	print(line);

	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		const struct bench_case *c = &cases[k];
		salsa64_n64_reset(n);
		salsa64_cpu_cop0_write(&n->cpu, SALSA64_CP0_STATUS, T_STATUS);
		for (unsigned j = 0; j < c->n; j++)
			salsa64_wr32(bench_rdram + T_CODE_PA + 4 * j, c->code[j]);
		salsa64_cpu_set_pc(&n->cpu, T_CODE);
		n->cpu.gpr[T0] = T_DATA;
		n->cpu.gpr[T1] = 0x12345;
		n->cpu.gpr[T5] = 0x7FFFFFFF;
		n->cpu.fpr[0] = t_fbits(c->f0);
		n->cpu.fpr[1] = t_fbits(c->f1);
		n->cpu.fcr31 = c->fcr31;

		uint64_t t0 = now_us();
		salsa64_n64_run(n, c->instructions);
		uint64_t us = now_us() - t0;
		if (!us)
			us = 1;
		snprintf(line, sizeof line, "bench alineado: %-8s %3llu M instr. en %6llu ms = %4llu.%01llu MIPS",
			 c->name, (unsigned long long)(c->instructions / 1000000), (unsigned long long)(us / 1000),
			 (unsigned long long)(c->instructions / us),
			 (unsigned long long)(c->instructions * 10 / us % 10));
		print(line);
	}
}
