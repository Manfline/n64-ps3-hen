/*
 * Salsa64 — pruebas del Controller Pak (src/core/mempak.c y el accesorio de
 * los controles en pif.c): verificación de la dirección, CRC de los datos,
 * formato inicial, leer y escribir por el PIF, y el archivo .mpk.
 */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/mempak.h"
#include "frontend/mempakfile.h"

#define CMD 63

static struct salsa64_mempak mp;
static struct mempakfile mf;
static uint8_t rom[0x40] = { [0x20] = 'T', 'E', 'S', 'T', [0x10] = 0x12, 0x34, 0x56, 0x78 };

static struct salsa64_bus *bus(void)
{
	return &t_n64.bus;
}

static uint8_t *ram(void)
{
	return t_n64.bus.pif_ram;
}

static bool exists(const char *path)
{
	return access(path, F_OK) == 0;
}

static unsigned log_lines;
static char log_last[256];

static void quiet_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(log_last, sizeof log_last, fmt, ap);
	va_end(ap);
	log_lines++;
}

static void connect(unsigned port, bool on)
{
	struct salsa64_pad p = { 0, 0, 0, on };
	salsa64_pif_set_pad(bus(), port, &p);
}

/* Un intercambio en el canal `port` (los anteriores saltados con 0x00).
 * Devuelve la posición del byte RX; el byte de longitud de RX, con sus bits
 * de error, queda en ram()[pos - 1]. */
static unsigned exchange(unsigned port, const uint8_t *tx, unsigned txlen, unsigned rxlen)
{
	unsigned at = port;
	memset(ram(), 0, SALSA64_PIF_RAM_SIZE);
	ram()[at] = (uint8_t)txlen;
	ram()[at + 1] = (uint8_t)rxlen;
	memcpy(ram() + at + 2, tx, txlen);
	memset(ram() + at + 2 + txlen, 0xFF, rxlen);
	ram()[at + 2 + txlen + rxlen] = 0xFE;
	ram()[CMD] = 0x01;
	salsa64_pif_written(bus());
	salsa64_pif_execute(bus());
	return at + 2 + txlen;
}

static void put_addr(uint8_t *tx, uint16_t addr)
{
	uint16_t raw = (uint16_t)((addr & 0xFFE0) | salsa64_mempak_addr_crc(addr));
	tx[1] = (uint8_t)(raw >> 8);
	tx[2] = (uint8_t)raw;
}

/* Lee 32 bytes; devuelve el CRC que contestó el pak. */
static uint8_t pak_read(unsigned port, uint16_t addr, uint8_t out[32])
{
	uint8_t tx[3] = { 0x02 };
	put_addr(tx, addr);
	unsigned rx = exchange(port, tx, 3, 33);
	CHECK_EQ(ram()[port + 1], 33);  /* sin bits de error */
	memcpy(out, ram() + rx, 32);
	return ram()[rx + 32];
}

/* Escribe 32 bytes; devuelve el CRC que contestó el pak. */
static uint8_t pak_write(unsigned port, uint16_t addr, const uint8_t data[32])
{
	uint8_t tx[35] = { 0x03 };
	put_addr(tx, addr);
	memcpy(tx + 3, data, 32);
	unsigned rx = exchange(port, tx, 35, 1);
	CHECK_EQ(ram()[port + 1], 1);
	return ram()[rx];
}

static uint8_t pad_status(unsigned port)
{
	static const uint8_t info[] = { 0x00 };
	unsigned rx = exchange(port, info, 1, 3);
	CHECK_EQ(ram()[rx], 0x05);
	return ram()[rx + 2];
}

/* El Controller Pak conectado a los cuatro controles para la prueba. */
static void plug(void)
{
	salsa64_mempak_init(&mp);
	for (unsigned p = 0; p < 4; p++) {
		mp.present[p] = 1;
		connect(p, true);
	}
	bus()->mempak = &mp;
}

