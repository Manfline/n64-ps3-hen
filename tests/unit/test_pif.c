/*
 * Salsa64 — pruebas del PIF (comandos joybus de los controles por el SI) y
 * del mapeo del DualShock 3 (src/frontend/padmap.c).
 */
#include "test.h"

#include <string.h>

#include "frontend/padmap.h"

#define SI_DRAM_ADDR      0x04800000u
#define SI_PIF_RD64B      0x04800004u
#define SI_PIF_WR64B      0x04800010u
#define SI_STATUS         0x04800018u
#define MI_INTR           0x04300008u
#define PIF_RAM           0x1FC007C0u
#define CMD               63

static struct salsa64_bus *bus(void)
{
	return &t_n64.bus;
}

static uint8_t *ram(void)
{
	return t_n64.bus.pif_ram;
}

/* Copia `n` bytes al inicio de la PIF RAM (lo demás en 0) con el byte de
 * comandos dado, como lo dejaría una escritura del CPU. */
static void pif_load(const uint8_t *bytes, unsigned n, uint8_t cmd)
{
	memset(ram(), 0, SALSA64_PIF_RAM_SIZE);
	memcpy(ram(), bytes, n);
	ram()[CMD] = cmd;
	salsa64_pif_written(bus());
}

static void pad(unsigned port, uint16_t buttons, int8_t x, int8_t y)
{
	struct salsa64_pad p = { buttons, x, y, 1 };
	salsa64_pif_set_pad(bus(), port, &p);
}

static void unplug(unsigned port)
{
	struct salsa64_pad p = { 0, 0, 0, 0 };
	salsa64_pif_set_pad(bus(), port, &p);
}

/* ---- PIF ----------------------------------------------------------------- */

/* Formato de libultra: 0xFF de relleno, un intercambio por canal y 0xFE. */
static void pif_info_and_state(void)
{
	static const uint8_t cmds[] = {
		0xFF, 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF,        /* canal 0: identificación */
		0xFF, 0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,  /* canal 1: estado */
		0xFF, 0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,  /* canal 2: estado, desconectado */
		0xFE,
	};
	pad(0, 0, 0, 0);
	pad(1, SALSA64_PAD_A | SALSA64_PAD_C_DOWN, 40, -12);
	unplug(2);
	pif_load(cmds, sizeof cmds, 0x01);
	CHECK_EQ(ram()[CMD], 0x00);  /* el PIF apaga el bit al preparar */

	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[2], 0x03);
	CHECK_EQ(ram()[4], 0x05);
	CHECK_EQ(ram()[5], 0x00);
	CHECK_EQ(ram()[6], 0x02);  /* sin accesorio */
	CHECK_EQ(ram()[9], 0x04);
	CHECK_EQ(ram()[11], 0x80);
	CHECK_EQ(ram()[12], 0x04);
	CHECK_EQ(ram()[13], 40);
	CHECK_EQ(ram()[14], 0xF4);  /* -12 */
	CHECK_EQ(ram()[17], 0x84);  /* no hay dispositivo */
	CHECK_EQ(ram()[19], 0xFF);  /* sin respuesta: el espacio queda igual */

	/* Cada lectura vuelve a ejecutar los intercambios preparados. */
	pad(1, SALSA64_PAD_START, -80, 80);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[11], 0x10);
	CHECK_EQ(ram()[12], 0x00);
	CHECK_EQ(ram()[13], 0xB0);
	CHECK_EQ(ram()[14], 80);
}

