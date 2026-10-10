#include "leds.h"
#include "config.h"
#include <Adafruit_NeoPixel.h>
#include <math.h>

static Adafruit_NeoPixel strip(LED_COUNT, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
static const char* NAMES[LED_ANIM_COUNT] = {"Rainbow", "Chase", "Breathe", "VU meter"};

static bool     s_on = true;
static bool     s_cleared = false;
static uint8_t  s_anim = 0;
static uint32_t s_volUntil = 0;
static uint8_t  s_vol = 0;

void ledsBegin() {
  strip.begin();
  // Set ONCE: ~1 % global brightness. Never call setBrightness() again
  // (the library rescales the pixel buffer on every change).
  strip.setBrightness(LED_BRIGHTNESS_RAW);
  strip.clear();
  strip.show();
}

void ledsSetEnabled(bool on) { s_on = on; if (on) s_cleared = false; }
void ledsSetAnim(uint8_t a)  { s_anim = a % LED_ANIM_COUNT; }
const char* ledsAnimName(uint8_t a) { return NAMES[a % LED_ANIM_COUNT]; }

void ledsFlashVolume(uint8_t vol) {
  s_vol = vol;
  s_volUntil = millis() + 1200;
}

void ledsOff() {
  strip.clear();
  strip.show();
  s_cleared = true;
}

void ledsTick(bool playing, uint8_t vu, uint8_t vol) {
  static uint32_t last = 0;
  static float peak = 16, lvl = 0;
  uint32_t now = millis();
  if (now - last < 30) return;
  last = now;

  if (!s_on) {
    if (!s_cleared) { strip.clear(); strip.show(); s_cleared = true; }
    return;
  }
  s_cleared = false;
  strip.clear();

  if (now < s_volUntil) {
    // Volume overlay
    int n = (s_vol * LED_COUNT + VOLUME_MAX / 2) / VOLUME_MAX;
    if (s_vol > 0 && n < 1) n = 1;
    if (n > LED_COUNT) n = LED_COUNT;
    for (int i = 0; i < n; i++) strip.setPixelColor(i, strip.Color(120, 200, 255));
  } else {
    switch (s_anim) {
      case 0: {  // Rainbow
        uint32_t sp = playing ? 18 : 5;
        uint16_t base = (uint16_t)((now * sp) & 0xFFFF);
        for (int i = 0; i < LED_COUNT; i++)
          strip.setPixelColor(i, strip.ColorHSV((uint16_t)(base + i * (65536 / LED_COUNT))));
        break;
      }
      case 1: {  // Chase with tail
        uint32_t period = playing ? 80 : 220;
        int head = (now / period) % LED_COUNT;
        static const uint8_t tail[4] = {255, 110, 45, 15};
        for (int k = 0; k < 4; k++) {
          int idx = (head - k + LED_COUNT) % LED_COUNT;
          uint8_t v = tail[k];
          strip.setPixelColor(idx, strip.Color(0, v * 3 / 4, v));
        }
        break;
      }
      case 2: {  // Breathe
        uint32_t period = playing ? 4000 : 6500;
        float ph = (now % period) / (float)period;
        float v = 0.5f - 0.5f * cosf(ph * 2.0f * (float)PI);
        v = v * v;
        uint8_t b = (uint8_t)(v * 255);
        uint32_t c = strip.Color(b * 3 / 4, b / 4, b);
        for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, c);
        break;
      }
      case 3: {  // VU meter (auto-gain so it works whatever the raw range is)
        float x = playing ? (float)vu : 0.0f;
        peak = fmaxf(x, peak * 0.995f);
        if (peak < 16) peak = 16;
        float t = x / peak;
        lvl = fmaxf(t, lvl - 0.08f);
        int lit = (int)(lvl * LED_COUNT + 0.5f);
        for (int i = 0; i < lit && i < LED_COUNT; i++) {
          uint32_t c;
          if (i < LED_COUNT / 2) c = strip.Color(0, 255, 0);
          else if (i < LED_COUNT * 3 / 4 + 1) c = strip.Color(255, 200, 0);
          else c = strip.Color(255, 0, 0);
          strip.setPixelColor(i, c);
        }
        break;
      }
    }
  }
  strip.show();
}
