/*
 * Salsa64 — plugin de entrada para mupen64plus que reproduce un guion de
 * control con la sintaxis del --pad de salsa64-run ("CAMPO:BOTONES[@X,Y];…",
 * cada estado vale hasta el siguiente), para sacar capturas de referencia de
 * una escena con otra HLE (tools/m64p-ref.sh).
 *
 * El campo se cuenta por tiempo real desde que arranca la ROM (el VI va a
 * 60 Hz con el limitador de velocidad de mupen64plus), no por sondeos del
 * control: Zelda OoT sondea unas dos veces por campo. M64P_PAD trae el guion,
 * M64P_PAD_OFFSET un desplazamiento en campos y M64P_PAD_PORT (1 a 4, por
 * omisión 1) el puerto al que va; los puertos anteriores están conectados y
 * quietos, como con dos mandos en la PS3.
 *
 * Compilar (macOS): cc -O2 -shared -fPIC -o mupen64plus-input-pad.dylib pad_plugin.c
 * Usar: mupen64plus --input ./mupen64plus-input-pad.dylib …
 *
 * Solo define lo que el núcleo 2.6 exige; las entradas de VRU están vacías.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { M64ERR_SUCCESS = 0 } m64p_error;
typedef enum { M64PLUGIN_INPUT = 4 } m64p_plugin_type;
typedef union { unsigned int Value; } BUTTONS;  /* bits como en m64p_plugin.h */
typedef struct { int Present, RawData, Plugin, Type; } CONTROL;
typedef struct { CONTROL *Controls; } CONTROL_INFO;

#define MAX_STEPS 512

struct step { unsigned field, value; };
static struct step steps[MAX_STEPS];
static int nsteps, next;
static unsigned cur;
static long offset;
static int port;  /* 0 a 3 */
static struct timespec t0;

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static unsigned button(const char *s, size_t n)
{
	static const struct { const char *name; unsigned bit; } tab[] = {
		{ "A", 0x80 }, { "B", 0x40 }, { "Z", 0x20 }, { "START", 0x10 }, { "DU", 0x08 }, { "DD", 0x04 },
		{ "DL", 0x02 }, { "DR", 0x01 }, { "CR", 0x100 }, { "CL", 0x200 }, { "CD", 0x400 }, { "CU", 0x800 },
		{ "R", 0x1000 }, { "L", 0x2000 },
	};
	for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
		if (strlen(tab[i].name) == n && !strncmp(tab[i].name, s, n))
			return tab[i].bit;
	return 0;  /* "-" y lo desconocido no suman nada */
}

static void parse(const char *spec)
{
	while (*spec && nsteps < MAX_STEPS) {
		char *end;
		unsigned long f = strtoul(spec, &end, 10);
		if (end == spec || *end != ':')
			break;
		spec = end + 1;
		unsigned v = 0;
		int x = 0, y = 0;
		while (*spec && *spec != ';') {
			if (*spec == '@') {
				x = (int)strtol(spec + 1, &end, 10);
				if (*end == ',')
					y = (int)strtol(end + 1, &end, 10);
				spec = end;
				continue;
			}
			size_t n = strcspn(spec, "+;@");
			v |= button(spec, n);
			spec += n;
			if (*spec == '+')
				spec++;
		}
		v |= ((unsigned)(x & 0xFF)) << 16 | ((unsigned)(y & 0xFF)) << 24;
		steps[nsteps].field = (unsigned)f;
		steps[nsteps].value = v;
		nsteps++;
		if (*spec == ';')
			spec++;
	}
}

m64p_error PluginStartup(void *core, void *ctx, void (*dbg)(void *, int, const char *))
{
	(void)core; (void)ctx; (void)dbg;
	const char *p = getenv("M64P_PAD");
	if (p)
		parse(p);
	const char *o = getenv("M64P_PAD_OFFSET");
	offset = o ? strtol(o, NULL, 10) : 0;
	const char *q = getenv("M64P_PAD_PORT");
	port = q ? (int)strtol(q, NULL, 10) - 1 : 0;
	if (port < 0 || port > 3)
		port = 0;
	fprintf(stderr, "pad_plugin: %d estados, desplazamiento %ld campos\n", nsteps, offset);
	return M64ERR_SUCCESS;
}

m64p_error PluginShutdown(void) { return M64ERR_SUCCESS; }

m64p_error PluginGetVersion(m64p_plugin_type *type, int *ver, int *api, const char **name, int *caps)
{
	if (type) *type = M64PLUGIN_INPUT;
	if (ver) *ver = 0x010000;
	if (api) *api = 0x020101;
	if (name) *name = "Salsa64 pad script";
	if (caps) *caps = 0;
	return M64ERR_SUCCESS;
}

void InitiateControllers(CONTROL_INFO info)
{
	for (int i = 0; i < 4; i++) {
		info.Controls[i].Present = i <= port;
		info.Controls[i].Plugin = 1;  /* PLUGIN_NONE */
		info.Controls[i].RawData = 0;
		info.Controls[i].Type = 0;    /* CONT_TYPE_STANDARD */
	}
}

void GetKeys(int control, BUTTONS *keys)
{
	if (control != port) {
		keys->Value = 0;
		return;
	}
	long field = (long)((now_s() - ((double)t0.tv_sec + t0.tv_nsec * 1e-9)) * 60.0) + offset;
	while (next < nsteps && field >= (long)steps[next].field) {
		cur = steps[next].value;
		fprintf(stderr, "pad_plugin: campo %ld -> estado %d (campo %u) valor %08x\n", field, next,
			steps[next].field, cur);
		next++;
	}
	keys->Value = cur;
}

void ControllerCommand(int control, unsigned char *cmd) { (void)control; (void)cmd; }
void ReadController(int control, unsigned char *cmd) { (void)control; (void)cmd; }
int RomOpen(void) { next = 0; cur = 0; clock_gettime(CLOCK_MONOTONIC, &t0); return 1; }
void RomClosed(void) {}
void SDL_KeyDown(int mod, int sym) { (void)mod; (void)sym; }
void SDL_KeyUp(int mod, int sym) { (void)mod; (void)sym; }

/* VRU (micrófono): el núcleo 2.6 exige las entradas. */
void SendVRUWord(uint16_t length, uint16_t *word, uint8_t lang) { (void)length; (void)word; (void)lang; }
void SetMicState(int state) { (void)state; }
void ReadVRUResults(uint16_t *error_flags, uint16_t *num_results, uint16_t *mic_level, uint16_t *voice_level,
		    uint16_t *voice_length, uint16_t *matches)
{
	*error_flags = *num_results = *mic_level = *voice_level = *voice_length = *matches = 0;
}
void ClearVRUWords(uint8_t length) { (void)length; }
void SetVRUWordMask(uint8_t length, uint8_t *mask) { (void)length; (void)mask; }
