/* Host harness for Skipass Madness: compiles game.c against a stub PRG32 API.
 *
 *   host render <course> <ms> <out.ppm> [keys]  render a frame of a played run
 *   host title <course> <out.ppm>                render the title screen
 *   host sim                                     autopilot time and falls per piste
 *   host fuzz <seconds>                          random input soak (use sanitizers)
 *   host plan                                    course centrelines and profiles (CSV)
 *
 * keys: one character per 33 ms frame from: l r u d a b s . (none);
 * the last key repeats. Not part of the cartridge. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../game.c"

static uint16_t fb[200][320], palette[256];
static long rect_calls, rect_px;
static uint32_t host_ms, host_in;
static int peers_fake;

uint32_t prg32_ticks_ms(void) { return host_ms; }
uint32_t prg32_input_read(void) { return host_in; }
void prg32_palette_set(uint8_t i, uint16_t c) { palette[i] = c; }
void prg32_gfx_rect_indexed(int x, int y, int w, int h, uint8_t index) {
  if (x < 0 || y < 0 || x + w > 320 || y + h > 200 || w <= 0 || h <= 0) { fprintf(stderr, "rect out of range %d %d %d %d\n", x, y, w, h); abort(); }
  rect_calls++; rect_px += (long)w * h;
  for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) fb[j][i] = palette[index];
}
void prg32_gfx_text8(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  for (; *s; s++, x += 8) {
    const uint8_t *gl = FONT[glyph(*s)];
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
      int on = r < 7 && c >= 1 && c < 6 && (gl[r] >> (5 - c) & 1);
      if (x + c >= 0 && x + c < 320 && y + r >= 0 && y + r < 200) fb[y + r][x + c] = on ? fg : bg;
    }
  }
}
void prg32_sprite_draw_indexed(int x, int y, const prg32_indexed_sprite_t *sp, uint32_t frame) {
  if (frame >= sp->frame_count) { fprintf(stderr, "bad frame\n"); abort(); }
  const uint8_t *px = sp->pixels + frame * sp->width * sp->height / 2;
  for (int j = 0; j < sp->height; j++) for (int i = 0; i < sp->width; i++) {
    int n = j * sp->width + i;
    int c = (n & 1) ? (px[n >> 1] & 15) : (px[n >> 1] >> 4);
    if (c == sp->transparent_index) continue;
    if (x + i >= 0 && x + i < 320 && y + j >= 0 && y + j < 200) fb[y + j][x + i] = sp->palette[c];
  }
}
static int audio_log;
void prg32_audio_note_on_pan(uint8_t ch, uint8_t inst, uint8_t note, uint8_t vol, int8_t pan) { if (audio_log) printf("%u note ch%u inst%u n%u v%u p%d\n", host_ms, ch, inst, note, vol, pan); }
void prg32_audio_note_off(uint8_t ch) { (void)ch; }
void prg32_audio_stop_track(void) {}
void prg32_audio_stop_all(void) {}
void prg32_audio_play_track(uint16_t t) { if (audio_log) printf("%u PLAY_TRACK %u\n", host_ms, t); }
void prg32_audio_set_master_volume(uint8_t v) { (void)v; }
void prg32_audio_set_channel_volume(uint8_t ch, uint8_t v) { (void)ch; (void)v; }
void prg32_audio_set_channel_pan(uint8_t ch, int8_t pan) { (void)ch; (void)pan; }
bool prg32_multiplayer_available(void) { return peers_fake > 0; }
void prg32_multiplayer_init(void) {}
int prg32_multiplayer_join(const char *sig, uint32_t f) { (void)sig; (void)f; return 0; }
void prg32_multiplayer_tick(void) {}
int prg32_multiplayer_set_local_state(int16_t x, int16_t y, uint16_t s, uint16_t f) { (void)x; (void)y; (void)s; (void)f; return 0; }
int prg32_multiplayer_set_input(uint32_t i) { (void)i; return 0; }
int prg32_multiplayer_get_peer_count(void) { return peers_fake; }
int prg32_multiplayer_get_peer(int i, prg32_player_state_t *o) {
  memset(o, 0, sizeof *o);
  o->player_id = 100 + i; o->x = (int16_t)(i ? 600 : -500); o->y = (int16_t)((p.s + 2500 + i * 1800) / 20);
  o->sprite = (uint16_t)(g.course | (SPR_TUCK << 4)); o->flags = PLAY;
  return 0;
}

static void save(const char *path) {
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6 320 200 255\n");
  for (int y = 0; y < 200; y++) for (int x = 0; x < 320; x++) {
    uint16_t c = fb[y][x];
    unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}
static uint32_t key(char k) {
  switch (k) { case 'l': return PRG32_BTN_LEFT; case 'r': return PRG32_BTN_RIGHT; case 'u': return PRG32_BTN_UP;
    case 'd': return PRG32_BTN_DOWN; case 'a': return PRG32_BTN_A; case 'b': return PRG32_BTN_B; case 's': return PRG32_BTN_START; }
  return 0;
}
static void frame(void) { skipass_madness_update(); skipass_madness_draw(); host_ms += 33; }

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  host_ms = 1000;
  if (getenv("PEERS")) peers_fake = atoi(getenv("PEERS"));
  audio_log = getenv("AUDIO_LOG") != NULL;
  for (int i = 16; i < 232; i++) palette[i] = c565((uint8_t)i);   /* firmware default colour cube */
  skipass_madness_init();
  if (!strcmp(argv[1], "title")) {
    int c = atoi(argv[2]);
    for (int i = 0; i < c; i++) { host_in = PRG32_BTN_RIGHT; frame(); host_in = 0; frame(); }
    frame();
    save(argv[3]);
    return 0;
  }
  if (!strcmp(argv[1], "render")) {
    int c = atoi(argv[2]), ms = atoi(argv[3]);
    const char *keys = argc > 5 ? argv[5] : "";
    for (int i = 0; i < c; i++) { host_in = PRG32_BTN_RIGHT; frame(); host_in = 0; frame(); }
    host_in = PRG32_BTN_A; frame(); host_in = 0; frame();
    int n = 0, auto_pilot = !strcmp(keys, "auto");
    for (uint32_t t = 0; t < (uint32_t)ms; t += 33, n++) {
      if (auto_pilot) host_in = (g.mode == PLAY) ? autopilot() : 0;
      else { size_t L = strlen(keys); host_in = L ? key(keys[(size_t)n < L ? n : L - 1]) : 0; }
      frame();
    }
    save(argv[4]);
    rect_calls = rect_px = 0; skipass_madness_draw();
    printf("last frame: %ld rects, %ld px\n", rect_calls, rect_px);
    printf("mode %d s %d m x %d v %d km/h alt %d h %d crashes %d run %u limit %u\n", g.mode, (int)(p.s / 100), (int)p.x, p.v * 36 / 1000,
           (int)(p.alt / 100), p.h / 16, g.crashes, g.run_ms, g.limit_ms);
    return 0;
  }
  if (!strcmp(argv[1], "sim")) {
    for (int c = 0; c < 6; c++) {
      g.course = c;
      begin_run(0);
      g.mode = PLAY;
      int steps = 0, maxk = 0;
      while (p.s < c_len && steps < 20000) {
        g.now += DT;
        update_objects();
        uint32_t a = autopilot();
        physics_step(a, a);
        if (g.mode == CRASH) {
          if (getenv("WHY")) printf("  fall %d at %d m x %d\n", g.crash_reason, (int)(p.s / 100), (int)p.x);
          recover(); g.mode = PLAY; steps += 48;
        }
        steps++;
        if (getenv("TRACE") && atoi(getenv("TRACE")) == c && steps % 60 == 0)
          printf("  t %5.1f s %5d m v %3d km/h grade %3d%% hw %4d x %5d h %4d bend %4d env %02x\n", steps * 0.033, (int)(p.s / 100), p.v * 36 / 1000,
                 lerp16(c_sin, p.s) * 100 / 4096, lerp16(c_hw, p.s), (int)p.x, p.h / 16, (lerp16(c_phi, p.s + 1000) - lerp16(c_phi, p.s)) / 16, c_env[p.s / STEP]);
        int k = p.v * 36 / 1000; if (k > maxk) maxk = k;
      }
      printf("%s %-17s %4d m drop %3d m  time %5.1f s  falls %d  top %d km/h  avg %d km/h  objects %d\n", PCODE[c], PNAME[c], (int)(c_len / 100),
             TOP_M[c] - BOT_M[c], steps * 0.033, g.crashes, maxk, (int)(c_len / 100 * 3.6 / (steps * 0.033)), nob);
    }
    return 0;
  }
  if (!strcmp(argv[1], "plan")) {
    printf("course,sample,u_m,v_m,alt_m,half_width_m,env\n");
    for (int c = 0; c < 6; c++) {
      build_course(c);
      for (int i = 0; i < c_n; i++) printf("%d,%d,%d,%d,%d,%d,%d\n", c, i, (int)(c_u[i] / 100), (int)(c_v[i] / 100), (int)(c_alt[i] / 100), c_hw[i] / 100, c_env[i]);
    }
    return 0;
  }
  if (!strcmp(argv[1], "fuzz")) {
    int secs = atoi(argv[2]);
    srand(7);
    uint32_t hold = 0;
    for (int i = 0; i < secs * 30; i++) {
      if (rand() % 6 == 0) hold = (uint32_t)rand() & 0x7f;
      host_in = hold;
      if (rand() % 400 == 0) host_ms += (uint32_t)(rand() % 3000);
      frame();
    }
    printf("fuzz ok: mode %d course %d s %d\n", g.mode, g.course, (int)p.s);
    return 0;
  }
  return 2;
}
