/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — memoria de guardado del cartucho: EEPROM (canal 4 del PIF),
 * SRAM y FlashRAM (dominio 2 del PI).
 *
 * Comportamiento (n64brew.dev: Joybus Protocol, Peripheral Interface y
 * Memory map, consultadas el 2026-10-10):
 *   - EEPROM de 4 Kbit (512 bytes, 64 bloques de 8) y de 16 Kbit (2 KB,
 *     256 bloques), en el canal 4 del PIF:
 *       0x00 / 0xFF  información: identificador 0x0080 (4 Kbit) o 0x00C0
 *                    (16 Kbit) y un byte de estado (bit 7: escritura en
 *                    curso).
 *       0x04 b       lee el bloque b: 8 bytes.
 *       0x05 b d×8   escribe el bloque b; responde 0x00, o 0x80 si sigue
 *                    otra escritura en curso.
 *     En la de 4 Kbit se ignoran los dos bits altos del bloque (64–255
 *     repiten 0–63). En la consola una escritura tarda hasta 6 ms; aquí es
 *     instantánea y la EEPROM nunca está ocupada.
 *   - SRAM de 256 Kbit (32 KB) en 0x08000000, dominio 2 del PI: DMA en los
 *     dos sentidos (PI_WR_LEN: cartucho → RDRAM; PI_RD_LEN: RDRAM →
 *     cartucho) y accesos directos del CPU de 32 bits. Se repite cada
 *     32 KB en todo el dominio (0x08000000–0x0FFFFFFF). Sin latencias.
 *   - FlashRAM de 1 Mbit (128 KB) en el mismo lugar: un chip con modos
 *     (lectura, estado, identificador, búfer de página) que se manejan con
 *     comandos en 0x08010000. Detalle y fuentes en flashram.c.
 *
 * Se reconocen pero todavía no se emulan (quedan como "ninguno" y quien
 * llama lo avisa): SRAM de 768 Kbit en bancos y SRAM de 1 Mbit. Para
 * agregarlos: salsa64_save_pi_dma y los accesos directos despachan por
 * tipo.
 *
 * Tipo de guardado de una ROM (salsa64_save_detect), en este orden:
 *   1. ajuste manual: en el frontend, SALSA64_DATA_DIR/guardado.cfg
 *      (salsa64_save_parse_config);
 *   2. cabecera de homebrew ("Advanced Homebrew ROM Header", la que escribe
 *      n64tool de libdragon con N64_ROM_SAVETYPE): "ED" en 0x3C–0x3D y el
 *      tipo en los 4 bits altos de 0x3F;
 *   3. tabla por código de juego (savedb.c: solo códigos y tipo);
 *   4. ninguno.
 *
 * guardado.cfg, una línea por juego ('#' comenta; mayúsculas o minúsculas
 * da igual):
 *   NZL = sram256k        código de 3 caracteres: todas las regiones
 *   NK4E = eeprom16k      código de 4: una región
 *   EC7011B7 = ninguno    CRC1 del encabezado (8 dígitos hex): una ROM
 *   * = eeprom4k          cualquier ROM
 * Gana la línea más específica (CRC1, luego 4, 3 y *). Tipos: ninguno,
 * eeprom4k, eeprom16k, sram256k (o sram), flashram, y sram768k, sram1m
 * (reconocidos, sin emular).
 *
 * Persistencia: la hace quien llama (frontend o salsa64-run), con un
 * archivo por juego y tipo (salsa64_save_filename). El núcleo guarda los
 * datos en un búfer preasignado y cuenta las escrituras del juego que los
 * cambian; salsa64_save_due dice cuándo volcar: tras SALSA64_SAVE_QUIET_US
 * sin escrituras, o a lo más SALSA64_SAVE_MAX_DELAY_US después del primer
 * cambio si el juego no deja de escribir. Nada de esto asigna memoria.
 */
#ifndef SALSA64_CORE_SAVE_H
#define SALSA64_CORE_SAVE_H

#include "core/types.h"

enum salsa64_save_type {
	SALSA64_SAVE_NONE,
	SALSA64_SAVE_EEPROM_4K,
	SALSA64_SAVE_EEPROM_16K,
	SALSA64_SAVE_SRAM_256K,
	SALSA64_SAVE_SRAM_768K,  /* reconocido, sin emular */
	SALSA64_SAVE_FLASHRAM,
	SALSA64_SAVE_SRAM_1M,    /* reconocido, sin emular */
	SALSA64_SAVE_NTYPES
};

