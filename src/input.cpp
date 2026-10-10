#include "input.h"
#include "config.h"
#include <driver/gpio.h>

static volatile int32_t s_acc  = 0;
static volatile uint8_t s_prev = 0;
static portMUX_TYPE     s_mux  = portMUX_INITIALIZER_UNLOCKED;
static bool             s_dbl  = false;
static uint32_t         s_activity = 0;
static bool             s_invert = ENC_INVERT;

// Gray-code transition table, index = (old << 2) | new
static const DRAM_ATTR int8_t QEM[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static void IRAM_ATTR encIsr() {
  uint8_t st = (gpio_get_level((gpio_num_t)PIN_ENC_A) << 1) | gpio_get_level((gpio_num_t)PIN_ENC_B);
  s_prev = ((s_prev << 2) | st) & 0x0F;
  s_acc  = s_acc + QEM[s_prev];
}

void inputBegin() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_KEY, INPUT_PULLUP);
  pinMode(PIN_USER_KEY, USER_KEY_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  s_prev = (gpio_get_level((gpio_num_t)PIN_ENC_A) << 1) | gpio_get_level((gpio_num_t)PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encIsr, CHANGE);
  s_activity = millis();
}

void inputSetDoubleClick(bool enable) { s_dbl = enable; }
void inputSetInvert(bool invert) { s_invert = invert; }
void inputEnd() {
  detachInterrupt(digitalPinToInterrupt(PIN_ENC_A));
  detachInterrupt(digitalPinToInterrupt(PIN_ENC_B));
}
uint32_t inputLastActivity() { return s_activity; }

static bool emit(InputEvent& ev, EvType t, int8_t d = 0) {
  ev.type = t;
  ev.delta = d;
  return true;
}

bool inputPoll(InputEvent& ev) {
  static bool lastRaw = false, stable = false, pressed = false, rotated = false, longSent = false;
  static uint32_t rawT = 0, pressT = 0, relT = 0;
  static uint8_t clicks = 0;
  static bool uLast = false, uStable = false, uLongSent = false;
  static uint32_t uRawT = 0, uPressT = 0;
  uint32_t now = millis();

  // --- USER (top) button ---
  bool uRaw = USER_KEY_DOWN();
  if (uRaw != uLast) { uLast = uRaw; uRawT = now; }
  if (uRaw == uLast && uStable != uRaw && now - uRawT >= 20) {
    uStable = uRaw;
    s_activity = now;
    if (uStable) { uLongSent = false; uPressT = now; }
    else if (!uLongSent) return emit(ev, EV_USER_CLICK);
  }
  if (uStable && !uLongSent && now - uPressT >= USER_LONG_MS) {
    uLongSent = true;
    return emit(ev, EV_USER_LONG);
  }

  // --- wheel button debounce ---
  bool raw = digitalRead(PIN_ENC_KEY) == LOW;
  if (raw != lastRaw) { lastRaw = raw; rawT = now; }
  if (raw == lastRaw && stable != raw && now - rawT >= 12) {
    stable = raw;
    s_activity = now;
    if (stable) {
      pressed = true; rotated = false; longSent = false; pressT = now;
    } else {
      pressed = false;
      if (!rotated && !longSent) {
        if (s_dbl) {
          clicks++;
          relT = now;
          if (clicks >= 2) { clicks = 0; return emit(ev, EV_DOUBLE); }
        } else {
          return emit(ev, EV_CLICK);
        }
      }
    }
  }

  // --- long press ---
  if (pressed && !rotated && !longSent && now - pressT >= LONG_PRESS_MS) {
    longSent = true;
    clicks = 0;
    return emit(ev, EV_LONG);
  }

  // --- pending single click (waiting to see if a 2nd click follows) ---
  if (!pressed && clicks == 1 && (!s_dbl || now - relT >= DOUBLE_CLICK_MS)) {
    clicks = 0;
    return emit(ev, EV_CLICK);
  }

  // --- rotation ---
  int32_t d;
  portENTER_CRITICAL(&s_mux);
  d = s_acc / ENC_STEPS_PER_DETENT;
  s_acc = s_acc - d * ENC_STEPS_PER_DETENT;
  portEXIT_CRITICAL(&s_mux);
  if (d != 0) {
    if (s_invert) d = -d;
    d = constrain(d, -8, 8);
    s_activity = now;
    if (pressed) { rotated = true; clicks = 0; return emit(ev, EV_PRESS_ROTATE, (int8_t)d); }
    return emit(ev, EV_ROTATE, (int8_t)d);
  }
  return false;
}