/* Todo el camino: RDRAM → PIF por DMA, ejecución y PIF → RDRAM. */
static void pif_through_si_dma(void)
{
	static const uint8_t cmds[] = { 0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE };
	for (unsigned k = 0; k < 64; k += 4) {
		uint32_t w = 0;
		for (unsigned b = 0; b < 4; b++)
			w = w << 8 | (k + b < sizeof cmds ? cmds[k + b] : 0);
		t_wr32(0x3000 + k, k == 60 ? 0x00000001u : w);
	}
	pad(0, SALSA64_PAD_Z | SALSA64_PAD_R, 5, 6);

	salsa64_bus_write32(bus(), SI_DRAM_ADDR, 0x3000, 0xFFFFFFFFu);
	salsa64_bus_write32(bus(), SI_PIF_WR64B, PIF_RAM, 0xFFFFFFFFu);
	CHECK(salsa64_bus_read32(bus(), SI_STATUS) & SALSA64_SI_STATUS_INTR);
	CHECK((salsa64_bus_read32(bus(), MI_INTR) >> SALSA64_IRQ_SI) & 1);
	salsa64_bus_write32(bus(), SI_STATUS, 0, 0xFFFFFFFFu);
	CHECK_EQ(salsa64_bus_read32(bus(), SI_STATUS) & SALSA64_SI_STATUS_INTR, 0);

	salsa64_bus_write32(bus(), SI_DRAM_ADDR, 0x3100, 0xFFFFFFFFu);
	salsa64_bus_write32(bus(), SI_PIF_RD64B, PIF_RAM, 0xFFFFFFFFu);
	CHECK_EQ(t_rd32(0x3100), 0x01040120u);  /* TX RX cmd botones[15:8] */
	CHECK_EQ(t_rd32(0x3104), 0x100506FEu);  /* botones[7:0] X Y fin */
	CHECK_EQ(t_rd32(0x313C), 0);            /* byte de comandos ya en 0 */
	CHECK((salsa64_bus_read32(bus(), MI_INTR) >> SALSA64_IRQ_SI) & 1);
}

/* Una escritura directa del CPU a la PIF RAM también prepara los comandos. */
static void pif_direct_write(void)
{
	static const uint8_t cmds[] = { 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE };
	pif_load(cmds, sizeof cmds, 0x00);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0xFF);  /* sin el bit 0x01 no hay nada preparado */

	salsa64_bus_write32(bus(), PIF_RAM + 60, 0x00000001u, 0xFFFFFFFFu);
	CHECK_EQ(ram()[CMD], 0x00);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x05);
	CHECK_EQ(salsa64_bus_read32(bus(), PIF_RAM), 0x01030005u);
}

static void pif_escapes(void)
{
	static const uint8_t cmds[] = {
		0x00,                                      /* canal 0: saltar */
		0xFF, 0xFF,                                /* nada */
		0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF,        /* canal 1: identificación */
		0x81, 0x03, 0x00, 0xFF, 0xFF, 0xFF,        /* canal 2: TX con bit de saltar */
		0xFE,                                      /* fin */
		0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF,        /* después del fin: no cuenta */
	};
	pad(0, 0, 0, 0);
	pad(1, 0, 0, 0);
	pad(2, 0, 0, 0);
	pad(3, 0, 0, 0);
	pif_load(cmds, sizeof cmds, 0x01);
	salsa64_pif_execute(bus());
	CHECK_EQ(bus()->pif.handshake[0], -1);
	CHECK_EQ(bus()->pif.handshake[1], 3);
	CHECK_EQ(bus()->pif.handshake[2], 9);
	CHECK_EQ(bus()->pif.handshake[3], -1);
	CHECK_EQ(ram()[6], 0x05);
	CHECK_EQ(ram()[12], 0xFF);  /* saltado */
	CHECK_EQ(ram()[10], 0x03);
	CHECK_EQ(ram()[19], 0xFF);  /* después del fin */
}

/* L + R + Start: el control reporta RST, Start apagado y el stick en 0. */
static void pif_reset_combo(void)
{
	static const uint8_t cmds[] = { 0x01, 0x04, 0x01, 0, 0, 0, 0, 0xFE };
	pad(0, SALSA64_PAD_L | SALSA64_PAD_R | SALSA64_PAD_START | SALSA64_PAD_A, 50, 60);
	pif_load(cmds, sizeof cmds, 0x01);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x80);  /* A */
	CHECK_EQ(ram()[4], 0xB0);  /* RST, L, R */
	CHECK_EQ(ram()[5], 0);
	CHECK_EQ(ram()[6], 0);
	/* Sin el combo, RST nunca sale aunque lo pida el frontend. */
	pad(0, SALSA64_PAD_RST | SALSA64_PAD_START, 1, 2);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x10);
	CHECK_EQ(ram()[4], 0x00);
}

/* Una pulsación que se suelta antes de que el juego lea el control sale
 * pulsada en la lectura siguiente, y solo en esa; la identificación no la
 * gasta. Lo entregado queda en sent/reads para el log del frontend. */
