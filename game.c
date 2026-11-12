/*
 * Skipass Madness After Christmas -- an original PRG32 downhill skiing game
 * set on real-inspired pistes of the Roccaraso / Alto Sangro ski area
 * (Abruzzo, Italy).
 *
 * Portable, freestanding, integer-only RV32IMAC C for the 128 KiB cartridge
 * profile. No heap, no floating point, no initialized pointers.
 *
 * View: a third-person "drone follow" camera behind and above the skier. The
 * slope is rendered as course-aligned depth slices (a 2.5D voxel-style
 * terrain): every slice is projected with the real altitude profile, so
 * convex crests hide what lies beyond, steep pitches drop away and reveal the
 * valley, and flats rise toward the horizon. The camera pitches with the
 * slope and yaws partly with the skier's heading.
 *
 * Physics: SI-inspired fixed point (cm, cm/s, Q12 trigonometry). Gravity
 * along the heading g*sin(slope)*cos(heading), Coulomb snow friction
 * mu*g*cos(slope) (groomed, ice, powder, moguls, snowplough, skidding),
 * aerodynamic drag k*v^2 (upright, tuck), carving radius limits on the turn
 * rate, ballistic flight off kickers and crests, and landing impacts.
 *
 * Colour model (identical on QEMU RGB565 and the ESP32-C6 indexed panel):
 *  - sprites and text8 use exact 6x6x6 cube colours (palette 16..231);
 *  - backgrounds use custom palette slots 8..15 and 232..255.
 *
 * Audio policy: stereo SID-like tracker music only on title/attract,
 * winning and game-over screens. Skiing uses procedural stereo effects only.
 */
#include "prg32.h"
#include <stdint.h>
#include <stddef.h>
#include "sprites.h"

#define W 320
#define H 200
#define ROOM "skipass-madness-roccaraso-v2"
#define DT 33                 /* simulation step, ms */
#define G 981                 /* gravity, cm/s^2 */
#define FOCAL 160
#define CAM_H 380             /* camera height above the snow, cm */
#define CAM_BACK 820          /* camera distance behind the skier, cm */
#define Y0 82                 /* vanishing line of a flat slope */
#define STEP 1000             /* course sample spacing, cm (10 m) */
#define MAXS (PISTE_MAXN + RUNOUT + 2)   /* course samples incl. run-out */
#define RUNOUT 45             /* flat samples after the finish */
#define NSL 52                /* terrain depth slices */
#define OBJ_MAX 220
#define DL_MAX 320
#define TREE_GAP 650          /* powder strip between piste edge and forest, cm */

/* ------------------------------------------------------------ freestanding */
void *memset(void *d, int v, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)v; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }

static int absi(int v) { return v < 0 ? -v : v; }
static int sgn(int v) { return (v > 0) - (v < 0); }
static int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static uint32_t seed = 0x5a17u;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 16; }
static uint32_t hash(uint32_t a) { a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15; a *= 0x846ca68bu; return a ^ (a >> 16); }

/* Q12 sine for whole degrees 0..90. Angles elsewhere are degrees * 16. */
static const int16_t SIN_T[91] = {
  0,71,143,214,286,357,428,499,570,641,711,782,852,921,991,1060,1129,1198,1266,1334,1401,1468,1534,
  1600,1666,1731,1796,1860,1923,1986,2048,2110,2171,2231,2290,2349,2408,2465,2522,2578,2633,2687,
  2741,2793,2845,2896,2946,2996,3044,3091,3138,3183,3228,3271,3314,3355,3396,3435,3474,3511,3547,
  3582,3617,3650,3681,3712,3742,3770,3798,3824,3849,3873,3896,3917,3937,3956,3974,3991,4006,4021,
  4034,4046,4056,4065,4074,4080,4086,4090,4094,4095,4096};
static int isin(int a) {
  while (a > 2880) a -= 5760;
  while (a < -2880) a += 5760;
  int s = 1;
  if (a < 0) { a = -a; s = -1; }
  if (a > 1440) a = 2880 - a;
  int d = a >> 4, f = a & 15;
  int v = SIN_T[d] + (d < 90 ? ((SIN_T[d + 1] - SIN_T[d]) * f >> 4) : 0);
  return s * v;
}
static int icos(int a) { return isin(a + 1440); }
static int isqrt(uint32_t v) { uint32_t r = 0, b = 1u << 30; while (b > v) b >>= 2; while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return (int)r; }

/* ---------------------------------------------------------------- colours */
#define CI(r, g, b) ((uint8_t)(16 + 36 * (r) + 6 * (g) + (b)))
static const uint8_t R5[6] = {0, 7, 13, 19, 25, 31}, G6[6] = {0, 13, 26, 38, 51, 63};
static uint16_t c565(uint8_t ci) { int v = ci - 16; if (v < 0 || v >= 216) return 0; return (uint16_t)((R5[v / 36] << 11) | (G6[(v / 6) % 6] << 5) | R5[v % 6]); }
static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)); }
enum {
  P_SKY0 = 8, P_SKY1, P_SKY2, P_SKY3, P_FAR, P_FARSNOW, P_MID, P_MIDSNOW,
  P_VALLEY = 232, P_VALLEY2, P_TOWN, P_PIST_A, P_PIST_B, P_PIST_C, P_PIST_D, P_EDGE,
  P_POWDER, P_POWDER2, P_FOREST, P_FOREST2, P_FORESTFAR, P_PISTFAR, P_TREE, P_TREE2,
  P_SHADOW, P_ICE, P_WOOD, P_ROOF, P_MOGUL, P_STEEL, P_LIGHT, P_CAT
};
#define HUDC CI(0, 0, 1)
#define HUD2 CI(0, 1, 2)
#define WHITE CI(5, 5, 5)
#define YELLOW CI(5, 5, 0)
#define ORANGE CI(5, 3, 0)
#define GREY CI(3, 3, 3)
#define BLACK CI(0, 0, 0)
#define REDC CI(5, 0, 0)
#define BLUEC CI(0, 1, 5)
#define CYAN CI(2, 5, 5)
static const uint8_t DIFF_COL[3] = {CI(0, 1, 5), CI(5, 0, 0), CI(0, 0, 0)};

static void make_palette(void) {
  static const uint8_t pal[][4] = {
    {P_SKY0, 38, 92, 178}, {P_SKY1, 70, 132, 206}, {P_SKY2, 122, 172, 228}, {P_SKY3, 186, 214, 242},
    {P_FAR, 104, 126, 168}, {P_FARSNOW, 226, 236, 250}, {P_MID, 44, 74, 96}, {P_MIDSNOW, 182, 204, 226},
    {P_VALLEY, 206, 220, 236}, {P_VALLEY2, 178, 196, 214}, {P_TOWN, 176, 112, 82}, {P_PIST_A, 238, 244, 252},
    {P_PIST_B, 226, 235, 248}, {P_PIST_C, 246, 250, 255}, {P_PIST_D, 232, 240, 251}, {P_EDGE, 192, 210, 234},
    {P_POWDER, 252, 253, 255}, {P_POWDER2, 228, 236, 250}, {P_FOREST, 22, 56, 50}, {P_FOREST2, 32, 70, 62},
    {P_FORESTFAR, 84, 112, 126}, {P_PISTFAR, 214, 226, 242}, {P_TREE, 16, 68, 46}, {P_TREE2, 34, 98, 66},
    {P_SHADOW, 150, 170, 202}, {P_ICE, 168, 214, 242}, {P_WOOD, 122, 72, 40}, {P_ROOF, 84, 46, 30},
    {P_MOGUL, 196, 212, 238}, {P_STEEL, 86, 92, 108}, {P_LIGHT, 255, 228, 120}, {P_CAT, 212, 38, 38},
  };
  for (unsigned i = 0; i < sizeof pal / sizeof pal[0]; i++) prg32_palette_set(pal[i][0], rgb(pal[i][1], pal[i][2], pal[i][3]));
}

static int clip_y = H;   /* rows >= clip_y are hidden (terrain occlusion) */
static void box(int x, int y, int w, int h, uint8_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > clip_y) h = clip_y - y;
  if (w > 0 && h > 0) prg32_gfx_rect_indexed(x, y, w, h, c);
}
static void hline(int x0, int x1, int y, uint8_t c) { box(x0, y, x1 - x0, 1, c); }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void txt(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
  if (x < 0 || y < 0 || x + slen(s) * 8 > W || y + 8 > H) return;
  prg32_gfx_text8(x, y, s, c565(fg), c565(bg));
}
static void txt_c(int y, const char *s, uint8_t fg, uint8_t bg) { txt((W - slen(s) * 8) / 2, y, s, fg, bg); }
static void num(char *b, int v, int digits) { b[digits] = 0; if (v < 0) v = 0; while (digits--) { b[digits] = (char)('0' + v % 10); v /= 10; } }
static int scopy(char *d, const char *s) { int n = 0; while (s[n]) { d[n] = s[n]; n++; } d[n] = 0; return n; }
static void fmt_time(char *b, int ms) {   /* M:SS.T */
  if (ms < 0) ms = 0;
  int t = ms / 100;
  b[0] = (char)('0' + (t / 600) % 10); b[1] = ':'; b[2] = (char)('0' + (t / 100) % 6);
  b[3] = (char)('0' + (t / 10) % 10); b[4] = '.'; b[5] = (char)('0' + t % 10); b[6] = 0;
}

