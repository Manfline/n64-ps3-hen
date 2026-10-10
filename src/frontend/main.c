/*
 * Salsa64 — frontend mínimo para PS3.
 *
 * Carga una ROM, la arranca por HLE y, en cada campo del VI, corre el
 * intérprete hasta el fin del campo y presenta la imagen del VI con
 * lib/rsxkit: si la dibujó el RSX (tareas gráficas del RSP por HLE en un
 * SPE, src/rdp), ese destino de render; si no, el framebuffer de la RDRAM
 * como textura. Escalado a 4:3. La emulación y la presentación van en un
 * hilo del PPE por ahora.
 *
 * ROM: si existe SALSA64_DATA_DIR/embedded con "vitest" o "gfxtest", esa
 * ROM de prueba embebida (o con "file:<nombre>", ese archivo de
 * SALSA64_DATA_DIR, para probar una ROM en RPCS3 sin pisar el rom.z64 que
 * leen otras compilaciones, o con "dump:<nombre>", la repetición de un
 * volcado de la HLE en el RSX: replay.c); si no, SALSA64_DATA_DIR/rom.z64
 * (cualquiera de los tres órdenes de bytes) si existe; si no, gfxtest. Con las ROMs de
 * prueba el frontend se verifica a sí mismo (selftest.c, selftest_gfx.c) e
 * imprime "RESULTADO frontend: OK/FALLO"; en modo de prueba automática
 * (platform/ps3/autotest.h) sale después.
 *
 * HLE gráfica: en el SPE 0 (lib/spukit); con SALSA64_DATA_DIR/gfx_ppe
 * presente, en el PPE (para comparar tiempos).
 *
 * Capturas: con una ROM del disco, cada SHOT_US se guarda la pantalla en
 * SALSA64_DATA_DIR/screen-N.ppm (N de 1 a SHOT_MAX, en ronda), para verla
 * desde la Mac por FTP. En modo de prueba automática se captura cada
 * AUTO_SHOT_FIELDS campos y se sale tras AUTO_FIELDS (para correr una ROM
 * en RPCS3 sin manos). Si SALSA64_DATA_DIR/gfxdump dice un número de campo,
 * en ese campo se vuelca la salida de la última tarea gráfica a
 * SALSA64_DATA_DIR/gfxdump.bin (salsa64-run --raster-dump la dibuja).
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
#include <spukit/spukit.h>

#include "core/boot.h"
#include "gfxtest_rom_bin.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/gfx_spu.h"
#include "platform/ps3/log.h"
#include "rdp/rdp.h"
#include "rsp/gfx/hle.h"
#include "replay.h"
#include "screenshot.h"
#include "selftest.h"
#include "selftest_gfx.h"
#include "vitest_rom_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define ROM_PATH     SALSA64_DATA_DIR "/rom.z64"
#define EMBED_PATH   SALSA64_DATA_DIR "/embedded"
#define GFX_PPE_PATH SALSA64_DATA_DIR "/gfx_ppe"
#define ROM_MAX      (64u << 20)
#define FB_MAX_W     640
#define FB_MAX_H     576
#define STATS_US     5000000u
#define SHOT_US      10000000u
#define SHOT_MAX     9
#define AUTO_SHOT_FIELDS 300
#define AUTO_FIELDS      1800

static struct salsa64_n64 n64;
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static volatile int running = 1;
static int selftest_on;   /* vitest */
static int gfxtest_on;    /* gfxtest */
static struct salsa64_gfx_spu gfx_spu;
static struct salsa64_gfx_sync gfx_ppe;
static struct gfx_state gfx_ppe_state;

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
	if (gfxtest_on)
		gfxtest_isv(text, len);
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

static uint8_t *embedded(const uint8_t *bin, uint32_t bin_size, uint32_t *size)
{
	uint8_t *buf = memalign(128, bin_size);
	if (buf) {
		memcpy(buf, bin, bin_size);
		*size = bin_size;
	}
	return buf;
}