static void pif_held_press(void)
{
	static const uint8_t state[] = { 0x01, 0x04, 0x01, 0, 0, 0, 0, 0xFE };
	static const uint8_t info[] = { 0x01, 0x03, 0x00, 0, 0, 0, 0xFE };
	uint32_t reads = bus()->pif.reads[0];
	pad(0, SALSA64_PAD_R, 10, 0);
	pad(0, 0, 0, 0);  /* soltado entre dos lecturas del juego */
	pif_load(info, sizeof info, 0x01);
	salsa64_pif_execute(bus());
	CHECK_EQ(bus()->pif.reads[0], reads);
	pif_load(state, sizeof state, 0x01);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x00);
	CHECK_EQ(ram()[4], 0x10);  /* R, retenido */
	CHECK_EQ(ram()[5], 0);     /* el stick no se retiene */
	CHECK_EQ(bus()->pif.sent[0].buttons, SALSA64_PAD_R);
	CHECK_EQ(bus()->pif.sent[0].x, 0);
	CHECK_EQ(bus()->pif.reads[0], reads + 1);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[4], 0x00);  /* ya entregado: suelto */
	CHECK_EQ(bus()->pif.sent[0].buttons, 0);
	CHECK_EQ(bus()->pif.reads[0], reads + 2);

	/* Mantenido: sale en cada lectura; suelto después de entregarse, no. */
	pad(0, SALSA64_PAD_A, 0, 0);
	salsa64_pif_execute(bus());
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x80);
	pad(0, 0, 0, 0);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x00);

	/* Solo el flanco: ya entregado y mantenido unos campos sin lecturas,
	 * si se suelta antes de la siguiente sale suelto (no se come la
	 * soltada entre dos pulsaciones). */
	pad(0, SALSA64_PAD_A, 0, 0);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x80);
	pad(0, SALSA64_PAD_A, 0, 0);
	pad(0, SALSA64_PAD_A, 0, 0);
	pad(0, 0, 0, 0);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x00);
	pad(0, SALSA64_PAD_A, 0, 0);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x80);
	pad(0, 0, 0, 0);

	/* Desconectar o reiniciar la consola olvida lo retenido. */
	pad(0, SALSA64_PAD_B, 0, 0);
	unplug(0);
	pad(0, 0, 0, 0);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x00);
	pad(0, SALSA64_PAD_B, 0, 0);
	pad(0, 0, 0, 0);
	salsa64_n64_reset(&t_n64);
	pif_load(state, sizeof state, 0x01);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[3], 0x00);
}

/* Sin Controller Pak: la lectura da ceros y CRC 0xFF; la escritura, el CRC
 * de los datos complementado. */
static void pif_no_pak(void)
{
	uint8_t cmds[64];
	memset(cmds, 0xFF, sizeof cmds);
	cmds[0] = 0x03;  /* lectura: TX 3, RX 33 */
	cmds[1] = 0x21;
	cmds[2] = 0x02;
	cmds[3] = 0x80;
	cmds[4] = 0x01;
	cmds[3 + 2 + 33] = 0xFE;
	pad(0, 0, 0, 0);
	pif_load(cmds, 39, 0x01);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[1], 0x21);
	for (unsigned k = 0; k < 32; k++)
		CHECK_EQ(ram()[5 + k], 0);
	CHECK_EQ(ram()[37], 0xFF);

	memset(cmds, 0, sizeof cmds);
	cmds[0] = 0x23;  /* escritura: TX 35, RX 1 */
	cmds[1] = 0x01;
	cmds[2] = 0x03;
	cmds[3] = 0xC0;
	cmds[4] = 0x1B;
	for (unsigned k = 0; k < 32; k++)
		cmds[5 + k] = (uint8_t)k;
	cmds[37] = 0xAA;  /* respuesta */
	cmds[38] = 0xFE;
	pif_load(cmds, 39, 0x01);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[37], (uint8_t)~0x33);  /* CRC-8 (0x85) de 0..31 = 0x33 */
	CHECK_EQ(ram()[1], 0x01);
}

static void pif_crc8(void)
{
	uint8_t d[32];
	memset(d, 0, sizeof d);
	CHECK_EQ(salsa64_joybus_crc8(d, 32), 0x00);
	memset(d, 0xFF, sizeof d);
	CHECK_EQ(salsa64_joybus_crc8(d, 32), 0x0A);
	for (unsigned k = 0; k < 32; k++)
		d[k] = (uint8_t)k;
	CHECK_EQ(salsa64_joybus_crc8(d, 32), 0x33);
}

