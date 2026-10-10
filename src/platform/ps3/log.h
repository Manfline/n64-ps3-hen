/*
 * Retro64 — logger para PS3.
 *
 * Cada línea va a la TTY (stdout: RPCS3 la muestra en su consola y en
 * TTY.log) y, si hay red, por UDP hacia la Mac (tools/udplog.py).
 *
 * Destino UDP: broadcast 255.255.255.255:18194 por omisión. Para fijar una
 * IP, crear /dev_hdd0/retro64/log.cfg con "ip" o "ip:puerto".
 *
 * Cada línea lleva "#n", un número de secuencia por programa, para que el
 * receptor cuente pérdidas y duplicados del UDP.
 *
 * Sin asignaciones dinámicas: cada línea se arma en un búfer de pila de
 * RETRO64_LOG_LINE_MAX bytes. No es seguro entre hilos todavía.
 */
#ifndef RETRO64_PLATFORM_PS3_LOG_H
#define RETRO64_PLATFORM_PS3_LOG_H

#include "platform/ps3/paths.h"

#define RETRO64_LOG_PORT     18194
#define RETRO64_LOG_LINE_MAX 1024
#define RETRO64_LOG_CFG      RETRO64_DATA_DIR "/log.cfg"

void retro64_log_init(const char *program);
void retro64_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void retro64_log_shutdown(void);

#endif
