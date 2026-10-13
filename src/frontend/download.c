/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — descargas con libhttp; ver download.h. Basado en el ejemplo
 * network/httptest de PSL1GHT. */
#include "download.h"

#include <http/http.h>
#include <http/https.h>
#include <net/net.h>
#include <net/netctl.h>
#include <ppu-lv2.h>
#include <ssl/ssl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>

#include "cover.h"
#include "dlutil.h"
#include "i18n.h"
#include "namematch.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"

#define DL_CFG_PATH SALSA64_DATA_DIR "/descargas.cfg"
#define DL_COVERS   SALSA64_DATA_DIR "/covers"
#define DL_CHEATS   SALSA64_DATA_DIR "/cheats"
#define HTTP_POOL   0x10000
#define SSL_POOL    0x40000
#define CONN_US     (10 * 1000 * 1000)
#define RECV_US     (15 * 1000 * 1000)
#define INDEX_MAX   (6u << 20)
#define COVER_MAX   COVER_MAX_FILE_BYTES
#define CHEAT_MAX   (2u << 20)
#define USER_AGENT  "Mozilla/5.0 (PLAYSTATION 3; 1.00) Salsa64"

static struct {
	volatile int state, cancel;
	int what;
	volatile unsigned done, total, got, have, missing;
	char err[96];
	struct dl_rom *roms;
	unsigned n;
	sys_ppu_thread_t tid;
	bool joinable;
} job;

/* ---- La pila de red (se levanta y se baja en cada descarga) ---- */

/* Módulos del sistema que hacen falta; cada uno se carga solo si no lo
 * estaba y se descarga solo si lo cargó esta descarga. La red (libnet) es
 * del logger UDP (platform/ps3/log.c): netInitialize no hace nada si ya está
 * y aquí nunca se llama a netDeinitialize, que le cortaría el log. */
static const sysModuleId net_mods[] = { SYSMODULE_NET, SYSMODULE_NETCTL, SYSMODULE_HTTP, SYSMODULE_HTTP_UTIL,
					SYSMODULE_SSL, SYSMODULE_HTTPS };
#define NET_MODS (sizeof net_mods / sizeof net_mods[0])

static struct {
	void *http_pool, *ssl_pool, *certs;
	httpsData ca;
	bool http, https, ssl, mod[NET_MODS];
} stack;

static void net_down(void)
{
	if (stack.https)
		httpsEnd();
	if (stack.ssl)
		sslEnd();
	if (stack.http)
		httpEnd();
	for (size_t i = NET_MODS; i-- > 0;)
		if (stack.mod[i])
			sysModuleUnload(net_mods[i]);
	free(stack.http_pool);
	free(stack.ssl_pool);
	free(stack.certs);
	memset(&stack, 0, sizeof stack);
}

/* 0 si quedó lista; si no, un código negativo y `what` dice en qué paso. */
static int net_up(const char **what)
{
	int rc;
	u32 cert_size = 0;
	memset(&stack, 0, sizeof stack);
	*what = "módulos de red";
	for (size_t i = 0; i < NET_MODS; i++) {
		if (sysModuleIsLoaded(net_mods[i]) == 0)
			continue;  /* ya estaba (libnet, por el logger) */
		if ((rc = sysModuleLoad(net_mods[i])) < 0)
			return rc;
		stack.mod[i] = true;
	}
	*what = "netInitialize";
	if ((rc = netInitialize()) < 0)
		return rc;
	stack.http_pool = malloc(HTTP_POOL);
	*what = "memoria";
	if (!stack.http_pool)
		return -1;
	*what = "httpInit";
	if ((rc = httpInit(stack.http_pool, HTTP_POOL)) < 0)
		return rc;
	stack.http = true;
	stack.ssl_pool = malloc(SSL_POOL);
	*what = "memoria";
	if (!stack.ssl_pool)
		return -1;
	*what = "sslInit";
	if ((rc = sslInit(stack.ssl_pool, SSL_POOL)) < 0)
		return rc;
	stack.ssl = true;
	*what = "certificados";
	if ((rc = sslCertificateLoader(SSL_LOAD_CERT_ALL, NULL, 0, &cert_size)) < 0)
		return rc;
	stack.certs = malloc(cert_size);
	if (!stack.certs)
		return -1;
	if ((rc = sslCertificateLoader(SSL_LOAD_CERT_ALL, stack.certs, cert_size, NULL)) < 0)
		return rc;
	stack.ca.ptr = stack.certs;
	stack.ca.size = cert_size;
	*what = "httpsInit";
	if ((rc = httpsInit(1, &stack.ca)) < 0)
		return rc;
	stack.https = true;
	return 0;
}

