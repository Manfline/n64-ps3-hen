/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — Controller Pak (memoria de 32 KB enchufada al control).
 *
 * Comportamiento (n64brew.dev: Joybus Protocol, Controller Pak y
 * Controller Pak/Filesystem; libdragon, documentación del accesorio;
 * consultadas el 2026-10-15). No deriva de código de otros emuladores.
 *
 * Joybus (pif.c): el comando 0x02 lee 32 bytes y el 0x03 escribe 32; la
 * petición trae 3 bytes, `cmd, dirección alta, dirección baja`. La
 * dirección es de 16 bits: los 11 bits altos son la dirección (múltiplo de
 * 32) y los 5 bajos su verificación (salsa64_mempak_addr_crc: no es un CRC
 * al uso, es el XOR de una tabla con un valor por cada bit de la dirección).
 * Los datos llevan CRC-8 (polinomio 0x85, semilla 0): la lectura contesta
 * 32 bytes y su CRC; la escritura contesta el CRC de lo recibido. Sin pak
 * (o con la dirección mal verificada) la lectura da 32 ceros y un CRC 0xFF
 * y la escritura el CRC de los datos complementado.
 * El espacio 0x0000–0x7FFF es la SRAM del pak; desde 0x8000 el Controller
 * Pak no tiene nada (los registros de ese rango son de otros accesorios,
 * como el Rumble Pak): se lee 0 y la escritura se ignora, que es lo que
 * hace que un juego lo distinga de un Rumble Pak.
 *
 * Sistema de archivos que el juego espera encontrar (páginas de 256 bytes,
 * 128 en total, todo en bytes sin orden de palabra):
 *   página 0   ID. Cuatro copias de 32 bytes en 0x20, 0x60, 0x80 y 0xC0:
 *                0x00 número de serie (24 bytes, único por pak)
 *                0x18 sin uso (0)      0x19 ID de dispositivo (bit 0 = 1)
 *                0x1A bancos de 32 KB (1)   0x1B sin uso (0)
 *                0x1C suma 1: suma de las 14 palabras de 16 bits (big-endian)
 *                     de 0x00 a 0x1B
 *                0x1E suma 2: 0xFFF2 menos la suma 1
 *   página 1   tabla de inodos: palabra 0 = 0x0000 con la suma de
 *              verificación en el byte bajo (suma de los bytes 0x02–0xFF
 *              módulo 256); la palabra n (big-endian) describe la página
 *              n: 0 reservada, 1 último de una cadena, 3 libre, ≥ 5 la
 *              página siguiente. Las páginas 0–4 son del sistema (0).
 *   página 2   copia exacta de la página 1.
 *   páginas 3–4 tabla de notas: 16 entradas de 32 bytes (código de juego,
 *              editor, página inicial, estado, extensión y nombre); un pak
 *              nuevo las trae en cero.
 *   páginas 5–127 datos de los juegos (123 páginas).
 * Un pak recién formateado tiene, pues, el ID con sus sumas, las 123
 * páginas libres (suma de la tabla 0x71 = 123 × 3 mod 256) y las notas en
 * cero. Quien guarda en el pak lo formatea él mismo si no encuentra un ID
 * válido, pero muchos juegos no lo hacen y esperan un pak ya formateado.
 *
 * Persistencia: la hace quien llama (frontend/mempakfile.c), con un archivo
 * ".mpk" por juego de 4 × 32 KB, el control 1 en 0x0000, el 2 en 0x8000, el 3
 * en 0x10000 y el 4 en 0x18000. Igual que el guardado del cartucho
 * (save.h), el núcleo solo cuenta las escrituras que cambian datos y dice
 * cuándo volcar; nada de esto asigna memoria.
 */
#ifndef SALSA64_CORE_MEMPAK_H
#define SALSA64_CORE_MEMPAK_H

#include "core/types.h"

#define SALSA64_MEMPAK_PORTS  4
#define SALSA64_MEMPAK_SIZE   (32u << 10)                         /* un pak */
#define SALSA64_MEMPAK_TOTAL  (SALSA64_MEMPAK_PORTS * SALSA64_MEMPAK_SIZE)
#define SALSA64_MEMPAK_BLOCK  32u                                 /* bytes por lectura o escritura */
#define SALSA64_MEMPAK_PAGE   256u
#define SALSA64_MEMPAK_QUIET_US     2000000u
#define SALSA64_MEMPAK_MAX_DELAY_US 30000000u

struct salsa64_mempak {
	uint8_t present[SALSA64_MEMPAK_PORTS];  /* hay un pak enchufado en ese control */
	uint32_t writes;                        /* escrituras del juego que cambiaron datos */
	uint32_t flushed;                       /* `writes` del último volcado */
	uint32_t seen;                          /* `writes` en la última consulta a _due */
	uint64_t seen_us;
	uint64_t dirty_us;
	uint8_t data[SALSA64_MEMPAK_PORTS][SALSA64_MEMPAK_SIZE] __attribute__((aligned(128)));
};

/* Los cuatro paks formateados y ninguno enchufado; contadores en cero. */
void salsa64_mempak_init(struct salsa64_mempak *mp);
/* Deja un pak (32 KB) recién formateado; `seed` varía el número de serie. */
void salsa64_mempak_format(uint8_t *pak, uint32_t seed);
/* El ID (sus cuatro copias) y las dos tablas de inodos son coherentes. */
bool salsa64_mempak_valid(const uint8_t *pak);
/* Copia un archivo ".mpk" (hasta 128 KB). Un pak que quede entero en 0x00 o
 * en 0xFF (archivo corto o vacío) se formatea. false si la longitud no es
 * la esperada. */
bool salsa64_mempak_load(struct salsa64_mempak *mp, const uint8_t *data, uint32_t len);

/* Verificación de 5 bits de una dirección (en los 5 bits bajos del valor
 * devuelto; la dirección se toma con sus 5 bits bajos en cero). */
uint8_t salsa64_mempak_addr_crc(uint16_t addr);
/* `raw` son los dos bytes de la petición (dirección y verificación). */
bool salsa64_mempak_addr_ok(uint16_t raw);

/* Leen y escriben un bloque de 32 bytes del pak del puerto. false si la
 * dirección está mal verificada (no se toca nada y `out` queda en ceros). */
bool salsa64_mempak_read(const struct salsa64_mempak *mp, unsigned port, uint16_t raw, uint8_t *out);
bool salsa64_mempak_write(struct salsa64_mempak *mp, unsigned port, uint16_t raw, const uint8_t *in);

bool salsa64_mempak_dirty(const struct salsa64_mempak *mp);
/* Llamar a menudo con un reloj en µs: true cuando toca volcar. */
bool salsa64_mempak_due(struct salsa64_mempak *mp, uint64_t now_us);
/* Después de escribir mp->data al disco. */
void salsa64_mempak_flushed(struct salsa64_mempak *mp);

#endif
