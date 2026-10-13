/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — perfil persistente del recompilador, por juego: cuántas veces
 * entra la ejecución a cada bloque, guardado en un archivo y usado en la
 * sesión siguiente para compilar los bloques calientes juntos, en una zona
 * contigua de la caché de código (docs/rec-ppe.md, «Perfil persistente y
 * bloques calientes»).
 *
 * Identidad de un bloque: su dirección virtual, la huella (hash de un solo
 * sentido) de sus instrucciones MIPS y cuántas son. El código de la N64
 * llega por DMA en overlays y a veces se reescribe: un bloque cuya huella no
 * coincide con la de la entrada es otro bloque y tiene su propia entrada.
 * Los gemelos por Status.FR comparten la entrada (mismos bytes, misma
 * dirección).
 *
 * De cada bloque caliente se registran además sus sitios de salida (salto
 * condicional tomado, el salto final no tomado, J/JAL, fin de bloque): el
 * destino y cuántas veces se pasó por ahí. De ahí salen, al guardar, cuántas
 * veces se tomó y no cada salto condicional y los sucesores más frecuentes
 * del bloque. Solo se registra; quién lo usa está en el diseño de los pasos
 * siguientes (docs/rec-ppe.md).
 *
 * El archivo no contiene ni un byte de código del juego ni código traducido:
 * direcciones, huellas, cuentas y banderas. La caché de código no se
 * persiste nunca.
 *
 * Todo preasignado: la tabla la pone quien llama y no se asigna nada al
 * cargar ni al guardar. El archivo se lee entero al abrir el juego y se
 * escribe por trozos (salsa64_rec_profile_save_step) desde el hilo que no
 * emula, o de una vez al cerrar. Si falta, está corrupto o es de otro juego,
 * el perfil arranca vacío y el emulador funciona igual.
 */
#ifndef SALSA64_CPU_REC_PROFILE_H
#define SALSA64_CPU_REC_PROFILE_H

#include <stdio.h>

#include "core/types.h"

#define SALSA64_RPF_ENTRIES 32768u
#define SALSA64_RPF_SLOTS   65536u   /* índice abierto por dirección: el doble de las entradas */
#define SALSA64_RPF_SITES   8192u    /* sitios de salida registrados (solo bloques calientes) */
#define SALSA64_RPF_BLOCK_SITES 8    /* sitios registrados por bloque, a lo más */
#define SALSA64_RPF_PATH_MAX 256
/* Entradas a bloques de una sesión a partir de las cuales su cuenta pesa como
 * una sesión larga al mezclarla con la del archivo (salsa64_rec_profile_merge):
 * 2^26 son entre medio minuto y un minuto de juego (Perfect Dark cuenta 1,2 M
 * por segundo de consola; GoldenEye 2,5 M). */
#define SALSA64_RPF_LONG_SESSION (1ull << 26)

/* Banderas de una entrada (salsa64_rec_profile_entry.flags). */
#define SALSA64_RPF_FR_DEP    1u  /* el código depende de Status.FR (registros impares de la FPU) */
#define SALSA64_RPF_FR_SEEN0  2u  /* se compiló con FR = 0 */
#define SALSA64_RPF_FR_SEEN1  4u  /* se compiló con FR = 1 */
#define SALSA64_RPF_SITES_OVER 8u /* tiene más sitios de salida de los que se registran */

/* Clase de un sitio de salida. */
enum salsa64_rpf_site_kind {
	SALSA64_RPF_SITE_COND,   /* salto condicional tomado (interno o final; destino fijo) */
	SALSA64_RPF_SITE_FALL,   /* el salto final no tomado (sigue en la instrucción siguiente) */
	SALSA64_RPF_SITE_JUMP,   /* J o JAL tomado */
	SALSA64_RPF_SITE_END,    /* fin del bloque sin salto (64 instrucciones) */
	SALSA64_RPF_SITE_KINDS
};

