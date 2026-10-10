#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ===================== LilyGO T-Embed CC1101 pin map =====================
// Verify against your board revision if something does not work.
#define PIN_PWR_EN        15   // peripheral power rail (WS2812, CC1101, SD, ...) - must be HIGH
#define PIN_SPI_SCK       11   // shared SPI bus: TFT + SD + CC1101
#define PIN_SPI_MISO      10
#define PIN_SPI_MOSI       9
#define PIN_TFT_CS        41
#define PIN_TFT_DC        16
#define PIN_TFT_RST       -1   // shares a pin with I2S LRC on this board -> use software reset
#define PIN_TFT_BL        21
#define PIN_SD_CS         13
#define PIN_CC1101_CS     12   // kept HIGH (unused)
#define PIN_I2S_BCLK      46   // MAX98357A
#define PIN_I2S_LRC       40
#define PIN_I2S_DOUT       7
#define PIN_ENC_A          4
#define PIN_ENC_B          5
#define PIN_ENC_KEY        0
#define PIN_USER_KEY       6   // BOARD_USER_KEY (top button): back / long-press = deep sleep / wake
#define PIN_LED_DATA      14   // 8x WS2812 around the wheel
#define LED_COUNT          8
#define PIN_I2C_SDA        8   // BQ27220 fuel gauge (battery %)
#define PIN_I2C_SCL       18

// ===================== Display =====================
#define TFT_W              320
#define TFT_H              170
#define TFT_ROTATION       3   // flip to 1 if the picture is upside down

// ===================== Input =====================
#define ENC_STEPS_PER_DETENT 2 // quadrature steps per click of the wheel (try 4 if too sensitive)
#define ENC_INVERT           0 // set 1 to reverse rotation direction
#define LONG_PRESS_MS      700
#define DOUBLE_CLICK_MS    300
#define USER_KEY_ACTIVE_LOW 1  // USER key pulls the pin to GND when pressed
#define USER_LONG_MS      1000 // hold the USER key this long for deep sleep
#define USER_KEY_DOWN()   (digitalRead(PIN_USER_KEY) == (USER_KEY_ACTIVE_LOW ? LOW : HIGH))

// ===================== LEDs =====================
// Wheel LEDs are ALWAYS driven at 1 % brightness. All pixel writes go through
// Adafruit_NeoPixel whose global brightness is set once, here, at begin().
#define LED_BRIGHTNESS_PERCENT 1
#define LED_BRIGHTNESS_RAW     ((255 * LED_BRIGHTNESS_PERCENT + 50) / 100)   // = 3

// ===================== Misc =====================
#define VOLUME_MAX         21
#define MAX_LIST_ENTRIES   300
#define CFG_PATH           "/player/config.json"
#define CACHE_DIR          "/player/cache"

struct Settings {
  uint8_t volume     = 8;
  bool    ledOn      = true;
  uint8_t ledAnim    = 0;
  uint8_t brightness = 80;   // screen %
  uint8_t sleepMin   = 5;    // idle minutes before deep sleep (0 = never)
  bool    shuffle    = false;
  uint8_t orientation = 0;       // 0 = normal, 1 = flipped 180 deg
  bool    wheelInvert = (ENC_INVERT);
  // power options
  uint16_t dimAfter  = 20;       // seconds of inactivity before dimming (0 = never)
  uint8_t  dimPct    = 12;       // dimmed brightness %
  uint16_t offAfter  = 0;        // seconds of inactivity before the screen turns off (0 = never)
  bool     cutPower  = false;    // EXPERIMENTAL: cut the peripheral rail in deep sleep. On at least one
                                 // T-Embed CC1101 this stops the USER key from waking the device -> keep OFF
  bool     wifiSave  = false;    // Wi-Fi modem sleep (saves power, can stutter streams)
};
extern Settings g_set;
void settingsLoad();
void settingsSave();

// The TFT, SD card (and CC1101) share one SPI bus; every user must hold this lock.
extern SemaphoreHandle_t g_spiMutex;
struct SpiGuard {
  SpiGuard()  { xSemaphoreTakeRecursive(g_spiMutex, portMAX_DELAY); }
  ~SpiGuard() { xSemaphoreGiveRecursive(g_spiMutex); }
};
