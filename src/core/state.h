/*
 * Salsa64 — estados guardados (save states): toda la máquina en un archivo
 * para seguir exactamente desde ahí.
 *
 * Formato (big-endian, igual en la PS3 y en el host: un estado de la
 * consola se carga en salsa64-run y al revés):
 *   cabecera de SALSA64_STATE_HEADER bytes: magia "SLS64ST\0", versión,
 *   tamaño de la cabecera y del archivo, CRC1 y CRC2 de la cabecera de la
 *   ROM, tamaño de la ROM, título (20 bytes), tamaño de la RDRAM, campo del
 *   VI y banderas;
 *   después, secciones «etiqueta (4 bytes) + largo (4 bytes) + datos»:
 *     THMB  miniatura de la imagen del VI (ancho, alto, RGBA5551), la
 *           primera para que el menú la lea sin leer el resto
 *     CPU0  VR4300: GPR, HI/LO, PC y retardo, COP0 con la TLB, FPU, FCR31
 *     SCHD  tiempos del planificador de eventos
 *     RCP0  MI, SP, DP, VI, AI, PI, RI, SI, registros de la RDRAM y la
 *           tarea del RSP en curso (ya recogida del backend)
 *     SPME  DMEM e IMEM del RSP
 *     PIF0  controles retenidos, intercambios del PIF y la PIF RAM
 *     CART  guardado del cartucho en memoria (EEPROM o SRAM)
 *     HLEG  estado de la HLE gráfica entre tareas (RDP y TMEM), formato del
 *           backend
 *     HLEA  estado de la HLE de audio entre tareas, formato del backend
 *     RDRM  la RDRAM entera, sin comprimir, al final
 * Cada sección se lee campo por campo: una sección más corta que lo que
 * espera esta versión deja el resto en cero (para agregar campos al final
 * sin romper los estados viejos) y las etiquetas desconocidas se saltan.
 * Un cambio incompatible sube SALSA64_STATE_VERSION; los estados de una
 * versión anterior a SALSA64_STATE_MIN_VERSION se rechazan con un mensaje.
 *
 * Cuándo: con la emulación quieta, en el límite de un campo, y después de
 * salsa64_rsp_quiesce (la tarea del RSP en vuelo recogida). Al cargar, quien
 * llama vacía la caché del recompilador si la hay (salsa64_state_load lo
 * hace con cpu->rec) y olvida lo que tenga de la imagen anterior (destinos
 * del RSX, colas entre hilos, audio en cola).
 *
 * La E/S es de quien llama, por tramos (write/read): la RDRAM va directo
 * de y hacia su búfer, sin copia; lo demás pasa por `scratch`
 * (SALSA64_STATE_SCRATCH bytes). Nada de esto asigna memoria.
 */
#ifndef SALSA64_CORE_STATE_H
#define SALSA64_CORE_STATE_H

#include "core/n64.h"

#define SALSA64_STATE_VERSION     1u
#define SALSA64_STATE_MIN_VERSION 1u
#define SALSA64_STATE_HEADER      64u
#define SALSA64_STATE_SCRATCH     (128u << 10)
#define SALSA64_STATE_SLOTS       10
#define SALSA64_STATE_FLAG_THUMB  0x1u

/* Miniatura: RGBA5551 en el orden de bytes del host, fila por fila. */
#define SALSA64_STATE_THUMB_W 160
#define SALSA64_STATE_THUMB_H 120

struct salsa64_state_thumb {
	uint16_t width, height;
	const uint16_t *pixels;
};

/* Lo que dice la cabecera (salsa64_state_peek). */
struct salsa64_state_info {
	uint32_t version;
	uint32_t file_size;
	uint32_t crc1, crc2;
	uint32_t rom_size;
	char title[21];
	uint32_t rdram_size;
	uint32_t field;
	uint32_t flags;
};

/* Escriben o leen exactamente `len` bytes; false si falló. */
typedef bool (*salsa64_state_write_fn)(void *ctx, const void *data, uint32_t len);
typedef bool (*salsa64_state_read_fn)(void *ctx, void *data, uint32_t len);

/* Guarda la máquina (con la ROM en el bus y salsa64_rsp_quiesce hecho).
 * `thumb` puede ser NULL. En `err`, el motivo si falla. */
bool salsa64_state_save(struct salsa64_n64 *n64, const struct salsa64_state_thumb *thumb,
			salsa64_state_write_fn write, void *ctx, uint8_t *scratch, char *err, size_t err_size);

/* Carga un estado sobre la máquina en marcha (la misma ROM ya en el bus,
 * salsa64_rsp_quiesce hecho). Valida la cabecera (versión, ROM, tamaño de
 * la RDRAM) antes de tocar nada; si la lectura falla a mitad de la RDRAM,
 * la máquina queda inservible y lo dice `err` (*broken = true). */
bool salsa64_state_load(struct salsa64_n64 *n64, salsa64_state_read_fn read, void *ctx, uint8_t *scratch,
			char *err, size_t err_size, bool *broken);

/* Lee y valida solo la cabecera (64 bytes); false con el motivo si no es un
 * estado de Salsa64 o es de una versión que no se puede cargar. */
bool salsa64_state_peek(const uint8_t header[SALSA64_STATE_HEADER], struct salsa64_state_info *info, char *err,
			size_t err_size);

/* Tamaño del archivo que escribiría salsa64_state_save ahora. */
uint32_t salsa64_state_size(const struct salsa64_n64 *n64, bool thumb);

/* Nombre del archivo de la ranura: "<título>-<CRC1>.st<slot>", como los
 * guardados del cartucho (salsa64_save_basename). */
void salsa64_state_filename(const uint8_t *rom, unsigned slot, char *buf, size_t size);

#endif