static void unplug(void)
{
	bus()->mempak = NULL;
	for (unsigned p = 0; p < 4; p++)
		connect(p, false);
}

/* ---- Verificaciones ------------------------------------------------------- */

static void addr_crc(void)
{
	/* El valor de cada bit de la dirección, del 15 al 5. */
	static const uint8_t bit[11] = { 0x01, 0x1A, 0x0D, 0x1C, 0x0E, 0x07, 0x19, 0x16, 0x0B, 0x1F, 0x15 };
	for (unsigned i = 0; i < 11; i++)
		CHECK_EQ(salsa64_mempak_addr_crc((uint16_t)(0x8000u >> i)), bit[i]);
	CHECK_EQ(salsa64_mempak_addr_crc(0x0000), 0x00);
	CHECK_EQ(salsa64_mempak_addr_crc(0x8000), 0x01);  /* 0x8001: el registro del Rumble Pak */
	CHECK_EQ(salsa64_mempak_addr_crc(0x0020), 0x15);
	CHECK_EQ(salsa64_mempak_addr_crc(0xC000), 0x01 ^ 0x1A);
	CHECK_EQ(salsa64_mempak_addr_crc(0xFFE0), 0x01 ^ 0x1A ^ 0x0D ^ 0x1C ^ 0x0E ^ 0x07 ^ 0x19 ^ 0x16 ^ 0x0B ^ 0x1F ^ 0x15);
	/* Los 5 bits bajos de la entrada no cuentan. */
	CHECK_EQ(salsa64_mempak_addr_crc(0x801F), 0x01);
	CHECK(salsa64_mempak_addr_ok(0x8001));
	CHECK(salsa64_mempak_addr_ok(0x0000));
	CHECK(!salsa64_mempak_addr_ok(0x8000));
	CHECK(!salsa64_mempak_addr_ok(0x0020));
	CHECK(salsa64_mempak_addr_ok(0x0035));
}

static void data_crc(void)
{
	uint8_t d[32];
	for (unsigned i = 0; i < 32; i++)
		d[i] = (uint8_t)i;
	CHECK_EQ(salsa64_joybus_crc8(d, 32), 0x33);  /* calculado aparte con el polinomio 0x85 */
	memset(d, 0xFF, 32);
	CHECK_EQ(salsa64_joybus_crc8(d, 32), 0x0A);
	memset(d, 0, 32);
	CHECK_EQ(salsa64_joybus_crc8(d, 32), 0x00);
	d[0] = 1;
	CHECK_EQ(salsa64_joybus_crc8(d, 32), 0x04);
}

/* ---- Formato ----------------------------------------------------------------- */

