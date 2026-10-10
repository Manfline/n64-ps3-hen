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
 * (platform/ps3/autotest.h) sale después.
 *
 * Salir: START + SELECT, o "Salir del juego" desde el menú del sistema.
 * Lo que la ROM imprime por el ISViewer va al log.
 */
#include <io/pad.h>
#include <malloc.h>
#include <ppu-types.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

#include <rsxkit/rsxkit.h>

#include "core/boot.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "selftest.h"
#include "vitest_rom_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define ROM_PATH     SALSA64_DATA_DIR "/rom.z64"
#define ROM_MAX      (64u << 20)
#define FB_MAX_W     640
#define FB_MAX_H     576
#define STATS_US     5000000u

static struct salsa64_n64 n64;
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static volatile int running = 1;
static int selftest_on;

static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)param, (void)user;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

static int exit_pressed(void)
{
	padInfo info;
	padData data;

	if (ioPadGetInfo(&info) != 0)
		return 0;
	for (int i = 0; i < MAX_PADS; i++)
		if (info.status[i] && ioPadGetData(i, &data) == 0 && data.BTN_START && data.BTN_SELECT)
			return 1;
	return 0;
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

	ioPadInit(7);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	if (selftest_on)
		selftest_start();

	const rsxkit_view view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f };
	enum selftest_state result = SELFTEST_RUNNING;
	uint64_t t_stats = sysGetSystemTime(), cyc_stats = n64.cpu.cycles;
	uint64_t us_emu = 0, us_present = 0;
	uint32_t fields_stats = 0, blank = 0;

	while (running) {
		sysUtilCheckCallback();
		if (exit_pressed())
			break;

		uint64_t t0 = sysGetSystemTime();
		salsa64_n64_run_field(&n64);
		uint64_t t1 = sysGetSystemTime();

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
			}
		}

		uint64_t dt = t2 - t_stats;
		if (dt >= STATS_US) {
			uint64_t cyc = n64.cpu.cycles - cyc_stats;
			salsa64_log("%.1f campos/s (%.0f %% de %s), %.1f MIPS; por campo: emulación %.1f ms, "
				    "presentación %.1f ms; %u sin imagen",
				    fields_stats * 1e6 / dt,
				    fields_stats * 1e6 / dt * 100.0 * n64.bus.vi.field_cycles / 93750000.0,
				    info.tv == SALSA64_TV_PAL ? "PAL" : "NTSC",
				    (double)cyc / dt, us_emu / 1000.0 / fields_stats,
				    us_present / 1000.0 / fields_stats, blank);
			t_stats = t2;
			cyc_stats = n64.cpu.cycles;
			us_emu = us_present = 0;
			fields_stats = blank = 0;
		}
	}

	rsxkit_shutdown();
	ioPadEnd();
	salsa64_log("salida normal tras %u campos", n64.bus.vi.fields);
	salsa64_log_shutdown();
	return 0;
}
