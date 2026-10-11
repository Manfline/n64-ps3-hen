/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — verificación del frontend con audiotest. Ver selftest_audio.h.
 */
#include "selftest_audio.h"

#include <string.h>

#include "../../tests/roms/src/audiotest/tone.h"
#include "audio_out.h"
#include "platform/ps3/log.h"

/* Las dos fases de la ROM son 180 búferes; el último puede seguir en cola. */
#define MIN_FRAMES (179u * TONE_FRAMES)

static struct {
	int cpu, hle;  /* 1 OK, -1 FALLO */
	int logged;
} st;

void audiotest_start(void)
{
	memset(&st, 0, sizeof st);
}

static bool starts(const uint8_t *text, uint32_t len, const char *s)
{
	size_t n = strlen(s);
	return len >= n && !memcmp(text, s, n);
}

void audiotest_isv(const uint8_t *text, uint32_t len)
{
	if (starts(text, len, "audiotest: HLE OK"))
		st.hle = 1;
	else if (starts(text, len, "audiotest: OK"))
		st.cpu = 1;
	else if (starts(text, len, "audiotest: FALLO"))
		st.hle = st.cpu = -1;
}

enum selftest_state audiotest_state(uint32_t fields)
{
	if (st.cpu < 0 || st.hle < 0)
		return SELFTEST_FAILED;
	if (st.cpu > 0 && st.hle > 0) {
		const struct audio_sync_stats *o = audio_out_stats();
		uint64_t in = audio_out_ai_frames();
		bool ok = in >= MIN_FRAMES && audio_out_blocks() > 0;
		if (!st.logged++)
			salsa64_log("audiotest: salida: %llu muestras del AI, %llu bloques al puerto, %llu en silencio",
				    (unsigned long long)in, (unsigned long long)audio_out_blocks(),
				    (unsigned long long)o->silent_frames);
		return ok ? SELFTEST_OK : SELFTEST_FAILED;
	}
	return fields >= SELFTEST_MAX_FIELDS ? SELFTEST_FAILED : SELFTEST_RUNNING;
}