static void format_initial(void)
{
	static uint8_t pak[SALSA64_MEMPAK_SIZE];
	memset(pak, 0xAA, sizeof pak);
	CHECK(!salsa64_mempak_valid(pak));
	salsa64_mempak_format(pak, 7);
	CHECK(salsa64_mempak_valid(pak));

	/* El ID: cuatro copias iguales, con sus sumas. */
	static const unsigned at[4] = { 0x20, 0x60, 0x80, 0xC0 };
	for (unsigned k = 0; k < 4; k++) {
		const uint8_t *id = pak + at[k];
		CHECK(memcmp(id, pak + 0x20, 32) == 0);
		unsigned sum = 0;
		for (unsigned i = 0; i < 28; i += 2)
			sum += (unsigned)(id[i] << 8 | id[i + 1]);
		sum &= 0xFFFF;
		CHECK_EQ(id[0x1C] << 8 | id[0x1D], sum);
		CHECK_EQ(id[0x1E] << 8 | id[0x1F], (0xFFF2u - sum) & 0xFFFF);
		CHECK_EQ(id[0x19], 0x01);  /* Controller Pak */
		CHECK_EQ(id[0x1A], 0x01);  /* un banco */
		CHECK_EQ(id[0x18], 0x00);
		CHECK_EQ(id[0x1B], 0x00);
	}
	/* Lo que no es ID en la página 0 sigue en cero. */
	for (unsigned i = 0; i < 0x20; i++)
		CHECK_EQ(pak[i], 0);
	for (unsigned i = 0x40; i < 0x60; i++)
		CHECK_EQ(pak[i], 0);

	/* Inodos: páginas 0 a 4 reservadas, el resto libres (3); suma 0x71. */
	const uint8_t *fat = pak + 0x100;
	CHECK_EQ(fat[0], 0);
	CHECK_EQ(fat[1], 0x71);
	for (unsigned p = 1; p < 5; p++)
		CHECK_EQ(fat[2 * p] << 8 | fat[2 * p + 1], 0);
	for (unsigned p = 5; p < 128; p++)
		CHECK_EQ(fat[2 * p] << 8 | fat[2 * p + 1], 3);
	CHECK(memcmp(pak + 0x100, pak + 0x200, 0x100) == 0);  /* copia en la página 2 */
	/* Notas en cero y páginas de datos en cero. */
	for (unsigned i = 0x300; i < SALSA64_MEMPAK_SIZE; i++)
		CHECK_EQ(pak[i], 0);

	/* Dos series distintas para dos paks; la misma para la misma semilla. */
	static uint8_t other[SALSA64_MEMPAK_SIZE];
	salsa64_mempak_format(other, 8);
	CHECK(memcmp(other + 0x20, pak + 0x20, 24) != 0);
	salsa64_mempak_format(other, 7);
	CHECK(memcmp(other, pak, sizeof pak) == 0);

	/* Una sola suma rota basta para que no sea válido. */
	pak[0x1A0] ^= 1;
	CHECK(!salsa64_mempak_valid(pak));  /* un inodo cambiado sin actualizar la suma */
	salsa64_mempak_format(pak, 7);
	pak[0x2A0] = 9;  /* tabla de respaldo con otra cosa */
	CHECK(!salsa64_mempak_valid(pak));
	salsa64_mempak_format(pak, 7);
	pak[0x65] ^= 1;
	CHECK(!salsa64_mempak_valid(pak));  /* una copia del ID rota */
}

/* ---- Por el PIF ---------------------------------------------------------------- */

static void status_bits(void)
{
	plug();
	CHECK_EQ(pad_status(0), 0x01);  /* hay pak */
	mp.present[2] = 0;
	CHECK_EQ(pad_status(2), 0x02);  /* sin pak */
	CHECK_EQ(pad_status(1), 0x01);
	bus()->mempak = NULL;
	CHECK_EQ(pad_status(0), 0x02);  /* sin Controller Pak en el bus */
	unplug();
}

static void read_formatted(void)
{
	uint8_t got[32];
	plug();
	/* Un pak nuevo se lee ya formateado: el ID en 0x20 y la tabla en 0x100. */
	uint8_t crc = pak_read(0, 0x0020, got);
	CHECK(memcmp(got, mp.data[0] + 0x20, 32) == 0);
	CHECK_EQ(crc, salsa64_joybus_crc8(got, 32));
	pak_read(0, 0x0100, got);
	CHECK_EQ(got[1], 0x71);
	/* Cada control tiene el suyo. */
	pak_read(3, 0x0020, got);
	CHECK(memcmp(got, mp.data[3] + 0x20, 32) == 0);
	CHECK(memcmp(mp.data[0] + 0x20, mp.data[3] + 0x20, 24) != 0);
	unplug();
}