/* De dónde salió el tipo. */
enum salsa64_save_source {
	SALSA64_SAVE_FROM_NONE,    /* sin indicación */
	SALSA64_SAVE_FROM_CONFIG,  /* ajuste manual */
	SALSA64_SAVE_FROM_HEADER,  /* cabecera de homebrew */
	SALSA64_SAVE_FROM_TABLE,   /* tabla por código de juego */
};

/* Valor de `manual` para salsa64_save_detect sin ajuste manual. */
#define SALSA64_SAVE_AUTO (-1)

#define SALSA64_SAVE_MAX        (128u << 10)  /* el tipo emulado más grande */
#define SALSA64_SRAM_ADDR       0x08000000u
#define SALSA64_SRAM_SPAN       0x08000000u  /* dominio 2 hasta la ROM */
#define SALSA64_SAVE_QUIET_US   2000000u
#define SALSA64_SAVE_MAX_DELAY_US 30000000u

/* FlashRAM (flashram.c). Se emula un solo modelo, el MX29L1100 de Macronix,
 * con direcciones de lectura en palabras de 16 bits. */
#define SALSA64_FLASH_SIZE      (128u << 10)
#define SALSA64_FLASH_PAGE      128u
#define SALSA64_FLASH_CMD       0x10000u      /* registro de comandos, desde 0x08000000 */
#define SALSA64_FLASH_TYPE_ID   0x11118001u   /* primera palabra del identificador */
#define SALSA64_FLASH_MX29L1100 0x00C2001Eu   /* fabricante (Macronix) y dispositivo */

enum salsa64_flash_mode {
	SALSA64_FLASH_READ_ARRAY,  /* el dominio muestra los datos */
	SALSA64_FLASH_STATUS,      /* el registro de estado */
	SALSA64_FLASH_SILICON_ID,  /* el identificador de 8 bytes */
	SALSA64_FLASH_LOAD_PAGE,   /* el búfer de página */
};

/* Bits del registro de estado. */
#define SALSA64_FLASH_ST_READY       0x80u  /* WSM_READY: acepta borrar y programar */
#define SALSA64_FLASH_ST_ERASE_OK    0x08u
#define SALSA64_FLASH_ST_PROGRAM_OK  0x04u
#define SALSA64_FLASH_ST_ERASE_BUSY  0x02u
#define SALSA64_FLASH_ST_PROGRAM_BUSY 0x01u

/* Estado del chip (lo que no son los datos). Va en los estados guardados:
 * sección FLSH de state.c. */
struct salsa64_flash {
	uint8_t mode;          /* enum salsa64_flash_mode */
	uint8_t status;
	uint8_t erase;         /* borrado preparado: 0 ninguno, 1 sector, 2 chip */
	uint8_t erase_sector;  /* 0–7, con erase = 1 */
	uint32_t commands;     /* comandos recibidos, para el log y las pruebas */
	uint32_t unknown;      /* comandos que no se reconocieron */
	uint8_t page[SALSA64_FLASH_PAGE];
};

struct salsa64_save {
	enum salsa64_save_type type;  /* NONE si el tipo pedido no se emula */
	uint32_t size;                /* bytes de datos */
	uint32_t writes;              /* escrituras del juego que cambiaron datos */
	uint32_t flushed;             /* `writes` del último volcado */
	uint32_t seen;                /* `writes` en la última consulta a _due */
	uint64_t seen_us;             /* cuándo se vio cambiar `writes` */
	uint64_t dirty_us;            /* primer cambio desde el último volcado */
	struct salsa64_flash flash;   /* solo con SALSA64_SAVE_FLASHRAM */
	uint8_t data[SALSA64_SAVE_MAX] __attribute__((aligned(128)));
};

struct salsa64_bus;

/* Prepara `s` para el tipo dado, con los datos en blanco (0xFF). Si el tipo
 * no se emula queda como NONE. */
void salsa64_save_init(struct salsa64_save *s, enum salsa64_save_type type);
/* Copia el contenido de un archivo de guardado. Si el tamaño no coincide
 * copia lo que quepa (el resto queda como estaba) y devuelve false. */
