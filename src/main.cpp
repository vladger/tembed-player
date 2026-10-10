#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "config.h"
#include "input.h"
#include "leds.h"
#include "player.h"
#include "sources.h"
#include "ui.h"

Settings g_set;
SemaphoreHandle_t g_spiMutex = nullptr;

// ---------------------------------------------------------------------
// persistent settings (NVS)
// ---------------------------------------------------------------------
void settingsLoad() {
  Preferences p;
  p.begin("player", false);
  g_set.volume      = min<uint8_t>(p.getUChar("vol", 8), VOLUME_MAX);
  g_set.ledOn       = p.getBool("ledOn", true);
  g_set.ledAnim     = p.getUChar("ledAnim", 0) % LED_ANIM_COUNT;
  g_set.brightness  = constrain(p.getUChar("bright", 80), 10, 100);
  g_set.sleepMin    = p.getUChar("sleepMin", 5);
  g_set.shuffle     = p.getBool("shuffle", false);
  g_set.orientation = p.getUChar("orient", 0) & 1;
  g_set.wheelInvert = p.getBool("wheelInv", ENC_INVERT);
  g_set.dimAfter    = p.getUShort("dimAfter", 20);
  g_set.dimPct      = constrain(p.getUChar("dimPct", 12), 1, 100);
  g_set.offAfter    = p.getUShort("offAfter", 0);
  g_set.cutPower    = p.getBool("cutPwr2", false);   // new key: old default (true) is ignored
  g_set.wifiSave    = p.getBool("wifiSave", false);
  p.end();
}

void settingsSave() {
  Preferences p;
  p.begin("player", false);
  p.putUChar("vol", g_set.volume);
  p.putBool("ledOn", g_set.ledOn);
  p.putUChar("ledAnim", g_set.ledAnim);
  p.putUChar("bright", g_set.brightness);
  p.putUChar("sleepMin", g_set.sleepMin);
  p.putBool("shuffle", g_set.shuffle);
  p.putUChar("orient", g_set.orientation);
  p.putBool("wheelInv", g_set.wheelInvert);
  p.putUShort("dimAfter", g_set.dimAfter);
  p.putUChar("dimPct", g_set.dimPct);
  p.putUShort("offAfter", g_set.offAfter);
  p.putBool("cutPwr2", g_set.cutPower);
  p.putBool("wifiSave", g_set.wifiSave);
  p.end();
}

// ---------------------------------------------------------------------
// deep sleep
// ---------------------------------------------------------------------
// Pins that are driven low and latched (gpio hold) while in deep sleep when
// "cut power" is enabled. The wake pin (USER key) is deliberately NOT in here.
static const uint8_t HOLD_PINS[] = {
  PIN_TFT_BL, PIN_TFT_CS, PIN_TFT_DC, PIN_SD_CS, PIN_CC1101_CS,
  PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO,
  PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT, PIN_LED_DATA, PIN_PWR_EN
};

static void releaseHolds() {
  gpio_deep_sleep_hold_dis();
  for (uint8_t p : HOLD_PINS) gpio_hold_dis((gpio_num_t)p);
  rtc_gpio_deinit((gpio_num_t)PIN_USER_KEY);   // give the wake pin back to the digital GPIO matrix
  rtc_gpio_deinit((gpio_num_t)PIN_ENC_KEY);
}

static void waitKeysReleased(uint32_t timeoutMs) {
  uint32_t t = millis();
  while (millis() - t < timeoutMs && (digitalRead(PIN_ENC_KEY) == LOW || USER_KEY_DOWN())) delay(10);
}

static void enterDeepSleep() {
  // Release the buttons first, otherwise the wake pin is still active and we'd wake instantly.
  waitKeysReleased(3000);

  uiShowSleepScreen();
  delay(600);

  inputEnd();
  playerStop();
  delay(150);
  ledsOff();
  settingsSave();

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  sdEnd();
  uiDisplayOff();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  if (g_set.cutPower) {
    ledcDetachPin(PIN_TFT_BL);
    for (uint8_t p : HOLD_PINS) {
      pinMode(p, OUTPUT);
      digitalWrite(p, LOW);          // peripheral rail off + all lines low (no back-powering)
      gpio_hold_en((gpio_num_t)p);
    }
    gpio_deep_sleep_hold_en();
  }

  // Wake on a single press of the USER key (ext0, RTC pull resistor keeps the line defined).
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_KEY, USER_KEY_ACTIVE_LOW ? 0 : 1);
  if (USER_KEY_ACTIVE_LOW) {
    rtc_gpio_pullup_en((gpio_num_t)PIN_USER_KEY);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_USER_KEY);
  } else {
    rtc_gpio_pulldown_en((gpio_num_t)PIN_USER_KEY);
    rtc_gpio_pullup_dis((gpio_num_t)PIN_USER_KEY);
  }

  esp_deep_sleep_start();          // never returns; the next press of USER reboots into setup()
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  releaseHolds();                                  // no-op on cold boot, needed after deep sleep

  pinMode(PIN_PWR_EN, OUTPUT);
  digitalWrite(PIN_PWR_EN, HIGH);                  // power the peripheral rail
  pinMode(PIN_ENC_KEY, INPUT_PULLUP);
  pinMode(PIN_USER_KEY, USER_KEY_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  pinMode(PIN_CC1101_CS, OUTPUT); digitalWrite(PIN_CC1101_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);     digitalWrite(PIN_SD_CS, HIGH);
  delay(80);

  g_spiMutex = xSemaphoreCreateRecursiveMutex();
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);

  settingsLoad();
  uiBegin();

  waitKeysReleased(3000);                          // swallow the press that woke us up
  inputBegin();
  inputSetInvert(g_set.wheelInvert);

  ledsBegin();
  ledsSetEnabled(g_set.ledOn);
  ledsSetAnim(g_set.ledAnim);

  sdBegin();
  loadConfig();
  sdCleanCache();

  playerBegin();
  playerSetVolume(g_set.volume);

  uiReady();
}

void loop() {
  InputEvent ev;
  while (inputPoll(ev)) uiHandle(ev);
  if (uiTick()) enterDeepSleep();
  delay(4);
}
