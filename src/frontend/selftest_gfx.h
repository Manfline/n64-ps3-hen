/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — verificación del frontend con la ROM gfxtest
 * (tests/roms/src/gfxtest): que la ROM reporte por el ISViewer que sus
 * tareas gráficas terminaron bien y que la pantalla del RSX muestre la
 * escena en los puntos de expect.h. Deja además la imagen, muestreada a la
 * resolución de la N64, en SALSA64_DATA_DIR/gfxtest.ppm.
 */
#ifndef SALSA64_FRONTEND_SELFTEST_GFX_H
#define SALSA64_FRONTEND_SELFTEST_GFX_H

#include <rsxkit/rsxkit.h>

#include "core/rcp.h"
#include "selftest.h"

void gfxtest_start(void);
void gfxtest_isv(const uint8_t *text, uint32_t len);
/* ¿Hay que verificar la pantalla de este campo? (después del OK de la ROM) */
bool gfxtest_want_screen(void);
/* Después de dibujar y de rsxkit_finish. */
void gfxtest_screen(const struct salsa64_vi_frame *f, const rsxkit_rect *r);
enum selftest_state gfxtest_state(uint32_t fields);

#endif
