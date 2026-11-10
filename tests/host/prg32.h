/* Minimal host stand-in for the PRG32 cartridge API used by game.c.
 * Only for the host harness in tests/host (rendering previews, autopilot
 * timing and sanitizer fuzzing); the real cartridge uses the SDK header. */
#ifndef HOST_PRG32_H
#define HOST_PRG32_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PRG32_BTN_LEFT (1u << 0)
#define PRG32_BTN_RIGHT (1u << 1)
#define PRG32_BTN_UP (1u << 2)
#define PRG32_BTN_DOWN (1u << 3)
#define PRG32_BTN_A (1u << 4)
#define PRG32_BTN_B (1u << 5)
#define PRG32_BTN_START (1u << 6)
#define PRG32_SPRITE_BPP_4 4
#define PRG32_MP_FLAG_ENABLE (1u << 0)

typedef struct {
  const uint8_t *pixels;
  const uint16_t *palette;
  uint16_t width, height, frame_count, palette_count;
  uint8_t bits_per_pixel;
  int16_t transparent_index;
} prg32_indexed_sprite_t;

typedef struct {
  uint32_t player_id;
  int16_t x, y;
  uint16_t sprite, flags;
  uint32_t input, frame, last_seen_ms;
} prg32_player_state_t;

uint32_t prg32_ticks_ms(void);
uint32_t prg32_input_read(void);
void prg32_gfx_rect_indexed(int x, int y, int w, int h, uint8_t index);
void prg32_gfx_text8(int x, int y, const char *s, uint16_t fg, uint16_t bg);
void prg32_palette_set(uint8_t index, uint16_t rgb565);
void prg32_sprite_draw_indexed(int x, int y, const prg32_indexed_sprite_t *sprite, uint32_t frame);
void prg32_audio_note_on_pan(uint8_t ch, uint8_t inst, uint8_t note, uint8_t vol, int8_t pan);
void prg32_audio_note_off(uint8_t ch);
void prg32_audio_stop_track(void);
void prg32_audio_stop_all(void);
void prg32_audio_play_track(uint16_t t);
void prg32_audio_set_master_volume(uint8_t v);
void prg32_audio_set_channel_volume(uint8_t ch, uint8_t v);
void prg32_audio_set_channel_pan(uint8_t ch, int8_t pan);
bool prg32_multiplayer_available(void);
void prg32_multiplayer_init(void);
int prg32_multiplayer_join(const char *sig, uint32_t flags);
void prg32_multiplayer_tick(void);
int prg32_multiplayer_set_local_state(int16_t x, int16_t y, uint16_t sprite, uint16_t flags);
int prg32_multiplayer_set_input(uint32_t input);
int prg32_multiplayer_get_peer_count(void);
int prg32_multiplayer_get_peer(int index, prg32_player_state_t *out);
#endif
