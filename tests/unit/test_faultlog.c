/*
 * Salsa64 — pruebas del aviso de fallos del CPU (src/core/faultlog.c):
 * una línea por tipo y pc para las excepciones de dirección y de bus del
 * código del juego, nada para las de la TLB ni las del arranque.
 */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/faultlog.h"

static unsigned lines;
static char last[320];

static void capture(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last, sizeof last, fmt, ap);
	va_end(ap);
	lines++;
}

/* La máquina como al empezar una prueba, con el aviso (y lo ya avisado) igual. */
static void again(void)
{
	void (*hook)(struct salsa64_cpu *) = CPU->exc_hook;
	salsa64_n64_reset(&t_n64);
	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE);
	CPU->exc_hook = hook;
}

static void arm(void)
{
	CPU->exc_hook = NULL;
	lines = 0;
	last[0] = '\0';
	salsa64_faultlog_install(CPU, capture);
}

static void disarm(void)
{
	CPU->exc_hook = NULL;
}

/* Una carga desalineada en código de KSEG0: AdEL, con el pc, la dirección y
 * ra en la línea; la misma falla otra vez no repite el aviso. */
static void adel_once(void)
{
	arm();
	t_n64.bus.vi.fields = 1438;
	R[T0] = T_DATA + 2;
	R[31] = 0xFFFFFFFF80002468ull;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(lines, 1);
	CHECK(strstr(last, "AdEL") != NULL);
	CHECK(strstr(last, "campo 1438") != NULL);
	CHECK(strstr(last, "pc 0x80001000") != NULL);
	CHECK(strstr(last, "0xFFFFFFFF80002002") != NULL);
	CHECK(strstr(last, "ra 0x80002468") != NULL);

	again();
	R[T0] = T_DATA + 2;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EQ(lines, 1);  /* mismo tipo y pc */

	/* Otro pc: otro aviso. */
	again();
	R[T0] = T_DATA + 2;
	T_LOAD(NOP, LW(T1, 0, T0));
	t_step(2);
	CHECK_EQ(lines, 2);
	CHECK(strstr(last, "pc 0x80001004") != NULL);
	disarm();
}

/* Escribir en una dirección desalineada: AdES. */
static void ades(void)
{
	arm();
	R[T0] = T_DATA + 1;
	T_LOAD(SW(T1, 0, T0));
	t_step(1);
	CHECK_EQ(lines, 1);
	CHECK(strstr(last, "AdES") != NULL);
	disarm();
}

/* Un salto a una dirección impar (un callback pisado): falla al buscar la
 * instrucción, con el pc = la dirección mala; se avisa por ra en KSEG0. */
static void jump_to_garbage(void)
{
	arm();
	R[T0] = 0x10001;
	R[31] = 0xFFFFFFFF800028D4ull;
	T_LOAD(JR(T0), NOP);
	t_step(3);
	CHECK_EQ(lines, 1);
	CHECK(strstr(last, "AdEL") != NULL);
	CHECK(strstr(last, "pc 0x00010001") != NULL);
	CHECK(strstr(last, "ra 0x800028D4") != NULL);
	disarm();
}

/* Lo que no es un fallo del juego no avisa: una syscall, y una dirección
 * mala con ra fuera de KSEG0 (arranque, TLB). */
static void other_exceptions_are_silent(void)
{
	arm();
	T_LOAD(SYSCALL);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_SYS, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(lines, 0);

	again();
	R[T0] = 0x10001;
	R[31] = 0x1234;
	T_LOAD(JR(T0), NOP);
	t_step(3);
	CHECK_EQ(lines, 0);
	disarm();
}

/* No pisa un gancho que ya estaba (--trace-exc). */
static void keeps_existing_hook(void)
{
	static void (*const other)(struct salsa64_cpu *) = (void (*)(struct salsa64_cpu *))0x1;
	CPU->exc_hook = other;
	salsa64_faultlog_install(CPU, capture);
	CHECK(CPU->exc_hook == other);
	disarm();
}

void t_suite_faultlog(void)
{
	RUN(adel_once);
	RUN(ades);
	RUN(jump_to_garbage);
	RUN(other_exceptions_are_silent);
	RUN(keeps_existing_hook);
}
