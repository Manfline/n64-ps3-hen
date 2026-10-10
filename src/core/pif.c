/*
 * Salsa64 — PIF: comandos joybus en la PIF RAM. Ver pif.h.
 *
 * Escrito desde n64brew.dev (PIF-NUS, Joybus Protocol, consultadas el
 * 2026-10-10); no deriva de código de otros emuladores.
 */
#include "core/bus.h"

#include <string.h>

#include "core/save.h"

#define CMD_BYTE       63            /* byte de comandos del PIF */
#define CMD_JOYBUS     0x01          /* preparar los intercambios joybus */

#define ESC_SKIP       0x00          /* saltar al canal siguiente */
#define ESC_RESET      0xFD          /* señal de reset en la línea */
#define ESC_END        0xFE
#define ESC_NOP        0xFF

#define TX_SKIP        0x80          /* no ejecutar este intercambio */
#define LEN_MASK       0x3F
#define RX_NO_DEVICE   0x80
#define RX_SHORT       0x40          /* la respuesta trajo menos bytes de los pedidos */

/* Comandos del control estándar. */
#define JB_INFO        0x00
#define JB_STATE       0x01
#define JB_PAK_READ    0x02
#define JB_PAK_WRITE   0x03
#define JB_RESET       0xFF

#define PAK_BLOCK      32
#define PAD_ID_HI      0x05          /* control estándar: 0x0500 */
#define PAD_ID_LO      0x00
#define PAD_NO_PAK     0x02          /* estado: no hay accesorio */

#define PAD_RESET_COMBO (SALSA64_PAD_L | SALSA64_PAD_R | SALSA64_PAD_START)

void salsa64_pif_reset(struct salsa64_bus *bus)
{
	struct salsa64_pif *pif = &bus->pif;
	for (int ch = 0; ch < SALSA64_PIF_CHANNELS; ch++)
		pif->handshake[ch] = -1;
	pif->parsed = 0;
}

void salsa64_pif_set_pad(struct salsa64_bus *bus, unsigned port, const struct salsa64_pad *pad)
{
	if (port < SALSA64_PIF_PORTS)
		bus->pif.pads[port] = *pad;
}

uint8_t salsa64_joybus_crc8(const uint8_t *data, unsigned len)
{
	uint8_t crc = 0;
	for (unsigned i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x85 : crc << 1);
	}
	return crc;
}

/* Recorre la PIF RAM y guarda el primer intercambio de cada canal. */
static void parse(struct salsa64_pif *pif, const uint8_t *ram)
{
	unsigned i = 0, ch = 0;
	for (int k = 0; k < SALSA64_PIF_CHANNELS; k++)
		pif->handshake[k] = -1;
	while (i < CMD_BYTE && ch < SALSA64_PIF_CHANNELS) {
		uint8_t b = ram[i];
		if (b == ESC_END)
			break;
		if (b == ESC_NOP || b == ESC_RESET) {
			i++;
			continue;
		}
		if (b == ESC_SKIP) {
			i++;
			ch++;
			continue;
		}
		if (i + 1 >= CMD_BYTE)
			break;
		unsigned len = 2u + (b & LEN_MASK) + (ram[i + 1] & LEN_MASK);
		if (i + len > CMD_BYTE)
			break;
		pif->handshake[ch++] = (int8_t)i;
		i += len;
	}
	pif->parsed = 1;
}

void salsa64_pif_written(struct salsa64_bus *bus)
{
	uint8_t *ram = bus->pif_ram;
	if (!(ram[CMD_BYTE] & CMD_JOYBUS))
		return;
	parse(&bus->pif, ram);
	ram[CMD_BYTE] &= (uint8_t)~CMD_JOYBUS;
}

/* Respuesta del control estándar en `reply`; devuelve su longitud, o -1 si
 * el control no contesta (comando desconocido o incompleto). */
static int pad_command(const struct salsa64_pad *pad, const uint8_t *tx, unsigned txlen, uint8_t *reply)
{
	switch (tx[0]) {
	case JB_INFO:
	case JB_RESET:
		reply[0] = PAD_ID_HI;
		reply[1] = PAD_ID_LO;
		reply[2] = PAD_NO_PAK;
		return 3;
	case JB_STATE: {
		uint16_t b = pad->buttons & (uint16_t)~SALSA64_PAD_RST;
		int8_t x = pad->x, y = pad->y;
		if ((b & PAD_RESET_COMBO) == PAD_RESET_COMBO) {
			b = (uint16_t)((b & ~SALSA64_PAD_START) | SALSA64_PAD_RST);
			x = y = 0;
		}
		reply[0] = (uint8_t)(b >> 8);
		reply[1] = (uint8_t)b;
		reply[2] = (uint8_t)x;
		reply[3] = (uint8_t)y;
		return 4;
	}
	case JB_PAK_READ:
		/* Sin pak: 32 ceros y el CRC complementado (0xFF). */
		if (txlen < 3)
			return -1;
		memset(reply, 0, PAK_BLOCK);
		reply[PAK_BLOCK] = 0xFF;
		return PAK_BLOCK + 1;
	case JB_PAK_WRITE:
		/* Sin pak: solo el CRC de los datos, complementado. */
		if (txlen < 3 + PAK_BLOCK)
			return -1;
		reply[0] = (uint8_t)~salsa64_joybus_crc8(tx + 3, PAK_BLOCK);
		return 1;
	default:
		return -1;
	}
}

void salsa64_pif_execute(struct salsa64_bus *bus)
{
	struct salsa64_pif *pif = &bus->pif;
	uint8_t *ram = bus->pif_ram;
	if (!pif->parsed)
		return;
	for (int ch = 0; ch < SALSA64_PIF_CHANNELS; ch++) {
		if (pif->handshake[ch] < 0)
			continue;
		unsigned at = (unsigned)pif->handshake[ch];
		uint8_t txb = ram[at];
		if (txb & TX_SKIP)
			continue;
		unsigned txlen = txb & LEN_MASK, rxlen = ram[at + 1] & LEN_MASK;
		uint8_t *tx = ram + at + 2, *rx = tx + txlen;
		uint8_t reply[PAK_BLOCK + 1];
		int n = -1;
		if (txlen && ch < SALSA64_PIF_PORTS && pif->pads[ch].connected)
			n = pad_command(&pif->pads[ch], tx, txlen, reply);
		else if (ch == SALSA64_PIF_PORTS && bus->save)  /* EEPROM del cartucho */
			n = salsa64_save_joybus(bus->save, tx, txlen, reply);
		uint8_t err = 0;
		if (n < 0) {
			err = RX_NO_DEVICE;
		} else {
			unsigned copy = (unsigned)n < rxlen ? (unsigned)n : rxlen;
			memcpy(rx, reply, copy);
			if ((unsigned)n < rxlen)
				err = RX_SHORT;
		}
		ram[at + 1] = (uint8_t)(rxlen | err);
	}
}
