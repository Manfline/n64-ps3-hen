/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — verificación del frontend con la ROM audiotest
 * (tests/roms/src/audiotest): que la ROM reporte por el ISViewer que el AI
 * y la HLE de audio (en el SPE) se comportaron, y que su audio llegó a la
 * salida y el puerto lo fue consumiendo.
 */
#ifndef SALSA64_FRONTEND_SELFTEST_AUDIO_H
#define SALSA64_FRONTEND_SELFTEST_AUDIO_H

#include "selftest.h"

void audiotest_start(void);
void audiotest_isv(const uint8_t *text, uint32_t len);
enum selftest_state audiotest_state(uint32_t fields);

#endif
