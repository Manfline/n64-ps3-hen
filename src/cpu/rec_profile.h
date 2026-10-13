/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — perfil persistente del recompilador, por juego: cuántas veces
 * entra la ejecución a cada bloque, guardado en un archivo y usado en la
 * sesión siguiente para compilar los bloques calientes juntos, en una zona
 * contigua de la caché de código (docs/rec-ppe.md, «Perfil persistente y
 * bloques calientes»).
 *
 * Identidad de un bloque: su dirección virtual, la huella (hash) de sus
 * instrucciones MIPS y cuántas son. El código de la N64 llega por DMA en
 * overlays y a veces se reescribe: un bloque cuya huella no coincide con la
 * de la entrada es otro bloque y tiene su propia entrada. Los gemelos por
 * Status.FR comparten la entrada (mismos bytes, misma dirección).
 *
 * Todo preasignado: la tabla la pone quien llama (un megabyte) y no se
 * asigna nada al cargar ni al guardar. El archivo se lee entero al abrir el
 * juego y se escribe por trozos (salsa64_rec_profile_save_step) desde el
 * hilo que no emula, o de una vez al cerrar. Si falta o está corrupto, el
 * perfil arranca vacío y el emulador funciona igual.
 */
#ifndef SALSA64_CPU_REC_PROFILE_H
#define SALSA64_CPU_REC_PROFILE_H

#include <stdio.h>

#include "core/types.h"

#define SALSA64_RPF_ENTRIES 32768u
#define SALSA64_RPF_SLOTS   65536u   /* índice abierto por dirección: el doble de las entradas */
#define SALSA64_RPF_PATH_MAX 256
/* Entradas a bloques de una sesión a partir de las cuales su cuenta pesa como
 * una sesión larga al mezclarla con la del archivo (salsa64_rec_profile_merge):
 * 2^26 son entre medio minuto y un minuto de juego (Perfect Dark cuenta 1,2 M
 * por segundo de consola; GoldenEye más). */
#define SALSA64_RPF_LONG_SESSION (1ull << 26)

struct salsa64_rec_profile_entry {
	uint64_t hash;       /* huella de las instrucciones MIPS (salsa64_rec_profile_hash) */
	uint32_t vpc;        /* dirección virtual (extensión de signo de 32 bits) */
	uint32_t score;      /* entradas al bloque según el archivo (sesiones anteriores) */
	uint32_t hits;       /* entradas al bloque en esta sesión (ya sumadas desde rec->hits[]) */
	uint16_t n;          /* instrucciones MIPS */
	uint16_t words;      /* palabras de PPC que ocupó la última vez que se compiló */
	uint8_t hot;         /* en esta sesión se compila en la zona caliente */
	uint8_t pad[7];
};

struct salsa64_rec_profile {
	uint32_t count;          /* entradas en uso: las primeras `count` de entries[] */
	uint32_t loaded;         /* de ellas, las que venían del archivo */
	uint32_t hot_entries;    /* marcadas calientes al cargar */
	uint32_t hot_words;      /* palabras que suman */
	uint32_t hot_threshold;  /* puntaje mínimo de las calientes (0: ninguna) */
	uint32_t full;           /* bloques sin entrada porque la tabla estaba llena */
	uint64_t session;        /* entradas a bloques contadas en esta sesión */
	bool dirty;              /* hay algo que guardar */
	uint16_t index[SALSA64_RPF_SLOTS];   /* índice + 1 en entries[]; 0 = libre */
	struct salsa64_rec_profile_entry entries[SALSA64_RPF_ENTRIES];
};

/* Escritura por trozos (salsa64_rec_profile_save_begin/step). */
struct salsa64_rec_profile_writer {
	FILE *f;                 /* NULL: no hay escritura en curso */
	uint32_t count, pos;     /* entradas a escribir (fijadas al empezar) y ya escritas */
	uint64_t sum;            /* suma de verificación de lo escrito */
	bool long_session;       /* cómo se mezclan las cuentas (fijado al empezar) */
	char path[SALSA64_RPF_PATH_MAX];
	char tmp[SALSA64_RPF_PATH_MAX + 4];
};

void salsa64_rec_profile_init(struct salsa64_rec_profile *p);

/* Huella de las `n` instrucciones de un bloque. */
uint64_t salsa64_rec_profile_hash(const uint32_t *words, unsigned n);

/* Entrada de (vpc, hash, n): índice + 1, o 0 si no está (y, con `add`, si la
 * tabla está llena). Al agregar, `words` es lo que ocupa el bloque. */
unsigned salsa64_rec_profile_lookup(struct salsa64_rec_profile *p, uint32_t vpc, uint64_t hash, unsigned n,
				    unsigned words, bool add);
/* El bloque de la entrada `idx` (índice + 1) se volvió a compilar con `words` palabras. */
void salsa64_rec_profile_set_words(struct salsa64_rec_profile *p, unsigned idx, unsigned words);
/* Suma `hits` entradas al bloque de `idx` en esta sesión (saturando). */
void salsa64_rec_profile_count(struct salsa64_rec_profile *p, unsigned idx, uint32_t hits);

/* Puntaje con que se guarda una entrada: la cuenta de esta sesión mezclada
 * con la del archivo. Una sesión larga (SALSA64_RPF_LONG_SESSION entradas o
 * más) hace decaer lo que no se ejecutó: max(hits, (score + hits) / 2); una
 * corta solo puede subir: max(score, hits). */
uint32_t salsa64_rec_profile_merge(uint32_t score, uint32_t hits, bool long_session);

/* Marca calientes las entradas de mayor puntaje cuyas palabras, sumadas,
 * caben en `cap_words` (0: ninguna). Las de puntaje 0 nunca. */
void salsa64_rec_profile_mark_hot(struct salsa64_rec_profile *p, uint32_t cap_words);

/* Lee el archivo en una tabla vacía. false si no existe o está corrupto (la
 * tabla queda vacía). */
bool salsa64_rec_profile_load(struct salsa64_rec_profile *p, const char *path);
/* Escribe la tabla entera en `path` (primero <path>.tmp y rename). */
bool salsa64_rec_profile_save(const struct salsa64_rec_profile *p, const char *path);
/* Lo mismo por trozos: begin abre y escribe la cabecera; cada step escribe
 * hasta `max_entries` entradas y devuelve 1 al terminar (archivo cerrado y
 * renombrado), 0 si falta, −1 si falló (archivo descartado). La tabla puede
 * seguir recibiendo cuentas mientras tanto: el archivo lleva lo que había al
 * escribir cada entrada. */
bool salsa64_rec_profile_save_begin(const struct salsa64_rec_profile *p, struct salsa64_rec_profile_writer *w,
				    const char *path);
int salsa64_rec_profile_save_step(const struct salsa64_rec_profile *p, struct salsa64_rec_profile_writer *w,
				  unsigned max_entries);
void salsa64_rec_profile_save_abort(struct salsa64_rec_profile_writer *w);

#endif
