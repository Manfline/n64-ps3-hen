/*
 * Salsa64 — PIF: comandos joybus en la PIF RAM (controles).
 *
 * Comportamiento (n64brew.dev: PIF-NUS y Joybus Protocol):
 *   - Cuando el CPU escribe la PIF RAM con el bit 0x01 del byte de comandos
 *     (PIF RAM[63]) encendido, el PIF recorre la RAM, guarda dónde empieza
 *     el intercambio de cada canal y apaga ese bit.
 *   - En cada lectura de la PIF RAM por DMA del SI, el PIF ejecuta esos
 *     intercambios, deja las respuestas en el espacio reservado de cada uno
 *     y después hace la copia a la RDRAM.
 *   - Formato: bytes de escape 0x00 (saltar canal), 0xFD (reset de la
 *     línea), 0xFE (fin), 0xFF (nada); si no, un intercambio
 *     `TX RX [tx bytes] [rx bytes]` con las longitudes en los 6 bits bajos.
 *     TX bit 7 = saltar este intercambio; RX bit 7 = no hay dispositivo,
 *     bit 6 = faltaron bytes en la respuesta (los pone el PIF).
 *   - Canales 0–3: puertos de control; 4: cartucho (EEPROM; aún no).
 *
 * Control estándar: identificación (0x00 y 0xFF: 05 00 + estado), estado
 * (0x01: botones, X, Y) y, sin Controller Pak ni Rumble, lectura y
 * escritura del accesorio respondidas como un control sin pak. L + R +
 * Start reporta RST, Start apagado y el stick en (0, 0).
 *
 * Sin temporización: el DMA del SI sigue siendo instantáneo.
 */
#ifndef SALSA64_CORE_PIF_H
#define SALSA64_CORE_PIF_H

#include "core/types.h"

#define SALSA64_PIF_PORTS    4
#define SALSA64_PIF_CHANNELS 5  /* 4 puertos + cartucho */

/* Botones en la respuesta del comando 0x01, el bit más alto primero. */
enum salsa64_pad_button {
	SALSA64_PAD_A       = 0x8000,
	SALSA64_PAD_B       = 0x4000,
	SALSA64_PAD_Z       = 0x2000,
	SALSA64_PAD_START   = 0x1000,
	SALSA64_PAD_D_UP    = 0x0800,
	SALSA64_PAD_D_DOWN  = 0x0400,
	SALSA64_PAD_D_LEFT  = 0x0200,
	SALSA64_PAD_D_RIGHT = 0x0100,
	SALSA64_PAD_RST     = 0x0080,  /* lo pone el control con L + R + Start */
	SALSA64_PAD_L       = 0x0020,
	SALSA64_PAD_R       = 0x0010,
	SALSA64_PAD_C_UP    = 0x0008,
	SALSA64_PAD_C_DOWN  = 0x0004,
	SALSA64_PAD_C_LEFT  = 0x0002,
	SALSA64_PAD_C_RIGHT = 0x0001,
};

/* Estado de un control tal como lo da el frontend. Stick: + es derecha y
 * arriba; un control original llega a unos ±80. */
struct salsa64_pad {
	uint16_t buttons;
	int8_t x, y;
	uint8_t connected;
};

struct salsa64_pif {
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	int8_t handshake[SALSA64_PIF_CHANNELS];  /* posición del byte TX de cada canal, -1 si no hay */
	uint8_t parsed;                          /* hay intercambios listos para ejecutar */
};

struct salsa64_bus;

/* Al reset: olvida los intercambios; los controles siguen conectados. */
void salsa64_pif_reset(struct salsa64_bus *bus);
/* Lo llama el frontend en cada cuadro (o cuando cambia). */
void salsa64_pif_set_pad(struct salsa64_bus *bus, unsigned port, const struct salsa64_pad *pad);
/* Después de cada escritura del CPU en la PIF RAM (DMA o directa). */
void salsa64_pif_written(struct salsa64_bus *bus);
/* Antes de copiar la PIF RAM a la RDRAM por DMA del SI. */
void salsa64_pif_execute(struct salsa64_bus *bus);

/* CRC-8 de los datos del accesorio: polinomio 0x85, semilla 0. */
uint8_t salsa64_joybus_crc8(const uint8_t *data, unsigned len);

#endif