/* Respuesta más corta que lo pedido: bit 0x40; más larga: se recorta. */
static void pif_rx_length(void)
{
	static const uint8_t cmds[] = {
		0x01, 0x06, 0x01, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,  /* canal 0: pide 6, llegan 4 */
		0x01, 0x02, 0x01, 0xEE, 0xEE,                          /* canal 1: pide 2 de 4 */
		0x00, 0x00,                                            /* canales 2 y 3 */
		0x01, 0x03, 0x00, 0xEE, 0xEE, 0xEE,                    /* canal 4: cartucho */
		0xFE,
	};
	pad(0, SALSA64_PAD_B, 1, 2);
	pad(1, SALSA64_PAD_B, 3, 4);
	pif_load(cmds, sizeof cmds, 0x01);
	salsa64_pif_execute(bus());
	CHECK_EQ(ram()[1], 0x46);
	CHECK_EQ(ram()[3], 0x40);
	CHECK_EQ(ram()[6], 0x02);
	CHECK_EQ(ram()[7], 0xEE);
	CHECK_EQ(ram()[10], 0x02);
	CHECK_EQ(ram()[12], 0x40);
	CHECK_EQ(ram()[13], 0x00);
	CHECK_EQ(ram()[14], 0x00);  /* lo que sigue queda igual */
	CHECK_EQ(bus()->pif.handshake[4], 16);
	CHECK_EQ(ram()[17], 0x83);  /* sin EEPROM todavía */
	CHECK_EQ(ram()[19], 0xEE);
}

/* El reset de la consola olvida los intercambios, no los controles. */
static void pif_reset_keeps_pads(void)
{
	static const uint8_t cmds[] = { 0x01, 0x03, 0x00, 0, 0, 0, 0xFE };
	pad(3, SALSA64_PAD_A, 0, 0);
	pif_load(cmds, sizeof cmds, 0x01);
	CHECK(bus()->pif.parsed);
	salsa64_n64_reset(&t_n64);
	CHECK(!bus()->pif.parsed);
	CHECK_EQ(bus()->pif.handshake[0], -1);
	CHECK(bus()->pif.pads[3].connected);
	CHECK_EQ(bus()->pif.pads[3].buttons, SALSA64_PAD_A);
}

/* ---- Mapeo del DualShock 3 --------------------------------------------- */

static struct salsa64_ds3 ds3(uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
{
	struct salsa64_ds3 s = { buttons, lx, ly, rx, ry };
	return s;
}

#define DS3(b) (1u << SALSA64_DS3_##b)

static void padmap_buttons(void)
{
	struct salsa64_padmap m;
	struct salsa64_pad p;
	salsa64_padmap_default(&m);
	struct salsa64_ds3 in = ds3(DS3(CROSS) | DS3(SQUARE) | DS3(L2) | DS3(START) | DS3(L1) |
				    DS3(R1) | DS3(UP) | DS3(RIGHT), 128, 128, 128, 128);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK(p.connected);
	CHECK_EQ(p.buttons, SALSA64_PAD_A | SALSA64_PAD_B | SALSA64_PAD_Z | SALSA64_PAD_START |
			    SALSA64_PAD_L | SALSA64_PAD_R | SALSA64_PAD_D_UP | SALSA64_PAD_D_RIGHT);
	CHECK_EQ(p.x, 0);
	CHECK_EQ(p.y, 0);
	/* Lo que no está asignado no hace nada. */
	in = ds3(DS3(CIRCLE) | DS3(TRIANGLE) | DS3(R2) | DS3(SELECT) | DS3(L3) | DS3(R3), 128, 128, 128, 128);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.buttons, 0);
}

