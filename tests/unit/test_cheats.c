/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pruebas de los trucos (src/frontend/cheats.c): el .cht, los activos y los
 * códigos GameShark sobre la RDRAM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "frontend/cheats.h"
#include "core/bus.h"
#include "test.h"

static struct cheat_set cs;

static void parse(const char *text)
{
	cheats_parse(&cs, text, strlen(text));
}

static void parse_basic(void)
{
	parse("cheats = 3\n"
	      "\n"
	      "cheat0_desc = \"Vidas infinitas\"\n"
	      "cheat0_code = \"8107A6C4 0063\"\n"
	      "cheat0_enable = false\n"
	      "\n"
	      "cheat1_desc = \"Salud y munición\"\n"
	      "cheat1_code = \"8100FFFF 0001+8100FFFE 0002+800C0001 00AB\"\n"
	      "cheat1_enable = true\n"
	      "cheat2_desc = \"Con ñ y tildes: áéí\"\n"
	      "cheat2_code = \"D1001000 0005+80002000 0001\"\n"
	      "cheat2_enable = false\n");
	CHECK_EQ(cs.n, 3);
	CHECK(!strcmp(cs.c[0].desc, "Vidas infinitas"));
	CHECK_EQ(cs.c[0].count, 1);
	CHECK_EQ(cs.l[cs.c[0].first].addr, 0x8107A6C4u);
	CHECK_EQ(cs.l[cs.c[0].first].val, 0x0063u);
	CHECK(!cs.c[0].on);
	CHECK(!strcmp(cs.c[1].desc, "Salud y munición"));
	CHECK_EQ(cs.c[1].count, 3);
	CHECK_EQ(cs.l[cs.c[1].first + 2].addr, 0x800C0001u);
	CHECK_EQ(cs.l[cs.c[1].first + 2].val, 0xABu);
	CHECK(cs.c[1].on);
	CHECK(!strcmp(cs.c[2].desc, "Con ñ y tildes: áéí"));
	CHECK_EQ(cs.unsupported, 0);
}

static void parse_edges(void)
{
	/* Sin orden, con CRLF, minúsculas, un «?», un tipo que no se aplica y un
	 * truco sin código válido (no se cuenta). */
	parse("cheat2_code = \"81001000 00ff\"\r\n"
	      "cheat2_desc = \"Tercero\"\r\n"
	      "CHEAT0_DESC = \"Primero\"\r\n"
	      "cheat0_code = \"89001000 0001\"\r\n"
	      "cheat1_desc = \"Sin código\"\r\n"
	      "cheat3_code = \"8100?000 0001+80002000 0002\"\r\n"
	      "cheat4_desc = \"Basura\"\r\n"
	      "cheat4_code = \"hola\"\r\n"
	      "cheat5_code = \"80003000 0003\"\r\n");
	CHECK_EQ(cs.n, 4);  /* 0, 2, 3 y 5; el 1 y el 4 no tienen código */
	CHECK(!strcmp(cs.c[0].desc, "Primero"));
	CHECK(!strcmp(cs.c[1].desc, "Tercero"));
	CHECK_EQ(cs.l[cs.c[1].first].val, 0xFFu);
	CHECK_EQ(cs.c[2].count, 1);  /* la línea con «?» se salta, queda la otra */
	CHECK(!strcmp(cs.c[2].desc, "Truco 4"));  /* sin descripción: lleva su número */
	CHECK(!strcmp(cs.c[3].desc, "Truco 6"));
	CHECK_EQ(cs.unsupported, 3);  /* el 89, el «?» y «hola» */
	/* Descripción larga: se corta sin partir un carácter UTF-8. */
	char big[256];
	snprintf(big, sizeof big, "cheat0_desc = \"");
	for (int i = 0; i < 40; i++)
		strcat(big, "ñ");
	strcat(big, "\"\ncheat0_code = \"80000000 0001\"\n");
	parse(big);
	CHECK_EQ(cs.n, 1);
	CHECK(strlen(cs.c[0].desc) < CHEAT_DESC_MAX);
	CHECK_EQ(strlen(cs.c[0].desc) % 2, 0);  /* dos bytes por ñ: ninguna a medias */
	/* Vacío. */
	parse("");
	CHECK_EQ(cs.n, 0);
}

