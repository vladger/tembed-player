#pragma once
#include <Arduino.h>

enum EvType : uint8_t {
  EV_NONE,
  EV_ROTATE,        // wheel turned            (delta = detents, +cw)
  EV_PRESS_ROTATE,  // wheel turned while held (delta)
  EV_CLICK,
  EV_DOUBLE,        // only produced when double-click is enabled
  EV_LONG,
  EV_USER_CLICK,    // top USER button, short press  -> back
  EV_USER_LONG      // top USER button, long press   -> deep sleep
};

struct InputEvent {
  EvType type;
  int8_t delta;
};

void     inputBegin();
bool     inputPoll(InputEvent& ev);          // call repeatedly until false
void     inputSetDoubleClick(bool enable);
void     inputSetInvert(bool invert);        // reverse wheel direction
void     inputEnd();                         // detach interrupts (before deep sleep)   // when off, clicks fire instantly
uint32_t inputLastActivity();