struct salsa64_rec_profile_entry {
	uint64_t hash;       /* huella de las instrucciones MIPS (salsa64_rec_profile_hash) */
	uint32_t vpc;        /* dirección virtual (extensión de signo de 32 bits) */
	uint32_t score;      /* entradas al bloque según el archivo (sesiones anteriores) */
	uint32_t hits;       /* entradas al bloque en esta sesión (ya sumadas desde rec->hits[]) */
	uint16_t n;          /* instrucciones MIPS */
	uint16_t words;      /* palabras de PPC que ocupó la última vez que se compiló */
	uint16_t site;       /* primer sitio de salida registrado (índice + 1 en sites[]), 0 = ninguno */
	uint8_t hot;         /* en esta sesión se compila en la zona caliente */
	uint8_t flags;       /* SALSA64_RPF_* */
	uint8_t nsites;      /* sitios registrados */
	uint8_t pad[3];
};

/* Un sitio de salida de un bloque caliente. Los sitios de una entrada van
 * encadenados por `next`. */
struct salsa64_rec_profile_site {
	uint64_t succ_hash;  /* huella del bloque que hay en el destino (rec.c lo resuelve); 0 = sin resolver */
	uint32_t target;     /* destino (dirección virtual) */
	uint32_t taken;      /* veces esta sesión: lo incrementa el código generado */
	uint32_t taken0;     /* veces según el archivo */
	uint32_t not_taken0; /* no tomado según el archivo (solo COND) */
	uint16_t entry;      /* índice + 1 */
	uint16_t next;       /* siguiente de la misma entrada, índice + 1 */
	uint8_t k;           /* instrucción del salto en el bloque (n en END) */
	uint8_t kind;        /* enum salsa64_rpf_site_kind */
	uint8_t pad[2];
};

/* Identidad del juego en la cabecera del archivo: el código del cartucho y
 * los CRC de la cabecera de la ROM. Todo en 0: no se comprueba. */
struct salsa64_rec_profile_game {
	char code[4];
	uint32_t crc1, crc2;
};

struct salsa64_rec_profile {
	uint32_t count;          /* entradas en uso: las primeras `count` de entries[] */
	uint32_t loaded;         /* de ellas, las que venían del archivo */
	uint32_t nsites;         /* sitios en uso: los primeros `nsites` de sites[] */
	uint32_t hot_entries;    /* marcadas calientes al cargar */
	uint32_t hot_words;      /* palabras que suman */
	uint32_t hot_threshold;  /* puntaje mínimo de las calientes (0: ninguna) */
	uint32_t full;           /* bloques sin entrada porque la tabla estaba llena */
	uint32_t sites_full;     /* sitios sin registrar porque sites[] estaba lleno */
	uint64_t session;        /* entradas a bloques contadas en esta sesión */
	bool dirty;              /* hay algo que guardar */
	struct salsa64_rec_profile_game game;
	uint16_t index[SALSA64_RPF_SLOTS];   /* índice + 1 en entries[]; 0 = libre */
	struct salsa64_rec_profile_entry entries[SALSA64_RPF_ENTRIES];
	struct salsa64_rec_profile_site sites[SALSA64_RPF_SITES];
};

/* Escritura por trozos (salsa64_rec_profile_save_begin/step). */
struct salsa64_rec_profile_writer {
	FILE *f;                 /* NULL: no hay escritura en curso */
	uint32_t count, nsites;  /* entradas y sitios a escribir (fijados al empezar) */
	uint32_t pos;            /* entradas, y después sitios, ya escritos */
	uint8_t phase;           /* 0 entradas, 1 sitios */
	bool long_session;       /* cómo se mezclan las cuentas (fijado al empezar) */
	uint64_t sum;            /* suma de verificación de lo escrito */
	char path[SALSA64_RPF_PATH_MAX];
	char tmp[SALSA64_RPF_PATH_MAX + 4];
};

