/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — modo de prueba automática.
 *
 * tools/rpcs3-run.py crea SALSA64_AUTOTEST_FLAG en el dev_hdd0 de RPCS3: los
 * programas interactivos salen solos después de verificar, y RPCS3 se cierra
 * limpio ("Exit RPCS3 when process finishes"). En la PS3 real el archivo no
 * existe y los programas esperan al usuario.
 */
#ifndef SALSA64_PLATFORM_PS3_AUTOTEST_H
#define SALSA64_PLATFORM_PS3_AUTOTEST_H

#include <sys/stat.h>

#include "platform/ps3/paths.h"

#define SALSA64_AUTOTEST_FLAG SALSA64_DATA_DIR "/autotest"

static inline int salsa64_autotest(void)
{
	struct stat st;
	return stat(SALSA64_AUTOTEST_FLAG, &st) == 0;
}

#endif