static void write_read(void)
{
	uint8_t a[32], b[32], got[32];
	for (unsigned i = 0; i < 32; i++) {
		a[i] = (uint8_t)(i * 7 + 1);
		b[i] = (uint8_t)~a[i];
	}
	plug();
	CHECK_EQ(pak_write(1, 0x1240, a), salsa64_joybus_crc8(a, 32));
	CHECK_EQ(mp.writes, 1);
	CHECK(memcmp(mp.data[1] + 0x1240, a, 32) == 0);
	CHECK_EQ(pak_read(1, 0x1240, got), salsa64_joybus_crc8(a, 32));
	CHECK(memcmp(got, a, 32) == 0);
	/* No tocó a los demás ni a los bloques de al lado. */
	pak_read(0, 0x1240, got);
	CHECK(memcmp(got, a, 32) != 0);
	pak_read(1, 0x1220, got);
	for (unsigned i = 0; i < 32; i++)
		CHECK_EQ(got[i], 0);
	/* Escribir lo mismo no ensucia; escribir otra cosa sí. */
	pak_write(1, 0x1240, a);
	CHECK_EQ(mp.writes, 1);
	pak_write(1, 0x1240, b);
	CHECK_EQ(mp.writes, 2);
	/* Primer y último bloque. */
	pak_write(0, 0x0000, a);
	pak_write(0, 0x7FE0, b);
	pak_read(0, 0x7FE0, got);
	CHECK(memcmp(got, b, 32) == 0);
	CHECK(memcmp(mp.data[0], a, 32) == 0);
	CHECK(memcmp(mp.data[0] + 0x7FE0, b, 32) == 0);
	unplug();
}

static void bad_address(void)
{
	uint8_t a[32], got[32], tx[35] = { 0x03, 0x12, 0x40 };  /* la verificación de 0x1240 es otra */
	memset(a, 0x5A, 32);
	plug();
	CHECK(!salsa64_mempak_addr_ok(0x1240));
	memcpy(tx + 3, a, 32);
	unsigned rx = exchange(0, tx, 35, 1);
	CHECK_EQ(ram()[rx], (uint8_t)~salsa64_joybus_crc8(a, 32));  /* como un pak ausente */
	CHECK_EQ(mp.writes, 0);
	CHECK_EQ(pad_status(0), 0x01 | 0x04);  /* dirección mal verificada */

	tx[0] = 0x02;
	rx = exchange(0, tx, 3, 33);
	for (unsigned i = 0; i < 32; i++)
		CHECK_EQ(ram()[rx + i], 0);
	CHECK_EQ(ram()[rx + 32], 0xFF);
	/* Un comando bueno quita el aviso. */
	pak_read(0, 0x0020, got);
	CHECK_EQ(pad_status(0), 0x01);
	unplug();
}

static void high_addresses(void)
{
	uint8_t a[32], got[32];
	memset(a, 0x80, 32);
	plug();
	/* Desde 0x8000 el Controller Pak no tiene registros: 0 y nada se guarda
	 * (un Rumble Pak devolvería 0x80 al sondear). */
	CHECK_EQ(pak_write(0, 0x8000, a), salsa64_joybus_crc8(a, 32));
	CHECK_EQ(mp.writes, 0);
	pak_read(0, 0x8000, got);
	for (unsigned i = 0; i < 32; i++)
		CHECK_EQ(got[i], 0);
	pak_read(0, 0xFFE0, got);
	for (unsigned i = 0; i < 32; i++)
		CHECK_EQ(got[i], 0);
	unplug();
}

static void absent(void)
{
	uint8_t got[32], a[32];
	memset(a, 0x11, 32);
	/* Control sin pak: ceros con CRC 0xFF, y la escritura contesta el CRC complementado. */
	salsa64_mempak_init(&mp);
	connect(0, true);
	bus()->mempak = &mp;
	CHECK_EQ(pak_read(0, 0x0020, got), 0xFF);
	for (unsigned i = 0; i < 32; i++)
		CHECK_EQ(got[i], 0);
	CHECK_EQ(pak_write(0, 0x0020, a), (uint8_t)~salsa64_joybus_crc8(a, 32));
	CHECK_EQ(mp.writes, 0);
	bus()->mempak = NULL;
	CHECK_EQ(pak_read(0, 0x0020, got), 0xFF);
	/* Control desconectado: nadie contesta. */
	connect(1, false);
	uint8_t tx[3] = { 0x02 };
	put_addr(tx, 0x20);
	exchange(1, tx, 3, 33);
	CHECK_EQ(ram()[2] & 0x80, 0x80);
	unplug();
}

