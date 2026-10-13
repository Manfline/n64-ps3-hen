/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — mapeo del DualShock 3 al control de N64.
 *
 * No depende de PSL1GHT (main.c convierte padData a struct salsa64_ds3):
 * corre igual en las pruebas del host.
 *
 * Por omisión:
 *   stick izquierdo → stick (zona muerta y rango ajustables; Y invertida:
 *                     en el DS3 crece hacia abajo, en la N64 hacia arriba)
 *   stick derecho   → botones C (umbral ajustable)
 *   cruceta → cruceta · × → A · □ → B · L2 → Z · L1 → L · R1 → R ·
 *   START → Start. ○, △, R2, L3, R3 y SELECT libres (START + SELECT sale
 *   del emulador).
 *
 * Cada botón de la N64 sale de uno o dos botones del DS3 (o de ninguno), y
 * se cambia con un archivo de texto (SALSA64_DATA_DIR/controles.cfg):
 *
 *   # comentario
 *   Z = L2 R2
 *   START = START
 *   C_ABAJO = CIRCULO
 *   zona_muerta = 12
 *
 * Nombres de la N64: A B Z START L R ARRIBA ABAJO IZQUIERDA DERECHA
 * C_ARRIBA C_ABAJO C_IZQUIERDA C_DERECHA. Del DS3: CRUZ CIRCULO CUADRADO
 * TRIANGULO L1 R1 L2 R2 L3 R3 START SELECT ARRIBA ABAJO IZQUIERDA DERECHA
 * NADA. Ajustes: zona_muerta (0–100), rango (1–127), umbral_c (1–127).
 * Mayúsculas o minúsculas da igual.
 */
#ifndef SALSA64_FRONTEND_PADMAP_H
#define SALSA64_FRONTEND_PADMAP_H

#include <stddef.h>
#include <stdint.h>

#include "core/pif.h"

enum salsa64_ds3_button {
	SALSA64_DS3_CROSS, SALSA64_DS3_CIRCLE, SALSA64_DS3_SQUARE, SALSA64_DS3_TRIANGLE,
	SALSA64_DS3_L1, SALSA64_DS3_R1, SALSA64_DS3_L2, SALSA64_DS3_R2,
	SALSA64_DS3_L3, SALSA64_DS3_R3, SALSA64_DS3_START, SALSA64_DS3_SELECT,
	SALSA64_DS3_UP, SALSA64_DS3_DOWN, SALSA64_DS3_LEFT, SALSA64_DS3_RIGHT,
	SALSA64_DS3_NBUTTONS,
	SALSA64_DS3_NONE = 0xFF,
};

/* Estado del DS3: botones en bits (1 << enum salsa64_ds3_button) y sticks
 * de 0 a 255 con centro en 128 (X crece a la derecha, Y hacia abajo). */
struct salsa64_ds3 {
	uint32_t buttons;
	uint8_t lx, ly, rx, ry;
};

/* Botones de la N64 que se pueden asignar, en el orden de la tabla. */
enum salsa64_n64_input {
	SALSA64_IN_A, SALSA64_IN_B, SALSA64_IN_Z, SALSA64_IN_START,
	SALSA64_IN_L, SALSA64_IN_R,
	SALSA64_IN_D_UP, SALSA64_IN_D_DOWN, SALSA64_IN_D_LEFT, SALSA64_IN_D_RIGHT,
	SALSA64_IN_C_UP, SALSA64_IN_C_DOWN, SALSA64_IN_C_LEFT, SALSA64_IN_C_RIGHT,
	SALSA64_IN_COUNT,
};

struct salsa64_padmap {
	uint8_t source[SALSA64_IN_COUNT];  /* botón del DS3 o SALSA64_DS3_NONE */
	uint8_t source2[SALSA64_IN_COUNT]; /* otro botón que hace lo mismo, o SALSA64_DS3_NONE */
	uint8_t deadzone;     /* stick izquierdo: desplazamiento del DS3 que se ignora */
	uint8_t range;        /* valor de la N64 con el stick a fondo */
	uint8_t c_threshold;  /* stick derecho: desplazamiento que activa un botón C */
};

void salsa64_padmap_default(struct salsa64_padmap *map);
/* DS3 → control de N64 (conectado). */
void salsa64_padmap_apply(const struct salsa64_padmap *map, const struct salsa64_ds3 *in,
			  struct salsa64_pad *out);
/* Aplica las líneas de `text` sobre `map`. Devuelve cuántas líneas no se
 * entendieron; la primera queda descrita en `err` (puede ser NULL). */
int salsa64_padmap_parse(struct salsa64_padmap *map, const char *text, size_t len,
			 char *err, size_t err_size);
/* Describe el estado de un control en texto ("A Z C_ABAJO stick=(40,-12)"). */
void salsa64_pad_describe(const struct salsa64_pad *pad, char *buf, size_t size);

#endif
