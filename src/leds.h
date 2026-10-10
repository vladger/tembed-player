#pragma once
#include <Arduino.h>

#define LED_ANIM_COUNT 4

void        ledsBegin();
void        ledsSetEnabled(bool on);
void        ledsSetAnim(uint8_t anim);
const char* ledsAnimName(uint8_t anim);
void        ledsFlashVolume(uint8_t vol);                       // brief volume bar overlay
void        ledsTick(bool playing, uint8_t vu, uint8_t vol);    // call often; self rate-limited
void        ledsOff();                                          // before deep sleep