/* Los certificados de la consola son viejos (sin las raíces de GitHub): se
 * sigue aunque no se pueda comprobar la cadena. Lo que se baja es solo texto
 * y PNG, nunca algo que se ejecute. */
static int ssl_accept(s32 verErr, sslCert const certs[], int n, const char *host, httpSslId id, void *arg)
{
	(void)certs;
	(void)n;
	(void)id;
	(void)arg;
	if (verErr)
		salsa64_log("descargas: certificado de %s no verificable (0x%x): se sigue", host ? host : "?", (unsigned)verErr);
	return 0;
}

/* GET de `url` a un búfer que crece hasta `cap`. 0 bien; si no, -1 y *code
 * lleva el estado HTTP (>= 100) o el error de libhttp (negativo). */
static int http_get(const char *url, uint8_t **buf, size_t *len, size_t cap, int *code)
{
	int rc, status = 0, result = -1;
	httpClientId cid = 0;
	httpTransId tid = 0;
	httpUri uri;
	void *pool = NULL;
	u32 need = 0;
	size_t used = 0, room = 0;
	uint8_t *data = NULL;

	*code = 0;
	if ((rc = httpCreateClient(&cid)) < 0) {
		*code = rc;
		goto end;
	}
	httpClientSetConnTimeout(cid, CONN_US);
	httpClientSetRecvTimeout(cid, RECV_US);
	httpClientSetSendTimeout(cid, RECV_US);
	httpClientSetUserAgent(cid, USER_AGENT);
	httpClientSetAutoRedirect(cid, 1);
	httpClientSetSslCallback(cid, ssl_accept, NULL);
	if ((rc = httpUtilParseUri(&uri, url, NULL, 0, &need)) < 0) {
		*code = rc;
		goto end;
	}
	pool = malloc(need);
	if (!pool || (rc = httpUtilParseUri(&uri, url, pool, need, NULL)) < 0) {
		*code = pool ? rc : -1;
		goto end;
	}
	if ((rc = httpCreateTransaction(&tid, cid, HTTP_METHOD_GET, &uri)) < 0 || (rc = httpSendRequest(tid, NULL, 0, NULL)) < 0) {
		*code = rc;
		goto end;
	}
	if ((rc = httpResponseGetStatusCode(tid, &status)) < 0) {
		*code = rc;
		goto end;
	}
	if (status != HTTP_STATUS_CODE_OK) {
		*code = status;
		goto end;
	}
	for (;;) {
		if (job.cancel) {
			*code = -2;
			goto end;
		}
		if (room - used < 16384) {
			size_t nsz = room ? room * 2 : 65536;
			if (nsz > cap + 16384)
				nsz = cap + 16384;
			if (nsz <= used + 1) {
				*code = -3;  /* más grande de lo permitido */
				goto end;
			}
			uint8_t *n = realloc(data, nsz);
			if (!n) {
				*code = -1;
				goto end;
			}
			data = n;
			room = nsz;
		}
		u32 got = 0;
		rc = httpRecvResponse(tid, (char *)data + used, (u32)(room - used - 1), &got);
		if (rc < 0) {
			*code = rc;
			goto end;
		}
		if (!got)
			break;
		used += got;
		if (used > cap) {
			*code = -3;
			goto end;
		}
	}
	data[used] = '\0';
	*buf = data;
	*len = used;
	data = NULL;
	result = 0;
end:
	if (tid)
		httpDestroyTransaction(tid);
	if (cid)
		httpDestroyClient(cid);
	free(pool);
	free(data);
	return result;
}

