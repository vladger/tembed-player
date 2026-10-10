#include "player.h"
#include "config.h"
#include "sources.h"
#include <Audio.h>
#include <SD.h>
#include <esp_system.h>

// All ESP32-audioI2S calls happen inside ONE task. The UI talks to it through a
// command queue and a few volatile status variables.

struct Cmd {
  uint8_t type;
  uint8_t src;
  bool    stream;
  char    path[320];
  char    name[96];
};
enum { C_PLAY = 1, C_PAUSE, C_STOP };

static Audio*            audio = nullptr;
static QueueHandle_t     g_q;
static SemaphoreHandle_t g_metaMux;

static volatile PlayState g_state = PS_STOPPED;
static volatile SrcType   g_src = SRC_NONE;
static volatile uint32_t  g_pos = 0, g_dur = 0, g_br = 0, g_sr = 0;
static volatile uint8_t   g_vu = 0, g_vol = 8;
static volatile int       g_pct = -1;
static volatile bool      g_cancel = false, g_ended = false, g_volDirty = true;
static volatile int32_t   g_peakAcc = 0;      // peak |sample| since the last status update
static volatile bool      g_seen = false;     // sample hook has delivered data
static volatile bool      g_realVu = false;   // sample hook works in this library build
static char               g_err[48] = "";
static PlayerMeta         g_meta;

// ---------- helpers ----------
static void setField(char* dst, size_t n, const char* s) {
  while (*s == ' ') s++;
  xSemaphoreTake(g_metaMux, portMAX_DELAY);
  strlcpy(dst, s, n);
  xSemaphoreGive(g_metaMux);
}

static String stem(const char* name) {
  String s(name);
  int i = s.lastIndexOf('.');
  if (i > 0 && s.length() - i <= 6) s.remove(i);
  return s;
}

static String extOf(const char* p) {
  String s(p);
  int i = s.lastIndexOf('.');
  if (i < 0 || (int)s.length() - i > 6) return "mp3";
  String e = s.substring(i + 1);
  e.toLowerCase();
  return e;
}

static void stopAll() {
  bool sd = (g_src == SRC_SD || g_src == SRC_FTP);
  if (sd) xSemaphoreTakeRecursive(g_spiMutex, portMAX_DELAY);
  audio->stopSong();
  if (sd) xSemaphoreGiveRecursive(g_spiMutex);
  g_state = PS_STOPPED;
  g_pos = 0; g_dur = 0; g_vu = 0; g_br = 0;
}

static void fail(const char* msg) {
  strlcpy(g_err, msg, sizeof(g_err));
  g_state = PS_ERROR;
}

// ---------- commands ----------
static void doPlay(const Cmd& c) {
  g_cancel = false;
  stopAll();
  g_ended = false;
  g_err[0] = 0;
  g_pct = -1;
  g_src = (SrcType)c.src;

  PlayerMeta fresh;
  memset(&fresh, 0, sizeof(fresh));
  if (c.stream) {
    strlcpy(fresh.title, c.name, sizeof(fresh.title));
    strlcpy(fresh.station, c.name, sizeof(fresh.station));
  } else {
    strlcpy(fresh.title, stem(c.name).c_str(), sizeof(fresh.title));
  }
  xSemaphoreTake(g_metaMux, portMAX_DELAY);
  g_meta = fresh;
  xSemaphoreGive(g_metaMux);

  g_state = PS_LOADING;
  bool ok = false;

  switch (c.src) {
    case SRC_SD: {
      SpiGuard g;
      ok = audio->connecttoFS(SD, c.path);
      if (!ok) fail("Cannot open file");
      break;
    }
    case SRC_HTTP:
      ok = audio->connecttohost(c.path);
      if (!ok) fail("Cannot connect");
      break;
    case SRC_FTP: {
      if (!sdOk()) { fail("FTP needs SD card"); return; }
      String local = String(CACHE_DIR) + "/track." + extOf(c.path);
      { SpiGuard g; SD.mkdir("/player"); SD.mkdir(CACHE_DIR); }
      g_pct = 0;
      bool d = ftpDownload(String(c.path), local, g_pct, g_cancel);
      if (g_cancel) { g_pct = -1; g_state = PS_STOPPED; return; }
      if (!d) { g_pct = -1; fail("FTP download failed"); return; }
      g_pct = -1;
      SpiGuard g;
      ok = audio->connecttoFS(SD, local.c_str());
      if (!ok) fail("Cannot open file");
      break;
    }
    default:
      fail("Bad source");
  }
  if (ok) g_state = PS_PLAYING;
}

static void handle(const Cmd& c) {
  switch (c.type) {
    case C_PLAY: doPlay(c); break;
    case C_PAUSE:
      if (g_state == PS_PLAYING || g_state == PS_PAUSED) {
        audio->pauseResume();
        g_state = (g_state == PS_PLAYING) ? PS_PAUSED : PS_PLAYING;
      }
      break;
    case C_STOP:
      g_cancel = false;
      stopAll();
      break;
  }
}

