/*
 * Retro64 — logger para PS3 (TTY + UDP). Ver log.h.
 */
#include "platform/ps3/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/net.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/systime.h>

static int log_sock = -1;
static struct sockaddr_in log_dest;
static char log_tag[24] = "retro64";
static u64 log_t0;
static u32 log_seq;
static FILE *log_file;
static int log_send_fails, log_send_ret, log_send_errno;

static void log_load_dest(void)
{
	char cfg[64] = "255.255.255.255";
	unsigned port = RETRO64_LOG_PORT;

	FILE *f = fopen(RETRO64_LOG_CFG, "r");
	if (f) {
		if (fgets(cfg, sizeof cfg, f)) {
			cfg[strcspn(cfg, " \r\n")] = '\0';
			char *colon = strchr(cfg, ':');
			if (colon) {
				*colon = '\0';
				port = (unsigned)strtoul(colon + 1, NULL, 10);
			}
		}
		fclose(f);
	}

	memset(&log_dest, 0, sizeof log_dest);
	log_dest.sin_family = AF_INET;
	log_dest.sin_port = htons(port);
	if (inet_pton(AF_INET, cfg, &log_dest.sin_addr) != 1)
		inet_pton(AF_INET, "255.255.255.255", &log_dest.sin_addr);
}

void retro64_log_init(const char *program)
{
	snprintf(log_tag, sizeof log_tag, "%s", program);
	log_t0 = sysGetSystemTime();

	char path[64];
	mkdir(RETRO64_DATA_DIR, 0777);
	snprintf(path, sizeof path, RETRO64_DATA_DIR "/%s.log", log_tag);
	log_file = fopen(path, "w");

	int ret = netInitialize();
	if (ret < 0) {
		retro64_log("red no disponible (netInitialize = %d): solo TTY y archivo", ret);
		return;
	}

	log_sock = netSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (log_sock < 0) {
		retro64_log("netSocket falló (%d): solo TTY", log_sock);
		return;
	}

	int on = 1;
	netSetSockOpt(log_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
	log_load_dest();
	retro64_log("log UDP hacia %s:%u", inet_ntoa(log_dest.sin_addr), ntohs(log_dest.sin_port));
}

void retro64_log(const char *fmt, ...)
{
	char line[RETRO64_LOG_LINE_MAX];
	u64 us = sysGetSystemTime() - log_t0;
	int n = snprintf(line, sizeof line, "[%s %4u.%03u #%u] ", log_tag,
			 (unsigned)(us / 1000000), (unsigned)(us / 1000 % 1000), (unsigned)++log_seq);

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
	va_end(ap);

	size_t len = strlen(line);
	if (len == 0 || line[len - 1] != '\n') {
		line[len++] = '\n';
		line[len] = '\0';
	}

	fputs(line, stdout);
	fflush(stdout);
	if (log_file) {
		fputs(line, log_file);
		fflush(log_file);
	}
	if (log_sock >= 0) {
		int ret = netSendTo(log_sock, line, len, 0, (const struct sockaddr *)&log_dest, sizeof log_dest);
		if (ret < 0 && log_send_fails++ == 0) {
			log_send_ret = ret;
			log_send_errno = errno;
		}
	}
}

void retro64_log_shutdown(void)
{
	if (log_send_fails)
		retro64_log("netSendTo falló %d veces (primero: %d, errno %d)",
			    log_send_fails, log_send_ret, log_send_errno);
	if (log_sock >= 0) {
		netClose(log_sock);
		log_sock = -1;
		netDeinitialize();
	}
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
}
