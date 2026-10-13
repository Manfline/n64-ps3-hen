/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pruebas de las descargas (src/frontend/dlutil.c): descargas.cfg, el
 * escape de nombres para las direcciones y los índices (lista de texto y
 * JSON de la API de GitHub), con la búsqueda por nombre parecido sobre ellos.
 */
#include <stdio.h>
#include <string.h>

#include "frontend/dlutil.h"
#include "frontend/namematch.h"
#include "test.h"

static void cfg(void)
{
	struct dl_cfg c;
	dl_cfg_default(&c);
	CHECK(strstr(c.covers, "Named_Boxarts/") != NULL);
	CHECK(strstr(c.cheats, "Config/Cheats/") != NULL);
	CHECK(c.covers[strlen(c.covers) - 1] == '/');
	/* La plantilla que se escribe la primera vez dice lo mismo que lo de fábrica. */
	struct dl_cfg t;
	memset(&t, 0, sizeof t);
	const char *tpl = dl_cfg_template();
	dl_cfg_parse(&t, tpl, strlen(tpl));
	CHECK(!strcmp(t.covers, c.covers));
	CHECK(!strcmp(t.covers_index, c.covers_index));
	CHECK(!strcmp(t.cheats, c.cheats));
	CHECK(!strcmp(t.cheats_index, c.cheats_index));
	/* Apagadas salvo «activar = si»; la plantilla las enciende. */
	CHECK(!c.enabled);
	CHECK(t.enabled);
	/* Un servidor propio: se le pone la «/» final a la base y no al índice. */
	static const char own[] = "# mío\r\ncaratulas = http://10.0.0.20:8000/covers\r\n  trucos=http://10.0.0.20:8000/cheats/ \n"
				  "trucos_indice = http://10.0.0.20:8000/cheats/index.txt\nrara = 5\nsinigual\n";
	dl_cfg_parse(&c, own, sizeof own - 1);
	CHECK(!strcmp(c.covers, "http://10.0.0.20:8000/covers/"));
	CHECK(!strcmp(c.cheats, "http://10.0.0.20:8000/cheats/"));
	CHECK(!strcmp(c.cheats_index, "http://10.0.0.20:8000/cheats/index.txt"));
	CHECK(strstr(c.covers_index, "api.github.com") != NULL);  /* lo que no se dice queda */
	CHECK(!c.enabled);
	static const char on[] = "activar = si\n", off[] = "activar=no\n";
	dl_cfg_parse(&c, on, sizeof on - 1);
	CHECK(c.enabled);
	dl_cfg_parse(&c, off, sizeof off - 1);
	CHECK(!c.enabled);
	/* Una línea enorme no rompe nada ni se toma. */
	char big[900];
	memset(big, 'a', sizeof big);
	memcpy(big, "trucos = http://", 16);
	big[sizeof big - 1] = '\n';
	dl_cfg_parse(&c, big, sizeof big);
	CHECK(!strcmp(c.cheats, "http://10.0.0.20:8000/cheats/"));
}

static void escape(void)
{
	char out[128];
	dl_url_escape("007 - GoldenEye (USA).png", out, sizeof out);
	CHECK(!strcmp(out, "007%20-%20GoldenEye%20%28USA%29.png"));
	dl_url_escape("Pokémon & Co's [!].cht", out, sizeof out);
	CHECK(!strcmp(out, "Pok%C3%A9mon%20%26%20Co%27s%20%5B%21%5D.cht"));
	/* Si no cabe, corta sin pasarse y termina en '\0'. */
	char tiny[8];
	dl_url_escape("aaaa bbbb", tiny, sizeof tiny);
	CHECK(strlen(tiny) < sizeof tiny);
	CHECK(!strncmp(tiny, "aaaa", 4));
}

