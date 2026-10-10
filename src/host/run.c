/*
 * Salsa64 — ejecutor de línea de comandos para el host, para pruebas
 * automatizadas (tools/test-roms.sh). No depende de PSL1GHT.
 *
 *   salsa64-run [opciones] rom.z64|rom.v64|rom.n64
 *
 * Carga la ROM, la arranca por HLE (core/boot.c) y corre el intérprete en
 * bloques de ciclos hasta que se cumple una condición. Lo que la ROM imprime
 * por el ISViewer sale por stdout; el resumen, por stderr.
 *
 * Toda la memoria se reserva antes de empezar a emular: la RDRAM y el búfer
 * del ISViewer son estáticos y la ROM se lee una vez con malloc.
 *
 * Con --frame, al terminar se escribe en PPM la imagen que muestra el VI.
 * Con --no-idle no se saltan los bucles de espera (cpu/idle.c): el
 * resultado debe ser idéntico, solo más lento.
 *
 * Guardado del cartucho (core/save.c): el tipo sale de la cabecera o de la
 * tabla por código de juego, o de --save. Con --save-dir se lee al empezar
 * y se escribe al terminar en ese directorio, con el mismo nombre que en la
 * PS3 (frontend/savefile.c); sin la opción queda solo en memoria.
 *
 * Con --rec corre el recompilador (solo en un host ppc64 big-endian: qemu,
 * tools/test-rec.sh). --lockstep además corre un segundo N64 con el
 * intérprete y, después de cada bloque recompilado, lo lleva al mismo ciclo
 * y compara el CPU (y cada tanto la RDRAM): la primera diferencia termina
 * con código 4 y el detalle.
 *
 * Códigos de salida:
 *   0  apareció el texto de --until-text, o se agotaron los ciclos sin pedirlo
 *   1  apareció alguno de los textos de --fail-text
 *   2  se agotaron los ciclos esperando --until-text, o el CPU quedó en un
 *      bucle infinito sin interrupciones posibles
 *   3  error de argumentos o de carga
 *   4  --lockstep encontró una diferencia con el intérprete
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/boot.h"
#include "core/endian.h"
#include "cpu/rec.h"
#include "host/rec_host.h"
#include "frontend/savefile.h"

#define DEFAULT_CYCLES (1ull << 32)  /* ~46 s de la consola */
#define DEFAULT_SLICE  (1ull << 20)
#define MAX_TEXT       255
#define MAX_FAIL_TEXTS 4

enum stop_reason {
	STOP_NONE,
	STOP_UNTIL,
	STOP_FAIL,
	STOP_CYCLES,
	STOP_STUCK,
};

static struct salsa64_n64 n64;
static uint8_t rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct salsa64_rec rec;

/* --lockstep: la máquina de referencia, solo con el intérprete. */
static struct salsa64_n64 ref;
static uint8_t ref_rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static uint8_t ref_isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct {
	uint64_t exits;
	uint64_t ram_every;
	uint64_t history[16];   /* últimos bloques, para el informe */
	uint32_t fields;        /* campo del VI de la última comparación de la RDRAM */
} lock;

static struct salsa64_save save;
static struct savefile save_file;

/* Búsqueda de texto en la salida, también entre dos impresiones. */
static struct {
	const char *until, *fail[MAX_FAIL_TEXTS];
	unsigned nfail;
	size_t keep;
	size_t tail_len;
	char window[MAX_TEXT + SALSA64_ISV_SIZE];
	bool quiet;
	bool midline;  /* la última impresión no terminó en salto de línea */
	enum stop_reason hit;
} watch;

static void isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	(void)ctx;
	if (!watch.quiet && len) {
		fwrite(text, 1, len, stdout);
		watch.midline = text[len - 1] != '\n';
	}

	size_t n = watch.tail_len + len;
	memcpy(watch.window + watch.tail_len, text, len);
	if (watch.hit == STOP_NONE) {
		for (unsigned k = 0; k < watch.nfail; k++)
			if (memmem(watch.window, n, watch.fail[k], strlen(watch.fail[k])))
				watch.hit = STOP_FAIL;
		if (watch.hit == STOP_NONE && watch.until &&
		    memmem(watch.window, n, watch.until, strlen(watch.until)))
			watch.hit = STOP_UNTIL;
		if (watch.hit != STOP_NONE)
			salsa64_n64_stop(&n64);
	}
	watch.tail_len = n < watch.keep ? n : watch.keep;
	memmove(watch.window, watch.window + n - watch.tail_len, watch.tail_len);
}