void salsa64_rec_profile_init(struct salsa64_rec_profile *p);

/* La identidad de la ROM (cabecera en el orden de bytes de la N64). */
void salsa64_rec_profile_game_of(struct salsa64_rec_profile_game *g, const uint8_t *rom);

/* Huella de las `n` instrucciones de un bloque. */
uint64_t salsa64_rec_profile_hash(const uint32_t *words, unsigned n);

/* Entrada de (vpc, hash, n): índice + 1, o 0 si no está (y, con `add`, si la
 * tabla está llena). Al agregar, `words` es lo que ocupa el bloque. */
unsigned salsa64_rec_profile_lookup(struct salsa64_rec_profile *p, uint32_t vpc, uint64_t hash, unsigned n,
				    unsigned words, bool add);
/* El bloque de la entrada `idx` (índice + 1) se volvió a compilar con `words` palabras. */
void salsa64_rec_profile_set_words(struct salsa64_rec_profile *p, unsigned idx, unsigned words);
/* Suma banderas SALSA64_RPF_* a la entrada. */
void salsa64_rec_profile_set_flags(struct salsa64_rec_profile *p, unsigned idx, unsigned flags);
/* Suma `hits` entradas al bloque de `idx` en esta sesión (saturando). */
void salsa64_rec_profile_count(struct salsa64_rec_profile *p, unsigned idx, uint32_t hits);

/* El sitio de salida (k, kind, target) de la entrada `idx`: índice + 1 en
 * sites[], registrándolo si no estaba; 0 si no cabe (sites[] lleno o el
 * bloque ya tiene SALSA64_RPF_BLOCK_SITES). El código generado incrementa
 * sites[j - 1].taken. */
unsigned salsa64_rec_profile_site(struct salsa64_rec_profile *p, unsigned idx, unsigned k, unsigned kind,
				  uint32_t target);

/* Puntaje con que se guarda una entrada: la cuenta de esta sesión mezclada
 * con la del archivo. Una sesión larga (SALSA64_RPF_LONG_SESSION entradas o
 * más) hace decaer lo que no se ejecutó: max(hits, (score + hits) / 2); una
 * corta solo puede subir: max(score, hits). Las cuentas de los sitios solo
 * se suman (saturando). */
uint32_t salsa64_rec_profile_merge(uint32_t score, uint32_t hits, bool long_session);

/* Marca calientes las entradas de mayor puntaje cuyas palabras, sumadas,
 * caben en `cap_words` (0: ninguna). Las de puntaje 0 nunca. */
void salsa64_rec_profile_mark_hot(struct salsa64_rec_profile *p, uint32_t cap_words);

/* Lee el archivo en una tabla vacía con la identidad `game` (NULL: sin
 * comprobar). false si no existe, está corrupto o es de otro juego (la
 * tabla queda vacía, con `game`). */
bool salsa64_rec_profile_load(struct salsa64_rec_profile *p, const char *path,
			      const struct salsa64_rec_profile_game *game);
/* Escribe la tabla entera en `path` (primero <path>.tmp y rename). */
bool salsa64_rec_profile_save(const struct salsa64_rec_profile *p, const char *path);
/* Lo mismo por trozos: begin abre y escribe la cabecera; cada step escribe
 * hasta `max_items` entradas o sitios y devuelve 1 al terminar (archivo
 * cerrado y renombrado), 0 si falta, −1 si falló (archivo descartado). La
 * tabla puede seguir recibiendo cuentas mientras tanto: el archivo lleva lo
 * que había al escribir cada entrada. */
bool salsa64_rec_profile_save_begin(const struct salsa64_rec_profile *p, struct salsa64_rec_profile_writer *w,
				    const char *path);
int salsa64_rec_profile_save_step(const struct salsa64_rec_profile *p, struct salsa64_rec_profile_writer *w,
				  unsigned max_items);
void salsa64_rec_profile_save_abort(struct salsa64_rec_profile_writer *w);

#endif