/* ------------------------------------------------------- 5x7 display font */
static const uint8_t FONT[][7] = {
  {0,0,0,0,0,0,0},
  {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
  {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
  {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
  {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
  {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x11,0x19,0x15,0x13,0x11,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
  {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
  {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
  {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x15,0x0A},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
  {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
  {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
  {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
  {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
  {0x00,0x04,0x04,0x00,0x04,0x04,0x00},{0x04,0x04,0x04,0x04,0x04,0x00,0x04},{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
  {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},{0x00,0x04,0x04,0x1F,0x04,0x04,0x00},{0x04,0x04,0x08,0x00,0x00,0x00,0x00},
  {0x01,0x01,0x02,0x04,0x08,0x10,0x10},
};
static int glyph(char c) {
  if (c >= 'A' && c <= 'Z') return 1 + c - 'A';
  if (c >= '0' && c <= '9') return 27 + c - '0';
  switch (c) { case ':': return 37; case '!': return 38; case '.': return 39; case '-': return 40;
    case '+': return 41; case '\'': return 42; case '/': return 43; }
  return 0;
}
static int big_w(const char *s, int sc) { return slen(s) * 6 * sc - sc; }
static void big(int x, int y, const char *s, int sc, uint8_t col, uint8_t shadow) {
  for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
    int ox = pass ? 0 : sc, oy = pass ? 0 : sc;
    uint8_t c = pass ? col : shadow;
    int cx = x;
    for (const char *p = s; *p; p++, cx += 6 * sc) {
      const uint8_t *gl = FONT[glyph(*p)];
      for (int r = 0; r < 7; r++) {
        int bits = gl[r], b = 4;
        while (b >= 0) {
          if (!(bits >> b & 1)) { b--; continue; }
          int run = 0;
          while (b >= 0 && (bits >> b & 1)) { run++; b--; }
          box(cx + ox + (4 - b - run) * sc, y + oy + r * sc, run * sc, sc, c);
        }
      }
    }
  }
}
static void big_c(int y, const char *s, int sc, uint8_t col, uint8_t shadow) { big((W - big_w(s, sc)) / 2, y, s, sc, col, shadow); }

/* ------------------------------------------------------------- the pistes
 * Six runs of the Roccaraso / Alto Sangro area traced from the panoramic
 * piste map by scripts/trace_pistes.py: real start/finish altitudes, the
 * map's turns, pitches, gullies, forest and lift crossings, back-projected
 * from the map's perspective to metres. See pistes.h. */
#include "pistes.h"
/* Skipass validity per piste in seconds (about 1.35x an expert autopilot). */
static const uint16_t LIMIT_S[NPISTE] = {100, 270, 210, 110, 140, 165};
#define PISTE_CASE(i, P) case i: *drop = P##_DROP[k]; *bend = P##_BEND[k]; *hw = P##_HW[k]; *env = P##_ENV[k]; break;
static void piste_row(int c, int k, int *drop, int *bend, int *hw, int *env) {
  switch (c) {
    PISTE_CASE(0, E1) PISTE_CASE(1, M1) PISTE_CASE(2, D4) PISTE_CASE(3, D1) PISTE_CASE(4, T8) default: PISTE_CASE(5, K2)
  }
}
/* Landmark i of piste c: huts first, then lift crossings. 0 when done. */
#define MARK_CASE(i, P) case i: \
    if (m < (int)(sizeof P##_HUTS / sizeof P##_HUTS[0])) { *k = P##_HUTS[m][0]; *name = P##_HUTS[m][1]; *side = P##_HUTS[m][2]; return 1; } \
    m -= (int)(sizeof P##_HUTS / sizeof P##_HUTS[0]); \
    if (m < P##_NLIFTS) { *k = P##_LIFTS[m]; *name = -1; *side = 0; return 1; } \
    return 0;
static int piste_mark(int c, int m, int *k, int *name, int *side) {
  switch (c) {
    MARK_CASE(0, E1) MARK_CASE(1, M1) MARK_CASE(2, D4) MARK_CASE(3, D1) MARK_CASE(4, T8) default: MARK_CASE(5, K2)
  }
}

/* Course samples: altitude, world centreline, direction, slope, half width. */
static int32_t c_alt[MAXS], c_u[MAXS], c_v[MAXS];
static int16_t c_phi[MAXS], c_sin[MAXS], c_cos[MAXS], c_hw[MAXS];
static uint8_t c_env[MAXS];   /* forest density and gully walls per side, see pistes.h */
/* Gully walls: level 1 bank, 2 gully side, 3 canyon wall (height, horizontal run, cm). */
static const int16_t WALL_H[4] = {0, 400, 900, 1600}, WALL_RUN[4] = {0, 500, 900, 1100};
#define WALL_FOOT 150             /* flat snow between the piste edge and the wall foot */
static int env_at(int32_t s) { int k = (int)(s / STEP); return c_env[k < 0 ? 0 : (k >= MAXS ? MAXS - 1 : k)]; }
static int forest_of(int env, int side) { return (env >> (side > 0 ? 2 : 0)) & 3; }
static int wall_of(int env, int side) { return (env >> (side > 0 ? 6 : 4)) & 3; }
static uint8_t prof[64];
static int c_n, c_total, c_course = -1;
static int32_t c_len;

static int32_t lerp32(const int32_t *a, int32_t s) {
  int32_t mx = (int32_t)(c_total - 1) * STEP - 1;
  if (s < 0) s = 0;
  if (s > mx) s = mx;
  int i = (int)(s / STEP), f = (int)(s % STEP);
  return a[i] + (a[i + 1] - a[i]) * f / STEP;
}
static int lerp16(const int16_t *a, int32_t s) {
  int32_t mx = (int32_t)(c_total - 1) * STEP - 1;
  if (s < 0) s = 0;
  if (s > mx) s = mx;
  int i = (int)(s / STEP), f = (int)(s % STEP);
  return a[i] + (a[i + 1] - a[i]) * f / STEP;
}
static void build_course(int c) {
  if (c == c_course) return;
  c_course = c;
  static int16_t raw[MAXS], bend[MAXS], wid[MAXS];
  int n = PISTE_N[c] - 1;              /* steps; sample n is the finish */
  for (int i = 0; i <= n; i++) {
    int d, b, hw, env;
    piste_row(c, i, &d, &b, &hw, &env);
    raw[i] = (int16_t)(d * 10);        /* dm -> cm drop over the next 10 m */
    bend[i] = (int16_t)(b * 2);        /* 1/8 deg -> 1/16 deg */
    wid[i] = (int16_t)(hw * 25);       /* 25 cm units -> cm */
    c_env[i] = (uint8_t)env;
  }
  c_n = n + 1;
  c_total = c_n + RUNOUT;
  for (int i = c_n; i < c_total; i++) { raw[i] = (int16_t)(i < c_n + 4 ? raw[n - 1] / 2 : 0); bend[i] = 0; wid[i] = (int16_t)(wid[n] + 400); c_env[i] = 0; }
  for (int i = 1; i < c_total - 1; i++)   /* soften the 1 dm quantisation of the drop */
    raw[i] = (int16_t)((raw[i - 1] + 2 * raw[i] + raw[i + 1]) / 4);
  int32_t sum = 0;
  for (int i = 0; i < n; i++) sum += raw[i];
  int32_t drop = (int32_t)(TOP_M[c] - BOT_M[c]) * 100;
  int32_t alt = (int32_t)TOP_M[c] * 100, u = 0, v = 0;
  int phi = BEARING[c] * 16;
  for (int i = 0; i < c_total; i++) {
    int32_t d = sum ? (int32_t)raw[i] * (drop / 16) / (sum / 16) : 0;
    if (i >= n) d = raw[i];
    c_alt[i] = alt; c_u[i] = u; c_v[i] = v; c_phi[i] = (int16_t)phi; c_hw[i] = wid[i];
    int hyp = isqrt((uint32_t)(STEP * STEP + d * d));
    c_sin[i] = (int16_t)(d * 4096 / hyp);
    c_cos[i] = (int16_t)(STEP * 4096 / hyp);
    alt -= d;
    u += icos(phi) * STEP / 4096;
    v += isin(phi) * STEP / 4096;
    if (i + 1 < c_total) phi += bend[i + 1];   /* the map's turn arriving at the next sample */
  }
  c_len = (int32_t)(c_n - 1) * STEP;
  for (int i = 0; i < 64; i++) {
    int32_t a = lerp32(c_alt, c_len * i / 63);
    prof[i] = (uint8_t)clampi((int)((a - (int32_t)BOT_M[c] * 100) * 24 / (drop > 0 ? drop : 1)), 0, 24);
  }
}

/* ------------------------------------------------------------- game state */
enum { TITLE, ATTRACT, COUNT, PLAY, CRASH, WIN, OVER };
enum { OB_TOURIST, OB_KID, OB_BOARDER, OB_CAT, OB_CANNON, OB_MOGULS, OB_ICE, OB_KICKER, OB_CHECK, OB_LIFT, OB_HUT, OB_START, OB_FINISH };
typedef struct { int32_t s; int16_t x, base, amp, w, v; uint16_t phase, rate; uint8_t type, pal, done, name; } obj_t;
typedef struct {
  int32_t s, x, alt;          /* along-course cm, lateral cm, skier altitude cm */
  int32_t sa, xa;             /* sub-cm accumulators */
  int v, vy, h, w;            /* speed cm/s, vertical cm/s, heading deg16, turn rate deg16/s */
  int va;                     /* sub-step velocity accumulator, cm/s * ms */
  int air, pose, push_ms, scrape, plough, tuck, surface;
} skier_t;
static struct {
  int mode, course, crashes, checks, misses_near, top_kmh, score, cam_yaw, best_rank;
  uint32_t now, last_ms, mode_ms, run_ms, limit_ms, input_prev, toast_until, attract_ms, push_ready;
  uint8_t joined, mp_ready, peers_online, hiss_on, wind_on, scrape_on, hiss_note, finished;
  int ai;                     /* autopilot drives the skier (attract mode) */
  int assist;                 /* MAESTRO: instructor steering while no Left/Right is held */
  char toast[26];
  int crash_reason;
} g;
static skier_t p;
static obj_t ob[OBJ_MAX];
static int nob;

/* ------------------------------------------------------------------ audio */
enum { CH_HISS, CH_WIND, CH_SCRAPE, CH_CHIME, CH_THUD, CH_CRASH, CH_WHOOSH, CH_CLICK };
enum { I_HISS = 8, I_WIND, I_SCRAPE, I_CHIME, I_THUD, I_CRASH, I_CLICK, I_WHOOSH };
static void audio_quiet(void) {
  prg32_audio_stop_track();
  prg32_audio_stop_all();
  for (int i = 0; i < 8; i++) { prg32_audio_note_off((uint8_t)i); prg32_audio_set_channel_volume((uint8_t)i, 255); }
  g.hiss_on = g.wind_on = g.scrape_on = 0;
}
static void music_play(int track) { audio_quiet(); prg32_audio_play_track((uint16_t)track); }
static void sfx(int ch, int inst, int note, int vol, int pan) {
  if (g.ai) return;   /* the attract demo plays music only */
  prg32_audio_note_on_pan((uint8_t)ch, (uint8_t)inst, (uint8_t)clampi(note, 0, 127), (uint8_t)clampi(vol, 0, 255), (int8_t)clampi(pan, -64, 63));
}
static void held_off(void) {
  if (g.hiss_on) prg32_audio_note_off(CH_HISS);
  if (g.wind_on) prg32_audio_note_off(CH_WIND);
  if (g.scrape_on) prg32_audio_note_off(CH_SCRAPE);
  g.hiss_on = g.wind_on = g.scrape_on = 0;
}
/* Continuous stereo ski sound: snow hiss follows speed and heading, wind
 * grows with v^2 and gusts across the stereo field, carving/snowplough
 * scrape sits on the side of the skis. */
static void ski_audio(void) {
  if (g.ai) return;
  int kmh = p.v * 36 / 1000;
  int hiss = (p.air || g.mode != PLAY || p.v < 60) ? 0 : clampi(70 + kmh * 2, 0, 235);
  int pan = clampi(p.h / 24 + (int)(p.x / 120), -60, 60);
  int note = clampi(38 + kmh / 4, 38, 70);
  if (p.surface == 2) note += 6;   /* ice rasps higher */
  if (hiss) {
    if (!g.hiss_on || absi(note - g.hiss_note) >= 3) { sfx(CH_HISS, I_HISS, note, hiss, pan); g.hiss_on = 1; g.hiss_note = (uint8_t)note; }
    else { prg32_audio_set_channel_volume(CH_HISS, (uint8_t)hiss); prg32_audio_set_channel_pan(CH_HISS, (int8_t)pan); }
  } else if (g.hiss_on) { prg32_audio_note_off(CH_HISS); g.hiss_on = 0; }
  int wind = g.mode == PLAY ? clampi((kmh - 25) * (kmh - 25) / 12, 0, 220) : 0;
  int gust = isin((int)(g.now / 3) % 5760) * 50 / 4096;
  if (wind > 8) {
    if (!g.wind_on) { sfx(CH_WIND, I_WIND, 34 + kmh / 6, wind, gust); g.wind_on = 1; }
    else { prg32_audio_set_channel_volume(CH_WIND, (uint8_t)wind); prg32_audio_set_channel_pan(CH_WIND, (int8_t)gust); }
  } else if (g.wind_on) { prg32_audio_note_off(CH_WIND); g.wind_on = 0; }
  int scrape = (p.air || g.mode != PLAY) ? 0 : clampi(p.scrape, 0, 240);
  if (scrape > 20) {
    int sp = clampi(-sgn(p.w) * 50, -60, 60);
    if (!g.scrape_on) { sfx(CH_SCRAPE, I_SCRAPE, 64 + kmh / 8, scrape, sp); g.scrape_on = 1; }
    else { prg32_audio_set_channel_volume(CH_SCRAPE, (uint8_t)scrape); prg32_audio_set_channel_pan(CH_SCRAPE, (int8_t)sp); }
  } else if (g.scrape_on) { prg32_audio_note_off(CH_SCRAPE); g.scrape_on = 0; }
}

/* --------------------------------------------------------------- sprites */
static prg32_indexed_sprite_t spr_l, spr_m, spr_s;
static uint16_t pals[9][14];
enum { PAL_PLAYER, PAL_RIVAL, PAL_T0 };
static void sprite_common(prg32_indexed_sprite_t *d) {
  d->frame_count = SPR_FRAMES; d->palette_count = 14;
  d->bits_per_pixel = PRG32_SPRITE_BPP_4; d->transparent_index = 0; d->palette = pals[0];
}
static void make_sprites(void) {
  static const uint8_t jackets[9][5] = {   /* jacket, shade, trousers, lit, helmet */
    {CI(5,0,0), CI(3,0,0), CI(0,0,2), CI(0,1,4), CI(5,5,5)},
    {CI(0,4,1), CI(0,2,0), CI(1,1,1), CI(2,2,2), CI(5,5,0)},
    {CI(3,0,4), CI(2,0,2), CI(1,1,1), CI(2,2,3), CI(5,2,4)},
    {CI(0,4,5), CI(0,2,3), CI(0,0,1), CI(0,1,3), CI(5,5,5)},
    {CI(5,3,0), CI(3,1,0), CI(1,1,2), CI(2,2,4), CI(1,1,1)},
    {CI(5,2,4), CI(3,1,2), CI(0,0,2), CI(1,1,4), CI(5,5,5)},
    {CI(2,4,0), CI(1,2,0), CI(2,1,0), CI(3,2,1), CI(0,1,4)},
    {CI(5,5,1), CI(4,3,0), CI(0,0,1), CI(0,1,3), CI(5,0,0)},
    {CI(1,2,5), CI(0,0,3), CI(2,2,2), CI(3,3,3), CI(5,5,5)},
  };
  for (int i = 0; i < 9; i++) {
    uint16_t *q = pals[i];
    q[0] = 0; q[1] = c565(BLACK); q[2] = c565(jackets[i][0]); q[3] = c565(jackets[i][1]);
    q[4] = c565(jackets[i][2]); q[5] = c565(jackets[i][3]); q[6] = c565(jackets[i][4]); q[7] = c565(ORANGE);
    q[8] = c565(i == PAL_RIVAL ? ORANGE : YELLOW); q[9] = c565(GREY); q[10] = c565(CI(1, 1, 1));
    q[11] = c565(WHITE); q[12] = c565(CI(3, 3, 4)); q[13] = c565(CI(4, 5, 5));
  }
  spr_l.pixels = spr_24x32; spr_l.width = 24; spr_l.height = 32;
  spr_m.pixels = spr_12x16; spr_m.width = 12; spr_m.height = 16;
  spr_s.pixels = spr_6x8; spr_s.width = 6; spr_s.height = 8;
  sprite_common(&spr_l); sprite_common(&spr_m); sprite_common(&spr_s);
}
/* Draw a person standing at (x, ybase); ppm16 = pixels per metre * 16. */
static void draw_person(int x, int ybase, int ppm16, int frame, int pal, int clip, int scale100) {
  if (ybase > clip + 2) return;
  int hpx = ppm16 * 17 * scale100 / 16000;
  if (hpx >= 26) { spr_l.palette = pals[pal]; prg32_sprite_draw_indexed(x - 12, ybase - 32, &spr_l, (uint32_t)frame); }
  else if (hpx >= 12) { spr_m.palette = pals[pal]; prg32_sprite_draw_indexed(x - 6, ybase - 16, &spr_m, (uint32_t)frame); }
  else if (hpx >= 5) { spr_s.palette = pals[pal]; prg32_sprite_draw_indexed(x - 3, ybase - 8, &spr_s, (uint32_t)frame); }
  else if (hpx >= 2) box(x, ybase - 2, 1, 2, CI(2, 1, 1));
}

/* ---------------------------------------------------------- course objects */
static void add_obj(int32_t s, int x, int type) {
  if (nob >= OBJ_MAX) return;
  obj_t *o = &ob[nob++];
  memset(o, 0, sizeof *o);
  o->s = s; o->x = o->base = (int16_t)x; o->type = (uint8_t)type;
  o->pal = (uint8_t)(PAL_T0 + rnd() % 7);
  o->phase = (uint16_t)(rnd() % 5760);
}
static void place_objects(int c) {
  nob = 0;
  seed = 0x5a17u + (uint32_t)c * 977u;   /* identical layout on every networked console */
  add_obj(900, 0, OB_START);
  int mk, mname, mside;
  for (int m = 0; piste_mark(c, m, &mk, &mname, &mside); m++) {   /* huts and lift crossings from the map */
    int32_t s = (int32_t)mk * STEP;
    int hw = c_hw[mk];
    if (mname < 0) add_obj(s, 0, OB_LIFT);
    else { add_obj(imax(s, 600), (mside ? 1 : -1) * (hw + 1300), OB_HUT); ob[nob - 1].name = (uint8_t)mname; }
  }
  int dens = DIFF[c] == 0 ? 230 : (DIFF[c] == 1 ? 150 : 90);
  for (int k = 9; k < c_n - 6; k++) {
    int32_t s = (int32_t)k * STEP + (int32_t)(rnd() % 600);
    int hw = c_hw[k];
    int gully = wall_of(c_env[k], -1) >= 2 && wall_of(c_env[k], 1) >= 2;
    if (gully && rnd() % 4) continue;               /* few people stop in a narrow gully */
    int lat = (int)(rnd() % (unsigned)(2 * hw - 300)) - hw + 150;
    unsigned r = rnd() % 1000;
    if (k % 30 == 0) { add_obj((int32_t)k * STEP, 0, OB_CHECK); continue; }
    if ((int)r < dens) {                                    /* holiday crowd */
      add_obj(s, lat, OB_TOURIST);
      obj_t *o = &ob[nob - 1];
      o->v = (int16_t)(260 + rnd() % 420);
      o->amp = (int16_t)(250 + rnd() % (unsigned)imax(hw - 400, 300));
      o->base = (int16_t)clampi(lat, -hw + o->amp / 2, hw - o->amp / 2);
      o->rate = (uint16_t)(1400 + rnd() % 1400);
      continue;
    }
    r -= (unsigned)dens;
    if (r < 40 && DIFF[c] == 0) {                           /* ski school line */
      for (int i = 0; i < 4; i++) {
        add_obj(s - i * 260, lat, OB_KID);
        obj_t *o = &ob[nob - 1];
        o->v = 230; o->amp = 450; o->base = (int16_t)clampi(lat, -hw + 400, hw - 400);
        o->rate = 1500; o->phase = (uint16_t)(5760 - i * 300); o->pal = (uint8_t)(i == 0 ? PAL_T0 + 6 : PAL_T0 + 1 + (i & 1));
      }
      continue;
    }
    if (r < 90) { add_obj(s, lat, OB_BOARDER); continue; }
    if (r < 170) { add_obj(s, (rnd() & 1 ? 1 : -1) * (hw + 250), OB_CANNON); continue; }
    if (r < 250 && c_sin[k] > 1000) { add_obj(s, clampi(lat, -hw + 700, hw - 700), OB_MOGULS); ob[nob - 1].w = (int16_t)(500 + rnd() % 300); continue; }
    if (r < 290) { add_obj(s, clampi(lat, -hw + 500, hw - 500), OB_ICE); ob[nob - 1].w = (int16_t)(350 + rnd() % 250); continue; }
    if (r < 330 && DIFF[c] < 2 && c_sin[k] < 1400) { add_obj(s, clampi(lat, -hw + 400, hw - 400), OB_KICKER); ob[nob - 1].w = 220; continue; }
  }
  if (DIFF[c] > 0) { add_obj((int32_t)(c_n / 2) * STEP, -c_hw[c_n / 2] / 3, OB_CAT); ob[nob - 1].v = -110; }
  add_obj(c_len, 0, OB_FINISH);
}
static void update_objects(void) {
  for (int i = 0; i < nob; i++) {
    obj_t *o = &ob[i];
    if (o->type == OB_TOURIST || o->type == OB_KID) {
      if (o->s > p.s + 60000 || o->s < p.s - 20000) continue;
      if (o->s < c_len - 1500) o->s += o->v * DT / 1000;
      else {                       /* at the arrival they step aside behind the fence */
        int side = (o->base < 0 ? -1 : 1) * (lerp16(c_hw, o->s) + 350);
        o->base = (int16_t)(o->base + clampi(side - o->base, -12, 12)); o->amp = (int16_t)imax(o->amp - 8, 60);
      }
      o->phase = (uint16_t)((o->phase + o->rate * DT / 1000) % 5760);
      o->x = (int16_t)(o->base + o->amp * isin(o->phase) / 8192);
    } else if (o->type == OB_CAT) {
      if (o->s > STEP * 5) o->s += o->v * DT / 1000;
      o->x = (int16_t)(o->base + 40 * isin((int)(g.now / 4) % 5760) / 4096);
    }
  }
}

/* ------------------------------------------------------------------ skier */
static void toast(const char *s) { scopy(g.toast, s); g.toast_until = g.now + 1600; }
static void reset_skier(void) {
  memset(&p, 0, sizeof p);
  p.s = 400; p.alt = lerp32(c_alt, p.s);
}
static void crash(int reason) {
  g.crashes++;
  g.crash_reason = reason;
  p.v = 0; p.w = 0; p.air = 0;
  held_off();
  sfx(CH_CRASH, I_CRASH, 40, 240, clampi((int)(p.x / 60), -60, 60));
  sfx(CH_THUD, I_THUD, 30, 255, 0);
  g.mode = CRASH; g.mode_ms = g.now;
}
static int ground_at(int32_t s) { return (int)lerp32(c_alt, s); }

static int ap_avoid;   /* the autopilot is steering round a hazard */
/* MAESTRO line choice: score lateral lines across the piste against every
 * hazard in the next ~2 s (people where they will be when we arrive) and
 * take the cheapest, preferring small corrections. No pointers, no heap. */
static uint32_t autopilot(void) {
  int hw = lerp16(c_hw, p.s);
  int look = imax(3500, p.v * 2);
  int16_t hx[12], hr[12]; int32_t hd[12]; int nh = 0;
  for (int i = 0; i < nob && nh < 12; i++) {
    obj_t *o = &ob[i];
    int32_t ds = o->s - p.s;
    if (ds < -100 || ds > look) continue;
    if (o->type > OB_ICE || o->type == OB_CANNON) continue;
    int rad = o->type == OB_CAT ? 520 : (o->type == OB_MOGULS || o->type == OB_ICE ? o->w + 120 : 200);
    int ox = o->x;
    if (o->type == OB_TOURIST || o->type == OB_KID) {
      int moving = o->s < c_len - 1500;
      int closing = imax(p.v - (moving ? o->v : 0), 150);
      int t_ms = (int)(imax((int)ds, 0) * 1000 / closing);
      ox = o->base + o->amp * isin((int)((o->phase + (int32_t)o->rate * t_ms / 1000) % 5760)) / 8192;
      rad += absi(ox - o->x) / 4;
    }
    hx[nh] = (int16_t)ox; hr[nh] = (int16_t)rad; hd[nh] = ds; nh++;
  }
  int target = 0;
  ap_avoid = 0;
  if (nh) {
    int best = 1 << 30, here = 1 << 30;
    for (int x = -hw + 220; x <= hw - 220; x += 80) {
      int cost = absi(x - (int)p.x) / 4 + absi(x) / 16;
      for (int k = 0; k < nh; k++) {
        /* the line between here and x, sampled where the hazard will be */
        int frac = (int)clampi((int)(hd[k] * 256 / 1600), 0, 256);
        int lx = (int)p.x + (x - (int)p.x) * frac / 256;
        int gap = absi(lx - hx[k]) - hr[k];
        if (gap < 160) cost += (160 - gap) * 40 * 4096 / (int)(4096 + hd[k]);
      }
      if (cost < best) { best = cost; target = x; }
      if (absi(x - (int)p.x) < 40) here = cost;
    }
    ap_avoid = here > absi((int)p.x) / 16 + 200;
  }
  target = clampi(target, -hw + 200, hw - 200);
  int aim = 1600;
  int dphi = lerp16(c_phi, p.s + aim) - lerp16(c_phi, p.s);
  int want = clampi((target - (int)p.x) * 916 / aim + dphi, -40 * 16, 40 * 16);
  uint32_t in = 0;
  if (p.h < want - 24) in |= PRG32_BTN_RIGHT;
  else if (p.h > want + 24) in |= PRG32_BTN_LEFT;
  int kmh = p.v * 36 / 1000;
  int vmax = DIFF[g.course] == 2 ? 92 : (DIFF[g.course] == 1 ? 86 : 74);
  if (kmh > vmax + 6) in |= PRG32_BTN_DOWN;
  else if (kmh < vmax && absi(p.h - want) < 64 && !ap_avoid) in |= PRG32_BTN_UP;
  if ((p.v < 450 || (p.v < 700 && lerp16(c_sin, p.s) < 330)) && g.now >= g.push_ready) in |= PRG32_BTN_B;   /* skate and pole the flats */
  return in;
}

static void recover(void) {   /* back on the skis after a fall, clear of the crowd */
  int hw = lerp16(c_hw, p.s);
  p.x = clampi((int)p.x, -hw + 250, hw - 250); p.h = 0; p.w = 0; p.v = 0;
  p.alt = ground_at(p.s);
  for (int i = 0; i < nob; i++) if (ob[i].s > p.s - 400 && ob[i].s < p.s + 400 && ob[i].type <= OB_CANNON) ob[i].s += 900;
}
static void physics_step(uint32_t in, uint32_t press) {
  int sinT = lerp16(c_sin, p.s), cosT = lerp16(c_cos, p.s);
  int hw = lerp16(c_hw, p.s);
  int steer = ((in & PRG32_BTN_RIGHT) ? 1 : 0) - ((in & PRG32_BTN_LEFT) ? 1 : 0);
  p.tuck = (in & PRG32_BTN_UP) && !(in & PRG32_BTN_DOWN);
  p.plough = (in & PRG32_BTN_DOWN) ? 1 : 0;
  /* Surface under the skis: 0 groomed, 1 powder, 2 ice, 3 moguls. */
  int surf = absi((int)p.x) > hw ? 1 : 0;
  int on_kicker = -1;
  for (int i = 0; i < nob; i++) {
    obj_t *o = &ob[i];
    if (o->type < OB_MOGULS || o->type > OB_KICKER) continue;
    int32_t ds = p.s - o->s;
    int len = o->type == OB_KICKER ? 400 : 2500;
    if (ds < 0 || ds > len || absi((int)p.x - o->x) > o->w) continue;
    if (o->type == OB_KICKER) on_kicker = i;
    else surf = o->type == OB_ICE ? 2 : 3;
  }
  p.surface = surf;
  /* Turning: a carved arc of radius ~13 m limits the turn rate at speed;
   * at low speed the skier can pivot. Ice cuts the grip, the air most of it. */
  int carve = p.v * 917 / 1300;                      /* deg16/s */
  int wmax = clampi(carve, 48 * 16, 105 * 16);
  if (p.tuck) wmax /= 2;
  if (surf == 2) wmax = wmax * 2 / 5;
  if (p.air) wmax /= 6;
  int target = steer * wmax;
  /* Released skis drift back toward the fall line (faster on steep snow). */
  if (!steer && !p.air) target = -sgn(p.h) * imin(absi(p.h) * 2, 8 * 16 + sinT * 96 / 4096);
  p.w += clampi(target - p.w, -560, 560);
  p.h = clampi(p.h + p.w * DT / 1000, -85 * 16, 85 * 16);
  /* Friction: Coulomb mu * g * cos(slope), mu in Q12. */
  int mu = 184;                                      /* waxed ski on groomed snow ~0.045 */
  if (surf == 1) mu = 700;                           /* deep powder */
  if (surf == 2) mu = 82;                            /* ice */
  if (surf == 3) mu = 430;                           /* moguls */
  int skid = absi(p.w) > carve + 160 ? clampi((absi(p.w) - carve) * 1200 / (carve + 400), 0, 1400) : 0;
  if (p.plough && !p.tuck) skid += absi(p.h) > 55 * 16 ? 2600 : 1250;   /* hockey stop / snowplough */
  if (p.air) { mu = 0; skid = 0; }
  p.scrape = (skid / 8) + (surf == 2 ? 70 : 0) + (absi(p.w) > 40 * 16 ? 50 : 0);
  int a_g = G * sinT / 4096 * icos(p.h) / 4096;
  int a_f = (mu + skid) * G / 4096 * cosT / 4096;
  int k = p.tuck ? 50000 : (p.plough ? 15000 : 20800);  /* drag: v^2 / k, cm/s^2 */
  if (surf == 1) k = k * 2 / 3;
  int a_d = (int)((int32_t)p.v * p.v / k);
  p.va += (a_g - a_f - a_d) * DT;
  p.v += p.va / 1000; p.va %= 1000;
  if ((press & PRG32_BTN_B) && !p.air && p.v < 900 && g.now >= g.push_ready) {
    p.v += 95; p.push_ms = (int)g.now + 280; g.push_ready = g.now + 260;
    sfx(CH_CLICK, I_CLICK, 84, 150, -30); sfx(CH_WHOOSH, I_CLICK, 79, 120, 30);
  }
  if (p.v < 0) p.v = 0;
  p.v = imin(p.v, 4600);
  /* Move along the piste; the piste direction changes under the skier. */
  int phi0 = lerp16(c_phi, p.s);
  p.sa += (int32_t)p.v * icos(p.h) / 4096 * DT;
  p.xa += (int32_t)p.v * isin(p.h) / 4096 * DT;
  p.s += p.sa / 1000; p.sa %= 1000;
  p.x += p.xa / 1000; p.xa %= 1000;
  p.h -= lerp16(c_phi, p.s) - phi0;
  /* Vertical: follow the ground, or fly a ballistic arc off crests/kickers. */
  int32_t gnd = ground_at(p.s);
  if (!p.air) {
    int32_t ball = p.alt + (int32_t)p.vy * DT / 1000 - (int32_t)G * DT * DT / 2000000;
    if (on_kicker >= 0 && p.v > 300) {
      p.vy = (int)((gnd - p.alt) * 1000 / DT) + p.v * 1100 / 4096;
      p.air = 1; p.alt = gnd + 2;
      sfx(CH_WHOOSH, I_WHOOSH, 60, 200, 0);
    } else if (ball > gnd + 6 && p.v > 900) {
      p.air = 1; p.alt = ball; p.vy -= G * DT / 1000;
    } else {
      p.vy = (int)((gnd - p.alt) * 1000 / DT);
      p.alt = gnd;
      if (surf == 3 && p.v > 700 && (hash((uint32_t)(p.s / 240)) & 3) == 0) {   /* mogul bump */
        p.vy += p.v / 10; p.air = p.v > 1500; p.alt += 2;
        sfx(CH_THUD, I_THUD, 36 + (int)(rnd() % 6), 150, clampi((int)p.x / 80, -50, 50));
      }
    }
  } else {
    p.vy -= G * DT / 1000;
    p.alt += (int32_t)p.vy * DT / 1000;
    if (p.alt <= gnd) {
      int vg = -(int)((int32_t)p.v * sinT / 4096);
      int impact = vg - p.vy;
      p.alt = gnd; p.air = 0;
      p.vy = vg;
      if (impact > 1150 || (impact > 650 && absi(p.h) > 38 * 16)) { crash(1); return; }
      if (impact > 600) { p.v -= p.v / 6; toast("HARD LANDING"); }
      sfx(CH_THUD, I_THUD, 30, clampi(120 + impact / 6, 120, 255), 0);
    }
  }
  /* Off the piste: a gully wall banks the skier back toward the bed, losing
   * speed as it climbs; beyond its top, or into the forest, it is a fall.
   * Open snowfields (no forest on the map) are deep powder. */
  int side = sgn((int)p.x), env = env_at(p.s), out = absi((int)p.x) - hw;
  int wl = wall_of(env, side), fo = forest_of(env, side);
  if (wl && out > WALL_FOOT && !p.air) {
    int up = out - WALL_FOOT;
    if (up > WALL_RUN[wl]) { crash(5); p.x = side * (hw - 300); return; }
    p.h -= side * clampi(up / 10 + 8, 8, 60);              /* the bank turns the skis back */
    p.v -= (int)((int32_t)p.v * up * WALL_H[wl] / WALL_RUN[wl] / 60000);
    p.scrape += 60;
  } else if (!wl && !p.air && fo && out > TREE_GAP + (fo == 1 ? 900 : 0)) { crash(2); p.x = side * (hw - 300); return; }
  else if (!wl && !fo && out > 2600) { p.x = side * (hw + 2600); p.h -= side * 40; }
  for (int i = 0; i < nob; i++) {
    obj_t *o = &ob[i];
    int32_t ds = o->s - p.s;
    if (o->type == OB_HUT) {
      if (!o->done && ds < 4000 && ds > 0) {
        o->done = 1;
        char b[26];
        int n = scopy(b, "RIFUGIO ");
        scopy(b + n, o->name == 3 ? "TOPPE" : HUTN[o->name]);
        toast(b);
      }
      continue;
    }
    if (ds > 300 || ds < -300) continue;
    int dx = absi(o->x - (int)p.x);
    if (o->type == OB_CHECK || o->type == OB_FINISH || o->type == OB_START) {
      if (o->type == OB_CHECK && !o->done && ds <= 0) {
        o->done = 1; g.checks++; g.limit_ms += 5000; toast("CHECKPOINT +5 SEC");
        sfx(CH_CHIME, I_CHIME, 84, 220, -30); sfx(CH_CLICK, I_CHIME, 91, 200, 30);
      }
      continue;
    }
    int rad = o->type == OB_CAT ? 280 : (o->type == OB_BOARDER ? 110 : (o->type == OB_CANNON ? 70 : (o->type <= OB_KID ? 75 : 0)));
    if (!rad) continue;
    if (absi((int)ds) < 90 && dx < rad && p.alt - ground_at(p.s) < 90) { crash(o->type == OB_CAT ? 3 : 4); return; }
    if (ds < 0 && ds >= -(p.v * DT / 1000 + 2) && dx < rad + 160 && (o->type <= OB_BOARDER)) {
      g.misses_near++; g.score += 150;
      sfx(CH_WHOOSH, I_WHOOSH, 66, 190, clampi((o->x - (int)p.x) / 3, -60, 60));
    }
  }
  /* Animation pose. */
  int lean = p.w / (16 * 22);
  if (p.air) p.pose = SPR_AIR;
  else if ((int)g.now < p.push_ms) p.pose = SPR_PUSH;
  else if (p.plough && !p.tuck) p.pose = lean < 0 ? SPR_LEAN_L1 : (lean > 0 ? SPR_LEAN_R1 : SPR_PLOUGH);
  else if (p.tuck) p.pose = lean < 0 ? SPR_TUCK_L : (lean > 0 ? SPR_TUCK_R : SPR_TUCK);
  else if (lean <= -2) p.pose = SPR_LEAN_L2;
  else if (lean == -1) p.pose = SPR_LEAN_L1;
  else if (lean >= 2) p.pose = SPR_LEAN_R2;
  else if (lean == 1) p.pose = SPR_LEAN_R1;
  else p.pose = SPR_STRAIGHT;
  int kmh = p.v * 36 / 1000;
  if (kmh > g.top_kmh) g.top_kmh = kmh;
  g.score += p.v / 200;
}

/* ------------------------------------------------------------- game flow */
static void set_mode(int m) {
  g.mode = m; g.mode_ms = g.now;
  if (m == TITLE || m == ATTRACT) music_play(0);
  else if (m == WIN) music_play(1);
  else if (m == OVER) music_play(2);
  else audio_quiet();
}
static void begin_run(int ai) {
  g.ai = ai;
  build_course(g.course);
  place_objects(g.course);
  reset_skier();
  g.crashes = g.checks = g.misses_near = g.top_kmh = g.score = 0;
  g.cam_yaw = 0; g.finished = 0; g.best_rank = 0;
  g.limit_ms = (uint32_t)LIMIT_S[g.course] * 1000u;
  g.run_ms = 0; g.toast_until = 0; g.push_ready = 0;
  seed = hash(g.now) | 1u;
  if (ai) { g.mode = ATTRACT; g.mode_ms = g.now; g.attract_ms = 0; g.last_ms = g.now; }
  else { set_mode(COUNT); g.last_ms = g.now; }
}
static void finish_run(void) {
  g.finished = 1;
  int left = (int)g.limit_ms - (int)g.run_ms;
  g.score += left / 10 + g.checks * 500 + g.top_kmh * 10;
  set_mode(WIN);
}

/* ------------------------------------------------------------ multiplayer */
static void multiplayer(uint32_t input) {
  if (!prg32_multiplayer_available()) return;
  if (!g.mp_ready) { prg32_multiplayer_init(); g.mp_ready = 1; }
  if (!g.joined && prg32_multiplayer_join(ROOM, PRG32_MP_FLAG_ENABLE) == 0) g.joined = 1;
  if (!g.joined) return;
  int mode = g.mode == ATTRACT ? TITLE : g.mode;
  prg32_multiplayer_set_input(input);
  prg32_multiplayer_set_local_state((int16_t)clampi((int)p.x, -32000, 32000), (int16_t)clampi((int)(p.s / 20), 0, 32000),
                                    (uint16_t)(g.course | (p.pose << 4)),
                                    (uint16_t)(mode | (clampi(p.v * 36 / 1000, 0, 255) << 4) | (g.finished ? 0x8000 : 0)));
  prg32_multiplayer_tick();
  int n = prg32_multiplayer_get_peer_count();
  g.peers_online = (uint8_t)clampi(n, 0, 8);
}
static int peer_on_course(const prg32_player_state_t *q) {
  int m = q->flags & 15;
  return (q->sprite & 15) == (unsigned)g.course && (m == PLAY || m == CRASH || m == COUNT);
}
static int race_rank(void) {
  if (!g.joined) return 0;
  int n = prg32_multiplayer_get_peer_count(), rank = 1, field = 1;
  for (int i = 0; i < n; i++) {
    prg32_player_state_t q;
    if (prg32_multiplayer_get_peer(i, &q) != 0 || (q.sprite & 15) != (unsigned)g.course) continue;
    int m = q.flags & 15;
    if (m != PLAY && m != CRASH && m != COUNT && m != WIN) continue;
    field++;
    if ((q.flags & 0x8000) || (int32_t)q.y * 20 > p.s) rank++;
  }
  return field > 1 ? rank * 16 + field : 0;
}

/* ------------------------------------------------------------- the update */
void skipass_madness_init(void) {
  make_palette();
  make_sprites();
  memset(&g, 0, sizeof g);
  g.now = prg32_ticks_ms();
  prg32_audio_set_master_volume(160);
  build_course(0);
  place_objects(0);
  reset_skier();
  g.assist = 1;
  set_mode(TITLE);
}
void skipass_madness_update(void) {
  g.now = prg32_ticks_ms();
  uint32_t in = prg32_input_read(), press = in & ~g.input_prev;
  g.input_prev = in;
  if (g.mode == TITLE) {
    int old = g.course;
    if (press & (PRG32_BTN_LEFT | PRG32_BTN_UP)) g.course = (g.course + 5) % 6;
    if (press & (PRG32_BTN_RIGHT | PRG32_BTN_DOWN)) g.course = (g.course + 1) % 6;
    if (old != g.course) { build_course(g.course); place_objects(g.course); reset_skier(); g.mode_ms = g.now; sfx(CH_CLICK, I_CLICK, 76, 160, 0); }
    if (press & PRG32_BTN_B) { g.assist = !g.assist; g.mode_ms = g.now; sfx(CH_CLICK, I_CLICK, g.assist ? 84 : 67, 170, 0); }
    if (press & (PRG32_BTN_A | PRG32_BTN_START)) begin_run(0);
    else if (g.now - g.mode_ms > 15000) begin_run(1);
  } else if (g.mode == ATTRACT) {
    if (press) { g.ai = 0; g.course = (g.course + 1) % 6; build_course(g.course); place_objects(g.course); reset_skier(); g.mode = TITLE; g.mode_ms = g.now; }
    else {
      int steps = 0;
      while (g.now - g.last_ms >= DT && steps++ < 4) {
        g.last_ms += DT; g.attract_ms += DT;
        update_objects();
        uint32_t a = autopilot();
        physics_step(a, a & ~g.input_prev);
        if (g.mode == CRASH) { int hw = lerp16(c_hw, p.s); g.mode = ATTRACT; p.x = clampi((int)p.x, -hw + 300, hw - 300); p.h = 0; p.w = 0; }
      }
      if (g.now - g.last_ms > 500) g.last_ms = g.now;
      if (g.attract_ms > 45000 || p.s >= c_len) {
        g.ai = 0; g.course = (g.course + 1) % 6; build_course(g.course); place_objects(g.course); reset_skier();
        g.mode = TITLE; g.mode_ms = g.now;
      }
    }
  } else if (g.mode == COUNT) {
    int t = (int)(g.now - g.mode_ms);
    int beat = t / 1000;
    static int last_beat = -1;
    if (t < 50) last_beat = -1;
    if (beat != last_beat && beat <= 3) { last_beat = beat; sfx(CH_CLICK, I_CLICK, beat == 3 ? 84 : 72, 220, 0); }
    if (press & PRG32_BTN_START) set_mode(TITLE);
    else if (t >= 3000) { g.mode = PLAY; g.mode_ms = g.now; g.last_ms = g.now; toast(PNAME[g.course]); }
  } else if (g.mode == PLAY) {
    uint32_t start_press = press & PRG32_BTN_START;
    int steps = 0;
    while (g.now - g.last_ms >= DT && steps++ < 4 && g.mode == PLAY) {
      g.last_ms += DT; g.run_ms += DT;
      update_objects();
      uint32_t act = in;
      if (g.assist && !(in & (PRG32_BTN_LEFT | PRG32_BTN_RIGHT))) {
        act |= autopilot() & (PRG32_BTN_LEFT | PRG32_BTN_RIGHT);
        if (ap_avoid) act &= ~PRG32_BTN_UP;   /* MAESTRO stands you up to dodge */
      }
      physics_step(act, press);
      press = 0;
      if (p.s >= c_len) { finish_run(); break; }
      if (g.run_ms >= g.limit_ms) { set_mode(OVER); break; }
    }
    if (g.now - g.last_ms > 500) g.last_ms = g.now;
    if (g.mode == PLAY) ski_audio();
    if (start_press && g.mode == PLAY) { reset_skier(); set_mode(TITLE); }
  } else if (g.mode == CRASH) {
    int steps = 0;
    while (g.now - g.last_ms >= DT && steps++ < 4) { g.last_ms += DT; g.run_ms += DT; update_objects(); }
    if (g.now - g.last_ms > 500) g.last_ms = g.now;
    if (g.run_ms >= g.limit_ms) set_mode(OVER);
    else if (g.now - g.mode_ms > 1600) {
      recover();
      g.mode = PLAY; g.mode_ms = g.now; g.last_ms = g.now;
    }
  } else if (g.mode == WIN || g.mode == OVER) {
    if (g.now - g.mode_ms > 800) {
      if (press & (PRG32_BTN_A | PRG32_BTN_START)) begin_run(0);
      else if (press & PRG32_BTN_B) { g.ai = 0; reset_skier(); set_mode(TITLE); }
    }
  }
  multiplayer(in);
  if (g.mode == PLAY || g.mode == WIN) { int r = race_rank(); if (r) g.best_rank = r; }
}

/* ============================================================== rendering */
static struct { int32_t u, v, alt, s; int cs, sn, tanp, y0, yh, psi; } cam;
static int16_t sl_y[NSL], sl_x[NSL], sl_hw[NSL], sl_pw[NSL], sl_occ[NSL];
static int32_t sl_f[NSL], sl_s[NSL];
static int32_t delta[NSL];

static void world_at(int32_t s, int32_t x, int32_t *u, int32_t *v) {
  int phi = lerp16(c_phi, s);
  *u = lerp32(c_u, s) - x * isin(phi) / 4096;
  *v = lerp32(c_v, s) + x * icos(phi) / 4096;
}
/* Project a point on (or h cm above) the snow. Returns the depth, 0 if behind. */
static int32_t project(int32_t s, int32_t x, int32_t h, int *sx, int *sy) {
  int32_t u, v;
  world_at(s, x, &u, &v);
  int32_t du = u - cam.u, dv = v - cam.v;
  int32_t f = (du * cam.cs + dv * cam.sn) / 4096;
  int32_t l = (-du * cam.sn + dv * cam.cs) / 4096;
  if (f < 80) return 0;
  int32_t rel = lerp32(c_alt, s) + h - cam.alt + f * cam.tanp / 4096;
  int32_t px = 160 + FOCAL * l / f, py = cam.y0 - FOCAL * rel / f;
  *sx = (int)clampi((int)px, -2000, 2000);
  *sy = (int)clampi((int)py, -2000, 2000);
  return f;
}
static void setup_camera(void) {
  int target = p.h * 6 / 10;
  g.cam_yaw += (target - g.cam_yaw) / 4;
  int phi = lerp16(c_phi, p.s);
  cam.psi = phi + g.cam_yaw;
  cam.cs = icos(cam.psi); cam.sn = isin(cam.psi);
  int32_t u, v;
  world_at(p.s, p.x, &u, &v);
  cam.u = u - (int32_t)CAM_BACK * cam.cs / 4096;
  cam.v = v - (int32_t)CAM_BACK * cam.sn / 4096;
  cam.s = p.s - (int32_t)CAM_BACK * icos(g.cam_yaw) / 4096;
  int32_t gnd = lerp32(c_alt, p.s);
  cam.alt = lerp32(c_alt, cam.s) + CAM_H + (p.alt > gnd ? (p.alt - gnd) / 2 : 0);
  /* Pitch follows the average slope just ahead (60% of it), the slope
   * vanishing line moves down a little on steep pitches. */
  int sn = (lerp16(c_sin, p.s) + lerp16(c_sin, p.s + 1500) + lerp16(c_sin, p.s + 3000)) / 3;
  int cs = isqrt((uint32_t)(16777216 - sn * sn));
  int tan = sn * 4096 / (cs ? cs : 1);
  cam.tanp = tan * 6 / 10;
  cam.y0 = Y0 - FOCAL * tan / 4 / 4096;
  cam.yh = cam.y0 - FOCAL * cam.tanp / 4096;
}

static const int8_t RIDGE[36] = {14,18,24,30,34,32,26,22,20,24,19,14,12,16,20,18,13,10,12,15,18,22,20,16,12,10,14,17,21,26,23,18,14,11,10,12};
static const int8_t HILLS[36] = {6,9,7,5,8,11,9,6,4,6,8,10,7,5,6,9,12,10,7,5,4,7,9,8,6,5,8,11,9,6,5,7,10,8,6,5};
static int ridge(const int8_t *t, int deg) {
  deg %= 360; if (deg < 0) deg += 360;
  int i = deg / 10, f = deg % 10;
  return t[i] + (t[(i + 1) % 36] - t[i]) * f / 10;
}
static void background(void) {
  int yh = cam.yh;
  clip_y = H;
  box(0, 0, W, yh - 56, P_SKY0);
  box(0, yh - 56, W, 20, P_SKY1);
  box(0, yh - 36, W, 22, P_SKY2);
  box(0, yh - 14, W, 16, P_SKY3);
  box(0, yh + 2, W, H - yh - 2, P_VALLEY);
  for (int col = 0; col < W; col += 4) {
    int deg = (cam.psi + (col - 160) * 573 / 100) / 16;
    int hf = ridge(RIDGE, deg);
    int top = yh - hf;
    box(col, top, 4, hf + 3, P_FAR);
    if (hf > 17) box(col, top, 4, hf - 16 + (col & 4 ? 1 : 0), P_FARSNOW);
    int hm = ridge(HILLS, deg * 2 + 37);
    box(col, yh - hm + 3, 4, hm, P_MID);
    box(col, yh - hm + 3, 4, 1, P_MIDSNOW);
    /* The Alto Sangro plateau far below: snowfields cut by forest belts,
     * receding in bands that widen toward the viewer. */
    int band = 0;
    for (int y = yh + 4, hgt = 2; y < H; y += hgt, hgt += 2, band++) {
      uint32_t r = hash((uint32_t)((deg + band * 7) / (6 + band * 3) * 131 + band));
      if ((r & 3) == 0) box(col, y, 4, hgt / 2 + 1, P_VALLEY2);
      else if ((r & 15) == 1) box(col, y, 4, 1, P_FORESTFAR);
    }
  }
  /* Roccaraso and Rivisondoli on the plateau below. */
  static const int16_t TOWN[2] = {205, 245};
  for (int t = 0; t < 2; t++) {
    int d = TOWN[t] * 16 - cam.psi;
    while (d > 2880) d -= 5760;
    while (d < -2880) d += 5760;
    int x = 160 + d * 100 / 573;
    if (x < -30 || x > W + 30) continue;
    for (int i = 0; i < 7; i++) {
      int hx = x - 18 + i * 6 + (int)(hash((uint32_t)(i + t * 7)) % 3);
      box(hx, yh + 5 - (i & 1), 4, 3, P_TOWN);
      box(hx, yh + 4 - (i & 1), 4, 1, WHITE);
    }
  }
}

static void terrain_row(int y, int X, int hw, int pw, int lane, int band, int far) {
  int l0 = X - hw, r0 = X + hw;
  uint8_t forest = far ? P_FORESTFAR : (band ? P_FOREST : P_FOREST2);
  uint8_t powder = far ? P_PISTFAR : (band ? P_POWDER : P_POWDER2);
  hline(0, l0 - pw, y, forest);
  hline(l0 - pw, l0, y, powder);
  hline(r0, r0 + pw, y, powder);
  hline(r0 + pw, W, y, forest);
  int ew = hw / 18 + 1;
  hline(l0, l0 + ew, y, P_EDGE);
  hline(r0 - ew, r0, y, P_EDGE);
  int a0 = imax(l0 + ew, 0), a1 = imin(r0 - ew, W);
  if (far || lane < 4) { hline(a0, a1, y, far ? P_PISTFAR : (band ? P_PIST_A : P_PIST_C)); return; }
  int k = (a0 - X) / lane - 2;
  for (;; k++) {
    int a = X + k * lane, b = a + lane;
    if (a >= a1) break;
    if (b <= a0) continue;
    hline(imax(a, a0), imin(b, a1), y, (k & 1) ? (band ? P_PIST_B : P_PIST_D) : (band ? P_PIST_A : P_PIST_C));
  }
}
static void terrain(void) {
  int ymin = H;
  for (int i = 0; i < NSL; i++) {
    int32_t s = cam.s + delta[i];
    sl_s[i] = s;
    int sx, sy;
    int32_t f = project(s, 0, 0, &sx, &sy);
    if (!f) { sl_f[i] = 0; sl_y[i] = H; sl_occ[i] = (int16_t)ymin; continue; }
    int32_t hw = lerp16(c_hw, s);
    sl_f[i] = f; sl_x[i] = (int16_t)sx; sl_y[i] = (int16_t)clampi(sy, -100, 400);
    sl_hw[i] = (int16_t)clampi((int)(FOCAL * hw / f), 0, 4000);
    sl_pw[i] = (int16_t)clampi((int)(FOCAL * TREE_GAP / f), 0, 4000);
    int y0 = sl_y[i];
    if (i > 0 && sl_f[i - 1] && y0 < ymin) {
      int ya = sl_y[i - 1];
      int span = ya - y0;
      for (int y = imax(y0, 0); y < ymin; y++) {
        int t = span > 0 ? (y - y0) * 256 / span : 256;
        int X = sl_x[i] + (sl_x[i - 1] - sl_x[i]) * t / 256;
        int hwp = sl_hw[i] + (sl_hw[i - 1] - sl_hw[i]) * t / 256;
        int pw = sl_pw[i] + (sl_pw[i - 1] - sl_pw[i]) * t / 256;
        int32_t fr = sl_f[i] + (sl_f[i - 1] - sl_f[i]) * t / 256;
        int32_t sr = s + (sl_s[i - 1] - s) * t / 256;
        int lane = (int)(FOCAL * 450 / (fr > 1 ? fr : 1));
        int band = (int)((sr / 700) & 1);
        terrain_row(y, X, hwp, pw, lane, band, fr > 14000);
      }
      ymin = imax(y0, 0);
    } else if (i == 0 && y0 < ymin) {
      for (int y = imax(y0, 0); y < H; y++) {
        int lane = (int)(FOCAL * 450 / f);
        terrain_row(y, sl_x[0], sl_hw[0] + (y - y0) / 2, sl_pw[0] + (y - y0) / 3, lane, (int)((s / 700) & 1), 0);
      }
      ymin = imax(y0, 0);
    }
    sl_occ[i] = (int16_t)ymin;
  }
}
static int occlusion(int32_t s) {
  int occ = H;
  for (int i = 0; i < NSL; i++) {
    if (sl_s[i] > s) break;
    occ = sl_occ[i];
  }
  return occ;
}

/* Scalable scenery built from indexed rectangles. ppm16 = px per metre * 16. */
static void tree(int x, int y, int ppm16, int hm) {
  int hp = ppm16 * hm / 16;
  if (hp < 3) { box(x, y - 2, 1, 2, P_TREE); return; }
  if (hp < 8) { box(x - hp / 4, y - hp, imax(1, hp / 2), hp, P_TREE); box(x - hp / 6, y - hp, imax(1, hp / 3), 1, WHITE); return; }
  int tw = imax(1, hp / 14);
  box(x - tw, y - hp / 6, tw * 2, hp / 6, P_WOOD);
  box(x - hp / 3, y - 1, hp * 2 / 3, imax(1, hp / 18), P_SHADOW);
  for (int t = 0; t < 4; t++) {
    int top = y - hp + t * hp / 5;
    int bot = y - hp / 6 - (3 - t) * hp / 8;
    int wid = hp * (t + 2) / 13;
    int mid = (top + bot) / 2;
    box(x - wid / 2, top, wid, mid - top, P_TREE2);
    box(x - wid, mid, wid * 2, bot - mid, P_TREE);
    box(x - wid / 2, top, wid, imax(1, hp / 24), WHITE);
    box(x - wid, mid, wid / 2 + 1, imax(1, hp / 30), WHITE);
  }
}
static void pole(int x, int y, int ppm16, uint8_t col) {
  int hp = ppm16 * 18 / 160;
  if (hp < 2) return;
  int w = imax(1, ppm16 / 64);
  box(x, y - hp, w, hp, col);
  box(x, y - hp, w, imax(1, hp / 6), ORANGE);
}
static void banner(int xl, int xr, int y, int ppm16, int kind) {
  int hp = ppm16 * 45 / 160;
  int pw = imax(1, ppm16 / 40);
  uint8_t col = kind == OB_FINISH ? REDC : (kind == OB_START ? CI(0, 2, 0) : CI(0, 1, 4));
  box(xl, y - hp, pw, hp, P_STEEL);
  box(xr - pw, y - hp, pw, hp, P_STEEL);
  int bh = imax(2, ppm16 * 12 / 160);
  box(xl, y - hp, xr - xl, bh, col);
  box(xl, y - hp + bh, xr - xl, imax(1, bh / 6), WHITE);
  const char *t = kind == OB_FINISH ? "ARRIVO" : (kind == OB_START ? "PARTENZA" : "CHECKPOINT");
  int tw = slen(t) * 8;
  if (xr - xl > tw + 8 && bh >= 8 && y - hp >= 0 && y - hp + 8 < clip_y) txt((xl + xr - tw) / 2, y - hp + (bh - 8) / 2, t, WHITE, col);
}
static void hut(int x, int y, int ppm16, int name, int clip) {
  int w = ppm16 * 9 / 16, h = ppm16 * 4 / 16;
  if (w < 3) { box(x - 1, y - 2, 3, 2, P_WOOD); return; }
  box(x - w / 2, y - h, w, h, P_WOOD);
  for (int r = 0; r < 4; r++) box(x - w / 2 - w / 10 + r * w / 8, y - h - (r + 1) * h / 6, w + w / 5 - r * w / 4, h / 6 + 1, r == 3 ? WHITE : P_ROOF);
  box(x - w / 2 - w / 10, y - h - 1, w + w / 5, imax(1, h / 8), WHITE);
  if (w > 12) for (int i = 0; i < 3; i++) box(x - w / 2 + w / 8 + i * w / 3, y - h * 2 / 3, imax(1, w / 8), imax(1, h / 4), P_LIGHT);
  if (w > 110 && y - h - 20 >= 0 && y - h - 12 < clip) {
    const char *nm = HUTN[name];
    int tw = slen(nm) * 8;
    txt(x - tw / 2, y - h * 2 - 12, nm, YELLOW, CI(1, 0, 0));
  }
}
static void snowcat(int x, int y, int ppm16) {
  int w = ppm16 * 48 / 160, h = ppm16 * 28 / 160;
  if (w < 4) { box(x - 1, y - 2, 3, 2, P_CAT); return; }
  box(x - w / 2, y - h / 3, w, h / 3, BLACK);
  box(x - w / 2 + w / 12, y - h * 2 / 3, w - w / 6, h / 3, P_CAT);
  box(x - w / 4, y - h, w / 2, h / 3, P_CAT);
  box(x - w / 5, y - h + h / 12, w * 2 / 5, h / 5, CI(0, 1, 2));
  box(x - w * 6 / 10, y - h / 4, w * 12 / 10, imax(1, h / 6), P_STEEL);
  box(x - w / 3, y - h / 2, imax(1, w / 10), imax(1, h / 10), P_LIGHT);
  box(x + w / 3 - w / 10, y - h / 2, imax(1, w / 10), imax(1, h / 10), P_LIGHT);
  if ((g.now / 300) & 1) box(x - 1, y - h - imax(1, h / 8), imax(2, w / 14), imax(1, h / 8), ORANGE);
}
static void cannon(int x, int y, int ppm16, int side) {
  int hp = ppm16 * 40 / 160;
  if (hp < 3) return;
  int w = imax(1, ppm16 / 48);
  box(x, y - hp, w, hp, P_STEEL);
  int hx = x - side * hp / 4;
  box(hx - hp / 6, y - hp - hp / 8, hp / 3 + 1, hp / 4 + 1, YELLOW);
  for (int i = 0; i < 5; i++) {
    uint32_t r = hash((uint32_t)(g.now / 90 + i * 17));
    int d = (int)(r % (unsigned)(hp + 1));
    box(hx - side * d, y - hp - (int)((r >> 8) % (unsigned)(hp / 3 + 1)) + d / 3, imax(1, hp / 10), imax(1, hp / 10), WHITE);
  }
}
static void patch(int32_t s, int x, int w, int len, int kind) {
  int ax, ay, bx, by, cx2, cy2, dx2, dy2;
  if (!project(s, x - w, 0, &ax, &ay) || !project(s, x + w, 0, &bx, &by)) return;
  if (!project(s + len, x - w, 0, &cx2, &cy2) || !project(s + len, x + w, 0, &dx2, &dy2)) return;
  int ytop = imin(cy2, dy2), ybot = imax(ay, by);
  if (ybot - ytop > 120) return;
  for (int y = ytop; y < ybot; y++) {
    int t = ybot > ytop ? (y - ytop) * 256 / (ybot - ytop) : 0;
    int l = cx2 + (ax - cx2) * t / 256, r = dx2 + (bx - dx2) * t / 256;
    uint8_t c = kind == OB_ICE ? ((y & 1) ? P_ICE : P_PIST_B) : (kind == OB_KICKER ? (y < ytop + 2 ? ORANGE : P_MOGUL) : P_PIST_A);
    hline(l, r + 1, y, c);
  }
  if (kind == OB_KICKER) {   /* the lip of the ramp rises ~80 cm */
    int lx, ly, rx, ry;
    if (project(s + len, x - w, 80, &lx, &ly) && project(s + len, x + w, 80, &rx, &ry))
      for (int y = imin(ly, ry); y < ytop; y++) hline(lx, rx + 1, y, y == imin(ly, ry) ? ORANGE : WHITE);
  }
}
static void moguls(int32_t s, int x, int w) {
  for (int j = 4; j >= 0; j--)
    for (int i = -1; i <= 1; i++) {
      int mx, my;
      int32_t ms = s + j * 500 + (i & 1) * 250;
      int32_t f = project(ms, x + i * w * 2 / 3, 0, &mx, &my);
      if (!f) continue;
      int r = FOCAL * 110 / f;
      if (r < 1) { box(mx, my, 2, 1, P_MOGUL); continue; }
      box(mx - r, my - r / 2, r * 2, r / 2 + 1, P_POWDER);
      box(mx - r + r / 4, my - r / 2 - r / 3, r * 3 / 2, r / 3 + 1, P_POWDER);
      box(mx - r, my, r * 2, imax(1, r / 3), P_MOGUL);
    }
}
static void lift(int32_t s, int hw) {
  int lx, ly, rx, ry, ltx, lty, rtx, rty;
  if (!project(s, -(hw + 900), 0, &lx, &ly) || !project(s, -(hw + 900), 1100, &ltx, &lty)) return;
  if (!project(s + 300, hw + 900, 0, &rx, &ry) || !project(s + 300, hw + 900, 1100, &rtx, &rty)) return;
  int pw = imax(1, (ly - lty) / 16);
  box(lx - pw / 2, lty, pw, ly - lty, P_STEEL);
  box(rx - pw / 2, rty, pw, ry - rty, P_STEEL);
  box(ltx - pw * 3, lty, pw * 6, imax(1, pw / 2), P_STEEL);
  box(rtx - pw * 3, rty, pw * 6, imax(1, pw / 2), P_STEEL);
  for (int i = 0; i <= 24; i++) {     /* sagging cable */
    int t = i * 256 / 24;
    int sag = (t * (256 - t)) / 256 * (ly - lty) / 600;
    int cx = ltx + (rtx - ltx) * t / 256, cy = lty + (rty - lty) * t / 256 + sag;
    box(cx, cy, (rtx - ltx) / 24 + 1, 1, BLACK);
  }
  for (int c = 0; c < 3; c++) {        /* chairs travelling along the line */
    int t = (int)((g.now / 24 + (uint32_t)c * 85) % 256);
    int sag = (t * (256 - t)) / 256 * (ly - lty) / 600;
    int cx = ltx + (rtx - ltx) * t / 256, cy = lty + (rty - lty) * t / 256 + sag;
    int ch = imax(2, (ly - lty) / 6);
    box(cx, cy, 1, ch, BLACK);
    box(cx - ch / 2, cy + ch, ch, imax(1, ch / 3), CI(0, 1, 3));
  }
}

/* One band of gully wall between depth slice i (far) and i-1 (near), both
 * sides: the face from the wall foot up to the rim, the rim and the forest
 * or snow on the plateau beyond, drawn in strips of a few rows. */
static void wall_band(int i) {
  int32_t s0 = sl_s[i], s1 = sl_s[i - 1];
  int e0 = env_at(s0), e1 = env_at(s1);
  int hw0 = lerp16(c_hw, s0), hw1 = lerp16(c_hw, s1);
  int rib = (int)((s0 / 700) & 1);       /* alternate shades every 7 m, like the piste */
  for (int side = -1; side <= 1; side += 2) {
    int w0 = wall_of(e0, side), w1 = wall_of(e1, side);
    if (!w0 && !w1) continue;
    int bx0, by0, tx0, ty0, bx1, by1, tx1, ty1;
    if (!project(s0, side * (hw0 + WALL_FOOT), 0, &bx0, &by0) || !project(s1, side * (hw1 + WALL_FOOT), 0, &bx1, &by1)) continue;
    project(s0, side * (hw0 + WALL_FOOT + WALL_RUN[w0]), WALL_H[w0], &tx0, &ty0);
    project(s1, side * (hw1 + WALL_FOOT + WALL_RUN[w1]), WALL_H[w1], &tx1, &ty1);
    int lvl = imax(w0, w1);
    uint8_t face;
    if (lvl >= 3) face = side < 0 ? (rib ? P_SHADOW : P_MOGUL) : (rib ? P_FAR : P_SHADOW);     /* rock and snow */
    else face = side < 0 ? (rib ? P_POWDER2 : P_EDGE) : (rib ? P_EDGE : P_SHADOW);            /* snow banks */
    int fo = forest_of(e1, side);
    uint8_t top = fo >= 2 ? P_FOREST2 : (fo ? P_FORESTFAR : P_POWDER);
    int ya = imax(imin(ty0, ty1), 0), yb = imin(imax(by0, by1), clip_y);
    int stepy = clampi((yb - ya) / 24, sl_f[i] > 2500 ? 2 : 1, 4);   /* coarser far away */
    for (int y = ya; y < yb; y += stepy) {
      int yc = y + stepy / 2;
      /* The band is the quad rim0-foot0-foot1-rim1: inside it is bounded by the
       * far cross-section and then the foot line, outside by the rim line and
       * then the near cross-section. */
      int inner, rim;
      if (yc < by0) inner = by0 > ty0 ? tx0 + (bx0 - tx0) * (yc - ty0) / (by0 - ty0) : bx0;
      else inner = by1 > by0 ? bx0 + (bx1 - bx0) * (yc - by0) / (by1 - by0) : bx1;
      if (yc < ty0) inner = tx0;
      if (yc < ty1) rim = ty1 > ty0 ? tx0 + (tx1 - tx0) * (yc - ty0) / (ty1 - ty0) : tx1;
      else rim = by1 > ty1 ? tx1 + (bx1 - tx1) * (yc - ty1) / (by1 - ty1) : bx1;
      int hgt = imin(stepy, yb - y);
      if (side < 0) {
        inner = clampi(inner, 0, W); rim = clampi(rim, 0, inner);
        if (rim > 0 && yc < ty1) box(0, y, rim, hgt, top);
        if (inner > rim) box(rim, y, inner - rim, hgt, face);
      } else {
        inner = clampi(inner, 0, W); rim = clampi(rim, inner, W);
        if (rim < W && yc < ty1) box(rim, y, W - rim, hgt, top);
        if (rim > inner) box(inner, y, rim - inner, hgt, face);
      }
    }
  }
}

typedef struct { int32_t f; int16_t x, y, ppm, clip; uint8_t kind, a; int16_t b; } dl_t;
static dl_t dl[DL_MAX];
static int ndl;
enum { D_TREE = 100, D_POLE, D_PEER, D_PLAYER, D_WALL };
static void dl_add(int32_t f, int x, int y, int kind, int a, int b, int32_t s) {
  if (ndl >= DL_MAX || f <= 0) return;
  if (x < -400 || x > W + 400) return;
  dl_t *e = &dl[ndl++];
  e->f = f; e->x = (int16_t)x; e->y = (int16_t)y; e->kind = (uint8_t)kind; e->a = (uint8_t)a; e->b = (int16_t)b;
  e->ppm = (int16_t)clampi((int)(FOCAL * 1600 / f), 0, 32000);
  e->clip = (int16_t)occlusion(s);
}
static void gather(void) {
  ndl = 0;
  int k0 = (int)(cam.s / STEP) + 1, k1 = imin((int)((cam.s + 40000) / STEP), c_total - 2);
  int diffc = DIFF_COL[DIFF[g.course]];
  for (int k = k1; k >= k0; k--) {
    int hw = c_hw[k];
    for (int side = -1; side <= 1; side += 2) {
      uint32_t h = hash((uint32_t)(k * 2 + (side > 0) + g.course * 4096));
      int32_t s = (int32_t)k * STEP + (int32_t)(h % 700);
      int x, y;
      int32_t f;
      /* Forest as dense as on the map; on a gully side the trees stand on the rim. */
      static const uint8_t TREE_P[4] = {10, 45, 80, 96};
      int env = c_env[k], fo = forest_of(env, side), wl = wall_of(env, side);
      int lat = wl ? hw + WALL_FOOT + WALL_RUN[wl] + 100 : hw + TREE_GAP + 150;
      int th = wl ? WALL_H[wl] : 0;
      if ((int)((h >> 10) % 100) < TREE_P[fo]) {
        f = project(s, side * (lat + (int)((h >> 3) % 300)), th, &x, &y);
        dl_add(f, x, y, D_TREE, (uint8_t)(7 + (h >> 20) % 5), 0, s);
      }
      if (fo >= 2 && k < (int)(cam.s / STEP) + 18 && ((h >> 14) & 1)) {
        f = project(s + 300, side * (lat + 550 + (int)((h >> 5) % 500)), th, &x, &y);
        dl_add(f, x, y, D_TREE, (uint8_t)(8 + (h >> 22) % 4), 0, s + 300);
      }
      if (wl >= 2 && fo && ((h >> 16) & 3) == 0) {     /* a pine clinging to the gully side */
        f = project(s + 500, side * (hw + WALL_FOOT + WALL_RUN[wl] / 2), WALL_H[wl] / 2, &x, &y);
        dl_add(f, x, y, D_TREE, (uint8_t)(5 + (h >> 24) % 3), 0, s + 500);
      }
      if (k % 2 == 0 && k < c_n) { f = project((int32_t)k * STEP, side * (hw + 40), 0, &x, &y); dl_add(f, x, y, D_POLE, (uint8_t)diffc, 0, (int32_t)k * STEP); }
    }
  }
  for (int i = 1; i < NSL; i++)   /* gully wall bands between depth slices */
    if (sl_f[i] && sl_f[i - 1] && (wall_of(env_at(sl_s[i]), -1) || wall_of(env_at(sl_s[i]), 1) ||
                                  wall_of(env_at(sl_s[i - 1]), -1) || wall_of(env_at(sl_s[i - 1]), 1)))
      dl_add(sl_f[i - 1] + 1, 160, sl_y[i - 1], D_WALL, (uint8_t)i, 0, sl_s[i - 1]);
  for (int i = 0; i < nob; i++) {
    obj_t *o = &ob[i];
    if (o->s < cam.s + 200 || o->s > cam.s + 40000) continue;
    int x, y;
    int32_t f = project(o->s, o->x, 0, &x, &y);
    if (o->type == OB_LIFT || o->type == OB_MOGULS || o->type == OB_ICE || o->type == OB_KICKER || o->type == OB_CHECK || o->type == OB_START || o->type == OB_FINISH) x = 160;
    dl_add(f, x, y, o->type, 0, (int16_t)i, o->s);
  }
  if (g.joined) {
    int n = prg32_multiplayer_get_peer_count();
    for (int i = 0; i < n; i++) {
      prg32_player_state_t q;
      if (prg32_multiplayer_get_peer(i, &q) != 0 || !peer_on_course(&q)) continue;
      int32_t s = (int32_t)q.y * 20;
      if (s < cam.s + 200 || s > cam.s + 40000) continue;
      int x, y;
      int32_t f = project(s, q.x, 0, &x, &y);
      dl_add(f, x, y, D_PEER, (uint8_t)((q.sprite >> 4) % SPR_FRAMES), (int16_t)i, s);
    }
  }
  if (g.mode != TITLE) {
    int x, y;
    int32_t f = project(p.s, p.x, 0, &x, &y);
    dl_add(f, x, y, D_PLAYER, 0, 0, p.s);
  }
  for (int i = 1; i < ndl; i++) {   /* far to near */
    dl_t e = dl[i];
    int j = i - 1;
    while (j >= 0 && dl[j].f < e.f) { dl[j + 1] = dl[j]; j--; }
    dl[j + 1] = e;
  }
}
static void draw_items(void) {
  for (int i = 0; i < ndl; i++) {
    dl_t *e = &dl[i];
    clip_y = e->clip;
    if (e->y - 2 > clip_y && e->kind != D_WALL && e->kind != OB_LIFT && e->kind != OB_CHECK && e->kind != OB_FINISH && e->kind != OB_START) continue;
    obj_t *o = &ob[e->b];
    switch (e->kind) {
      case D_TREE: tree(e->x, e->y, e->ppm, e->a); break;
      case D_WALL: wall_band(e->a); break;
      case D_POLE: pole(e->x, e->y, e->ppm, e->a); break;
      case D_PEER: {
        int x = e->x, y = e->y;
        box(x - e->ppm / 32, y - 1, e->ppm / 16 + 1, 2, P_SHADOW);
        draw_person(x, y, e->ppm, e->a, PAL_RIVAL, clip_y, 100);
        if (e->ppm > 300 && y - 44 > 0) txt(x - 12, y - 44, "NET", YELLOW, CI(0, 2, 0));
        break;
      }
      case D_PLAYER: {
        int gx = e->x, gy = e->y;
        int air = (int)(p.alt - ground_at(p.s));
        int ay = gy - (air > 0 ? FOCAL * air / (int)e->f : 0);
        int sw = e->ppm / 14;
        box(gx - sw / 2, gy - 2, sw, 3, P_SHADOW);
        clip_y = H;
        int frame = g.mode == CRASH ? SPR_FALLEN : p.pose;
        if (p.scrape > 60 && g.mode == PLAY && !p.air)
          for (int k = 0; k < 6; k++) {
            uint32_t r = hash(g.now / 33 + (uint32_t)k * 31);
            box(gx - sgn(p.w) * (int)(r % 18) - 2, gy - (int)((r >> 8) % 10), 2, 2, WHITE);
          }
        draw_person(gx, ay + 1, e->ppm, frame, PAL_PLAYER, H, 100);
        if (g.mode == CRASH) for (int k = 0; k < 10; k++) {
          uint32_t r = hash((uint32_t)k * 77 + (g.now - g.mode_ms) / 60);
          int t = (int)(g.now - g.mode_ms) / 30;
          box(gx - 20 + (int)(r % 40), gy - 6 - (int)((r >> 8) % (unsigned)(10 + t)), 3, 3, WHITE);
        }
        break;
      }
      case OB_TOURIST: case OB_KID: {
        int dir = isin(o->phase + 1440);
        int frame = dir > 1500 ? SPR_LEAN_R1 : (dir < -1500 ? SPR_LEAN_L1 : SPR_PLOUGH);
        box(e->x - e->ppm / 40, e->y - 1, e->ppm / 20 + 1, 2, P_SHADOW);
        draw_person(e->x, e->y, e->ppm, frame, o->pal, clip_y, o->type == OB_KID ? 60 : 100);
        break;
      }
      case OB_BOARDER: draw_person(e->x, e->y, e->ppm, SPR_BOARDER, o->pal, clip_y, 100); break;
      case OB_CAT: snowcat(e->x, e->y, e->ppm); break;
      case OB_CANNON: cannon(e->x, e->y, e->ppm, sgn(o->x)); break;
      case OB_HUT: hut(e->x, e->y, e->ppm, o->name, clip_y); break;
      case OB_MOGULS: moguls(o->s, o->x, o->w); break;
      case OB_ICE: patch(o->s, o->x, o->w, 2500, OB_ICE); break;
      case OB_KICKER: patch(o->s, o->x, o->w, 400, OB_KICKER); break;
      case OB_LIFT: lift(o->s, lerp16(c_hw, o->s)); break;
      case OB_CHECK: case OB_START: case OB_FINISH: {
        int hw = lerp16(c_hw, o->s);
        int lx, ly, rx, ry;
        if (project(o->s, -hw, 0, &lx, &ly) && project(o->s, hw, 0, &rx, &ry) && ly <= clip_y + 2)
          banner(lx, rx, (ly + ry) / 2, e->ppm, e->kind);
        break;
      }
    }
  }
  clip_y = H;
}

static void scene(void) {
  setup_camera();
  background();
  terrain();
  gather();
  draw_items();
}

/* -------------------------------------------------------------------- HUD */
static void profile_panel(int x0, int y0) {
  box(x0, y0, 76, 34, HUDC);
  for (int i = 0; i < 64; i++) {
    int y = y0 + 30 - prof[i];
    box(x0 + 6 + i, y, 1, y0 + 31 - y, HUD2);
    box(x0 + 6 + i, y, 1, 1, CYAN);
  }
  int pi = clampi((int)(p.s * 63 / (c_len ? c_len : 1)), 0, 63);
  box(x0 + 5 + pi, y0 + 29 - prof[pi], 3, 3, REDC);
  if (g.joined) {
    int n = prg32_multiplayer_get_peer_count();
    for (int i = 0; i < n; i++) {
      prg32_player_state_t q;
      if (prg32_multiplayer_get_peer(i, &q) != 0 || !peer_on_course(&q)) continue;
      int qi = clampi((int)((int32_t)q.y * 20 * 63 / (c_len ? c_len : 1)), 0, 63);
      box(x0 + 5 + qi, y0 + 29 - prof[qi], 3, 3, CI(0, 5, 1));
    }
  }
  char b[8];
  num(b, (int)(p.alt / 100), 4);
  b[4] = ' '; b[5] = 'M'; b[6] = 0;
  txt(x0 + 20, y0 + 1, b, WHITE, HUDC);
}
static void hud(void) {
  char b[24];
  box(0, 0, W, 12, HUDC);
  uint8_t dc = DIFF_COL[DIFF[g.course]];
  box(2, 1, 20, 10, dc == BLACK ? CI(1, 1, 1) : dc);
  txt(4, 2, PCODE[g.course], WHITE, dc == BLACK ? CI(1, 1, 1) : dc);
  txt(26, 2, PNAME[g.course], WHITE, HUDC);
  int left = (int)g.limit_ms - (int)g.run_ms;
  fmt_time(b, left);
  uint8_t tc = left < 10000 && ((g.now / 250) & 1) ? REDC : YELLOW;
  txt(W - 8 * 6 - 4, 2, b, tc, HUDC);
  txt(W - 8 * 6 - 4 - 40, 2, "PASS", CI(3, 3, 5), HUDC);
  /* speed */
  box(2, 172, 92, 26, HUDC);
  num(b, p.v * 36 / 1000, 3);
  big(6, 177, b, 2, YELLOW, 0);
  txt(48, 176, "KM/H", WHITE, HUDC);
  const char *st = p.air ? "AIR" : (p.surface == 2 ? "ICE" : (p.surface == 1 ? "POWDER" : (p.surface == 3 ? "MOGULS" : (p.tuck ? "TUCK" : (p.plough ? "BRAKE" : "")))));
  txt(48, 187, st, CYAN, HUDC);
  profile_panel(242, 164);
  if (g.joined) {
    box(242, 151, 76, 11, HUDC);
    int r = g.best_rank;
    if (r) { b[0] = 'P'; b[1] = (char)('0' + r / 16); b[2] = '/'; b[3] = (char)('0' + (r & 15)); b[4] = 0; }
    else scopy(b, "SOLO");
    txt(246, 153, "NET", CI(0, 5, 1), HUDC);
    txt(278, 153, b, WHITE, HUDC);
  }
  if (g.now < g.toast_until) {
    int tw = slen(g.toast) * 8;
    box((W - tw) / 2 - 4, 16, tw + 8, 12, HUDC);
    txt((W - tw) / 2, 18, g.toast, YELLOW, HUDC);
  }
}

static void title_screen(void) {
  p.s = 1200; p.x = 0; p.h = 0; p.alt = ground_at(p.s);
  scene();
  big_c(6, "SKIPASS MADNESS", 2, WHITE, CI(0, 0, 2));
  txt_c(24, "AFTER CHRISTMAS", YELLOW, CI(0, 0, 2));
  box(14, 40, 292, 106, HUDC);
  txt_c(44, "ROCCARASO - ALTO SANGRO", CYAN, HUDC);
  for (int i = 0; i < 6; i++) {
    int y = 57 + i * 12;
    uint8_t bg = i == g.course ? CI(0, 1, 3) : HUDC;
    box(18, y - 2, 284, 11, bg);
    uint8_t dc = DIFF_COL[DIFF[i]];
    if (dc == BLACK) dc = CI(1, 1, 1);
    box(20, y - 1, 20, 10, dc);
    txt(22, y, PCODE[i], WHITE, dc);
    txt(46, y, PNAME[i], i == g.course ? YELLOW : WHITE, bg);
    char b[12];
    num(b, TOP_M[i], 4); b[4] = '-'; num(b + 5, BOT_M[i], 4); b[9] = 'M'; b[10] = 0;
    txt(214, y, b, CI(3, 4, 5), bg);
  }
  box(14, 150, 292, 22, HUDC);
  txt_c(152, "LEFT/RIGHT PISTE   A SKI!", WHITE, HUDC);
  txt(18, 132, g.assist ? "B: MAESTRO ASSIST ON " : "B: MAESTRO ASSIST OFF", g.assist ? CI(0, 5, 1) : GREY, HUDC);
  if (g.joined) {
    char b[32]; int n = scopy(b, "NET ROOM: ");
    b[n] = (char)('0' + g.peers_online + 1); scopy(b + n + 1, " SKIERS ONLINE");
    txt_c(162, b, CI(0, 5, 1), HUDC);
  } else txt_c(162, "SOLO - NETWORK OFFLINE", CI(3, 3, 3), HUDC);
  if ((g.now / 600) & 1) txt_c(184, "PRESS A OR START", YELLOW, CI(0, 0, 2));
}
static void results(int win) {
  box(36, 36, 248, 128, HUDC);
  if (win) big_c(44, "PISTE CONQUERED!", 2, YELLOW, BLACK);
  else big_c(44, "SKIPASS EXPIRED", 2, REDC, BLACK);
  char b[32], t[8];
  if (win) {
    int n = scopy(b, "ARRIVO ");
    scopy(b + n, PFINISH[g.course]);
    txt_c(66, b, CYAN, HUDC);
    fmt_time(t, (int)g.run_ms);
    n = scopy(b, "TIME     "); scopy(b + n, t);
    txt(64, 80, b, WHITE, HUDC);
  } else {
    txt_c(66, "THE MOUNTAIN WINS", CYAN, HUDC);
    int togo = (int)((c_len - p.s) / 100);
    int n = scopy(b, "TO GO    "); num(b + n, togo, 4); scopy(b + n + 4, " M");
    txt(64, 80, b, WHITE, HUDC);
  }
  int n = scopy(b, "TOP      "); num(b + n, g.top_kmh, 3); scopy(b + n + 3, " KM/H");
  txt(64, 92, b, WHITE, HUDC);
  n = scopy(b, "FALLS    "); num(b + n, g.crashes, 2);
  txt(64, 104, b, WHITE, HUDC);
  n = scopy(b, "GATES    "); num(b + n, g.checks, 2);
  txt(64, 116, b, WHITE, HUDC);
  n = scopy(b, "SCORE    "); num(b + n, g.score, 6);
  txt(64, 128, b, YELLOW, HUDC);
  if (g.best_rank) { b[0] = 'N'; b[1] = 'E'; b[2] = 'T'; b[3] = ' '; b[4] = 'P'; b[5] = (char)('0' + g.best_rank / 16); b[6] = '/'; b[7] = (char)('0' + (g.best_rank & 15)); b[8] = 0; txt(200, 104, b, CI(0, 5, 1), HUDC); }
  if (g.now - g.mode_ms > 800) txt_c(148, "A RETRY   B TITLE", CI(3, 5, 5), HUDC);
}

void skipass_madness_draw(void) {
  if (!delta[1]) { int32_t d = 300; for (int i = 0; i < NSL; i++) { delta[i] = d; d = d * 11 / 10 + 20; } }
  if (g.mode == TITLE) { title_screen(); return; }
  scene();
  if (g.mode == ATTRACT) {
    hud();
    box(70, 40, 180, 22, HUDC);
    txt_c(44, "DEMO - ROCCARASO", YELLOW, HUDC);
    if ((g.now / 500) & 1) txt_c(53, "PRESS ANY BUTTON", WHITE, HUDC);
    return;
  }
  hud();
  if (g.mode == COUNT) {
    int t = (int)(g.now - g.mode_ms) / 1000;
    char b[6];
    if (t < 3) { b[0] = (char)('3' - t); b[1] = 0; } else scopy(b, "VIA!");
    big_c(52, b, 5, YELLOW, BLACK);
    txt_c(100, PNAME[g.course], WHITE, HUDC);
  } else if (g.mode == CRASH) {
    static const char why[6][12] = {"WIPEOUT!", "BAD LANDING", "INTO TREES!", "SNOWCAT!", "COLLISION!", "GULLY WALL!"};
    int w = clampi(g.crash_reason, 0, 5);
    box(80, 64, 160, 20, HUDC);
    txt_c(70, why[w], ORANGE, HUDC);
  } else if (g.mode == WIN) results(1);
  else if (g.mode == OVER) results(0);
}
