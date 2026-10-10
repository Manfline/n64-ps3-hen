/*
 * Salsa64 — frontend mínimo para PS3.
 *
 * Carga una ROM, la arranca por HLE y, en cada campo del VI, corre el
 * intérprete hasta el fin del campo y presenta el framebuffer del VI con
 * lib/rsxkit (textura en la memoria del RSX, escalada a 4:3). Todo en un
 * hilo del PPE por ahora; repartir CPU y presentación entre los dos hilos
 * del PPE queda para cuando el intérprete se acerque a tiempo real.
 *
 * ROM: SALSA64_DATA_DIR/rom.z64 (cualquiera de los tres órdenes de bytes)
 * si existe; si no, la ROM de prueba vitest que va embebida. Con vitest,
 * el frontend se verifica a sí mismo (selftest.c) e imprime
 * "RESULTADO frontend: OK/FALLO"; en modo de prueba automática
 * (platform/ps3/autotest.h) sale después. Si no, mide 10 s sin detección de
 * bucles de espera y 10 s con ella ("MEDICIÓN …" en el log) y sigue.
 *
 * Controles: los DualShock 3 de los puertos 1–4 son los controles de la N64
 * (padmap.c; se cambian con SALSA64_DATA_DIR/controles.cfg). El puerto 1
 * siempre tiene un control conectado (neutro sin mando): el DS3 tarda unos
 * cuadros en aparecer y los juegos buscan controles al arrancar. Los cambios
 * de botones van al log.
 *
 * Guardado del cartucho (EEPROM, SRAM; savefile.c): tipo por
 * SALSA64_DATA_DIR/guardado.cfg, la cabecera de homebrew o la tabla por
 * código de juego; un archivo por juego en SALSA64_DATA_DIR/saves, escrito
 * unos segundos después de la última escritura del juego y al salir.
 *
 * Salir: START + SELECT, o "Salir del juego" desde el menú del sistema.
 * Lo que la ROM imprime por el ISViewer va al log.
 */
#include <io/pad.h>
#include <malloc.h>
#include <ppu-types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

#include <rsxkit/rsxkit.h>

#include "core/boot.h"
#include "padmap.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "savefile.h"
#include "selftest.h"
#include "vitest_rom_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define ROM_PATH     SALSA64_DATA_DIR "/rom.z64"
#define PADS_PATH    SALSA64_DATA_DIR "/controles.cfg"
#define SAVES_DIR    SALSA64_DATA_DIR "/saves"
#define SAVE_CFG     SALSA64_DATA_DIR "/guardado.cfg"
#define PADS_CFG_MAX 4096
#define ROM_MAX      (64u << 20)
#define FB_MAX_W     640
#define FB_MAX_H     576
#define STATS_US     5000000u
#define BENCH_US     10000000u

static struct salsa64_n64 n64;
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct salsa64_save save;
static struct savefile save_file;
static volatile int running = 1;
static int selftest_on;

static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)param, (void)user;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

static struct salsa64_padmap padmap;

/* Medición A/B con vitest: sin y con detección de bucles de espera. */
static struct {
	int stage;  /* 0 nada, 1 sin detección, 2 con detección, 3 terminada */
	uint64_t t0, us_emu, cycles0, idle0;
	uint32_t fields;
	double rate[2], ms[2];
} bench;

static void bench_begin(int stage, uint64_t now)
{
	bench.stage = stage;
	bench.t0 = now;
	bench.us_emu = 0;
	bench.fields = 0;
	bench.cycles0 = n64.cpu.cycles;
	bench.idle0 = n64.cpu.idle_cycles;
	salsa64_cpu_set_idle(&n64.cpu, stage == 2);
}

static void bench_field(uint64_t now, uint64_t us_emu)
{
	if (bench.stage != 1 && bench.stage != 2)
		return;
	bench.us_emu += us_emu;
	bench.fields++;
	uint64_t dt = now - bench.t0;
	if (dt < BENCH_US)
		return;
	int k = bench.stage - 1;
	uint64_t cyc = n64.cpu.cycles - bench.cycles0, idle = n64.cpu.idle_cycles - bench.idle0;
	bench.rate[k] = bench.fields * 1e6 / dt;
	bench.ms[k] = bench.us_emu / 1000.0 / bench.fields;
	salsa64_log("MEDICIÓN vitest %s detección de bucles de espera: %.2f campos/s, emulación %.2f ms "
		    "por campo, %.1f MIPS, %.1f %% del tiempo saltado (%u campos en %.1f s)",
		    k ? "con" : "sin", bench.rate[k], bench.ms[k], (double)(cyc - idle) / dt,
		    cyc ? 100.0 * idle / cyc : 0.0, bench.fields, dt / 1e6);
	if (k == 0) {
		bench_begin(2, now);
	} else {
		bench.stage = 3;
		salsa64_log("MEDICIÓN vitest: emulación %.2f → %.2f ms por campo (×%.1f), %.2f → %.2f campos/s",
			    bench.ms[0], bench.ms[1], bench.ms[1] > 0 ? bench.ms[0] / bench.ms[1] : 0.0,
			    bench.rate[0], bench.rate[1]);
	}
}

