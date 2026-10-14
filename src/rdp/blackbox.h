/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caja negra: un anillo en memoria con lo último que pasó entre la
 * HLE gráfica y la pantalla, para llevarlo a la Mac y repetirlo allí cuando
 * un problema solo ocurre en la PS3 (docs/cajanegra.md).
 *
 * El anillo guarda registros, los más viejos se tiran cuando no cabe uno
 * nuevo:
 *   TASK  la salida de la HLE de una tarea gráfica, en el formato del
 *         volcado gfxdump (gfx.h: gfx_dump_header; texturas incluidas, así
 *         se repite con salsa64-run --raster-dump), con un encabezado de
 *         tarea (struct bb_task_hdr: número, campo, tarea anterior…)
 *   FILD  lo que se presentó en un campo: registros del VI, la imagen que
 *         leyó el VI, si fue un cuadro verdadero o uno intermedio (y su t),
 *         de dónde salió la imagen y la hora (struct bb_field)
 *   NOTE  lo que otro módulo quiera dejar (p. ej. las decisiones del
 *         emparejado de la interpolación): una etiqueta de 4 letras y bytes
 *         opacos
 * Al pedirlo, se escribe todo el anillo, comprimido (spukit/lz.h) y desde un
 * hilo aparte, en un archivo (formato «S4BB», abajo). Mientras se escribe, el
 * anillo no recibe registros nuevos (así la copia no cambia debajo).
 *
 * Portable y sin PSL1GHT: compila con gcc nativo, con pruebas en
 * tests/unit/test_blackbox.c; la lectura (bb_find_task…) la usan salsa64-run
 * y las pruebas, y tools/cajanegra.py hace lo mismo en Python.
 *
 * Todo lo que llama a bb_* es de un solo hilo (el de presentación) salvo
 * bb_write, que corre con el anillo congelado.
 *
 * Archivo (todo big-endian):
 *   64 bytes de cabecera: magia «S4BB», versión (1), tamaño de la cabecera,
 *     banderas (BB_FLAG_*), campo del VI al pedirlo, tareas, campos, notas,
 *     CRC1 y CRC2 del juego, su título (20 bytes) y tareas descartadas
 *   registros: 16 bytes (tipo, largo sin comprimir, banderas, id) y su
 *     contenido: tal cual o, con BB_REC_LZ, como bloques de spukit/lz.h
 *     (ceil(largo / 64 KB) bloques con su encabezado de 4 bytes), uno detrás
 *     de otro hasta el registro END.
 */
#ifndef SALSA64_RDP_BLACKBOX_H
#define SALSA64_RDP_BLACKBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "rdp/gfxdump.h"

#define BB_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define BB_TASK BB_FOURCC('T', 'A', 'S', 'K')
#define BB_FIELD BB_FOURCC('F', 'I', 'L', 'D')
#define BB_NOTE BB_FOURCC('N', 'O', 'T', 'E')
#define BB_END BB_FOURCC('E', 'N', 'D', ' ')

#define BB_MAGIC 0x53344242u  /* "S4BB" */
#define BB_VERSION 1u
#define BB_HEADER 64u
#define BB_REC_HEADER 16u
#define BB_REC_LZ 0x1u  /* contenido en bloques de spukit/lz.h */
#define BB_FLAG_LZ 0x1u /* el archivo trae registros comprimidos */

/* ---- Contenido de los registros (palabras big-endian) ----------------------- */

/* TASK: 8 palabras y, detrás, el volcado de la tarea. */
enum {
	BB_TASK_NUM,      /* número de la tarea (la numeración de rdp.c: empieza en 0) */
	BB_TASK_FIELD,    /* campo del VI que presentación estaba procesando al traducirla */
	BB_TASK_PREV,     /* número de la tarea registrada antes, o 0xFFFFFFFF si no hay */
	BB_TASK_FLAGS,    /* BB_TASK_ADJ: la anterior es la inmediata (PREV + 1 == NUM) */
	BB_TASK_DRAWS,
	BB_TASK_TRIANGLES,
	BB_TASK_RECTS,
	BB_TASK_RES_FLAGS, /* gfx_result.flags */
	BB_TASK_WORDS
};
#define BB_TASK_ADJ 0x1u
#define BB_TASK_NONE 0xFFFFFFFFu

/* FILD: lo que presentó un campo. */
enum {
	BB_FLD_FIELD,        /* n64.bus.vi.fields al terminar el campo */
	BB_FLD_FLAGS,        /* BB_FLD_* */
	BB_FLD_TASKS,        /* tareas gráficas terminadas hasta ese campo */
	BB_FLD_TRANSLATED,   /* tareas traducidas al presentarlo */
	BB_FLD_VI_TYPE,      /* la imagen que leyó el VI */
	BB_FLD_VI_ORIGIN,
	BB_FLD_VI_WIDTH,
	BB_FLD_VI_HEIGHT,
	BB_FLD_VI_STRIDE,
	BB_FLD_VI_BPP,
	BB_FLD_VI_REGS,      /* 14 registros del VI (SALSA64_VI_NREGS) desde aquí */
	BB_FLD_KIND = BB_FLD_VI_REGS + 14, /* BB_KIND_*: cuadro verdadero, intermedio o repetido */
	BB_FLD_T,            /* t del intermedio, como bits de un float (0 para los verdaderos) */
	BB_FLD_K,            /* campos por cuadro del juego con los que se interpoló (0: sin interpolación) */
	BB_FLD_BEHIND,       /* campos que la emulación llevaba de ventaja */
	BB_FLD_TIME_HI,      /* hora de la PS3 (µs del reloj del sistema) */
	BB_FLD_TIME_LO,
	BB_FLD_SRC_TASK,     /* tarea que dibujó lo presentado, o 0xFFFFFFFF */
	BB_FLD_PREV_TASK,    /* tarea anterior de un intermedio (la «anterior» de la interpolación) o 0xFFFFFFFF */
	BB_FLD_WORDS
};
#define BB_FLD_SHOWN 0x01u    /* se presentó */
#define BB_FLD_SKIPPED 0x02u  /* se saltó (emulación iba por delante) */
#define BB_FLD_RSX 0x04u      /* la imagen salió de un destino del RSX */
#define BB_FLD_RDRAM 0x08u    /* la imagen salió de la RDRAM (framebuffer 2D) */
#define BB_FLD_BLANK 0x10u    /* sin imagen */
enum { BB_KIND_TRUE = 0, BB_KIND_INTERP = 1, BB_KIND_REPEAT = 2 };

