/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — aviso en el log de los fallos del CPU del juego.
 *
 * Muchos «cuelgues» son la pantalla de error del propio juego: un hilo
 * salta a una dirección que no existe (un callback pisado) o carga de una
 * dirección desalineada, libultra lo marca con FAULT y el hilo de fallos
 * del juego se queda leyendo el control. Desde fuera se ve como un juego
 * detenido que lee el control miles de veces por segundo. Este gancho de
 * excepciones deja una línea cuando el CPU toma una excepción de dirección
 * (AdEL, AdES) o de bus (IBE, DBE) con el PC en KSEG0 (código del juego, no
 * el arranque ni la TLB): el campo, el pc, la dirección que falló, ra y sp.
 * Una vez por tipo y pc: un juego que falla en un bucle no llena el log.
 *
 * Usa el gancho cpu->exc_hook; si ya hay uno (--trace-exc en el host) no
 * lo pisa. Sin asignaciones: la tabla de lo ya avisado es estática.
 */
#ifndef SALSA64_CORE_FAULTLOG_H
#define SALSA64_CORE_FAULTLOG_H

#include "cpu/vr4300.h"

typedef void (*salsa64_faultlog_fn)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Instala el aviso y olvida lo ya avisado (una por juego). */
void salsa64_faultlog_install(struct salsa64_cpu *cpu, salsa64_faultlog_fn log);

#endif