/* ---- Archivo ----------------------------------------------------------------------- */

static void persistence(void)
{
	char dir[] = "/tmp/salsa64-mpk-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	uint8_t a[32], got[32];
	memset(a, 0xC3, 32);

	/* Sin archivo: formateado, y nada en el disco hasta que haya cambios. */
	mempakfile_open(&mf, &mp, bus(), rom, dir, 0xF, quiet_log);
	CHECK(bus()->mempak == &mp);
	for (unsigned p = 0; p < 4; p++) {
		CHECK(mp.present[p]);
		CHECK(salsa64_mempak_valid(mp.data[p]));
		connect(p, true);
	}
	char path[300];
	snprintf(path, sizeof path, "%s/TEST-12345678.mpk", dir);
	CHECK(!exists(path));
	mempakfile_poll(&mf, 5000000);
	CHECK(!exists(path));

	/* Un cambio del juego se escribe diferido, no en cada comando. */
	pak_write(2, 0x4000, a);
	mempakfile_poll(&mf, 10000000);
	CHECK(!exists(path));
	mempakfile_poll(&mf, 12500000);  /* 2 s sin escrituras */
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f) {
		static uint8_t buf[SALSA64_MEMPAK_TOTAL + 1];
		CHECK_EQ(fread(buf, 1, sizeof buf, f), SALSA64_MEMPAK_TOTAL);
		fclose(f);
		/* Orden del .mpk de mupen64plus: el control 3 en 0x10000. */
		CHECK(memcmp(buf + 2 * 32768 + 0x4000, a, 32) == 0);
		CHECK(memcmp(buf + 0 * 32768 + 0x4000, a, 32) != 0);
	}

	/* Otro cambio, y al cerrar se escribe sin esperar. */
	uint8_t b[32];
	memset(b, 0x3C, 32);
	pak_write(0, 0x0400, b);
	mempakfile_close(&mf);
	CHECK(bus()->mempak == NULL);
	unplug();

	/* Al reabrir (otra estructura limpia) están los dos cambios. */
	salsa64_mempak_init(&mp);
	mempakfile_open(&mf, &mp, bus(), rom, dir, 0xF, quiet_log);
	for (unsigned p = 0; p < 4; p++)
		connect(p, true);
	pak_read(2, 0x4000, got);
	CHECK(memcmp(got, a, 32) == 0);
	pak_read(0, 0x0400, got);
	CHECK(memcmp(got, b, 32) == 0);
	CHECK(salsa64_mempak_valid(mp.data[1]));
	CHECK_EQ(mp.writes, 0);
	mempakfile_close(&mf);
	unplug();

	/* «Ninguno»: ni pak ni archivo nuevo. */
	remove(path);
	mempakfile_open(&mf, &mp, bus(), rom, dir, 0, quiet_log);
	CHECK(bus()->mempak == NULL);
	mempakfile_close(&mf);
	CHECK(!exists(path));

	/* Un archivo corto rellena el resto con paks formateados. */
	f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f) {
		fwrite(a, 1, 32, f);  /* ni siquiera un pak entero */
		fclose(f);
	}
	mempakfile_open(&mf, &mp, bus(), rom, dir, 0xF, quiet_log);
	CHECK(salsa64_mempak_valid(mp.data[3]));
	mempakfile_close(&mf);
	unplug();
	remove(path);
	char tmp[320];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	remove(tmp);
	rmdir(dir);
}