static bool write_file(const char *path, const uint8_t *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(data, 1, len, f) == len;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok)
		remove(path);
	return ok;
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(job.err, sizeof job.err, tr(fmt), ap);
	va_end(ap);
	job.state = DL_FAILED;
}

/* descargas.cfg; false si no existe (las descargas están apagadas). */
static bool read_cfg(struct dl_cfg *cfg)
{
	char text[2048];
	dl_cfg_default(cfg);
	FILE *f = fopen(DL_CFG_PATH, "r");
	if (!f)
		return false;
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	dl_cfg_parse(cfg, text, n);
	return true;
}

bool dl_enabled(void)
{
	struct dl_cfg cfg;
	bool on = read_cfg(&cfg) && cfg.enabled;
	salsa64_log("descargas: %s", on ? "encendidas (experimental, descargas.cfg)" : "apagadas");
	return on;
}

/* Nombre que se puede guardar: sin carpetas ni puntos al principio. */
static bool safe_name(const char *n)
{
	return n[0] && n[0] != '.' && !strchr(n, '/') && !strchr(n, '\\') && !strstr(n, "..") && strlen(n) < 160;
}

static void job_main(void *arg)
{
	(void)arg;
	struct dl_cfg cfg;
	const bool covers = job.what == 0;
	const char *ext = covers ? ".png" : ".cht", *dir = covers ? DL_COVERS : DL_CHEATS;
	const size_t min_key = covers ? 4 : 8, cap = covers ? COVER_MAX : CHEAT_MAX;
	uint8_t *index = NULL, *file = NULL;
	char *names = NULL;
	size_t ilen = 0, nlen = 0;
	int code = 0;
	const char *step = "";
	bool netctl = false;

	if (!read_cfg(&cfg) || !cfg.enabled) {
		fail("Las descargas están apagadas (descargas.cfg)");
		goto out;
	}
	const char *base = covers ? cfg.covers : cfg.cheats;
	char index_url[DL_URL_MAX + 16];
	snprintf(index_url, sizeof index_url, "%s", covers ? cfg.covers_index : cfg.cheats_index);
	if (!base[0]) {
		fail("Falta la dirección en descargas.cfg");
		goto out;
	}
	if (!index_url[0])
		snprintf(index_url, sizeof index_url, "%sindex.txt", base);

	int rc = net_up(&step);
	if (rc < 0) {
		salsa64_log("descargas: red: falló %s (%d)", step, rc);
		fail("No hay red: conecta la consola a internet");
		goto out;
	}
	s32 netstate = 0;
	netctl = netCtlInit() >= 0;
	if (!netctl || netCtlGetState(&netstate) < 0 || netstate != NET_CTL_STATE_IPObtained) {
		salsa64_log("descargas: sin conexión (estado %d)", (int)netstate);
		fail("No hay red: conecta la consola a internet");
		goto out;
	}
	mkdir(SALSA64_DATA_DIR, 0777);
	mkdir(dir, 0777);

	salsa64_log("descargas: índice %s", index_url);
	if (http_get(index_url, &index, &ilen, INDEX_MAX, &code) < 0) {
		salsa64_log("descargas: índice: error %d", code);
		if (code == -2) {
			job.state = DL_CANCELLED;
			goto out;
		}
		char why[24];
		if (code >= 100)
			snprintf(why, sizeof why, "HTTP %d", code);
		else
			snprintf(why, sizeof why, "0x%x", (unsigned)code);
		fail("No se pudo bajar el índice: %s", why);
		goto out;
	}
	names = malloc(ilen + 1);
	if (!names) {
		fail("No se pudo bajar el índice: %s", "memoria");
		goto out;
	}
	nlen = dl_index_names((const char *)index, ilen, ext, names, ilen + 1);
	free(index);
	index = NULL;
	salsa64_log("descargas: el índice trae %zu bytes de nombres %s", nlen, ext);

	for (unsigned i = 0; i < job.n && !job.cancel; i++) {
		const struct dl_rom *r = &job.roms[i];
		char path[320], found[256], url[DL_URL_MAX + 700], esc[600];
		if (name_find(dir, ext, r->name, r->title, r->code, min_key, path, sizeof path)) {
			job.have++;
		} else if (!name_find_list(names, nlen, ext, r->name, r->title, r->code, min_key, found, sizeof found) ||
			   !safe_name(found)) {
			job.missing++;
		} else {
			dl_url_escape(found, esc, sizeof esc);
			snprintf(url, sizeof url, "%s%s", base, esc);
			snprintf(path, sizeof path, "%s/%s", dir, found);
			size_t flen = 0;
			FILE *old = fopen(path, "rb");
			if (old) {
				/* Un archivo del usuario con ese nombre no se pisa. */
				fclose(old);
				job.have++;
			} else if (http_get(url, &file, &flen, cap, &code) == 0) {
				if (write_file(path, file, flen)) {
					job.got++;
					salsa64_log("descargas: %s -> %s (%zu bytes)", r->name, path, flen);
				} else {
					job.missing++;
					salsa64_log("descargas: no se pudo escribir %s", path);
				}
				free(file);
				file = NULL;
			} else {
				job.missing++;
				salsa64_log("descargas: %s: error %d", url, code);
				if (code == -2)
					break;
			}
		}
		job.done = i + 1;
	}
	job.state = job.cancel ? DL_CANCELLED : DL_DONE;
	salsa64_log("descargas: %s: %u nuevas, %u ya estaban, %u sin coincidencia", covers ? "carátulas" : "trucos", job.got,
		    job.have, job.missing);
out:
	free(index);
	free(file);
	free(names);
	if (netctl)
		netCtlTerm();
	net_down();
	sysThreadExit(0);
}

