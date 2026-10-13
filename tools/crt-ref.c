/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — referencia de los filtros CRT originales del autor (sus shaders
 * de Shadertoy, v1.3 de máscara de sombra y v1.1 de CRT con cable), píxel
 * por píxel y en doble precisión, para comparar contra lo que dibujan las
 * pasadas del RSX (src/frontend/filters, docs/filtros.md).
 *
 *   crt-ref mascara|rgb|svideo|compuesto ENTRADA.ppm ANCHO ALTO SALIDA.ppm
 *       la imagen del juego (a su resolución) filtrada a una pantalla de
 *       ANCHO × ALTO, con la forma 4:3 centrada; con el sufijo «-rsx»
 *       (mascara-rsx…), el resplandor como lo hace el RSX: en una rejilla
 *       (1 o 2 puntos por píxel del juego) que promedia la luz ya lineal con
 *       filtro bilineal, interpolada en cada píxel
 *   crt-ref compara A.ppm B.ppm [DIFERENCIA.ppm]
 *       diferencia por canal (promedio, percentil 99, máximo) en el
 *       rectángulo 4:3 de la pantalla; la imagen de diferencia va ×8
 *
 * Compilar: cc -O2 -o build/crt-ref tools/crt-ref.c -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	double x, y, z;
} v3;