static void selection(void)
{
	parse("cheat0_desc = \"A\"\ncheat0_code = \"80000000 0001\"\ncheat0_enable = true\n"
	      "cheat1_desc = \"B\"\ncheat1_code = \"80000001 0001\"\n"
	      "cheat2_desc = \"C ñ\"\ncheat2_code = \"80000002 0001\"\n");
	CHECK_EQ(cheats_active(&cs), 1);  /* el «enable = true» del archivo */
	static const char on[] = "C ñ\r\nNo existe\nB\n";
	cheats_sel_parse(&cs, on, sizeof on - 1);
	CHECK(!cs.c[0].on && cs.c[1].on && cs.c[2].on);  /* lo del archivo de activos manda */
	CHECK_EQ(cheats_active(&cs), 2);
	char buf[128];
	int n = cheats_sel_format(&cs, buf, sizeof buf);
	CHECK(n > 0);
	CHECK(!strcmp(buf, "B\nC ñ\n"));
	struct cheat_set other = cs;
	cheats_sel_parse(&other, buf, (size_t)n);
	CHECK(!other.c[0].on && other.c[1].on && other.c[2].on);
	CHECK(cheats_sel_format(&cs, buf, 4) < 0);  /* no cabe */
}

static void enable_all(void)
{
	for (unsigned k = 0; k < cs.n; k++)
		cs.c[k].on = true;
}

static void apply_writes(void)
{
	memset(t_n64.bus.rdram, 0, 0x4000);
	parse("cheat0_desc = \"x\"\n"
	      "cheat0_code = \"80001001 00AB+81001002 BEEF+A0001005 0011+F1001006 2233\"\n");
	enable_all();
	cheats_apply(&cs, &t_n64.bus);
	/* Orden de la N64: el byte en la dirección más baja. */
	CHECK_EQ(t_rd32(0x1000), 0x00AB0000u | 0x0000BEEFu);  /* 1001 = AB, 1002..1003 = BEEF */
	CHECK_EQ(t_rd32(0x1004), 0x00112233u);
	/* Apagado no escribe. */
	memset(t_n64.bus.rdram, 0, 0x4000);
	cs.c[0].on = false;
	cheats_apply(&cs, &t_n64.bus);
	CHECK_EQ(t_rd32(0x1000), 0);
	/* Una dirección impar de 16 bits se alinea; fuera de la RDRAM no hace nada. */
	parse("cheat0_code = \"81002003 1234+80FFFFFF 0001\"\n");
	enable_all();
	cheats_apply(&cs, &t_n64.bus);
	CHECK_EQ(t_rd32(0x2000), 0x00001234u);
}

static void apply_conditions(void)
{
	memset(t_n64.bus.rdram, 0, 0x4000);
	t_wr32(0x1000, 0x00050000);  /* bytes 00 05 00 00: 16 bits en 0x1000 = 5, byte en 0x1001 = 5 */
	parse("cheat0_code = \"D1001000 0005+80002000 0001\"\n"   /* igual: sí */
	      "cheat1_code = \"D1001000 0006+80002001 0001\"\n"   /* distinto: se salta */
	      "cheat2_code = \"D3001000 0006+80002002 0001\"\n"   /* distinto de 6: sí */
	      "cheat3_code = \"D2001001 0005+80002003 0001\"\n"   /* igual: se salta */
	      "cheat4_code = \"D0001001 0005+80002004 0001+80002005 0001\"\n"   /* sí, y la de después va siempre */
	      "cheat5_code = \"D0001001 0006+80002006 0001+80002007 0001\"\n");  /* se salta solo la de 2006 */
	enable_all();
	cheats_apply(&cs, &t_n64.bus);
	CHECK_EQ(t_rd32(0x2000), 0x01000100u);  /* 2000 = 1, 2001 = 0, 2002 = 1, 2003 = 0 */
	CHECK_EQ(t_rd32(0x2004), 0x01010001u);  /* 2004 y 2005 = 1, 2006 = 0 (saltada), 2007 = 1 */
}

static void apply_repeater(void)
{
	memset(t_n64.bus.rdram, 0, 0x4000);
	/* 5 veces, de 2 en 2 bytes, el valor sube de 3 en 3. */
	parse("cheat0_code = \"50000502 0003+81003000 0010\"\n");
	enable_all();
	cheats_apply(&cs, &t_n64.bus);
	CHECK_EQ(t_rd32(0x3000), 0x00100013u);
	CHECK_EQ(t_rd32(0x3004), 0x00160019u);
	CHECK_EQ(t_rd32(0x3008), 0x001C0000u);
}