static void counters_and_log(void)
{
	uint8_t a[32], got[32], tx[35] = { 0x03, 0x12, 0x40 };  /* dirección mal verificada */
	memset(a, 0x77, 32);
	log_lines = 0;
	mempakfile_open(&mf, &mp, bus(), rom, NULL, 0x3, quiet_log);  /* controles 1 y 2 */
	CHECK_EQ(log_lines, 1);  /* «solo en memoria» */
	connect(0, true);
	connect(2, true);  /* conectado, pero sin pak */
	pad_status(0);
	pak_read(0, 0x0020, got);
	pak_write(0, 0x0040, a);
	pak_write(0, 0x0060, a);
	pak_read(2, 0x0020, got);  /* control sin pak */
	memcpy(tx + 3, a, 32);
	exchange(0, tx, 35, 1);
	CHECK_EQ(mp.cmd_status, 1);
	CHECK_EQ(mp.cmd_reads, 1);
	CHECK_EQ(mp.cmd_writes, 2);
	CHECK_EQ(mp.cmd_absent, 1);
	CHECK_EQ(mp.cmd_bad_addr, 1);
	CHECK_EQ(mp.last_bad_addr, 0x1240);
	CHECK_EQ(mp.writes, 2);

	/* El resumen sale cuando el tráfico se calma, una vez. */
	log_lines = 0;
	mempakfile_stage(&mf, 10000000);
	CHECK_EQ(log_lines, 0);
	mempakfile_stage(&mf, 11000000);
	CHECK_EQ(log_lines, 0);
	mempakfile_stage(&mf, 12500000);
	CHECK_EQ(log_lines, 1);
	CHECK(strstr(log_last, "1 consultas de estado, 1 lecturas, 2 escrituras (2 bloques cambiados)") != NULL);
	CHECK(strstr(log_last, "1 a un control sin pak, 1 con la dirección mal verificada (la última 0x1240)") != NULL);
	mempakfile_stage(&mf, 20000000);
	CHECK_EQ(log_lines, 1);
	pak_read(0, 0x0020, got);
	mempakfile_stage(&mf, 21000000);
	CHECK_EQ(log_lines, 1);
	mempakfile_close(&mf);  /* al cerrar, lo que falte */
	CHECK_EQ(log_lines, 2);
	CHECK(strstr(log_last, "2 lecturas") != NULL);
	unplug();
}

static void load_blank(void)
{
	static uint8_t file[SALSA64_MEMPAK_TOTAL];
	salsa64_mempak_init(&mp);
	memset(file, 0xFF, sizeof file);  /* archivo en blanco: se formatea cada pak */
	CHECK(salsa64_mempak_load(&mp, file, sizeof file));
	for (unsigned p = 0; p < 4; p++)
		CHECK(salsa64_mempak_valid(mp.data[p]));
	/* Un pak ya con datos no se toca, aunque no sea válido. */
	memset(file, 0, sizeof file);
	file[0x1234] = 0x77;
	CHECK(salsa64_mempak_load(&mp, file, sizeof file));
	CHECK_EQ(mp.data[0][0x1234], 0x77);
	CHECK(!salsa64_mempak_load(&mp, file, 100));
	CHECK(salsa64_mempak_valid(mp.data[1]));
}

static void flush_policy(void)
{
	const uint64_t s = 1000000;
	salsa64_mempak_init(&mp);
	CHECK(!salsa64_mempak_due(&mp, 0));
	mp.writes++;
	CHECK(!salsa64_mempak_due(&mp, 10 * s));
	CHECK(salsa64_mempak_dirty(&mp));
	CHECK(salsa64_mempak_due(&mp, 12 * s));
	salsa64_mempak_flushed(&mp);
	CHECK(!salsa64_mempak_dirty(&mp));
	uint64_t t = 100 * s;
	for (int k = 0; k < 30; k++, t += s) {
		mp.writes++;
		CHECK(!salsa64_mempak_due(&mp, t));
	}
	mp.writes++;
	CHECK(salsa64_mempak_due(&mp, t));
}

void t_suite_mempak(void)
{
	RUN(addr_crc);
	RUN(data_crc);
	RUN(format_initial);
	RUN(status_bits);
	RUN(read_formatted);
	RUN(write_read);
	RUN(bad_address);
	RUN(high_addresses);
	RUN(absent);
	RUN(persistence);
	RUN(counters_and_log);
	RUN(load_blank);
	RUN(flush_policy);
}