bool salsa64_save_load(struct salsa64_save *s, const uint8_t *data, uint32_t len);
/* Conecta el guardado al cartucho (NULL lo quita). Sobrevive al reset. */
void salsa64_save_attach(struct salsa64_bus *bus, struct salsa64_save *s);

bool salsa64_save_emulated(enum salsa64_save_type type);
/* Nombre para el log y guardado.cfg ("eeprom4k", "ninguno"…). */
const char *salsa64_save_name(enum salsa64_save_type type);
const char *salsa64_save_source_name(enum salsa64_save_source src);

/* Tipo de la ROM (formato .z64, al menos 0x40 bytes). `manual` es un
 * enum salsa64_save_type o SALSA64_SAVE_AUTO. */
enum salsa64_save_type salsa64_save_detect(const uint8_t *rom, int manual, enum salsa64_save_source *src);
bool salsa64_save_from_header(const uint8_t *rom, enum salsa64_save_type *type);
bool salsa64_save_from_table(const uint8_t *rom, enum salsa64_save_type *type);  /* savedb.c */
/* Busca en el texto de guardado.cfg el ajuste de esta ROM: devuelve el tipo
 * o SALSA64_SAVE_AUTO si ninguna línea aplica. `bad` recibe cuántas líneas
 * no se entendieron; la primera queda descrita en `err` (puede ser NULL). */
int salsa64_save_parse_config(const char *text, size_t len, const uint8_t *rom, int *bad,
			      char *err, size_t err_size);
/* Archivo del guardado: "<título>-<CRC1>.<eep|sra|fla>", con el título del
 * encabezado reducido a letras, dígitos y '_'. */
void salsa64_save_filename(const uint8_t *rom, enum salsa64_save_type type, char *buf, size_t size);
/* El mismo nombre sin extensión (también el de los estados guardados). */
void salsa64_save_basename(const uint8_t *rom, char *buf, size_t size);

/* Comandos joybus del canal 4 (pif.c): respuesta en `reply` (hasta 8
 * bytes) y su longitud, o -1 si no hay EEPROM que conteste. */
int salsa64_save_joybus(struct salsa64_save *s, const uint8_t *tx, unsigned txlen, uint8_t *reply);
/* DMA del PI ya programado (pi.c): si su dirección del cartucho cae en la
 * SRAM o la FlashRAM, lo hace con la interrupción y devuelve true.
 * `to_rdram` = PI_WR_LEN. */
bool salsa64_save_pi_dma(struct salsa64_bus *bus, bool to_rdram);
/* Accesos directos del CPU al dominio 2 (bus.c), alineados a 4. Sin SRAM
 * ni FlashRAM, lectura 0 y la escritura se ignora. */
uint32_t salsa64_save_dom2_read32(const struct salsa64_bus *bus, uint32_t paddr);
void salsa64_save_dom2_write32(struct salsa64_bus *bus, uint32_t paddr, uint32_t value);

/* FlashRAM (flashram.c); `off` es la dirección desde 0x08000000. */
void salsa64_flash_reset(struct salsa64_flash *f);
uint32_t salsa64_flash_read32(const struct salsa64_save *s, uint32_t off);
void salsa64_flash_write32(struct salsa64_save *s, uint32_t off, uint32_t value);
/* DMA de `len` bytes entre `rdram` y el chip (`to_rdram` = PI_WR_LEN). */
void salsa64_flash_dma(struct salsa64_save *s, uint32_t off, uint8_t *rdram, uint32_t len, bool to_rdram);
/* Escritura del juego en los datos (save.c): solo cuenta en `writes` si
 * cambia algo. */
void salsa64_save_store(struct salsa64_save *s, uint32_t off, const uint8_t *src, uint32_t len);

/* Hay cambios sin volcar. */
bool salsa64_save_dirty(const struct salsa64_save *s);
/* Llamar a menudo (una vez por campo) con un reloj en µs: true cuando toca
 * volcar. */
bool salsa64_save_due(struct salsa64_save *s, uint64_t now_us);
/* Después de escribir s->data al disco. */
void salsa64_save_flushed(struct salsa64_save *s);

#endif