static void apply_twice_is_stable(void)
{
	memset(t_n64.bus.rdram, 0, 0x4000);
	parse("cheat0_code = \"80001000 0042\"\n");
	enable_all();
	for (int i = 0; i < 3; i++)
		cheats_apply(&cs, &t_n64.bus);
	CHECK_EQ(t_rd32(0x1000), 0x42000000u);
}

static void project64(void)
{
	static const char text[] =
		"[AAAAAAAA-BBBBBBBB-C:45]\r\n"
		"Name=Juego (U)\r\n"
		"\r\n"
		"$Vidas infinitas\r\n"
		"Note=una nota\r\n"
		"8107A6C4 0063\r\n"
		"\r\n"
		"$Grupo\\Sub\\Salud\r\n"
		"81000010 0001\r\n"
		"80000012 00FF\r\n"
		"\r\n"
		"$Gorra\r\n"
		"8033B177 00??\r\n"
		"20 Mano\r\n"
		"22 Invisible\r\n"
		"24 Metal\r\n"
		"\r\n"
		"$Sin opciones\r\n"
		"80000020 00??\r\n"
		"\r\n"
		"$Rara\r\n"
		"89000030 0001\r\n"
		"[CCCCCCCC-DDDDDDDD-C:45]\r\n"
		"Name=Juego (E)\r\n"
		"$Otra\r\n"
		"80000040 0001\r\n";
	/* Sin CRC conocido: la primera sección. */
	cheats_parse_pj64(&cs, text, sizeof text - 1, 0, 0);
	CHECK(!cs.crc_mismatch);
	CHECK_EQ(cs.n, 6);  /* vidas, salud, 3 gorras, la rara; «Sin opciones» no sale */
	CHECK(!strcmp(cs.c[0].desc, "Vidas infinitas"));
	CHECK(!strcmp(cs.c[1].desc, "Grupo > Sub > Salud"));
	CHECK_EQ(cs.c[1].count, 2);
	CHECK(!strcmp(cs.c[2].desc, "Gorra: Mano"));
	CHECK(!strcmp(cs.c[3].desc, "Gorra: Invisible"));
	CHECK(!strcmp(cs.c[4].desc, "Gorra: Metal"));
	CHECK_EQ(cs.l[cs.c[2].first].val, 0x20u);
	CHECK_EQ(cs.l[cs.c[3].first].val, 0x22u);
	CHECK_EQ(cs.l[cs.c[4].first].val, 0x24u);
	CHECK_EQ(cs.l[cs.c[4].first].addr, 0x8033B177u);
	CHECK(!strcmp(cs.c[5].desc, "Rara"));
	CHECK_EQ(cs.unsupported, 1);  /* el tipo 89 */
	/* Con el CRC de la segunda sección. */
	cheats_parse_pj64(&cs, text, sizeof text - 1, 0xCCCCCCCCu, 0xDDDDDDDDu);
	CHECK(!cs.crc_mismatch);
	CHECK_EQ(cs.n, 1);
	CHECK(!strcmp(cs.c[0].desc, "Otra"));
	/* CRC que no está: la primera, y se avisa. */
	cheats_parse_pj64(&cs, text, sizeof text - 1, 0x11111111u, 0x22222222u);
	CHECK(cs.crc_mismatch);
	CHECK_EQ(cs.n, 6);
	/* Una opción por la mitad alta del valor. */
	cheats_parse_pj64(&cs, "$X\n80001000 ??00\n05 A\n", strlen("$X\n80001000 ??00\n05 A\n"), 0, 0);
	CHECK_EQ(cs.n, 1);
	CHECK_EQ(cs.l[cs.c[0].first].val, 0x0500u);
	/* Aplicar la opción elegida. */
	memset(t_n64.bus.rdram, 0, 0x4000);
	cheats_parse_pj64(&cs, text, sizeof text - 1, 0, 0);
	cs.c[3].on = true;  /* Gorra: Invisible */
	cheats_parse_pj64(&cs, "$G\n80001001 00??\n20 A\n22 B\n", strlen("$G\n80001001 00??\n20 A\n22 B\n"), 0, 0);
	cs.c[1].on = true;
	cheats_apply(&cs, &t_n64.bus);
	CHECK_EQ(t_rd32(0x1000), 0x00220000u);
}