/* ROM embebida elegida en EMBED_PATH, o NULL. */
static uint8_t *load_embedded(uint32_t *size, const char **source)
{
	char name[80] = "";
	FILE *f = fopen(EMBED_PATH, "r");
	if (!f)
		return NULL;
	if (!fgets(name, sizeof name, f))
		name[0] = '\0';
	fclose(f);
	name[strcspn(name, " \r\n")] = '\0';
	if (!strcmp(name, "vitest")) {
		*source = "vitest (embebida)";
		selftest_on = 1;
		return embedded(vitest_rom_bin, vitest_rom_bin_size, size);
	}
	if (!strcmp(name, "gfxtest")) {
		*source = "gfxtest (embebida)";
		gfxtest_on = 1;
		return embedded(gfxtest_rom_bin, gfxtest_rom_bin_size, size);
	}
	if (!strncmp(name, "file:", 5)) {
		static char path[96];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/%s", name + 5);
		FILE *rf = fopen(path, "rb");
		long len = -1;
		if (rf && fseek(rf, 0, SEEK_END) == 0)
			len = ftell(rf);
		uint8_t *buf = NULL;
		if (rf && len >= 0x1000 && (uint32_t)len <= ROM_MAX && fseek(rf, 0, SEEK_SET) == 0 &&
		    (buf = memalign(128, (size_t)len)) && fread(buf, 1, (size_t)len, rf) == (size_t)len) {
			fclose(rf);
			*size = (uint32_t)len;
			*source = path;
			return buf;
		}
		free(buf);
		if (rf)
			fclose(rf);
		salsa64_log("no se pudo leer %s", path);
		return NULL;
	}
	salsa64_log("%s dice \"%s\": no es vitest, gfxtest ni file:<nombre>", EMBED_PATH, name);
	return NULL;
}

/* ROM del disco (preasignada una sola vez) o la embebida. */
static uint8_t *load_rom(uint32_t *size, const char **source)
{
	uint8_t *rom = load_embedded(size, source);
	if (rom)
		return rom;
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
	*source = "gfxtest (embebida)";
	gfxtest_on = 1;
	return embedded(gfxtest_rom_bin, gfxtest_rom_bin_size, size);
}

