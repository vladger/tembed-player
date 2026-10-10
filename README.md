## NOTE: This project was created with Claude AI.

# T-Embed CC1101 Music Player

Firmware for the **LilyGO T-Embed CC1101**: SD / FTP / HTTP music player and web-radio client with a small wheel-driven UI, wheel LED animations (always **1 % brightness**) and deep sleep.

## Build & flash

Download the .bin file from Releases, or build it yourself:

```bash
pip install platformio
pio run -t upload
```

`platformio.ini` pins the platform (`espressif32@6.9.0`, Arduino core 2.0.x) and `ESP32-audioI2S@2.3.0` (the callback API differs in 3.x, so don't bump it blindly).

## SD card setup

On first boot with an SD card the player creates `/player/config.json`. Edit it on a PC:

```json
{
  "wifi": [ { "ssid": "MyWiFi", "pass": "secret" }, { "ssid": "Phone", "pass": "hotspot" } ],
  "ftp":  { "host": "192.168.1.10", "port": 21, "user": "music", "pass": "pw", "path": "/Music" },
  "http": [
    { "name": "SomaFM Groove Salad", "url": "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "name": "My NAS albums",       "url": "http://192.168.1.10/music/" },
    { "name": "Single file",         "url": "https://example.com/song.mp3" }
  ]
}
```

* **Wi-Fi:** every network is tried in order; Wi-Fi only starts when you open FTP/HTTP (saves power).
* **HTTP entries** can be live streams (Icecast/Shoutcast, `.m3u`/`.pls`, AAC/MP3/...), single files, or a URL ending in `/` which is browsed as an Apache/nginx directory index.
* **SD music:** put files anywhere; `/music` is opened first if it exists. Formats: mp3, aac, m4a, wav, flac, ogg, opus.
* **FTP:** passive mode, Unix and Windows (IIS) listings. Files are downloaded to `/player/cache/` on the SD card, then played (progress is shown). The cache is wiped on boot.

## Controls

| Where | Rotate | Click | Double-click | Long press | Press + rotate |
|---|---|---|---|---|---|
| Menus / lists | move | select | - | back / up | - |
| Player | volume | play / pause | next track | back to the list | previous / next |
| Keyboard | next key | type key | - | backspace | move a whole row |

**Top USER button** (`BOARD_USER_KEY`, GPIO 6): short press = **back** (up a folder, leave settings, close the keyboard, cancel Wi-Fi...), hold for 1 s = **deep sleep**. In deep sleep a single press of the USER button wakes the device. If it doesn't react, check `PIN_USER_KEY` / `USER_KEY_ACTIVE_LOW` in `config.h`.

Playlists are the folder (or station list) you started from; they repeat. Shuffle is in Settings.

## On-screen keyboard (typing URLs in Streams)

Web / HTTP -> **+ Add URL...** opens the keyboard (pre-filled with `http://`). Turn the wheel to move through the keys, click to type; hold the wheel button and turn to jump a row; long-press the wheel for backspace. Control keys: `Aa1` switches between lower case / upper case / digits+symbols, `Del`, `http/s` toggles the scheme, `OK` confirms, `Esc` cancels (also the USER button).

On OK the URL is **saved to `/player/config.json`** (so it shows up in the station list next time) and starts playing. A URL ending in `/` is opened as a directory index instead.

## Settings (stored in flash)

* **LEDs** on/off and **LED animation** (Rainbow, Chase, Breathe, VU meter)
* **Screen brightness**, **Screen orientation** (Normal / Flipped 180 deg - the UI is laid out for landscape), **Wheel direction**, **Shuffle**
* **Power options** (submenu):
  * Battery level (click to refresh)
  * **Sleep now** - real deep sleep
  * **Auto sleep (idle)** - sleep after 2/5/10/30 min without input while nothing is playing
  * **Sleep timer** - stop and sleep after 15/30/60/90 min, even while playing
  * **Dim screen after** (10/20/30/60 s) and **Dimmed brightness**
  * **Screen off after** (30 s/1/2/5 min) - backlight off and no redraws; the first press only wakes the screen
  * **Sleep: cut power (risky)** - **off by default.** In deep sleep it switches off the peripheral rail (GPIO 15) and holds the bus pins low. On at least one T-Embed CC1101 this left the device unresponsive to the USER button (it can't be recovered without opening the case), so leave it off unless you have tested it on your unit
  * **Wi-Fi power save** - modem sleep (saves power but can make streams stutter)

## LEDs

The 8 wheel LEDs are initialised once with `LED_BRIGHTNESS_PERCENT = 1` (`src/config.h` -> raw brightness 3/255) and nothing else ever changes the brightness. At 1 % each colour channel only has about 4 distinct levels, so fades and rainbow hues look coarse; that is inherent to the setting. Turning the wheel in the player briefly shows the volume on the ring.

## Deep sleep

Triggered by holding the USER button, menu -> *Sleep*, Settings -> Power, the sleep timer, or automatically after the idle timeout while nothing is playing. It stops audio, turns off Wi-Fi/SD/display/LEDs, then calls `esp_deep_sleep_start()`. With the default settings (peripheral rail left on, tested working) a press of the **USER button** (ext0 wake, RTC pull-up) reboots the player; the home menu then offers **Continue** with the last track/station.

## Architecture notes

* Audio runs in its own task (core 1, higher priority); the UI talks to it via a command queue.
* TFT, SD and CC1101 share one SPI bus. A recursive mutex guards it; the UI pushes its PSRAM frame buffer in 34-row strips so SD playback never starves.
* The VU meter animation uses the library's per-sample hook (`audio_process_i2s`); if your library build doesn't call it, the meter shows a simulated level instead.

## First-boot checks

* **Upside-down screen:** Settings -> Screen orientation.
* **Wheel too sensitive / sluggish:** `ENC_STEPS_PER_DETENT` 2 <-> 4; wrong direction: Settings -> Wheel direction.
* **Pins:** the pin map in `config.h` follows the LilyGO reference design; the TFT reset line is not used (software reset) because it shares a pin with I2S LRC on this board.
* **Battery %:** read from the BQ27220 at I2C 0x55; hidden if it doesn't answer.

## Limitations

* HTTPS certificates are not verified (`setInsecure()`).
* FTP tracks are downloaded completely before playback starts (no FTP streaming).
* No gapless playback.
* Text is UTF-8 with Latin, Latin Extended-A and **Cyrillic** glyphs (anti-aliased Liberation Sans, SIL OFL; licence in `tools/`). Other scripts (Greek, CJK, ...) show as `?`; add ranges in `tools/gen_font.py` and re-run it to regenerate `src/ufont_data.cpp`.
* Tags / stream titles in Windows-1251 (or double-encoded UTF-8) are repaired automatically. File *names* on the SD card come straight from the FAT driver; if non-ASCII names still show as `?` there, that is the Arduino 2.x FAT code page, not the font.