bool dl_start(int what, const struct dl_rom *roms, unsigned n)
{
	if (job.state == DL_RUNNING)
		return false;
	dl_shutdown();
	if (!n)
		return false;
	job.roms = malloc((size_t)n * sizeof *roms);
	if (!job.roms)
		return false;
	memcpy(job.roms, roms, (size_t)n * sizeof *roms);
	job.n = n;
	job.what = what;
	job.done = job.got = job.have = job.missing = 0;
	job.total = n;
	job.cancel = 0;
	job.err[0] = '\0';
	job.state = DL_RUNNING;
	if (sysThreadCreate(&job.tid, job_main, NULL, 1500, 0x20000, THREAD_JOINABLE, (char *)"descargas") != 0) {
		job.state = DL_FAILED;
		snprintf(job.err, sizeof job.err, "%s", "hilo");
		free(job.roms);
		job.roms = NULL;
		return false;
	}
	job.joinable = true;
	return true;
}

void dl_cancel(void)
{
	job.cancel = 1;
}

void dl_get(struct dl_status *st)
{
	st->state = job.state;
	st->what = job.what;
	st->done = job.done;
	st->total = job.total;
	st->got = job.got;
	st->have = job.have;
	st->missing = job.missing;
	memcpy(st->err, job.err, sizeof st->err);
}

void dl_shutdown(void)
{
	if (job.joinable) {
		job.cancel = 1;
		u64 rv;
		sysThreadJoin(job.tid, &rv);
		job.joinable = false;
	}
	free(job.roms);
	job.roms = NULL;
}