static void ref_isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	(void)ctx, (void)text, (void)len;
}

#define DIFF(name, a, b)                                                                        \
	do {                                                                                    \
		if ((uint64_t)(a) != (uint64_t)(b)) {                                           \
			fprintf(stderr, "  %-10s rec=%016llx intérprete=%016llx\n", name,      \
				(unsigned long long)(a), (unsigned long long)(b));              \
			bad = true;                                                             \
		}                                                                               \
	} while (0)

/* Después de cada salida del código generado: la referencia al mismo ciclo y
 * comparar. */
static void lockstep_check(struct salsa64_rec *r, uint64_t block_pc, void *ctx)
{
	(void)r, (void)ctx;
	const struct salsa64_cpu *a = &n64.cpu, *b = &ref.cpu;
	lock.history[lock.exits % 16] = block_pc;
	lock.exits++;
	/* La referencia despacha los eventos que vencen en el ciclo al que
	 * llega; el despachador del recompilador lo haría en su próxima vuelta. */
	if (a->cycles >= n64.sched.next)
		salsa64_sched_dispatch(&n64.sched, a->cycles);
	if (b->cycles < a->cycles)
		salsa64_n64_run(&ref, a->cycles - b->cycles);

	bool bad = false;
	char name[16];
	DIFF("cycles", a->cycles, b->cycles);
	DIFF("pc", a->pc, b->pc);
	DIFF("next_pc", a->next_pc, b->next_pc);
	DIFF("branch", a->branch, b->branch);
	for (int k = 1; k < 32; k++) {
		snprintf(name, sizeof name, "r%d", k);
		DIFF(name, a->gpr[k], b->gpr[k]);
	}
	DIFF("hi", a->hi, b->hi);
	DIFF("lo", a->lo, b->lo);
	for (int k = 0; k < 32; k++) {
		snprintf(name, sizeof name, "f%d", k);
		DIFF(name, a->fpr[k], b->fpr[k]);
	}
	DIFF("fcr31", a->fcr31, b->fcr31);
	DIFF("status", a->status, b->status);
	DIFF("cause", a->cause, b->cause);
	DIFF("epc", a->epc, b->epc);
	DIFF("errorepc", a->error_epc, b->error_epc);
	DIFF("badvaddr", a->bad_vaddr, b->bad_vaddr);
	DIFF("compare", a->compare, b->compare);
	DIFF("count_off", a->count_offset, b->count_offset);
	DIFF("llbit", a->llbit, b->llbit);
	DIFF("entryhi", a->entry_hi, b->entry_hi);
	DIFF("mi.intr", n64.bus.mi.intr, ref.bus.mi.intr);
	DIFF("vi.fields", n64.bus.vi.fields, ref.bus.vi.fields);
	if (!bad && (lock.exits % lock.ram_every == 0 || n64.bus.vi.fields != lock.fields)) {
		lock.fields = n64.bus.vi.fields;
		for (uint32_t pa = 0; pa < n64.bus.rdram_size; pa += 4) {
			if (salsa64_rd32(rdram + pa) != salsa64_rd32(ref_rdram + pa)) {
				fprintf(stderr, "  RDRAM %08x rec=%08x intérprete=%08x\n", pa,
					salsa64_rd32(rdram + pa), salsa64_rd32(ref_rdram + pa));
				bad = true;
				break;
			}
		}
	}
	if (!bad)
		return;
	fprintf(stderr, "salsa64-run: --lockstep: diferencia después del bloque %016llx (salida %llu, ciclo %llu)\n"
		"  bloques anteriores:", (unsigned long long)block_pc, (unsigned long long)lock.exits,
		(unsigned long long)a->cycles);
	for (unsigned k = 1; k <= 16 && k <= lock.exits; k++)
		fprintf(stderr, " %08llx", (unsigned long long)(uint32_t)lock.history[(lock.exits - k) % 16]);
	fputc('\n', stderr);
	exit(4);
}

