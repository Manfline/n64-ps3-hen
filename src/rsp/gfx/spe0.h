/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — imágenes del programa del SPE 0 por familia de microcódigos
 * (docs/spe0-familias.md).
 *
 * La HLE gráfica entera no cabe en los 256 KB del local store con todo lo
 * que falta por escribir, así que src/spu/gfx se enlaza varias veces desde
 * las mismas fuentes: cada imagen lleva el código común (recorrido de la
 * display list, geometría, RDP, combiner, TMEM, caché de texturas) y solo
 * las familias de su grupo. El PPE (src/platform/ps3/gfx_spu.c) carga la
 * imagen de la familia de cada tarea y, si una tarea pide otra, cambia de
 * imagen llevando el estado del RDP. El host y el PPE compilan todo: sin
 * GFX_SPE0_IMG, todas las familias.
 *
 * Regla: lo que un juego mezcla dentro de una tarea (un G_LOAD_UCODE a otro
 * microcódigo de la misma generación, S2DEX2 dentro de F3DEX2 en Zelda y
 * Kirby, F3DFLX dentro de F3DEX2 en F-Zero X) va en la misma imagen; lo que
 * mezcla entre tareas, también, para no cambiar de imagen a cada cuadro (el
 * cambio funciona, pero cuesta: docs/spe0-familias.md).
 *
 * Solo C del preprocesador y funciones en línea: lo incluyen el SPE, el PPE
 * y el host.
 */
#ifndef SALSA64_RSP_GFX_SPE0_H
#define SALSA64_RSP_GFX_SPE0_H

#include <stdint.h>

#include "core/rcp.h"

/* Partes de la HLE que se pueden dejar fuera de una imagen. */
#define GFX_PART_F3D     0x01u  /* f3d.c: F3D, F3DEX 1.x, F3DWR (Wave Race) */
#define GFX_PART_F3DPD   0x02u  /* f3dpd.c: Perfect Dark (sobre F3D) */
#define GFX_PART_F3DDKR  0x04u  /* f3ddkr.c: Diddy Kong Racing, Jet Force Gemini, Mickey (sobre F3D) */
#define GFX_PART_F3DEX2  0x08u  /* f3dex2.c: F3DEX2, F3DZEX2 */
#define GFX_PART_S2DEX2  0x10u  /* s2dex.c: el tramo S2DEX2 dentro de una tarea de F3DEX2 */
#define GFX_PART_FLX     0x20u  /* flx.c: F3DFLX dentro de una tarea de F3DEX2 (F-Zero X) */
#define GFX_PART_F3DEXBG 0x40u  /* f3dexbg.c: Conker (sobre F3DEX2) */
#define GFX_PART_S2DEX   0x80u  /* s2dex.c completo: tareas S2DEX sueltas 2.x y 1.x, objetos y el 3D que sigue a su G_LOAD_UCODE */
#define GFX_PART_F5ROGUE 0x100u /* f5rogue.c: Rogue Squadron (Factor 5; sobre F3D) */
#define GFX_PART_F5INDY  0x200u /* f5indy.c: Indiana Jones y Battle for Naboo (Factor 5; con su propia DMEM, sin F3D) */
#define GFX_PART_TURBO3D 0x400u
#define GFX_PART_ACCLAIM 0x800u /* F3DEX2ACCLAIM: MOVEMEM y luces compactas sobre F3DEX2 */
#define GFX_PART_ALL     0xFFFu

/* Imágenes: el número va en el nombre del programa (spu_gfx<n>.bin). */
#define GFX_SPE0_F3D    0  /* primera generación: Fast3D, F3DEX 1.x y las variantes de Rare sobre F3D */
#define GFX_SPE0_F3DEX2 1  /* segunda generación de Nintendo: F3DEX2, F3DZEX2, S2DEX2 y F3DFLX en la tarea */
#define GFX_SPE0_CONKER 2  /* F3DEXBG (Conker): F3DEX2 con lo suyo */
#define GFX_SPE0_F5ROGUE 3 /* Rogue Squadron (Factor 5): lo común de F3D (G_TEXTURE, geometry mode, G_SETOTHERMODE) y su lista propia */
#define GFX_SPE0_F5INDY 4  /* Indiana Jones y Battle for Naboo (Factor 5): su lista y su DMEM de 4 KB */
#define GFX_SPE0_TURBO3D 5 /* Turbo3D y Acclaim: familias optativas que no comparten tareas */
#define GFX_SPE0_IMAGES 6

