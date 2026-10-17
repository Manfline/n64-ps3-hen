/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — listas de audio de prueba (selftest.c): una RDRAM de mentira
 * con entradas seudoaleatorias y una lista de comandos en la dirección 0
 * que pasa por casi todos los comandos de nead (Zelda OoT), ABI1 o naudio
 * (con o sin MP3) o por dos subtramas de MusyX. Dos
 * ejecuciones de la HLE sobre copias iguales deben dejar las copias
 * iguales: así se compara el SPE con el PPE al arrancar en la PS3.
 */
#ifndef SALSA64_RSP_AUDIO_SELFTEST_H
#define SALSA64_RSP_AUDIO_SELFTEST_H

#include <stdint.h>

#define AUD_SELFTEST_RAM 0x4000u

/* Llena `ram` (AUD_SELFTEST_RAM bytes) para el ABI pedido
 * (SALSA64_ABI_AUDIO o _AUDIO_GE: ABI1; SALSA64_ABI_NAUDIO*: naudio, con
 * MP3 en _MP3 y _CBFD; SALSA64_ABI_MUSYX*: MusyX; cualquier otro: nead) y
 * devuelve el tamaño de la lista en bytes (en MusyX, el número de
 * subtramas, que es lo que lleva `data_size`). */
uint32_t aud_selftest_build(uint8_t *ram, uint32_t abi, uint32_t seed);

#endif