static void padmap_sticks(void)
{
	struct salsa64_padmap m;
	struct salsa64_pad p;
	struct salsa64_ds3 in;
	salsa64_padmap_default(&m);

	in = ds3(0, 255, 0, 128, 128);  /* derecha y arriba a fondo */
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.x, 80);
	CHECK_EQ(p.y, 80);
	in = ds3(0, 0, 255, 128, 128);  /* izquierda y abajo */
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.x, -80);
	CHECK_EQ(p.y, -80);
	in = ds3(0, 128 + 12, 128 - 12, 128, 128);  /* dentro de la zona muerta */
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.x, 0);
	CHECK_EQ(p.y, 0);
	in = ds3(0, 128 + 13, 128 + 70, 128, 128);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.x, 1);    /* (1 × 80 + 57) / 115 */
	CHECK_EQ(p.y, -40);  /* (58 × 80 + 57) / 115 */

	/* Stick derecho → botones C, con umbral estricto. */
	in = ds3(0, 128, 128, 128 + 64, 128 - 64);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.buttons, 0);
	in = ds3(0, 128, 128, 128 + 65, 128 - 65);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.buttons, SALSA64_PAD_C_RIGHT | SALSA64_PAD_C_UP);
	in = ds3(0, 128, 128, 0, 255);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.buttons, SALSA64_PAD_C_LEFT | SALSA64_PAD_C_DOWN);

	/* Rango y zona muerta configurados. */
	m.range = 127;
	m.deadzone = 0;
	in = ds3(0, 255, 0, 128, 128);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.x, 127);
	CHECK_EQ(p.y, 127);
	in = ds3(0, 129, 127, 128, 128);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.x, 1);
	CHECK_EQ(p.y, 1);
}

static void padmap_config(void)
{
	static const char text[] =
		"# controles de prueba\n"
		"Z = R2\n"
		"  c_abajo   =  circulo   # comentario\r\n"
		"\n"
		"Start=Select\n"
		"B = nada\n"
		"zona_muerta = 0\n"
		"rango = 127\n"
		"umbral_c = 200\n"
		"turbo = CRUZ\n"
		"A = EQUIS\n"
		"umbral_c = 30";
	struct salsa64_padmap m;
	struct salsa64_pad p;
	char err[96];
	salsa64_padmap_default(&m);
	CHECK_EQ(salsa64_padmap_parse(&m, text, sizeof text - 1, err, sizeof err), 3);
	CHECK(strcmp(err, "línea 9: \"umbral_c = 200\"") == 0);
	CHECK_EQ(m.source[SALSA64_IN_Z], SALSA64_DS3_R2);
	CHECK_EQ(m.source[SALSA64_IN_C_DOWN], SALSA64_DS3_CIRCLE);
	CHECK_EQ(m.source[SALSA64_IN_START], SALSA64_DS3_SELECT);
	CHECK_EQ(m.source[SALSA64_IN_B], SALSA64_DS3_NONE);
	CHECK_EQ(m.source[SALSA64_IN_A], SALSA64_DS3_CROSS);  /* la línea mala no lo cambió */
	CHECK_EQ(m.deadzone, 0);
	CHECK_EQ(m.range, 127);
	CHECK_EQ(m.c_threshold, 30);

	struct salsa64_ds3 in = ds3(DS3(R2) | DS3(CIRCLE) | DS3(SELECT) | DS3(SQUARE) | DS3(L2),
				    128, 128, 128, 128);
	salsa64_padmap_apply(&m, &in, &p);
	CHECK_EQ(p.buttons, SALSA64_PAD_Z | SALSA64_PAD_C_DOWN | SALSA64_PAD_START);
}

static void padmap_describe(void)
{
	struct salsa64_pad p = { SALSA64_PAD_A | SALSA64_PAD_Z | SALSA64_PAD_C_DOWN, 40, -12, 1 };
	char buf[64];
	salsa64_pad_describe(&p, buf, sizeof buf);
	CHECK(strcmp(buf, "A Z C_ABAJO stick=(40,-12)") == 0);
	p.buttons = 0;
	salsa64_pad_describe(&p, buf, 12);
	CHECK(strcmp(buf, "stick=(40,-") == 0);
}

void t_suite_pif(void)
{
	RUN(pif_info_and_state);
	RUN(pif_through_si_dma);
	RUN(pif_direct_write);
	RUN(pif_escapes);
	RUN(pif_reset_combo);
	RUN(pif_held_press);
	RUN(pif_no_pak);
	RUN(pif_crc8);
	RUN(pif_rx_length);
	RUN(pif_reset_keeps_pads);
	RUN(padmap_buttons);
	RUN(padmap_sticks);
	RUN(padmap_config);
	RUN(padmap_describe);
}