static int file_exists(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f)
		fclose(f);
	return f != NULL;
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
	if ((rc = salsa64_rdp_init(0)) != RSXKIT_OK) {
		salsa64_log("RESULTADO frontend: FALLO al preparar el RDP (%d)", rc);
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}

	{
		char name[80] = "";
		FILE *ef = fopen(EMBED_PATH, "r");
		if (ef) {
			if (!fgets(name, sizeof name, ef))
				name[0] = '\0';
			fclose(ef);
			name[strcspn(name, " \r\n")] = '\0';
		}
		if (!strncmp(name, "dump:", 5)) {
			char path[96];
			snprintf(path, sizeof path, SALSA64_DATA_DIR "/%s", name + 5);
			int rr = salsa64_replay(path);
			salsa64_log("RESULTADO frontend: %s (repetición)", rr == 0 ? "OK" : "FALLO");
			salsa64_rdp_shutdown();
			rsxkit_shutdown();
			salsa64_log_shutdown();
			return rr == 0 ? 0 : 1;
		}
	}

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

	/* HLE gráfica: en un SPE, o en el PPE si se pide. */
	int gfx_on_spu = !file_exists(GFX_PPE_PATH);
	if (gfx_on_spu) {
		spukit_init(6);
		if ((rc = salsa64_gfx_spu_init(&gfx_spu, &n64.bus, salsa64_rdp_sink())) != 0) {
			salsa64_log("no arrancó el SPE de la HLE gráfica (%d): uso el PPE", rc);
			gfx_on_spu = 0;
		}
	}
	if (gfx_on_spu) {
		salsa64_rsp_set_gfx(&n64.bus, &gfx_spu.backend);
	} else {
		salsa64_gfx_sync_init(&gfx_ppe, &gfx_ppe_state, &n64.bus, salsa64_rdp_sink());
		salsa64_rsp_set_gfx(&n64.bus, &gfx_ppe.backend);
	}
	salsa64_log("HLE gráfica en %s", gfx_on_spu ? "el SPE" : "el PPE");

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
	if (gfxtest_on)
		gfxtest_start();

	const rsxkit_view view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f };
	enum selftest_state result = SELFTEST_RUNNING;
	uint64_t t_stats = sysGetSystemTime(), cyc_stats = n64.cpu.cycles;
	uint64_t t_shot = t_stats;
	uint32_t shots = 0, dump_field = 0;
	{
		FILE *df = fopen(SALSA64_DATA_DIR "/gfxdump", "r");
		if (df) {
			if (fscanf(df, "%u", &dump_field) != 1)
				dump_field = 0;
			fclose(df);
		}
	}
	uint64_t us_emu = 0, us_present = 0;
	uint32_t fields_stats = 0, blank = 0, rsx_frames = 0;
	struct salsa64_rdp_stats rdp0;
	salsa64_rdp_get_stats(&rdp0);
	uint32_t gfx_tasks0 = 0, gfx_waits0 = 0;
	uint64_t gfx_ticks0 = 0, gfx_wait0 = 0;
	uint64_t gfx_vtx0 = 0, gfx_state0 = 0, gfx_tex0 = 0, gfx_load0 = 0;

	while (running) {
		sysUtilCheckCallback();
		if (exit_pressed())
			break;

		uint64_t t0 = sysGetSystemTime();
		salsa64_n64_run_field(&n64);
		uint64_t t1 = sysGetSystemTime();

		struct salsa64_vi_frame f = { 0 };
		rsxkit_rect r = { 0, 0, 0, 0 };
		bool check = false, check_gfx = false;
		rsxkit_frame_begin(0x000000);
		if (salsa64_vi_frame(&n64.bus, &f) && f.width <= FB_MAX_W && f.height <= FB_MAX_H) {
			if (salsa64_rdp_present(&f, n64.bus.vi.fields, &view, &r)) {
				rsx_frames++;
				if (gfxtest_on && result == SELFTEST_RUNNING)
					check_gfx = gfxtest_want_screen();
			} else {
				if (selftest_on && result == SELFTEST_RUNNING)
					check = selftest_frame(&n64.bus, &f);
				rsxkit_fb_upload(fb, n64.bus.rdram + f.origin, f.width, f.height, f.stride,
						 f.bpp == 4 ? RSXKIT_RGBA8888 : RSXKIT_RGBA5551);
				rsxkit_fb_draw(fb, &view, &r);
			}
		} else {
			blank++;
		}
		if (check || check_gfx) {
			rsxkit_finish();
			if (check)
				selftest_screen(&f, &r);
			else
				gfxtest_screen(&f, &r);
		} else if (!selftest_on && !gfxtest_on && f.width &&
			   (autotest ? n64.bus.vi.fields / AUTO_SHOT_FIELDS != shots
				     : t1 - t_shot >= SHOT_US)) {
			char path[64];
			snprintf(path, sizeof path, SALSA64_DATA_DIR "/screen-%u.ppm", shots % SHOT_MAX + 1);
			rsxkit_finish();
			salsa64_screenshot(path, &f, &r);
			salsa64_log("captura %s (campo %u)", path, n64.bus.vi.fields);
			shots = autotest ? n64.bus.vi.fields / AUTO_SHOT_FIELDS : shots + 1;
			t_shot = t1;
		}
		if (dump_field && n64.bus.vi.fields >= dump_field) {
			rsxkit_finish();
			salsa64_log("volcado de la HLE en el campo %u: %s", n64.bus.vi.fields,
				    salsa64_rdp_dump(SALSA64_DATA_DIR "/gfxdump.bin") ? "bien" : "FALLO");
			dump_field = 0;
		}
		if (autotest && !selftest_on && !gfxtest_on && n64.bus.vi.fields >= AUTO_FIELDS) {
			salsa64_log("RESULTADO frontend: OK (ROM del disco, %u campos)", n64.bus.vi.fields);
			break;
		}
		rsxkit_frame_end();
		uint64_t t2 = sysGetSystemTime();
		us_emu += t1 - t0;
		us_present += t2 - t1;
		fields_stats++;

		if ((selftest_on || gfxtest_on) && result == SELFTEST_RUNNING) {
			result = selftest_on ? selftest_state(n64.bus.vi.fields) : gfxtest_state(n64.bus.vi.fields);
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
				    "presentación %.1f ms; %u sin imagen, %u del RSX",
				    fields_stats * 1e6 / dt,
				    fields_stats * 1e6 / dt * 100.0 * n64.bus.vi.field_cycles / 93750000.0,
				    info.tv == SALSA64_TV_PAL ? "PAL" : "NTSC",
				    (double)cyc / dt, us_emu / 1000.0 / fields_stats,
				    us_present / 1000.0 / fields_stats, blank, rsx_frames);
			/* Tareas gráficas: tiempo de la HLE (en el SPE), espera del PPE
			 * y traducción a comandos del RSX, por tarea. */
			struct salsa64_rdp_stats rs;
			salsa64_rdp_get_stats(&rs);
			uint32_t tasks = rs.tasks - rdp0.tasks;
			if (tasks) {
				double spu_ms = 0, wait_ms = 0;
				uint32_t waits = 0;
				if (gfx_on_spu) {
					spu_ms = (gfx_spu.spu_ticks - gfx_ticks0) * 1e3 / spukit_tick_hz() / tasks;
					wait_ms = (gfx_spu.wait_us - gfx_wait0) / 1e3 / tasks;
					waits = gfx_spu.waits - gfx_waits0;
				}
				salsa64_log("gfx: %u tareas (%.1f/s); por tarea: %.0f dibujos, %.0f triángulos, HLE en el "
					    "SPE %.2f ms, PPE esperando %.2f ms (%u esperas), traducción al RSX %.2f ms; "
					    "%u destinos creados, %u esperas al RSX",
					    tasks, tasks * 1e6 / dt, (double)(rs.draws - rdp0.draws) / tasks,
					    (double)(rs.triangles - rdp0.triangles) / tasks, spu_ms, wait_ms, waits,
					    (rs.us_translate - rdp0.us_translate) / 1e3 / tasks,
					    rs.targets_created - rdp0.targets_created, rs.set_waits - rdp0.set_waits);
				if (gfx_on_spu) {
					double k = 1e3 / spukit_tick_hz() / tasks;
					salsa64_log("gfx en el SPE por tarea: vértices %.2f ms, estado %.2f ms (texturas %.2f ms), "
						    "cargas a la TMEM %.2f ms", (gfx_spu.t_vtx - gfx_vtx0) * k,
						    (gfx_spu.t_state - gfx_state0) * k, (gfx_spu.t_tex - gfx_tex0) * k,
						    (gfx_spu.t_load - gfx_load0) * k);
				}
			}
			gfx_vtx0 = gfx_spu.t_vtx;
			gfx_state0 = gfx_spu.t_state;
			gfx_tex0 = gfx_spu.t_tex;
			gfx_load0 = gfx_spu.t_load;
			rdp0 = rs;
			gfx_tasks0 = gfx_spu.tasks;
			gfx_ticks0 = gfx_spu.spu_ticks;
			gfx_wait0 = gfx_spu.wait_us;
			gfx_waits0 = gfx_spu.waits;
			(void)gfx_tasks0;
			t_stats = t2;
			cyc_stats = n64.cpu.cycles;
			us_emu = us_present = 0;
			fields_stats = blank = rsx_frames = 0;
		}
	}

	salsa64_rsp_set_gfx(&n64.bus, NULL);  /* espera la tarea en vuelo */
	if (gfx_on_spu)
		salsa64_gfx_spu_shutdown(&gfx_spu);
	salsa64_rdp_shutdown();
	rsxkit_shutdown();
	ioPadEnd();
	salsa64_log("salida normal tras %u campos", n64.bus.vi.fields);
	salsa64_log_shutdown();
	return 0;
}
