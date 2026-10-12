/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — verificación del frontend con la ROM vitest
 * (tests/roms/src/vitest): que la ROM se verifique a sí misma por el
 * ISViewer, que el framebuffer del VI tenga su patrón en RGBA5551 y en
 * RGBA8888 y que la pantalla del RSX lo muestre. Ver main.c.
 */
#ifndef SALSA64_FRONTEND_SELFTEST_H
#define SALSA64_FRONTEND_SELFTEST_H

#include <rsxkit/rsxkit.h>

#include "core/bus.h"

enum selftest_state {
	SELFTEST_RUNNING,
	SELFTEST_OK,
	SELFTEST_FAILED,
};

void selftest_start(void);
/* Texto que imprime la ROM por el ISViewer. */
void selftest_isv(const uint8_t *text, uint32_t len);
/* Antes de presentar el campo: verifica el framebuffer en la RDRAM. Devuelve
 * true si este campo hay que verificarlo también en la pantalla. */
bool selftest_frame(const struct salsa64_bus *bus, const struct salsa64_vi_frame *f);
/* Después de dibujar (y de rsxkit_finish): verifica la pantalla. */
void selftest_screen(const struct salsa64_vi_frame *f, const rsxkit_rect *r);
/* Estado tras `fields` campos; se rinde después de SELFTEST_MAX_FIELDS. */
enum selftest_state selftest_state(uint32_t fields);

#define SELFTEST_MAX_FIELDS 600

#endif