static void files(void)
{
	char dir[] = "/tmp/salsa64-cheats-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char path[300];
	/* Sin archivo: 0 y los nombres esperados. */
	CHECK_EQ(cheats_load(&cs, dir, "Star Fox 64 (U) (V1.0).z64", NULL, NULL, 0, 0), 0);
	snprintf(path, sizeof path, "%s/Star Fox 64 (U) (V1.0).cht", dir);
	CHECK(!strcmp(cs.cht_path, path));
	/* El nombre del .cht con otras mayúsculas también vale. */
	snprintf(path, sizeof path, "%s/STAR FOX 64 (U) (V1.0).CHT", dir);
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("cheat0_desc = \"Vidas\"\ncheat0_code = \"80001000 0009\"\n"
		      "cheat1_desc = \"Bombas\"\ncheat1_code = \"80001001 0009\"\n", f);
		fclose(f);
	}
	CHECK_EQ(cheats_load(&cs, dir, "Star Fox 64 (U) (V1.0).z64", NULL, NULL, 0, 0), 2);
	CHECK_EQ(cheats_active(&cs), 0);
	cs.c[1].on = true;
	CHECK(cheats_save(&cs));
	/* Recargar: los activos vuelven, y el .cht del usuario no se tocó. */
	CHECK_EQ(cheats_load(&cs, dir, "Star Fox 64 (U) (V1.0).z64", NULL, NULL, 0, 0), 2);
	CHECK(!cs.c[0].on && cs.c[1].on);
	f = fopen(path, "r");
	char buf[256] = { 0 };
	if (f) {
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		buf[n] = '\0';
		fclose(f);
	}
	CHECK(strstr(buf, "cheat1_desc = \"Bombas\"") != NULL);
	CHECK(strstr(buf, "enable") == NULL);
	/* Nombres de RetroArch: por el título y solo de la región de la ROM. */
	snprintf(path, sizeof path, "%s/007 - The World Is Not Enough (E) (M3).cht", dir);
	f = fopen(path, "w");
	if (f) {
		fputs("cheat0_desc = \"Europa\"\ncheat0_code = \"80001000 0001\"\n", f);
		fclose(f);
	}
	snprintf(path, sizeof path, "%s/007 - The World Is Not Enough (U).cht", dir);
	f = fopen(path, "w");
	if (f) {
		fputs("cheat0_desc = \"USA\"\ncheat0_code = \"80001000 0002\"\n", f);
		fclose(f);
	}
	CHECK_EQ(cheats_load(&cs, dir, "twine.z64", "THE WORLD IS NOT ENOUGH", "NTWE", 0, 0), 1);  /* E = USA */
	CHECK(!strcmp(cs.c[0].desc, "USA"));
	CHECK_EQ(cheats_load(&cs, dir, "twine.z64", "THE WORLD IS NOT ENOUGH", "NTWP", 0, 0), 1);
	CHECK(!strcmp(cs.c[0].desc, "Europa"));
	CHECK_EQ(cheats_load(&cs, dir, "007 TWINE (E).z64", "TWINE", NULL, 0, 0), 0);  /* título corto */
	CHECK_EQ(cheats_load(&cs, dir, "twine.z64", "THE WORLD IS NOT ENOUGH", "NTWJ", 0, 0), 0);  /* Japón: ninguno */
	/* Un .cht de Project64 se reconoce solo. */
	snprintf(path, sizeof path, "%s/Wave Race 64 (U).cht", dir);
	f = fopen(path, "w");
	if (f) {
		fputs("[11111111-22222222-C:45]\nName=Wave Race 64 (U)\n\n$Vidas\n80001000 0009\n", f);
		fclose(f);
	}
	CHECK_EQ(cheats_load(&cs, dir, "wr.z64", "WAVE RACE 64", "NWRE", 0x11111111u, 0x22222222u), 1);
	CHECK(!strcmp(cs.c[0].desc, "Vidas"));
	CHECK_EQ(cheats_load(&cs, dir, "twine (J).z64", "THE WORLD IS NOT ENOUGH", NULL, 0, 0), 0);
	/* Limpieza. */
	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	CHECK_EQ(system(cmd), 0);
}

void t_suite_cheats(void)
{
	RUN(parse_basic);
	RUN(parse_edges);
	RUN(selection);
	RUN(apply_writes);
	RUN(apply_conditions);
	RUN(apply_repeater);
	RUN(apply_twice_is_stable);
	RUN(project64);
	RUN(files);
}