/* Lee controles.cfg si existe; si no, el mapeo por omisión. */
static void load_padmap(void)
{
	static char text[PADS_CFG_MAX];
	salsa64_padmap_default(&padmap);
	FILE *f = fopen(PADS_PATH, "rb");
	if (!f) {
		salsa64_log("controles: mapeo por omisión (no hay %s)", PADS_PATH);
		return;
	}
	size_t len = fread(text, 1, sizeof text, f);
	fclose(f);
	char err[96];
	int bad = salsa64_padmap_parse(&padmap, text, len, err, sizeof err);
	if (bad)
		salsa64_log("controles: %s con %d líneas que no entendí; la primera, %s", PADS_PATH, bad, err);
	else
		salsa64_log("controles: %s", PADS_PATH);
}

static struct salsa64_ds3 ds3_from(const padData *d)
{
	struct salsa64_ds3 s;
	s.buttons = (uint32_t)d->BTN_CROSS << SALSA64_DS3_CROSS | (uint32_t)d->BTN_CIRCLE << SALSA64_DS3_CIRCLE |
		    (uint32_t)d->BTN_SQUARE << SALSA64_DS3_SQUARE | (uint32_t)d->BTN_TRIANGLE << SALSA64_DS3_TRIANGLE |
		    (uint32_t)d->BTN_L1 << SALSA64_DS3_L1 | (uint32_t)d->BTN_R1 << SALSA64_DS3_R1 |
		    (uint32_t)d->BTN_L2 << SALSA64_DS3_L2 | (uint32_t)d->BTN_R2 << SALSA64_DS3_R2 |
		    (uint32_t)d->BTN_L3 << SALSA64_DS3_L3 | (uint32_t)d->BTN_R3 << SALSA64_DS3_R3 |
		    (uint32_t)d->BTN_START << SALSA64_DS3_START | (uint32_t)d->BTN_SELECT << SALSA64_DS3_SELECT |
		    (uint32_t)d->BTN_UP << SALSA64_DS3_UP | (uint32_t)d->BTN_DOWN << SALSA64_DS3_DOWN |
		    (uint32_t)d->BTN_LEFT << SALSA64_DS3_LEFT | (uint32_t)d->BTN_RIGHT << SALSA64_DS3_RIGHT;
	s.lx = (uint8_t)d->ANA_L_H;
	s.ly = (uint8_t)d->ANA_L_V;
	s.rx = (uint8_t)d->ANA_R_H;
	s.ry = (uint8_t)d->ANA_R_V;
	return s;
}

/* DualShock 3 → controles de la N64, una vez por campo. ioPadGetData da
 * len = 0 cuando nada cambió: se conserva el último estado. Devuelve 1 si
 * algún mando tiene START + SELECT. */