/* S2DEX suelto va en las imágenes 0 (Yoshi's Story: S2DEX 1.x y F3DEX 1.x en la misma tarea) y 1
 * (Resident Evil 2: S2DEX 2.x y F3DEX2, que se mezclan entre tareas). */
#define GFX_SPE0_PARTS_0 (GFX_PART_F3D | GFX_PART_F3DPD | GFX_PART_F3DDKR | GFX_PART_S2DEX)
#define GFX_SPE0_PARTS_1 (GFX_PART_F3DEX2 | GFX_PART_S2DEX2 | GFX_PART_FLX | GFX_PART_S2DEX)
#define GFX_SPE0_PARTS_2 (GFX_PART_F3DEX2 | GFX_PART_F3DEXBG)
#define GFX_SPE0_PARTS_3 (GFX_PART_F3D | GFX_PART_F5ROGUE)
#define GFX_SPE0_PARTS_4 GFX_PART_F5INDY
#define GFX_SPE0_PARTS_5 (GFX_PART_F3D | GFX_PART_TURBO3D | GFX_PART_F3DEX2 | GFX_PART_S2DEX2 | GFX_PART_ACCLAIM)

/* Entradas de la caché de texturas de cada imagen (hle.h, GFX_TC_ENTRIES):
 * lo que deja el local store con la holgura de pila de 1,5 KB. */
#define GFX_SPE0_TC_0 1024
#define GFX_SPE0_TC_1 1024
#define GFX_SPE0_TC_2 1024
#define GFX_SPE0_TC_3 1024
#define GFX_SPE0_TC_4 220
#define GFX_SPE0_TC_5 512

#define GFX_SPE0_CAT_(a, b) a##b
#define GFX_SPE0_CAT(a, b) GFX_SPE0_CAT_(a, b)

/* Lo que compila esta imagen (en el SPE, -DGFX_SPE0_IMG=<n>; en el host
 * también se puede, para probar que una imagen basta para un juego). */
#ifdef GFX_SPE0_IMG
#define GFX_PARTS GFX_SPE0_CAT(GFX_SPE0_PARTS_, GFX_SPE0_IMG)
#else
#define GFX_PARTS GFX_PART_ALL
#endif
#define GFX_HAS(part) ((GFX_PARTS & GFX_PART_##part) != 0)

/* Imagen que hace las tareas de un microcódigo; -1 si ninguna (sin HLE). */
static inline int gfx_spe0_image(uint32_t gbi)
{
	switch (gbi) {
#if !defined(GFX_SPE0_IMG) || GFX_SPE0_IMG == 5
	case SALSA64_GBI_TURBO3D:
	case SALSA64_GBI_TURBO3D_F3D:
		return GFX_SPE0_TURBO3D;
#endif
	case SALSA64_GBI_F3D:
	case SALSA64_GBI_F3DEX:
	case SALSA64_GBI_F3DWR:
	case SALSA64_GBI_F3DPD:
	case SALSA64_GBI_F3DDKR:
	case SALSA64_GBI_F3DJFG:
	case SALSA64_GBI_S2DEX1:
		return GFX_SPE0_F3D;
	case SALSA64_GBI_F3DEX2:
	case SALSA64_GBI_F3DZEX2:
	case SALSA64_GBI_S2DEX:
		return GFX_SPE0_F3DEX2;
#if !defined(GFX_SPE0_IMG) || GFX_SPE0_IMG == 5
	case SALSA64_GBI_F3DEX2ACCLAIM:
		return GFX_SPE0_TURBO3D;
#endif
	case SALSA64_GBI_F3DEXBG:
		return GFX_SPE0_CONKER;
	case SALSA64_GBI_F5ROGUE:
		return GFX_SPE0_F5ROGUE;
	case SALSA64_GBI_F5INDY:
		return GFX_SPE0_F5INDY;
	default:
		return -1;
	}
}

static inline const char *gfx_spe0_name(int img)
{
	static const char *const names[GFX_SPE0_IMAGES] = { "F3D", "F3DEX2", "Conker", "Factor 5", "Factor 5 Indy", "Turbo3D/Acclaim" };
	return img >= 0 && img < GFX_SPE0_IMAGES ? names[img] : "ninguna";
}

#endif
