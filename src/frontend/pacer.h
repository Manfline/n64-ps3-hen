/*
 * Salsa64 — ritmo de la emulación: cada campo del VI a la hora que le toca
 * según la tele emulada (NTSC 59,83 campos/s, PAL 49,92), con un reloj de
 * tiempo real. Sin esto, la emulación corre tan rápido como pueda (el
 * flip en vsync solo frena al hilo de presentación; docs/hilos-ppe.md).
 *
 * Política: la referencia `next_ns` es la hora a la que debe empezar el
 * próximo campo. Antes de emular un campo, si la hora no llegó se duerme
 * hasta ella; si ya pasó no se espera nada, y si el atraso supera
 * PACER_MAX_BEHIND_NS se olvida la deuda (la referencia se reinicia a
 * ahora): un tirón de la emulación no se "recupera" corriendo rápido
 * después. Lo mismo si va adelantada más de PACER_MAX_AHEAD_NS (solo pasa
 * si alguien avanzó la referencia sin esperar, o el reloj dio un salto):
 * nunca se duerme más de eso de una vez. Después de cada campo la
 * referencia avanza lo que duró el campo en ciclos del VR4300
 * (93,75 MHz), así que el ritmo medio es exacto aunque cada espera tenga
 * jitter.
 *
 * Punto de enganche para el audio: `sleep` es de quien llama (en la PS3,
 * sysUsleep) y se puede reemplazar por una espera al llenado del búfer de
 * audio; pacer_nudge mueve la referencia para un control fino (ajustar el
 * remuestreo o corregir la deriva entre el reloj y el audio). Si el audio
 * marca el ritmo por su cuenta, se desactiva (`enabled = 0`).
 *
 * Portable (sin cabeceras de la PS3): el reloj lo pasa quien llama.
 */
#ifndef SALSA64_FRONTEND_PACER_H
#define SALSA64_FRONTEND_PACER_H

#include <stdint.h>

#define PACER_MAX_BEHIND_NS 67000000ull  /* ~4 campos NTSC */
#define PACER_MAX_AHEAD_NS  67000000ull

typedef void (*pacer_sleep_fn)(void *ctx, uint64_t us);

struct pacer {
	int enabled;
	int started;             /* ya hay referencia */
	uint64_t next_ns;        /* hora del próximo campo */
	uint32_t rem;            /* resto de ciclos → ns (×32/3) */
	pacer_sleep_fn sleep;
	void *ctx;
	/* Estadísticas desde el arranque. */
	uint32_t waits;          /* campos en que durmió */
	uint32_t resets;         /* veces que reinició la referencia (atraso o adelanto grandes) */
	uint64_t us_requested;   /* µs pedidos a `sleep` */
};

void pacer_init(struct pacer *p, int enabled, pacer_sleep_fn sleep, void *ctx);
/* Antes de emular un campo, con la hora actual en µs: duerme si va
 * adelantado. Devuelve los µs que pidió dormir (0 si no esperó). */
uint64_t pacer_wait(struct pacer *p, uint64_t now_us);
/* Después de emular un campo de `cycles` ciclos del VR4300. */
void pacer_advance(struct pacer *p, uint64_t cycles);
/* Mueve la referencia: ns > 0 la retrasa (la emulación esperará más). */
void pacer_nudge(struct pacer *p, int64_t ns);

#endif