static int update_pads(void)
{
	static struct salsa64_ds3 last[SALSA64_PIF_PORTS] = {
		[0 ... SALSA64_PIF_PORTS - 1] = { 0, 128, 128, 128, 128 },
	};
	static struct salsa64_pad logged[SALSA64_PIF_PORTS];
	static struct salsa64_ds3 raw_logged[SALSA64_PIF_PORTS] = {
		[0 ... SALSA64_PIF_PORTS - 1] = { 0, 128, 128, 128, 128 },
	};
	static uint8_t present[SALSA64_PIF_PORTS];
	const uint32_t quit = 1u << SALSA64_DS3_START | 1u << SALSA64_DS3_SELECT;
	padInfo info;
	padData data;
	int exit = 0;

	if (ioPadGetInfo(&info) != 0)
		return 0;
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		struct salsa64_pad p = { 0, 0, 0, i == 0 };
		if (present[i] != !!info.status[i]) {
			present[i] = !!info.status[i];
			salsa64_log("mando %u %s", i + 1, present[i] ? "conectado" : "desconectado");
			last[i] = (struct salsa64_ds3){ 0, 128, 128, 128, 128 };  /* nada pegado */
		}
		if (info.status[i]) {
			if (ioPadGetData(i, &data) == 0 && data.len > 0)
				last[i] = ds3_from(&data);
			/* Los sticks del DS3 tal como llegan, para revisar el mapeo. */
			const struct salsa64_ds3 *a = &last[i], *b = &raw_logged[i];
			if (abs(a->lx - b->lx) >= 40 || abs(a->ly - b->ly) >= 40 ||
			    abs(a->rx - b->rx) >= 40 || abs(a->ry - b->ry) >= 40) {
				salsa64_log("mando %u: stick izquierdo (%u,%u), derecho (%u,%u)", i + 1,
					    a->lx, a->ly, a->rx, a->ry);
				raw_logged[i] = *a;
			}
			salsa64_padmap_apply(&padmap, &last[i], &p);
			if ((last[i].buttons & quit) == quit)
				exit = 1;
		}
		salsa64_pif_set_pad(&n64.bus, i, &p);

		/* Al log: conexión y botones; el stick, si se movió bastante. */
		struct salsa64_pad *o = &logged[i];
		int dx = p.x - o->x, dy = p.y - o->y;
		if (p.connected != o->connected || p.buttons != o->buttons ||
		    dx * dx + dy * dy >= 40 * 40) {
			char desc[128];
			if (p.connected) {
				salsa64_pad_describe(&p, desc, sizeof desc);
				salsa64_log("control %u: %s", i + 1, desc);
			} else {
				salsa64_log("control %u: desconectado", i + 1);
			}
			*o = p;
		}
	}
	return exit;
}

/* Texto del ISViewer: al log línea por línea. */
static void isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	static char line[256];
	static uint32_t n;
	(void)ctx;
	if (selftest_on)
		selftest_isv(text, len);
	for (uint32_t i = 0; i < len; i++) {
		if (text[i] == '\n' || n == sizeof line - 1) {
			line[n] = '\0';
			salsa64_log("rom: %s", line);
			n = 0;
		}
		if (text[i] != '\n')
			line[n++] = (char)text[i];
	}
}

/* ROM del disco (preasignada una sola vez) o la embebida. */
static uint8_t *load_rom(uint32_t *size, const char **source)
{
	FILE *f = fopen(ROM_PATH, "rb");
	if (f) {
		long len = -1;
		if (fseek(f, 0, SEEK_END) == 0)
			len = ftell(f);
		if (len >= 0x1000 && (uint32_t)len <= ROM_MAX && fseek(f, 0, SEEK_SET) == 0) {
			uint8_t *buf = memalign(128, (size_t)len);
			if (buf && fread(buf, 1, (size_t)len, f) == (size_t)len) {
				fclose(f);
				*size = (uint32_t)len;
				*source = ROM_PATH;
				return buf;
			}
			free(buf);
		}
		fclose(f);
		salsa64_log("no se pudo leer %s: uso la ROM de prueba", ROM_PATH);
	}
	uint8_t *buf = memalign(128, vitest_rom_bin_size);
	if (!buf)
		return NULL;
	memcpy(buf, vitest_rom_bin, vitest_rom_bin_size);
	*size = vitest_rom_bin_size;
	*source = "vitest (embebida)";
	selftest_on = 1;
	return buf;
}

