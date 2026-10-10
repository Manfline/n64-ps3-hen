/*
 * Salsa64 — Fase 0: prueba del logger UDP.
 *
 * Envía SALSA64_UDPLOG_COUNT líneas numeradas y una línea final con el total.
 * En la Mac: tools/udplog.py --until RESULTADO y comparar el total recibido.
 */
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "platform/ps3/log.h"

#define SALSA64_UDPLOG_COUNT 200

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	salsa64_log_init("udplog");
	salsa64_log("enviando %d líneas numeradas", SALSA64_UDPLOG_COUNT);

	for (int i = 0; i < SALSA64_UDPLOG_COUNT; i++) {
		salsa64_log("linea %03d/%03d", i + 1, SALSA64_UDPLOG_COUNT);
		if (i % 20 == 19)
			sysUsleep(10000); /* no saturar la cola de envío */
	}

	salsa64_log("RESULTADO udplog: enviadas %d líneas", SALSA64_UDPLOG_COUNT);
	salsa64_log_shutdown();
	return 0;
}