static void updateStatus() {
  if (g_state == PS_PLAYING || g_state == PS_PAUSED) {
    g_pos = audio->getAudioCurrentTime();
    g_dur = audio->getAudioFileDuration();
    g_br  = audio->getBitRate();
    g_sr  = audio->getSampleRate();
    if (g_state == PS_PAUSED) {
      g_vu = 0;
    } else if (g_seen) {
      // real level from the sample hook (audio_process_i2s below)
      int32_t p = g_peakAcc >> 7;                  // 0..255
      g_peakAcc = 0;
      g_seen = false;
      g_realVu = true;
      g_vu = p > 255 ? 255 : p;
    } else if (!g_realVu) {
      // this library build does not call the sample hook: show a lively simulated level
      g_vu = 60 + (esp_random() % 140);
    }
  } else {
    g_vu = 0;
  }
}

static void audioTask(void*) {
  audio = new Audio();
  audio->setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio->setVolume(g_vol);
  uint32_t lastStat = 0;
  for (;;) {
    Cmd c;
    while (xQueueReceive(g_q, &c, 0) == pdTRUE) handle(c);
    if (g_volDirty) { g_volDirty = false; audio->setVolume(g_vol); }

    bool sd = (g_src == SRC_SD || g_src == SRC_FTP);
    if (sd) xSemaphoreTakeRecursive(g_spiMutex, portMAX_DELAY);
    audio->loop();
    if (sd) xSemaphoreGiveRecursive(g_spiMutex);

    uint32_t now = millis();
    if (now - lastStat >= 50) { lastStat = now; updateStatus(); }
    vTaskDelay(1);
  }
}

// ---------- public API ----------
void playerBegin() {
  g_q = xQueueCreate(4, sizeof(Cmd));
  g_metaMux = xSemaphoreCreateMutex();
  memset(&g_meta, 0, sizeof(g_meta));
  xTaskCreatePinnedToCore(audioTask, "audio", 16384, nullptr, 4, nullptr, 1);
}

void playerPlay(SrcType src, const char* path, bool stream, const char* name) {
  Cmd c;
  memset(&c, 0, sizeof(c));
  c.type = C_PLAY; c.src = src; c.stream = stream;
  strlcpy(c.path, path, sizeof(c.path));
  strlcpy(c.name, name, sizeof(c.name));
  g_cancel = true;            // abort a running FTP download
  xQueueReset(g_q);
  xQueueSend(g_q, &c, 0);
}

void playerTogglePause() {
  Cmd c; memset(&c, 0, sizeof(c)); c.type = C_PAUSE;
  xQueueSend(g_q, &c, 0);
}

void playerStop() {
  Cmd c; memset(&c, 0, sizeof(c)); c.type = C_STOP;
  g_cancel = true;
  xQueueReset(g_q);
  xQueueSend(g_q, &c, 0);
}

void playerSetVolume(uint8_t v) { g_vol = v > VOLUME_MAX ? VOLUME_MAX : v; g_volDirty = true; }

PlayState   playerState()      { return g_state; }
uint32_t    playerPos()        { return g_pos; }
uint32_t    playerDur()        { return g_dur; }
uint32_t    playerBitrate()    { return g_br; }
uint32_t    playerSampleRate() { return g_sr; }
uint8_t     playerVU()         { return g_vu; }
int         playerLoadPct()    { return g_pct; }
const char* playerError()      { return g_err; }

bool playerPollEnded() {
  if (g_ended) { g_ended = false; return true; }
  return false;
}

void playerGetMeta(PlayerMeta& m) {
  xSemaphoreTake(g_metaMux, portMAX_DELAY);
  m = g_meta;
  xSemaphoreGive(g_metaMux);
}

// ---------- ESP32-audioI2S callbacks (called from the audio task) ----------
void audio_info(const char* info) {
#ifdef PLAYER_DEBUG
  Serial.printf("[audio] %s\n", info);
#endif
}

void audio_id3data(const char* info) {
  if (!strncasecmp(info, "Title:", 6))       setField(g_meta.title, sizeof(g_meta.title), info + 6);
  else if (!strncasecmp(info, "Artist:", 7)) setField(g_meta.artist, sizeof(g_meta.artist), info + 7);
}

void audio_showstation(const char* info) {
  if (info && *info) setField(g_meta.station, sizeof(g_meta.station), info);
}

void audio_showstreamtitle(const char* info) {
  if (info && *info) setField(g_meta.title, sizeof(g_meta.title), info);
}

// Optional hook: if this library build calls it, we get real per-sample levels for the
// VU meter animation. If it is never called, the simulated fallback above is used.
void audio_process_i2s(uint32_t* sample, bool* continueI2S) {
  int32_t l = (int16_t)(*sample & 0xFFFF);
  int32_t r = (int16_t)(*sample >> 16);
  if (l < 0) l = -l;
  if (r < 0) r = -r;
  int32_t m = l > r ? l : r;
  if (m > g_peakAcc) g_peakAcc = m;
  g_seen = true;
  *continueI2S = true;      // pass the sample through unchanged
}

void audio_eof_mp3(const char* info) {
  if (g_state == PS_PLAYING) { g_state = PS_STOPPED; g_ended = true; }
}

void audio_eof_stream(const char* info) {
  if (g_state == PS_PLAYING) { g_state = PS_STOPPED; g_ended = true; }
}
