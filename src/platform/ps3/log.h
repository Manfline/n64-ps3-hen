/*
 * Cell64 — logger para PS3.
 *
 * Cada línea va a la TTY (stdout: RPCS3 la muestra en su consola y en
 * TTY.log) y, si hay red, por UDP hacia la Mac (tools/udplog.py).
 *
 * Destino UDP: broadcast 255.255.255.255:18194 por omisión. Para fijar una
 * IP, crear /dev_hdd0/tmp/cell64_log.cfg con "ip" o "ip:puerto".
 *
 * Sin asignaciones dinámicas: cada línea se arma en un búfer de pila de
 * CELL64_LOG_LINE_MAX bytes. No es seguro entre hilos todavía.
 */
#ifndef CELL64_PLATFORM_PS3_LOG_H
#define CELL64_PLATFORM_PS3_LOG_H

#define CELL64_LOG_PORT     18194
#define CELL64_LOG_LINE_MAX 1024
#define CELL64_LOG_CFG      "/dev_hdd0/tmp/cell64_log.cfg"

void cell64_log_init(const char *program);
void cell64_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cell64_log_shutdown(void);

#endif