int main(void)
{
	salsa64_log_init("salsa64");
	const int autotest = salsa64_autotest();

	int rc = rsxkit_init(NULL);
	rsxkit_fb *fb = rc == RSXKIT_OK ? rsxkit_fb_create(FB_MAX_W, FB_MAX_H) : NULL;
	if (!fb) {
		salsa64_log("RESULTADO frontend: FALLO al iniciar el RSX (%d)", rc);
		salsa64_log_shutdown();
		return 1;
	}
	const rsxkit_screen *sc = rsxkit_screen_info();
	salsa64_log("RSX listo: %ux%u, tele %s", sc->width, sc->height, sc->aspect > 1.5f ? "16:9" : "4:3");

	uint32_t rom_size = 0;
	const char *source = NULL;
	uint8_t *rom = load_rom(&rom_size, &source);
	uint8_t *rdram = memalign(128, SALSA64_RDRAM_MAX);
	if (!rom || !rdram) {
		salsa64_log("RESULTADO frontend: FALLO sin memoria");
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	salsa64_rom_normalize(rom, rom_size);

	salsa64_n64_init(&n64, rdram, SALSA64_RDRAM_MAX);
	salsa64_bus_set_isviewer(&n64.bus, isv_buf, isv_out, NULL);
	salsa64_bus_set_rom(&n64.bus, rom, rom_size);
	struct salsa64_boot_info info;
	if (!salsa64_boot_hle(&n64, SALSA64_BOOT_TV_AUTO, &info)) {
		salsa64_log("RESULTADO frontend: FALLO al arrancar %s: %s", source, info.error);
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	char title[21];
	memcpy(title, rom + 0x20, 20);
	title[20] = '\0';
	salsa64_log("ROM %s \"%s\", %u bytes, %s, entrada 0x%08X, TV %s", source, title, rom_size,
		    info.kind == SALSA64_BOOT_ELF ? "ELF (IPL3 libre)" : "carga plana",
		    info.entry, info.tv == SALSA64_TV_PAL ? "PAL" : info.tv == SALSA64_TV_MPAL ? "MPAL" : "NTSC");
	savefile_open(&save_file, &save, &n64.bus, rom, SAVES_DIR, SAVE_CFG, SALSA64_SAVE_AUTO, salsa64_log);

	ioPadInit(7);
	load_padmap();
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	if (selftest_on)
		selftest_start();

	const rsxkit_view view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f };
	enum selftest_state result = SELFTEST_RUNNING;
	uint64_t t_stats = sysGetSystemTime(), cyc_stats = n64.cpu.cycles, idle_stats = n64.cpu.idle_cycles;
	uint64_t us_emu = 0, us_present = 0;
	uint32_t fields_stats = 0, blank = 0;

	while (running) {
		sysUtilCheckCallback();
		if (update_pads())
			break;

		uint64_t t0 = sysGetSystemTime();
		salsa64_n64_run_field(&n64);
		uint64_t t1 = sysGetSystemTime();
		savefile_poll(&save_file, t1);

		struct salsa64_vi_frame f;
		rsxkit_rect r;
		bool check = false;
		rsxkit_frame_begin(0x000000);
		if (salsa64_vi_frame(&n64.bus, &f) && f.width <= FB_MAX_W && f.height <= FB_MAX_H) {
			if (selftest_on && result == SELFTEST_RUNNING)
				check = selftest_frame(&n64.bus, &f);
			rsxkit_fb_upload(fb, n64.bus.rdram + f.origin, f.width, f.height, f.stride,
					 f.bpp == 4 ? RSXKIT_RGBA8888 : RSXKIT_RGBA5551);
			rsxkit_fb_draw(fb, &view, &r);
		} else {
			blank++;
		}
		if (check) {
			rsxkit_finish();
			selftest_screen(&f, &r);
		}
		rsxkit_frame_end();
		uint64_t t2 = sysGetSystemTime();
		us_emu += t1 - t0;
		us_present += t2 - t1;
		fields_stats++;

		if (selftest_on && result == SELFTEST_RUNNING) {
			result = selftest_state(n64.bus.vi.fields);
			if (result != SELFTEST_RUNNING) {
				salsa64_log("RESULTADO frontend: %s (campo %u)", result == SELFTEST_OK ? "OK" : "FALLO",
					    n64.bus.vi.fields);
				if (autotest)
					break;
				if (result == SELFTEST_OK)
					bench_begin(1, t2);
			}
		} else {
			bench_field(t2, t1 - t0);
		}

		uint64_t dt = t2 - t_stats;
		if (dt >= STATS_US) {
			/* Una instrucción por ciclo, salvo los saltados en bucles de espera. */
			uint64_t cyc = n64.cpu.cycles - cyc_stats, idle = n64.cpu.idle_cycles - idle_stats;
			salsa64_log("%.1f campos/s (%.0f %% de %s), %.1f MIPS, %.0f %% en bucles de espera; "
				    "por campo: emulación %.1f ms, presentación %.1f ms; %u sin imagen",
				    fields_stats * 1e6 / dt,
				    fields_stats * 1e6 / dt * 100.0 * n64.bus.vi.field_cycles / 93750000.0,
				    info.tv == SALSA64_TV_PAL ? "PAL" : "NTSC",
				    (double)(cyc - idle) / dt, cyc ? 100.0 * idle / cyc : 0.0,
				    us_emu / 1000.0 / fields_stats, us_present / 1000.0 / fields_stats, blank);
			t_stats = t2;
			cyc_stats = n64.cpu.cycles;
			idle_stats = n64.cpu.idle_cycles;
			us_emu = us_present = 0;
			fields_stats = blank = 0;
		}
	}

	savefile_close(&save_file);
	rsxkit_shutdown();
	ioPadEnd();
	salsa64_log("salida normal tras %u campos", n64.bus.vi.fields);
	salsa64_log_shutdown();
	return 0;
}