static void save_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("salsa64-run: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* Instrucción en una dirección de KSEG0/KSEG1, o 0 si no cae en la RDRAM. */
static uint32_t peek(uint64_t va)
{
	uint32_t pa = (uint32_t)va & 0x1FFFFFFF;
	if ((va >> 30) != 0x3FFFFFFFEull || pa + 4 > n64.bus.rdram_size)
		return 0;
	return salsa64_rd32(n64.bus.rdram + pa);
}

static uint64_t trace_exc_left;

static void trace_exc(struct salsa64_cpu *cpu)
{
	if (!trace_exc_left)
		return;
	trace_exc_left--;
	fprintf(stderr, "excepción %2u en ciclo %llu: pc=%016llx [%08x] epc=%016llx bd=%u badvaddr=%016llx status=%08x\n",
		SALSA64_CAUSE_EXC(cpu->cause), (unsigned long long)cpu->cycles,
		(unsigned long long)cpu->cur_pc, peek(cpu->cur_pc), (unsigned long long)cpu->epc,
		cpu->cause >> 31, (unsigned long long)cpu->bad_vaddr, cpu->status);
}

/* `b .` (o `j .`) con NOP en el delay slot y sin interrupción que lo saque.
 * El bloque puede terminar sobre el salto o sobre su delay slot. */
static bool cpu_stuck(void)
{
	const struct salsa64_cpu *cpu = &n64.cpu;
	uint64_t at = cpu->branch ? cpu->pc - 4 : cpu->pc;
	uint32_t i = peek(at);
	bool self = i == 0x1000FFFFu ||
		    (i >> 26 == 2 && (i & 0x03FFFFFF) == (((uint32_t)at >> 2) & 0x03FFFFFF));
	if (!self || peek(at + 4) != 0 || (cpu->branch && cpu->next_pc != at))
		return false;
	uint32_t st = cpu->status;
	bool irq_possible = (st & SALSA64_ST_IE) && !(st & (SALSA64_ST_EXL | SALSA64_ST_ERL)) &&
			    (st & SALSA64_ST_IM);
	return !irq_possible;
}

static uint8_t *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t *buf = NULL;
	long len;
	if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
	    (buf = malloc((size_t)len)) && fread(buf, 1, (size_t)len, f) == (size_t)len) {
		*size = (size_t)len;
	} else {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

static void dump_regs(void)
{
	static const char *const names[32] = {
		"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
	};
	const struct salsa64_cpu *cpu = &n64.cpu;
	for (int r = 0; r < 32; r++)
		fprintf(stderr, "%s=%016llx%s", names[r], (unsigned long long)cpu->gpr[r], r % 4 == 3 ? "\n" : " ");
	fprintf(stderr, "pc=%016llx status=%08x cause=%08x epc=%016llx badvaddr=%016llx\n",
		(unsigned long long)cpu->pc, cpu->status, cpu->cause,
		(unsigned long long)cpu->epc, (unsigned long long)cpu->bad_vaddr);
}

/* Imagen del VI en PPM (P6), RGBA5551 o RGBA8888 → RGB de 8 bits. */
static bool dump_frame(const char *path)
{
	struct salsa64_vi_frame f;
	if (!salsa64_vi_frame(&n64.bus, &f)) {
		fprintf(stderr, "salsa64-run: el VI no muestra imagen (VI_CONTROL = 0x%08x)\n",
			n64.bus.vi.regs[SALSA64_VI_CONTROL]);
		return false;
	}
	FILE *out = fopen(path, "wb");
	if (!out)
		return false;
	fprintf(out, "P6\n%u %u\n255\n", f.width, f.height);
	for (uint32_t y = 0; y < f.height; y++) {
		const uint8_t *line = n64.bus.rdram + f.origin + y * f.stride;
		for (uint32_t x = 0; x < f.width; x++) {
			uint8_t rgb[3];
			if (f.bpp == 2) {
				uint32_t v = salsa64_rd16(line + 2 * x);
				uint32_t c[3] = { v >> 11, (v >> 6) & 31, (v >> 1) & 31 };
				for (int k = 0; k < 3; k++)
					rgb[k] = (uint8_t)(c[k] << 3 | c[k] >> 2);
			} else {
				memcpy(rgb, line + 4 * x, 3);
			}
			fwrite(rgb, 1, 3, out);
		}
	}
	fprintf(stderr, "salsa64-run: imagen del VI %ux%u (%s, origen 0x%06x) en %s; %u campos\n",
		f.width, f.height, f.bpp == 2 ? "RGBA5551" : "RGBA8888", f.origin, path,
		n64.bus.vi.fields);
	return fclose(out) == 0;
}

static void usage(FILE *out)
{
	fputs("uso: salsa64-run [opciones] rom\n"
	      "  -c, --cycles N        límite de ciclos del VR4300 (por omisión 2^32)\n"
	      "  -u, --until-text T    terminar (código 0) cuando el ISViewer imprima T\n"
	      "  -f, --fail-text T     terminar con código 1 si el ISViewer imprime T (hasta 4)\n"
	      "      --tv ntsc|pal|mpal  tipo de TV (por omisión, según el encabezado)\n"
	      "      --ram 4|8         MB de RDRAM (por omisión 8)\n"
	      "      --slice N         ciclos por bloque entre verificaciones (2^20)\n"
	      "  -q, --quiet           no copiar la salida del ISViewer a stdout\n"
	      "  -r, --regs            volcar los registros del CPU al terminar\n"
	      "      --trace-exc N     mostrar las primeras N excepciones del CPU\n"
	      "      --frame ARCHIVO   al terminar, guardar en PPM la imagen del VI\n"
	      "      --rec             usar el recompilador (host ppc64 big-endian)\n"
	      "      --lockstep[=N]    con --rec, comparar contra el intérprete después de cada\n"
	      "                        bloque, y la RDRAM en cada campo del VI y cada N bloques\n"
	      "                        (por omisión 4096)\n"
	      "      --no-idle         no saltar los bucles de espera\n"
	      "      --save TIPO       tipo de guardado: ninguno, eeprom4k, eeprom16k, sram256k\n"
	      "      --save-dir DIR    leer y escribir el guardado del cartucho en DIR\n", out);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "cycles", required_argument, NULL, 'c' },
		{ "until-text", required_argument, NULL, 'u' },
		{ "fail-text", required_argument, NULL, 'f' },
		{ "tv", required_argument, NULL, 'T' },
		{ "ram", required_argument, NULL, 'R' },
		{ "slice", required_argument, NULL, 'S' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "regs", no_argument, NULL, 'r' },
		{ "trace-exc", required_argument, NULL, 'X' },
		{ "frame", required_argument, NULL, 'F' },
		{ "rec", no_argument, NULL, 'E' },
		{ "lockstep", optional_argument, NULL, 'L' },
		{ "no-idle", no_argument, NULL, 'I' },
		{ "save", required_argument, NULL, 'V' },
		{ "save-dir", required_argument, NULL, 'D' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	uint64_t max_cycles = DEFAULT_CYCLES, slice = DEFAULT_SLICE;
	uint32_t ram = SALSA64_RDRAM_MAX;
	int tv = SALSA64_BOOT_TV_AUTO, save_type = SALSA64_SAVE_AUTO, c;
	bool regs = false, idle = true, use_rec = false, lockstep = false;
	const char *frame_path = NULL, *save_dir = NULL;

	while ((c = getopt_long(argc, argv, "c:u:f:qrh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': max_cycles = strtoull(optarg, NULL, 0); break;
		case 'u': watch.until = optarg; break;
		case 'f':
			if (watch.nfail == MAX_FAIL_TEXTS) {
				usage(stderr);
				return 3;
			}
			watch.fail[watch.nfail++] = optarg;
			break;
		case 'S': slice = strtoull(optarg, NULL, 0); break;
		case 'q': watch.quiet = true; break;
		case 'r': regs = true; break;
		case 'X': trace_exc_left = strtoull(optarg, NULL, 0); break;
		case 'F': frame_path = optarg; break;
		case 'E': use_rec = true; break;
		case 'L':
			use_rec = lockstep = true;
			lock.ram_every = optarg ? strtoull(optarg, NULL, 0) : 4096;
			if (!lock.ram_every)
				lock.ram_every = 1;
			break;
		case 'I': idle = false; break;
		case 'D': save_dir = optarg; break;
		case 'V':
			for (int t = 0; t < SALSA64_SAVE_NTYPES; t++)
				if (!strcmp(optarg, salsa64_save_name(t)))
					save_type = t;
			if (save_type == SALSA64_SAVE_AUTO) {
				usage(stderr);
				return 3;
			}
			break;
		case 'T':
			if (!strcmp(optarg, "ntsc")) tv = SALSA64_TV_NTSC;
			else if (!strcmp(optarg, "pal")) tv = SALSA64_TV_PAL;
			else if (!strcmp(optarg, "mpal")) tv = SALSA64_TV_MPAL;
			else { usage(stderr); return 3; }
			break;
		case 'R':
			if (!strcmp(optarg, "4")) ram = 4u << 20;
			else if (!strcmp(optarg, "8")) ram = 8u << 20;
			else { usage(stderr); return 3; }
			break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 3;
		}
	}
	size_t longest = watch.until ? strlen(watch.until) : 0;
	for (unsigned k = 0; k < watch.nfail; k++)
		if (strlen(watch.fail[k]) > longest)
			longest = strlen(watch.fail[k]);
	if (optind != argc - 1 || slice == 0 || longest > MAX_TEXT) {
		usage(stderr);
		return 3;
	}
	watch.keep = longest ? longest - 1 : 0;

	const char *path = argv[optind];
	size_t rom_size = 0;
	uint8_t *rom = read_file(path, &rom_size);
	if (!rom) {
		fprintf(stderr, "salsa64-run: no se pudo leer %s\n", path);
		return 3;
	}
	static const char *const fmt_names[] = { ".z64", ".v64 (convertida)", ".n64 (convertida)" };
	enum salsa64_rom_format fmt = salsa64_rom_normalize(rom, rom_size);

	salsa64_n64_init(&n64, rdram, ram);
	salsa64_cpu_set_idle(&n64.cpu, idle);
	if (trace_exc_left)
		n64.cpu.exc_hook = trace_exc;
	salsa64_bus_set_isviewer(&n64.bus, isv_buf, isv_out, NULL);
	salsa64_bus_set_rom(&n64.bus, rom, (uint32_t)(rom_size > UINT32_MAX ? UINT32_MAX : rom_size));
	struct salsa64_boot_info info;
	if (!salsa64_boot_hle(&n64, tv, &info)) {
		fprintf(stderr, "salsa64-run: no se pudo arrancar %s: %s\n", path, info.error);
		return 3;
	}
	static const char *const tv_names[] = { "PAL", "NTSC", "MPAL" };
	if (info.kind == SALSA64_BOOT_ELF)
		fprintf(stderr, "salsa64-run: %s %s, %zu bytes; ELF en 0x%X (IPL3 libre), %u bytes cargados, "
			"entrada 0x%08X, TV %s, RDRAM %u MB\n", path, fmt_names[fmt], rom_size, info.elf_offset,
			info.loaded, info.entry, tv_names[info.tv], ram >> 20);
	else
		fprintf(stderr, "salsa64-run: %s %s, %zu bytes; carga plana (CIC %s), %u bytes, "
			"entrada 0x%08X, TV %s, RDRAM %u MB\n", path, fmt_names[fmt], rom_size,
			salsa64_cic_name(info.cic), info.loaded, info.entry, tv_names[info.tv], ram >> 20);
	savefile_open(&save_file, &save, &n64.bus, rom, save_dir, NULL, save_type, save_log);

	if (use_rec) {
		void *code = salsa64_host_rec_buffer();
		rec.opts = salsa64_host_rec_opts();
		if (!code || !salsa64_rec_init(&rec, &n64.cpu, code, SALSA64_HOST_REC_BYTES)) {
			fprintf(stderr, "salsa64-run: el recompilador no corre en este host\n");
			return 3;
		}
		if (lockstep) {
			/* La referencia arranca igual; cada bloque vuelve a C para comparar. */
			salsa64_n64_init(&ref, ref_rdram, ram);
			salsa64_cpu_set_idle(&ref.cpu, idle);
			salsa64_bus_set_isviewer(&ref.bus, ref_isv_buf, ref_isv_out, NULL);
			salsa64_bus_set_rom(&ref.bus, rom, (uint32_t)(rom_size > UINT32_MAX ? UINT32_MAX : rom_size));
			struct salsa64_boot_info ref_info;
			if (!salsa64_boot_hle(&ref, tv, &ref_info)) {
				fprintf(stderr, "salsa64-run: la referencia no arrancó\n");
				return 3;
			}
			rec.no_link = true;
			rec.after_exit = lockstep_check;
		}
		fprintf(stderr, "salsa64-run: recompilador activo%s%s; caché en %p, llamadas a C %s\n",
			lockstep ? " con --lockstep" : "", rec.opts & SALSA64_REC_OPT_PIN ? ", registros fijos" : "",
			code, rec.calls_near ? "con bl" : "por mtctr/bctrl");
	}

	/* Bucle principal: bloques de `slice` ciclos y verificación entre ellos. */
	enum stop_reason reason = STOP_NONE;
	uint64_t start_cycles = n64.cpu.cycles, start_idle = n64.cpu.idle_cycles, t0 = now_us();
	while (reason == STOP_NONE) {
		uint64_t done = n64.cpu.cycles - start_cycles;
		if (done >= max_cycles) {
			reason = STOP_CYCLES;
			break;
		}
		salsa64_n64_run(&n64, max_cycles - done < slice ? max_cycles - done : slice);
		if (watch.hit != STOP_NONE)
			reason = watch.hit;
		else if (cpu_stuck())
			reason = STOP_STUCK;
	}
	uint64_t us = now_us() - t0, cycles = n64.cpu.cycles - start_cycles;
	uint64_t idle_cycles = n64.cpu.idle_cycles - start_idle;
	if (watch.midline)
		putchar('\n');
	fflush(stdout);

	static const char *const reason_names[] = {
		[STOP_UNTIL] = "apareció el texto esperado",
		[STOP_FAIL] = "apareció el texto de falla",
		[STOP_CYCLES] = "se agotaron los ciclos",
		[STOP_STUCK] = "bucle infinito sin interrupciones posibles",
	};
	/* Una instrucción por ciclo, salvo los ciclos saltados en bucles de espera. */
	fprintf(stderr, "salsa64-run: %s; %llu ciclos (%.2f s de consola) en %.2f s, %.1f MIPS, "
		"%.1f %% en bucles de espera, pc=0x%016llx\n",
		reason_names[reason], (unsigned long long)cycles, cycles / 93.75e6, us / 1e6,
		us ? (cycles - idle_cycles) / (double)us : 0.0,
		cycles ? 100.0 * idle_cycles / cycles : 0.0, (unsigned long long)n64.cpu.pc);
	if (use_rec) {
		const struct salsa64_rec_stats *st = &rec.stats;
		fprintf(stderr, "salsa64-run: recompilador: %llu bloques (%u KB de caché), %llu solo intérprete, "
			"%llu entradas, %llu enlaces, %llu invalidados, %llu vaciados, %llu instr. interpretadas (%.1f %%)%s\n",
			(unsigned long long)st->compiled, salsa64_rec_used(&rec) / 256,
			(unsigned long long)st->interp_only, (unsigned long long)st->entries,
			(unsigned long long)st->linked, (unsigned long long)st->invalidated,
			(unsigned long long)st->flushes, (unsigned long long)st->interp_steps,
			cycles ? 100.0 * (double)st->interp_steps / (double)cycles : 0.0,
			lockstep ? "; --lockstep sin diferencias" : "");
	}
	savefile_close(&save_file);
	if (regs)
		dump_regs();
	if (frame_path && !dump_frame(frame_path)) {
		free(rom);
		return 3;
	}
	free(rom);

	switch (reason) {
	case STOP_FAIL:   return 1;
	case STOP_STUCK:  return 2;
	case STOP_CYCLES: return watch.until ? 2 : 0;
	default:          return 0;
	}
}
