/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — un hilo del PPE de baja prioridad que hace un trabajo largo de
 * disco o de espera (escribir un estado comprimido, la caja negra) sin
 * detener la emulación ni la presentación. Un trabajo a la vez: pedir otro
 * con uno en marcha da false y quien llama decide (esperar o avisar).
 *
 * El hilo vive de bgjob_init a bgjob_shutdown. La función del trabajo corre
 * en él y puede dormir, escribir con sysLv2Fs* o newlib y hablar con SPEs
 * (spukit); no debe tocar la máquina emulada ni el RSX. Su resultado lo lee
 * quien lo pidió cuando bgjob_busy() ya dice false (hay barrera de memoria
 * entre el final del trabajo y ese false).
 */
#ifndef SALSA64_PLATFORM_PS3_BGJOB_H
#define SALSA64_PLATFORM_PS3_BGJOB_H

#include <stdbool.h>
#include <stdint.h>

/* Por debajo de la emulación (1000) y la presentación (1001), como el hilo
 * que escribe el perfil. */
#define BGJOB_PRIO  1500
#define BGJOB_STACK (64u << 10)

typedef void (*bgjob_fn)(void *arg);

/* Crea el hilo. false si no pudo (quien llama hace el trabajo a la antigua). */
bool bgjob_init(void);
bool bgjob_ready(void);
/* Pide el trabajo; false si hay uno en marcha o no hay hilo. */
bool bgjob_submit(bgjob_fn fn, void *arg);
bool bgjob_busy(void);
/* Espera a que termine el trabajo en marcha (duerme de a 1 ms); true si
 * terminó antes de `timeout_us` (0: sin límite). */
bool bgjob_wait(uint64_t timeout_us);
/* Espera lo que haya y para el hilo. */
void bgjob_shutdown(void);

#endif
