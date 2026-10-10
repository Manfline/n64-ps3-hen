/*
 * Salsa64 — micro-benchmark del intérprete: un bucle con la mezcla típica
 * de un juego (cargas, almacenamientos, ALU, salto, FPU) corriendo desde
 * KSEG0 sobre la RDRAM. Da millones de instrucciones emuladas por segundo.
 *
 * Solo vale como referencia en la PS3 real (docs/perf-log.md). Como guía:
 * el VR4300 corre a 93,75 MHz y un juego ejecuta del orden de 30–60 millones
 * de instrucciones por segundo.
 */
#include <stdio.h>

#include "test.h"

void salsa64_unit_bench(void (*print)(const char *line), uint64_t (*now_us)(void))
{
	static const uint32_t loop[] = {
		LW(T1, 0, T0),
		ADDIU(T1, T1, 1),
		SW(T1, 0, T0),
		ADDU(T2, T2, T1),
		SLL(T3, T2, 3),
		XOR(T4, T3, T2),
		ADD_F(F_S, 0, 0, 1),
		ADDIU(T5, T5, -1),
		BNE(T5, ZERO, -9),
		NOP,
	};
	const uint64_t cycles = 20000000;  /* 1 instrucción = 1 ciclo por ahora */
	char line[128];

	salsa64_n64_reset(&t_n64);
	t_set_status(T_STATUS);
	t_load(loop, sizeof loop / sizeof loop[0]);
	salsa64_cpu_set_pc(CPU, T_CODE);
	R[T0] = T_DATA;
	R[T5] = 0x7FFFFFFF;
	CPU->fpr[1] = t_fbits(1.0f);

	uint64_t t0 = now_us();
	salsa64_n64_run(&t_n64, cycles);
	uint64_t us = now_us() - t0;

	snprintf(line, sizeof line, "bench: %llu instrucciones en %llu ms = %llu.%01llu MIPS",
		 (unsigned long long)cycles, (unsigned long long)(us / 1000),
		 (unsigned long long)(us ? cycles / us : 0),
		 (unsigned long long)(us ? cycles * 10 / us % 10 : 0));
	print(line);
}
