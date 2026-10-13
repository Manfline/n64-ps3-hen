/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — fastmem en la PS3 (docs/fastmem.md).
 *
 * La RDRAM vive al principio de una ventana de 512 MB reservada con
 * sys_mmapper, donde todo lo demás está sin mapear; el código generado
 * accede a KSEG0/KSEG1 con `va & 0x1FFFFFFF` sobre la ventana y sin prueba
 * (cpu/rec.h, SALSA64_REC_OPT_FASTMEM). Un acceso que no es RDRAM falla;
 * lv2 suspende el hilo y avisa por una cola de eventos (syscall 327), y un
 * hilo ayudante parchea el acceso hacia su camino frío
 * (salsa64_rec_fastmem_fault) y lo reanuda (57). Antes de confiar en eso,
 * una autoprueba hace fallar un acceso desde un hilo propio: si no llega el
 * evento o el contexto (58) no trae el pc, fastmem queda apagado.
 */
#ifndef SALSA64_PLATFORM_PS3_FASTMEM_H
#define SALSA64_PLATFORM_PS3_FASTMEM_H

#include <stdbool.h>
#include <stdint.h>

#include "platform/ps3/paths.h"

struct salsa64_rec;

/* Con este archivo, la RDRAM va a la ventana y el recompilador compila los
 * accesos sin prueba (si la autoprueba pasa). */
#define SALSA64_FASTMEM_FLAG SALSA64_DATA_DIR "/jit-fastmem"

int salsa64_ps3_fastmem_wanted(void);

/* Reserva la ventana y mapea en ella la RDRAM (`bytes`, páginas de 1 MB).
 * Devuelve la RDRAM (el principio de la ventana) o NULL, con el motivo en
 * el log. Una sola vez por proceso. */
void *salsa64_ps3_fastmem_window(uint32_t bytes);
/* La ventana ya reservada, o NULL. */
void *salsa64_ps3_fastmem_ptr(void);

/* Arranca el ayudante sobre `rec` (ya iniciado con fastmem) y hace la
 * autoprueba. false si lv2 no avisa de los fallos o el contexto no trae el
 * pc: quien llama apaga fastmem (salsa64_rec_fastmem_off). Se puede llamar
 * en cada juego: el ayudante y la cola se crean una vez. */
bool salsa64_ps3_fastmem_start(struct salsa64_rec *rec);
void salsa64_ps3_fastmem_stop(void);

/* Para el log: fallos atendidos y desconocidos desde el arranque. */
void salsa64_ps3_fastmem_counts(uint32_t *faults, uint32_t *unknown, uint32_t *spu);

#endif