static v3 V(double x, double y, double z) { return (v3){ x, y, z }; }
static v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 mul(v3 a, double k) { return V(a.x * k, a.y * k, a.z * k); }
static v3 mulv(v3 a, v3 b) { return V(a.x * b.x, a.y * b.y, a.z * b.z); }
static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static double mixd(double a, double b, double t) { return a + (b - a) * t; }
static v3 mixv(v3 a, v3 b, double t) { return add(a, mul(add(b, mul(a, -1.0)), t)); }
static double dot3(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double fract(double x) { return x - floor(x); }

/* ---- Imagen ---------------------------------------------------------------- */

static int src_w, src_h;
static unsigned char *src_px;

static unsigned char *read_ppm(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	int maxv;
	if (fscanf(f, "P6 %d %d %d", w, h, &maxv) != 3 || maxv != 255) {
		fclose(f);
		return NULL;
	}
	fgetc(f);
	unsigned char *p = malloc((size_t)*w * *h * 3);
	if (fread(p, 3, (size_t)*w * *h, f) != (size_t)*w * *h) {
		free(p);
		p = NULL;
	}
	fclose(f);
	return p;
}

static void write_ppm(const char *path, const unsigned char *p, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(p, 3, (size_t)w * h, f);
	fclose(f);
}

/* Un píxel del juego, codificado; negro fuera (srcTexel del cable). */
static v3 texel0(double x, double y)
{
	if (x < 0 || y < 0 || x >= src_w || y >= src_h)
		return V(0, 0, 0);
	const unsigned char *p = src_px + ((size_t)(int)floor(y) * src_w + (int)floor(x)) * 3;
	return V(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
}

/* Como clamp_to_edge (srcTexel de la máscara). */
static v3 texel_clamp(double x, double y)
{
	return texel0(clampd(floor(x), 0, src_w - 1), clampd(floor(y), 0, src_h - 1));
}

static v3 powv(v3 c, double g)
{
	return V(pow(fmax(c.x, 0), g), pow(fmax(c.y, 0), g), pow(fmax(c.z, 0), g));
}

static void fit_rect(double ow, double oh, double aspect, double r[4])
{
	double w, h;
	if (ow / oh > aspect) {
		w = oh * aspect;
		h = oh;
	} else {
		w = ow;
		h = ow / aspect;
	}
	w = floor(w);
	h = floor(h);
	r[0] = floor((ow - w) * 0.5);
	r[1] = floor((oh - h) * 0.5);
	r[2] = w;
	r[3] = h;
}

static v3 sat_adjust(v3 c, double sat)
{
	double lum = dot3(c, V(0.2126, 0.7152, 0.0722));
	v3 r = mixv(V(lum, lum, lum), c, sat);
	return V(fmax(r.x, 0), fmax(r.y, 0), fmax(r.z, 0));
}

/* ---- Máscara de sombra v1.3 ------------------------------------------------ */

#define M_TPP 1.50
#define M_DOT 0.80
#define M_MS 1.00
#define M_KB 0.65
#define M_BMIN 0.22
#define M_BMAX 0.62
#define M_HBLUR 0.40
#define M_GLOW 0.12
#define M_GLOWR 2.0
#define M_BRIGHT 1.15
#define M_SAT 1.05
#define M_BLACK 0.00
#define GIN 2.4
#define GOUT 2.2

static v3 m_fetch(double x, double y)
{
	if (x < 0 || y < 0 || x >= src_w || y >= src_h)
		return V(0, 0, 0);
	return powv(texel_clamp(x, y), GIN);
}

static v3 m_fetch_linear(double x, double y)
{
	if (x < 0 || y < 0 || x > src_w || y > src_h)
		return V(0, 0, 0);
	double qx = x - 0.5, qy = y - 0.5, fx = fract(qx), fy = fract(qy);
	double bx = floor(qx) + 0.5, by = floor(qy) + 0.5;
	v3 a0 = mixv(texel_clamp(bx, by), texel_clamp(bx + 1, by), fx);
	v3 a1 = mixv(texel_clamp(bx, by + 1), texel_clamp(bx + 1, by + 1), fx);
	return powv(mixv(a0, a1, fy), GIN);
}

static v3 m_scanline(double y, double x)
{
	double cx = floor(x) + 0.5, ws = 0;
	v3 s = V(0, 0, 0);
	for (int i = -2; i <= 2; i++) {
		double px = cx + i, d = px - x, w = exp(-0.5 * d * d / (M_HBLUR * M_HBLUR));
		s = add(s, mul(m_fetch(px, y), w));
		ws += w;
	}
	return mul(s, 1.0 / ws);
}

static double erf1(double x)
{
	double s = x < 0 ? -1 : x > 0 ? 1 : 0;
	x = fabs(x);
	double t = 1.0 / (1.0 + 0.3275911 * x);
	double y = 1.0 - ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) *
				 t * exp(-x * x);
	return s * y;
}

static double beam_box(double c, double d, double a)
{
	double s = mixd(M_BMIN, M_BMAX, sqrt(clampd(c, 0, 1))) * 0.5;
	if (a < 1e-3)
		return c * exp(-0.5 * d * d / (s * s)) / (s * 2.5066283);
	double k = 1.0 / (s * 1.4142136);
	return c * (erf1((d + a) * k) - erf1((d - a) * k)) / (4.0 * a);
}

static double dot_energy(double pcx, double pcy, double a, v3 sel)
{
	double y0 = floor(pcy - 0.5) + 0.5, y1 = y0 + 1.0;
	double c0 = dot3(sat_adjust(m_scanline(y0, pcx), M_SAT), sel);
	double c1 = dot3(sat_adjust(m_scanline(y1, pcx), M_SAT), sel);
	return beam_box(c0, pcy - y0, a) + beam_box(c1, y1 - pcy, a);
}

static void nearest_dot(double qx, double qy, double ch, double D, double H, double *cx, double *cy)
{
	double row = floor(qy / H), best = 1e20;
	for (int k = -1; k <= 2; k++) {
		double r = row + k, fi = qx / D - 0.5 * r, base = ch - 2.0 * r;
		double i = base + 3.0 * floor((fi - base) / 3.0 + 0.5);
		double x = (i + 0.5 * r) * D, y = r * H, d2 = (qx - x) * (qx - x) + (qy - y) * (qy - y);
		if (d2 < best) {
			best = d2;
			*cx = x;
			*cy = y;
		}
	}
}

static double cover_sum(double R)
{
	int n = (int)ceil(R + 1.0);
	double s = 0;
	for (int b = 0; b < 2 * n && b < 32; b++)
		for (int a = 0; a < 2 * n && a < 32; a++) {
			double qx = a - n + 0.5, qy = b - n + 0.5;
			s += clampd(R - sqrt(qx * qx + qy * qy) + 0.5, 0, 1);
		}
	return fmax(s, 1e-3);
}

/* El resplandor como en el RSX (src/frontend/filters): rejilla de g
 * puntos por píxel del juego; cada punto, 12 lecturas bilineales de la luz
 * lineal (0 fuera); en el píxel, la rejilla interpolada (borde repetido). */
static int rsx_glow, glow_g;
static v3 *glow_grid;

static v3 lin_texel(int x, int y)
{
	if (x < 0 || y < 0 || x >= src_w || y >= src_h)
		return V(0, 0, 0);
	return powv(texel0(x, y), GIN);
}

static v3 lin_bilinear(double x, double y)
{
	double u = x - 0.5, v = y - 0.5;
	int x0 = (int)floor(u), y0 = (int)floor(v);
	double fx = u - x0, fy = v - y0;
	return mixv(mixv(lin_texel(x0, y0), lin_texel(x0 + 1, y0), fx), mixv(lin_texel(x0, y0 + 1), lin_texel(x0 + 1, y0 + 1), fx),
		    fy);
}

static void make_glow_grid(double radius, double W)
{
	glow_g = (int)(W / src_w);
	if (glow_g > 2)
		glow_g = 2;
	if (glow_g < 1 || src_w * glow_g > 640)
		glow_g = 1;
	int gw = src_w * glow_g, gh = src_h * glow_g;
	glow_grid = calloc((size_t)gw * gh, sizeof *glow_grid);
	for (int y = 0; y < gh; y++)
		for (int x = 0; x < gw; x++) {
			double px = (x + 0.5) / glow_g, py = (y + 0.5) / glow_g;
			v3 g = V(0, 0, 0);
			for (int i = 0; i < 12; i++) {
				double a = i * 2.39996, rad = sqrt(i + 0.5) / sqrt(12.0) * radius;
				g = add(g, lin_bilinear(px + cos(a) * rad, py + sin(a) * rad));
			}
			glow_grid[(size_t)y * gw + x] = mul(g, 1.0 / 12.0);
		}
}

static v3 grid_texel(int x, int y)
{
	int gw = src_w * glow_g, gh = src_h * glow_g;
	x = x < 0 ? 0 : x >= gw ? gw - 1 : x;
	y = y < 0 ? 0 : y >= gh ? gh - 1 : y;
	return glow_grid[(size_t)y * gw + x];
}

static v3 glow_at(double px, double py)
{
	double u = px * glow_g - 0.5, v = py * glow_g - 0.5;
	int x0 = (int)floor(u), y0 = (int)floor(v);
	double fx = u - x0, fy = v - y0;
	return mixv(mixv(grid_texel(x0, y0), grid_texel(x0 + 1, y0), fx), mixv(grid_texel(x0, y0 + 1), grid_texel(x0 + 1, y0 + 1), fx),
		    fy);
}

static v3 mask_pixel(double fx, double fy, double W, double H)
{
	double tsx = src_w / W, tsy = src_h / H;
	double pxps = W / src_w;
	double D = 2.0 * fmax(2.0, floor(pxps / (3.0 * M_TPP) * 0.5 + 0.5));
	double Hr = fmax(1.0, floor(D * 0.8660254 + 0.5));
	double R = 0.5 * M_DOT * D;
	double avg = cover_sum(R) / (3.0 * D * Hr);
	double cmax = clampd(R + 0.5 - 0.7071068, 0, 1);
	double peak = mixd(1.0, cmax / avg, M_MS);
	double out[3];
	for (int ch = 0; ch < 3; ch++) {
		v3 sel = V(ch == 0, ch == 1, ch == 2);
		double cx = 0, cy = 0;
		nearest_dot(fx, fy, ch, D, Hr, &cx, &cy);
		double cov = clampd(R - hypot(fx - cx, fy - cy) + 0.5, 0, 1);
		double m = mixd(1.0, cov / avg, M_MS);
		double e = dot_energy(cx * tsx, cy * tsy, 0.5 * Hr * tsy, sel);
		double t = clampd((peak - 1.0 / fmax(e, 1e-4)) / fmax(peak - 1.0, 1e-4), 0, 1);
		out[ch] = e * mixd(m, 1.0, t * M_KB);
	}
	v3 col = V(out[0], out[1], out[2]);
	double px = fx * tsx, py = fy * tsy;
	v3 glow = V(0, 0, 0);
	if (rsx_glow) {
		glow = mul(glow_at(px, py), 12.0);
	} else {
		for (int i = 0; i < 12; i++) {
			double ang = i * 2.39996, rad = sqrt(i + 0.5) / sqrt(12.0) * M_GLOWR;
			glow = add(glow, m_fetch_linear(px + cos(ang) * rad, py + sin(ang) * rad));
		}
	}
	col = add(col, mul(glow, M_GLOW / 12.0));
	col = mul(col, M_BRIGHT);
	col = add(mul(col, 1.0 - M_BLACK), V(M_BLACK, M_BLACK, M_BLACK));
	col = V(clampd(col.x, 0, 1), clampd(col.y, 0, 1), clampd(col.z, 0, 1));
	return powv(col, 1.0 / GOUT);
}

/* ---- CRT con cable v1.1 ------------------------------------------------------ */

#define C_CYCLES 170.667
#define C_LP 0.5
#define C_LS 0.45
#define C_TAU 1.20
#define T_TPP 2.0
#define T_SW 0.75
#define T_SOFT 0.45
#define T_MS 1.00
#define T_KB 0.65
#define T_BMIN 0.20
#define T_BMAX 0.55
#define T_GLOW 0.12
#define T_GLOWR 2.0
#define T_BRIGHT 1.15
#define T_SAT 1.00

static int cable;

static v3 hold(double x, double y, double w)
{
	w = fmin(w, 1.0);
	double a = x + 0.5 * w, n = floor(a);
	return mixv(texel0(n - 0.5, y), texel0(n + 0.5, y), clampd((a - n) / w, 0, 1));
}

static v3 cable_line(double y, double x)
{
	double f = C_CYCLES / src_w, dt = 0.25 / f, scale = src_w / 256.0;
	double ls = C_LS * scale, tau = C_TAU * scale;
	if (cable == 0) {
		int jR = (int)ceil(3.0 * ls / dt);
		v3 s = V(0, 0, 0);
		double w = 0;
		for (int j = -jR; j <= jR; j++) {
			double dx = j * dt, wl = exp(-0.5 * dx * dx / (ls * ls));
			s = add(s, mul(hold(x + dx, y, dt), wl));
			w += wl;
		}
		return mul(s, 1.0 / w);
	}
	double kc = cable == 1 ? 0.0 : 1.0;
	double lead = tau + 1.5 * dt;
	int jLo = (int)floor(fmin(-3.0 * ls, lead - 7.0 * tau - 3.0 * dt) / dt);
	int jHi = (int)ceil(fmax(3.0 * ls, lead) / dt);
	double x0 = 6.2831853 * f * (x + jLo * dt);
	double th = x0 + 6.2831853 * fract(floor(y) * C_LP);
	double csx = cos(th), csy = sin(th), ek = exp(dt / tau);
	double wY = 0, wC = 0, sY = 0, sCx = 0, sCy = 0;
	for (int j = jLo; j <= jHi; j++) {
		double dx = j * dt, u = lead - dx;
		int inL = fabs(dx) <= 3.0 * ls, inC = u >= 0.0 && u <= 7.0 * tau + 3.0 * dt;
		if (inL || inC) {
			v3 p = hold(x + dx, y, dt);
			double Y = dot3(p, V(0.299, 0.587, 0.114));
			double C = dot3(p, V(0.596, -0.274, -0.322)) * csx + dot3(p, V(0.211, -0.523, 0.312)) * csy;
			if (inL) {
				double wl = exp(-0.5 * dx * dx / (ls * ls));
				sY += (Y + kc * C) * wl;
				wY += wl;
			}
			if (inC) {
				double wc = 0, m = 1;
				for (int k = 0; k < 4; k++) {
					double v = u - k * dt;
					if (v >= 0.0 && v <= 7.0 * tau)
						wc += v * m;
					m *= ek;
				}
				wc *= exp(-u / tau);
				sCx += 2.0 * (C + kc * Y) * csx * wc;
				sCy += 2.0 * (C + kc * Y) * csy * wc;
				wC += wc;
			}
		}
		double t = csx;
		csx = -csy;
		csy = t;
	}
	double Y = sY / fmax(wY, 1e-6), I = sCx / fmax(wC, 1e-6), Q = sCy / fmax(wC, 1e-6);
	return V(Y + 0.956 * I + 0.621 * Q, Y - 0.272 * I - 0.647 * Q, Y - 1.106 * I + 1.703 * Q);
}

static v3 beam_sigma(v3 c)
{
	return V(mixd(T_BMIN, T_BMAX, sqrt(clampd(c.x, 0, 1))) * 0.5, mixd(T_BMIN, T_BMAX, sqrt(clampd(c.y, 0, 1))) * 0.5,
		 mixd(T_BMIN, T_BMAX, sqrt(clampd(c.z, 0, 1))) * 0.5);
}

static v3 beam_profile(v3 c, double d)
{
	v3 s = beam_sigma(c);
	return V(c.x * exp(-0.5 * d * d / (s.x * s.x)) / (s.x * 2.5066), c.y * exp(-0.5 * d * d / (s.y * s.y)) / (s.y * 2.5066),
		 c.z * exp(-0.5 * d * d / (s.z * s.z)) / (s.z * 2.5066));
}

static double grille1(double ph, double centre)
{
	double d = fabs(ph - centre);
	d = fmin(d, 1.0 - d);
	double hw = T_SW / 6.0, e0 = hw * (1.0 - T_SOFT), t = clampd((d - e0) / (hw - e0), 0, 1);
	return 1.0 - t * t * (3.0 - 2.0 * t);
}

static v3 grille(double x, double pitch)
{
	double ph = fract(x / pitch);
	return V(grille1(ph, 1.0 / 6.0), grille1(ph, 3.0 / 6.0), grille1(ph, 5.0 / 6.0));
}

static double dmask1(double c, double s, double mn, double peak)
{
	double core = c / (s * 2.5066);
	double t = clampd((peak - 1.0 / fmax(core, 1e-4)) / fmax(peak - 1.0, 1e-4), 0, 1);
	return mixd(mn, 1.0, t * T_KB);
}

static v3 c_fetch(double x, double y)
{
	if (x < 0 || y < 0 || x > src_w || y > src_h)
		return V(0, 0, 0);
	double qx = x - 0.5, qy = y - 0.5, fx = fract(qx), fy = fract(qy);
	double bx = clampd(floor(qx), 0, src_w - 1) + 0.5, by = clampd(floor(qy), 0, src_h - 1) + 0.5;
	double b1x = clampd(floor(qx) + 1, 0, src_w - 1) + 0.5, b1y = clampd(floor(qy) + 1, 0, src_h - 1) + 0.5;
	v3 a0 = mixv(texel0(bx, by), texel0(b1x, by), fx);
	v3 a1 = mixv(texel0(bx, b1y), texel0(b1x, b1y), fx);
	return powv(mixv(a0, a1, fy), GIN);
}

static v3 cable_pixel(double fx, double fy, double W, double H)
{
	double px = fx / W * src_w, py = fy / H * src_h;
	double pxps = W / src_w;
	double pitch = 3.0 * fmax(1.0, floor(pxps / (3.0 * T_TPP) + 0.5));
	v3 avg = V(0, 0, 0);
	for (int k = 0; k < 96 && k < pitch; k++)
		avg = add(avg, grille(k + 0.5, pitch));
	avg = mul(avg, 1.0 / pitch);
	v3 g = grille(fx, pitch);
	v3 mn = V(mixd(1, g.x / avg.x, T_MS), mixd(1, g.y / avg.y, T_MS), mixd(1, g.z / avg.z, T_MS));
	v3 peak = V(mixd(1, 1 / avg.x, T_MS), mixd(1, 1 / avg.y, T_MS), mixd(1, 1 / avg.z, T_MS));
	double y0 = floor(py - 0.5) + 0.5, y1 = y0 + 1.0;
	v3 c0 = sat_adjust(powv(cable_line(y0, px), GIN), T_SAT);
	v3 c1 = sat_adjust(powv(cable_line(y1, px), GIN), T_SAT);
	v3 s0 = beam_sigma(c0), s1 = beam_sigma(c1);
	v3 m0 = V(dmask1(c0.x, s0.x, mn.x, peak.x), dmask1(c0.y, s0.y, mn.y, peak.y), dmask1(c0.z, s0.z, mn.z, peak.z));
	v3 m1 = V(dmask1(c1.x, s1.x, mn.x, peak.x), dmask1(c1.y, s1.y, mn.y, peak.y), dmask1(c1.z, s1.z, mn.z, peak.z));
	v3 col = add(mulv(beam_profile(c0, py - y0), m0), mulv(beam_profile(c1, y1 - py), m1));
	v3 glow = V(0, 0, 0);
	if (rsx_glow) {
		glow = mul(glow_at(px, py), 12.0);
	} else {
		for (int i = 0; i < 12; i++) {
			double a = i * 2.39996, rad = sqrt(i + 0.5) / sqrt(12.0) * T_GLOWR;
			glow = add(glow, c_fetch(px + cos(a) * rad, py + sin(a) * rad));
		}
	}
	col = add(col, mul(glow, T_GLOW / 12.0));
	col = mul(col, T_BRIGHT);
	col = V(clampd(col.x, 0, 1), clampd(col.y, 0, 1), clampd(col.z, 0, 1));
	return powv(col, 1.0 / GOUT);
}

/* ---- Programa ------------------------------------------------------------------ */

static int compare(const char *a_path, const char *b_path, const char *diff_path)
{
	int aw, ah, bw, bh;
	unsigned char *a = read_ppm(a_path, &aw, &ah), *b = read_ppm(b_path, &bw, &bh);
	if (!a || !b || aw != bw || ah != bh) {
		fprintf(stderr, "crt-ref: no se pueden comparar %s y %s\n", a_path, b_path);
		return 1;
	}
	double r[4];
	fit_rect(aw, ah, 4.0 / 3.0, r);
	static unsigned long hist[256];
	double sum = 0;
	int maxd = 0;
	unsigned long n = 0;
	unsigned char *d = calloc((size_t)aw * ah, 3);
	for (int y = (int)r[1]; y < (int)(r[1] + r[3]); y++)
		for (int x = (int)r[0]; x < (int)(r[0] + r[2]); x++)
			for (int c = 0; c < 3; c++) {
				size_t i = ((size_t)y * aw + x) * 3 + c;
				int v = abs((int)a[i] - (int)b[i]);
				hist[v]++;
				sum += v;
				n++;
				if (v > maxd)
					maxd = v;
				d[i] = (unsigned char)(v * 8 > 255 ? 255 : v * 8);
			}
	unsigned long acc = 0;
	int p99 = 0, p999 = 0;
	for (int v = 0; v < 256; v++) {
		acc += hist[v];
		if (!p99 && acc >= n * 0.99)
			p99 = v;
		if (!p999 && acc >= n * 0.999)
			p999 = v;
	}
	printf("diferencia en %.0fx%.0f: promedio %.2f/255, p99 %d, p99,9 %d, máximo %d; iguales %.1f %%, a 1 o menos "
	       "%.1f %%\n",
	       r[2], r[3], sum / n, p99, p999, maxd, 100.0 * hist[0] / n, 100.0 * (hist[0] + hist[1]) / n);
	if (diff_path)
		write_ppm(diff_path, d, aw, ah);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "compara"))
		return compare(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
	if (argc != 6) {
		fprintf(stderr, "uso: crt-ref mascara|rgb|svideo|compuesto ENTRADA.ppm ANCHO ALTO SALIDA.ppm\n"
				"     crt-ref compara A.ppm B.ppm [DIFERENCIA.ppm]\n");
		return 2;
	}
	char mode[32];
	snprintf(mode, sizeof mode, "%s", argv[1]);
	char *suffix = strstr(mode, "-rsx");
	if (suffix) {
		rsx_glow = 1;
		*suffix = '\0';
	}
	int mask = !strcmp(mode, "mascara");
	cable = !strcmp(mode, "svideo") ? 1 : !strcmp(mode, "compuesto") ? 2 : 0;
	src_px = read_ppm(argv[2], &src_w, &src_h);
	if (!src_px) {
		fprintf(stderr, "crt-ref: no se puede leer %s\n", argv[2]);
		return 1;
	}
	int ow = atoi(argv[3]), oh = atoi(argv[4]);
	double r[4];
	fit_rect(ow, oh, 4.0 / 3.0, r);
	if (rsx_glow)
		make_glow_grid(mask ? M_GLOWR : T_GLOWR, r[2]);
	unsigned char *out = calloc((size_t)ow * oh, 3);
	for (int y = 0; y < oh; y++)
		for (int x = 0; x < ow; x++) {
			double fx = x + 0.5 - r[0], fy = y + 0.5 - r[1];
			if (fx < 0 || fy < 0 || fx >= r[2] || fy >= r[3])
				continue;
			v3 c = mask ? mask_pixel(fx, fy, r[2], r[3]) : cable_pixel(fx, fy, r[2], r[3]);
			unsigned char *p = out + ((size_t)y * ow + x) * 3;
			p[0] = (unsigned char)floor(clampd(c.x, 0, 1) * 255.0 + 0.5);
			p[1] = (unsigned char)floor(clampd(c.y, 0, 1) * 255.0 + 0.5);
			p[2] = (unsigned char)floor(clampd(c.z, 0, 1) * 255.0 + 0.5);
		}
	write_ppm(argv[5], out, ow, oh);
	return 0;
}
