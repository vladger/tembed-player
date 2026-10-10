#pragma once
#include "input.h"

void uiBegin();                       // init display + splash
void uiReady();                       // after SD/config are loaded: show the home menu
void uiHandle(const InputEvent& ev);
bool uiTick();                        // returns true when deep sleep was requested
void uiShowSleepScreen();
void uiDisplayOff();
