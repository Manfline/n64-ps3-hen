/*
 * Salsa64 — tareas de audio del RSP: reconocer el ABI de la lista de audio
 * y el backend de la HLE (src/rsp/audio). Ver rcp.h.
 *
 * Cada microcódigo de audio se reconoce por unas pocas palabras de su
 * segmento de datos (ucode_data), como hace mupen64plus-rsp-hle
 * (`try_audio_task_detection` en src/hle.c, GPLv2; docs/THIRD_PARTY.md):
 * solo se comparan esas constantes, no se incluye nada del microcódigo.
 *
 *   [0] == 1 y [0x30] == 0xF0000F00   ABI1, según [0x28]
 *   [0] == 1, si no                   nead (ABI2), según [0x10]
 *   [0] != 1                          naudio y MusyX, según [0x10]
 *
 * Las ROMs de prueba de Salsa64 (sin microcódigo de Nintendo) llevan en
 * ucode_data el texto "Salsa64 ABI NEAD" o "Salsa64 ABI1".
 */
#include <string.h>

#include "core/bus.h"
#include "core/endian.h"

static const char *const names[SALSA64_ABI_COUNT] = {
	[SALSA64_ABI_UNKNOWN] = "desconocido",
	[SALSA64_ABI_AUDIO] = "ABI1",
	[SALSA64_ABI_AUDIO_GE] = "ABI1 (GoldenEye)",
	[SALSA64_ABI_AUDIO_BC] = "ABI1 (Blast Corps)",
	[SALSA64_ABI_NEAD_MK] = "nead (Mario Kart 64)",
	[SALSA64_ABI_NEAD_SF] = "nead (Star Fox 64)",
	[SALSA64_ABI_NEAD_SFJ] = "nead (Star Fox 64 J)",
	[SALSA64_ABI_NEAD_WRJB] = "nead (Wave Race J RevB)",
	[SALSA64_ABI_NEAD_FZ] = "nead (F-Zero X)",
	[SALSA64_ABI_NEAD_YS] = "nead (Yoshi's Story)",
	[SALSA64_ABI_NEAD_1080] = "nead (1080°)",
	[SALSA64_ABI_NEAD_OOT] = "nead (Zelda OoT)",
	[SALSA64_ABI_NEAD_MM] = "nead (Zelda MM)",
	[SALSA64_ABI_NEAD_MMB] = "nead (Zelda MM beta)",
	[SALSA64_ABI_NEAD_AC] = "nead (Animal Crossing)",
	[SALSA64_ABI_NEAD_EFZ] = "nead (F-Zero X Expansion)",
	[SALSA64_ABI_NAUDIO] = "naudio",
	[SALSA64_ABI_NAUDIO_BK] = "naudio (Banjo-Kazooie)",
	[SALSA64_ABI_NAUDIO_DK] = "naudio (Donkey Kong 64)",
	[SALSA64_ABI_NAUDIO_MP3] = "naudio con MP3 (Perfect Dark)",
	[SALSA64_ABI_NAUDIO_CBFD] = "naudio (Conker)",
	[SALSA64_ABI_MUSYX1] = "MusyX v1",
	[SALSA64_ABI_MUSYX2] = "MusyX v2",
};

const char *salsa64_abi_name(enum salsa64_abi abi)
{
	return abi < SALSA64_ABI_COUNT && names[abi] ? names[abi] : "?";
}

/* Solo espera una tarea de audio en vuelo (y avisa lo que escribió): una
 * gráfica no se toca, así el frontend puede soltar el audio sin terminar
 * la tarea gráfica a destiempo. El evento de fin de tarea sigue en pie y,
 * sin backend, solo termina la tarea. */
void salsa64_rsp_set_audio(struct salsa64_bus *bus, const struct salsa64_audio_backend *audio)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio)
		rsp->audio->finish(rsp->audio->ctx, bus);
	rsp->audio = audio;
}

enum salsa64_abi salsa64_rsp_detect_abi(const struct salsa64_bus *bus, const struct salsa64_task *task)
{
	uint32_t addr = task->ucode_data & 0x00FFFFFF;
	if (addr >= bus->rdram_size || bus->rdram_size - addr < 0x40)
		return SALSA64_ABI_UNKNOWN;
	const uint8_t *d = bus->rdram + addr;

	static const char test_nead[] = "Salsa64 ABI NEAD", test_abi1[] = "Salsa64 ABI1";
	if (!memcmp(d, test_nead, sizeof test_nead - 1))
		return SALSA64_ABI_NEAD_OOT;
	if (!memcmp(d, test_abi1, sizeof test_abi1 - 1))
		return SALSA64_ABI_AUDIO;

	if (salsa64_rd32(d) == 1) {
		if (salsa64_rd32(d + 0x30) == 0xF0000F00u) {
			switch (salsa64_rd32(d + 0x28)) {
			case 0x1E24138Cu: return SALSA64_ABI_AUDIO;
			case 0x1DC8138Cu: return SALSA64_ABI_AUDIO_GE;
			case 0x1E3C1390u: return SALSA64_ABI_AUDIO_BC;
			default:          return SALSA64_ABI_UNKNOWN;
			}
		}
		switch (salsa64_rd32(d + 0x10)) {
		case 0x11181350u: return SALSA64_ABI_NEAD_MK;
		case 0x111812E0u: return SALSA64_ABI_NEAD_SFJ;
		case 0x110412ACu: return SALSA64_ABI_NEAD_WRJB;
		case 0x110412CCu: return SALSA64_ABI_NEAD_SF;
		case 0x1CD01250u: return SALSA64_ABI_NEAD_FZ;
		case 0x1F08122Cu: return SALSA64_ABI_NEAD_YS;
		case 0x1F38122Cu: return SALSA64_ABI_NEAD_1080;
		case 0x1F681230u: return SALSA64_ABI_NEAD_OOT;
		case 0x1F801250u: return SALSA64_ABI_NEAD_MM;
		case 0x109411F8u: return SALSA64_ABI_NEAD_MMB;
		case 0x1EAC11B8u: return SALSA64_ABI_NEAD_AC;
		case 0x1F4C1230u: return SALSA64_ABI_NEAD_EFZ;
		case 0x00010010u: return SALSA64_ABI_MUSYX2;
		default:          return SALSA64_ABI_UNKNOWN;
		}
	}
	switch (salsa64_rd32(d + 0x10)) {
	case 0x00000001u: return SALSA64_ABI_MUSYX1;
	case 0x0000127Cu: return SALSA64_ABI_NAUDIO;
	case 0x00001280u: return SALSA64_ABI_NAUDIO_BK;
	case 0x1C58126Cu: return SALSA64_ABI_NAUDIO_DK;
	case 0x1AE8143Cu: return SALSA64_ABI_NAUDIO_MP3;
	case 0x1AB0140Cu: return SALSA64_ABI_NAUDIO_CBFD;
	default:          return SALSA64_ABI_UNKNOWN;
	}
}
