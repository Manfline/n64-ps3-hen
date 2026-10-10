/*
 * Salsa64 — captura de la pantalla del RSX muestreada a la resolución de la
 * imagen del VI (un píxel por píxel de la N64), en PPM. Para ver desde la
 * Mac lo que se ve en la tele: SALSA64_DATA_DIR/screen-N.ppm por FTP.
 */
#ifndef SALSA64_FRONTEND_SCREENSHOT_H
#define SALSA64_FRONTEND_SCREENSHOT_H

#include <rsxkit/rsxkit.h>

#include "core/rcp.h"

/* Después de dibujar el cuadro y de rsxkit_finish. */
void salsa64_screenshot(const char *path, const struct salsa64_vi_frame *f, const rsxkit_rect *r);

#endif