/* ---- El anillo ------------------------------------------------------------------ */

struct bb_rec {
	uint32_t size;    /* bytes del registro con su encabezado, múltiplo de 16; BB_PAD si es relleno */
	uint32_t kind;
	uint32_t id;      /* TASK: número de tarea; FILD: campo; NOTE: su etiqueta */
	uint32_t len;     /* bytes del contenido */
};
#define BB_PAD 0u

struct bb {
	uint8_t *mem;     /* alineado a 16, con BB_SLACK bytes más al final (lecturas de DMA) */
	uint32_t cap;     /* múltiplo de 16 */
	uint32_t head, tail, used;
	uint32_t nrec;    /* registros (sin relleno) */
	uint32_t ntask, nfield, nnote;
	uint32_t dropped_tasks;  /* tareas tiradas por falta de lugar */
	uint32_t last_task;      /* última TASK guardada, o BB_TASK_NONE */
	/* Para deshacer la última bb_alloc. */
	uint32_t undo_head, undo_used;
	bool undo_ok;
};
#define BB_SLACK 64u

void bb_init(struct bb *b, uint8_t *mem, uint32_t cap);
void bb_clear(struct bb *b);
/* Reserva un registro de `len` bytes de contenido (alineado a 16) y devuelve
 * dónde ponerlo, tirando los más viejos si hace falta. NULL si no cabe ni con
 * el anillo vacío. */
uint8_t *bb_alloc(struct bb *b, uint32_t kind, uint32_t id, uint32_t len);
/* Deshace la última bb_alloc (si el armado del contenido falló). */
void bb_cancel(struct bb *b);

/* Guarda una tarea: arma su volcado directo en el anillo. `field`: el campo
 * que presentación procesa. `work`: espacio de trabajo de gfxdump.h. */
bool bb_add_task(struct bb *b, uint32_t task, uint32_t field, const struct gfx_dump_src *src,
		 const struct gfx_result *res, const struct gfx_dump_work *work);
/* Guarda un campo: `words` son BB_FLD_WORDS palabras en el orden del host
 * (se pasan a big-endian aquí). */
bool bb_add_field(struct bb *b, const uint32_t words[BB_FLD_WORDS]);
/* Guarda una nota de otro módulo. */
bool bb_add_note(struct bb *b, uint32_t tag, const void *data, uint32_t len);

/* Recorrido del más viejo al más nuevo (sin el relleno). */
struct bb_iter {
	uint32_t pos, left;
};
void bb_iter_begin(const struct bb *b, struct bb_iter *it);
const struct bb_rec *bb_iter_next(const struct bb *b, struct bb_iter *it, const uint8_t **content);

/* ---- Escribir el archivo ---------------------------------------------------------- */

typedef bool (*bb_write_fn)(void *ctx, const void *data, uint32_t len);
/* Comprime `len` bytes de `src` y entrega los bloques con su encabezado: la
 * misma forma de salsa64_state_compress_fn (bgio_compress la cumple). */
typedef bool (*bb_compress_fn)(void *cctx, const uint8_t *src, uint32_t len, bb_write_fn write, void *wctx);

struct bb_meta {
	uint32_t field;      /* campo del VI al pedirlo */
	uint32_t crc1, crc2; /* del juego */
	char title[20];
};

/* Escribe el archivo con todo el anillo. Con `compress` NULL va sin
 * comprimir. Los TASK y las notas de 4 KB o más van comprimidos. `raw` (puede
 * ser NULL) recibe los bytes sin comprimir. false si `write` o `compress`
 * fallan. */
bool bb_write(const struct bb *b, const struct bb_meta *m, bb_compress_fn compress, void *cctx, bb_write_fn write,
	      void *wctx, uint64_t *raw);

/* ---- Leer un archivo (host) ------------------------------------------------------- */

struct bb_file_info {
	uint32_t version, flags, field, tasks, fields, notes, crc1, crc2, dropped;
	char title[21];
};
/* Valida la cabecera de un archivo en memoria. */
bool bb_file_info(const uint8_t *file, uint32_t len, struct bb_file_info *info);
/* Recorre los registros de un archivo en memoria: devuelve la posición del
 * siguiente en *pos (0 al empezar) y llena `rec` y `payload` (puntero dentro
 * del archivo, comprimido o no según rec_flags). false al llegar a END o si
 * el archivo está cortado. */
struct bb_file_rec {
	uint32_t kind, id, len, flags;
	const uint8_t *payload;
	uint32_t payload_bytes;  /* lo que ocupa en el archivo */
};
bool bb_file_next(const uint8_t *file, uint32_t len, uint32_t *pos, struct bb_file_rec *rec);
/* Contenido sin comprimir de un registro en `out` (`len` bytes). */
bool bb_file_content(const struct bb_file_rec *rec, uint8_t *out);

#endif