static void index_text(void)
{
	char out[512];
	static const char list[] = "\xEF\xBB\xBF" "007 - GoldenEye (USA).png\r\nleeme.txt\n\nMario Kart 64 (USA).PNG\n/x/y/Zelda (USA).png\n";
	size_t n = dl_index_names(list, sizeof list - 1, ".png", out, sizeof out);
	CHECK_EQ(n, strlen(out));
	CHECK(!strcmp(out, "007 - GoldenEye (USA).png\nMario Kart 64 (USA).PNG\nZelda (USA).png\n"));
	/* Se entrega al buscador tal cual. */
	char name[128];
	CHECK(name_find_list(out, n, ".png", "GoldenEye 007.z64", "GOLDENEYE", "NGEE", 4, name, sizeof name));
	CHECK(!strcmp(name, "007 - GoldenEye (USA).png"));
	CHECK(!name_find_list(out, n, ".png", "Banjo.z64", "BANJO", "NB7E", 4, name, sizeof name));
	CHECK(name_find_list(out, n, ".png", "Mario Kart 64 (E).z64", "MARIOKART64", "NKTP", 4, name, sizeof name) == false);
	/* Una lista que no cabe se corta en un nombre entero. */
	char small[40];
	n = dl_index_names(list, sizeof list - 1, ".png", small, sizeof small);
	CHECK_EQ(n, strlen(small));
	CHECK(n == 0 || small[n - 1] == '\n');
	CHECK_EQ(dl_index_names("", 0, ".png", out, sizeof out), 0);
}

static void index_json(void)
{
	char out[1024];
	/* API de contenidos de GitHub: cada entrada con name, path y direcciones. */
	static const char contents[] =
		"[{\"name\":\"1080 Snowboarding (JU) (M2).cht\",\"path\":\"Config/Cheats/1080 Snowboarding (JU) (M2).cht\","
		"\"download_url\":\"https://raw.githubusercontent.com/x/1080%20Snowboarding.cht\",\"type\":\"file\"},\n"
		" {\"name\": \"Pok\\u00e9mon Stadium (U).cht\", \"type\": \"file\"},\n"
		" {\"name\": \"README.md\", \"type\": \"file\"},\n"
		" {\"name\": \"Zelda \\\"Ocarina\\\" (U).cht\", \"type\": \"file\"}]";
	size_t n = dl_index_names(contents, sizeof contents - 1, ".cht", out, sizeof out);
	CHECK_EQ(n, strlen(out));
	CHECK(strstr(out, "1080 Snowboarding (JU) (M2).cht\n") != NULL);
	CHECK(strstr(out, "Pok\xC3\xA9mon Stadium (U).cht\n") != NULL);
	CHECK(strstr(out, "Zelda \"Ocarina\" (U).cht\n") != NULL);
	CHECK(strstr(out, "README") == NULL);
	CHECK(strstr(out, "http") == NULL);  /* las direcciones no son nombres */
	/* API de árboles: «path» con carpetas; solo el último tramo. */
	static const char tree[] = "{\"sha\":\"abc\",\"tree\":[{\"path\":\"Nintendo - Nintendo 64\",\"type\":\"tree\"},"
				   "{\"path\":\"Nintendo - Nintendo 64/Named_Boxarts/Star Fox 64 (USA).png\",\"type\":\"blob\"}],\"truncated\":false}";
	n = dl_index_names(tree, sizeof tree - 1, ".png", out, sizeof out);
	CHECK(!strcmp(out, "Star Fox 64 (USA).png\n"));
	/* JSON roto: no se pasa del texto. */
	static const char broken[] = "[{\"name\":\"Abc (U).cht";
	n = dl_index_names(broken, sizeof broken - 1, ".cht", out, sizeof out);
	CHECK_EQ(n, strlen(out));
	static const char err[] = "{\"message\":\"API rate limit exceeded\",\"documentation_url\":\"https://x\"}";
	CHECK_EQ(dl_index_names(err, sizeof err - 1, ".cht", out, sizeof out), 0);
}

void t_suite_dlutil(void)
{
	RUN(cfg);
	RUN(escape);
	RUN(index_text);
	RUN(index_json);
}
