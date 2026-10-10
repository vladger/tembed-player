#include "ui.h"
#include "config.h"
#include "input.h"
#include "leds.h"
#include "player.h"
#include "sources.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "textutil.h"
#include "ufont.h"
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>

// =====================================================================
// look & feel
// =====================================================================
#define COL(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
static const uint16_t C_BG    = COL(8, 10, 18);
static const uint16_t C_PANEL = COL(26, 30, 48);
static const uint16_t C_ACC   = COL(0, 200, 255);
static const uint16_t C_TXT   = COL(235, 238, 245);
static const uint16_t C_DIM   = COL(130, 138, 160);
static const uint16_t C_SEL   = COL(0, 90, 130);
static const uint16_t C_WARN  = COL(255, 170, 0);
static const uint16_t C_OK    = COL(70, 220, 120);
static const uint16_t C_ERR   = COL(255, 80, 80);

static const UFont* F_S = &UF_SMALL;   // Unicode fonts: Latin, Latin-Ext, Cyrillic
static const UFont* F_B = &UF_BOLD;

static Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
static GFXcanvas16*    cv = nullptr;

// =====================================================================
// state
// =====================================================================
enum Scr : uint8_t { S_HOME, S_BROWSER, S_PLAYER, S_SETTINGS, S_CONNECT, S_LOADING, S_KEYBOARD };
enum { H_NOW = 1, H_CONT, H_SD, H_FTP, H_HTTP, H_SET, H_SLEEP };
enum {  // settings (page 0) and power (page 1) rows
  SET_LED = 1, SET_ANIM, SET_BRIGHT, SET_ORIENT, SET_WHEEL, SET_SHUFFLE, SET_POWER,
  P_INFO, P_SLEEPNOW, P_AUTOSLEEP, P_TIMER, P_DIMAFTER, P_DIMLEVEL, P_OFFAFTER, P_CUTPOWER, P_WIFISAVE
};
enum { PA_BROWSE = 0, PA_RESUME = 1 };

struct Item { String label, right; int id; uint8_t icon; };   // icon: 0 none, 1 dir, 2 file, 3 up

static Scr      scr = S_HOME;
static bool     dirty = true;
static bool     sleepReq = false;
static String   toastMsg;
static uint32_t toastUntil = 0;
static String   busyMsg;

static std::vector<Item> items;
static int sel = 0, topRow = 0;
static int setPage = 0;                 // 0 = settings, 1 = power

// backlight
static uint8_t curBl = 255;
static inline bool screenIsOff() { return curBl == 0; }

// sleep timer
static uint32_t sleepTimerAt = 0;       // millis() deadline, 0 = off
static uint16_t sleepTimerMin = 0;

// browser
static SrcType             brSrc = SRC_NONE;
static std::vector<String> brStack;
static std::vector<int>    brSelStack;
static std::vector<Entry>  brList;
static bool                brValid = false;
static int                 brSel = 0;

// playlist
static SrcType            plSrc = SRC_NONE;
static std::vector<Entry> pl;
static int                plIdx = -1;
static int                retryCount = 0, badRun = 0;
static uint32_t           trackStartMs = 0, lastSkip = 0;

// network hand-off
static SrcType pendingSrc = SRC_NONE;
static uint8_t pendingAct = PA_BROWSE;

// battery
static int      batPct = -1;
static uint32_t lastBat = 0;

// on-screen keyboard
static String  kbBuf;
static uint8_t kbPage = 0;
static int     kbIdx = 0;
static int     kbReturnSel = 0;
static const char* KB_PAGES[3][3] = {
  {"abcdefghij", "klmnopqrst", "uvwxyz.-_/"},
  {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ.-_/"},
  {"0123456789", ":/.-_?=&%#", "@~+,;!*'()"}
};
static const char* KB_PAGE_NAMES[3] = {"abc", "ABC", "123"};
static const char* KB_CTRL[5] = {"Aa1", "Del", "http/s", "OK", "Esc"};
#define KB_KEYS 35   // 30 grid keys + 5 control keys

// persisted "last played"
struct LastTrack { uint8_t src = SRC_NONE; String path, name; bool stream = false; };
static LastTrack last;

static uint32_t settingsDirtyAt = 0;

template <typename T, size_t N> static T nextOpt(const T (&a)[N], T cur) {
  for (size_t i = 0; i < N; i++) if (a[i] == cur) return a[(i + 1) % N];
  return a[0];
}
static const uint8_t  BRIGHT_OPTS[] = {20, 40, 60, 80, 100};
static const uint8_t  SLEEP_OPTS[]  = {0, 2, 5, 10, 30};
static const uint16_t DIM_OPTS[]    = {0, 10, 20, 30, 60};
static const uint8_t  DIMLVL_OPTS[] = {3, 8, 12, 25, 40};
static const uint16_t OFF_OPTS[]    = {0, 30, 60, 120, 300};
static const uint16_t TIMER_OPTS[]  = {0, 15, 30, 60, 90};

// forward declarations
static void goHome();
static void playIndex(int i);
static bool loadBrowser();
static void buildBrowserItems();
static void goBack();

// =====================================================================
// small utilities
// =====================================================================
static void toast(const String& m, uint32_t ms = 2200) { toastMsg = m; toastUntil = millis() + ms; dirty = true; }

static void setBacklight(uint8_t pct) {
  if (pct == curBl) return;
  curBl = pct;
  ledcWrite(0, (uint32_t)pct * 255 / 100);
}

static void markSettingsDirty() { settingsDirtyAt = millis(); }

static uint8_t tftRotation() { return g_set.orientation ? ((TFT_ROTATION + 2) & 3) : TFT_ROTATION; }

static void applyOrientation() {
  SpiGuard g;
  tft.setRotation(tftRotation());
  tft.fillScreen(0);
}

// Valid UTF-8 for display (control chars removed, Windows-1251 / double-encoded tags repaired).
static String cleanText(const char* s) { return String(textClean(s).c_str()); }

static String fmtTime(uint32_t s) {
  char b[16];
  if (s >= 3600) snprintf(b, sizeof(b), "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  else snprintf(b, sizeof(b), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  return String(b);
}

static String fmtSecs(uint32_t s) {
  if (s == 0) return "Off";
  if (s < 60) return String(s) + " s";
  return String(s / 60) + " min";
}

// =====================================================================
// drawing primitives (all drawing goes to the PSRAM canvas)
// =====================================================================
static int textW(const String& s, const UFont* f) { return ufontWidth(f, s.c_str()); }

static void drawGlyph(const UFont* f, const UGlyph* g, int x, int baseY, uint16_t fg, uint16_t* buf) {
  int fr = fg >> 11, fgc = (fg >> 5) & 63, fb = fg & 31;
  for (int r = 0; r < g->h; r++) {
    int py = baseY + g->yo + r;
    if (py < 0 || py >= TFT_H) continue;
    for (int c = 0; c < g->w; c++) {
      int px = x + g->xo + c;
      if (px < 0 || px >= TFT_W) continue;
      int i = r * g->w + c;
      uint8_t b = f->bmp[g->off + (i >> 1)];
      uint8_t a = (i & 1) ? (b & 0x0F) : (b >> 4);
      if (!a) continue;
      uint16_t* p = buf + py * TFT_W + px;
      if (a == 15) { *p = fg; continue; }
      uint16_t bg = *p;
      int rr = (fr * a + (bg >> 11) * (15 - a) + 7) / 15;
      int gg = (fgc * a + ((bg >> 5) & 63) * (15 - a) + 7) / 15;
      int bb = (fb * a + (bg & 31) * (15 - a) + 7) / 15;
      *p = (rr << 11) | (gg << 5) | bb;
    }
  }
}

// align: 0 left, 1 centre, 2 right. y is the text baseline. UTF-8 in, anti-aliased out.
static void txt(int x, int y, const String& s, const UFont* f, uint16_t col, int align = 0) {
  const char* p = s.c_str();
  int px = x;
  if (align == 1) px = x - ufontWidth(f, p) / 2;
  else if (align == 2) px = x - ufontWidth(f, p);
  uint16_t* buf = cv->getBuffer();
  while (*p) {
    uint32_t cp = utf8Next(p);
    const UGlyph* g = cp <= 0xFFFF ? ufontGlyph(f, cp) : nullptr;
    if (!g) g = ufontGlyph(f, '?');
    if (!g) continue;
    if (g->w) drawGlyph(f, g, px, y, col, buf);
    px += g->adv;
  }
}

static String fit(const String& s, const UFont* f, int maxW) {
  if (maxW <= 0) return "";
  if (ufontWidth(f, s.c_str()) <= maxW) return s;
  size_t n = ufontFitBytes(f, s.c_str(), maxW - ufontWidth(f, ".."));
  return s.substring(0, n) + "..";
}

static void drawSpinner(int cx, int cy, int r, int dot = 3) {
  int ph = (millis() / 90) % 8;
  for (int i = 0; i < 8; i++) {
    float a = i * (float)PI / 4.0f - (float)PI / 2.0f;
    int age = (ph - i + 8) % 8;
    uint16_t col = age == 0 ? C_ACC : age == 1 ? COL(0, 150, 190) : age == 2 ? COL(0, 100, 130) : C_PANEL;
    cv->fillCircle(cx + (int)(cosf(a) * r), cy + (int)(sinf(a) * r), dot, col);
  }
}

static void iconDir(int x, int y, uint16_t c)  { cv->fillRect(x, y + 2, 6, 3, c); cv->fillRoundRect(x, y + 4, 15, 10, 2, c); }
static void iconFile(int x, int y, uint16_t c) { cv->fillCircle(x + 4, y + 11, 3, c); cv->fillRect(x + 6, y + 1, 2, 10, c); cv->fillRect(x + 6, y + 1, 7, 3, c); }
static void iconUp(int x, int y, uint16_t c)   { cv->fillTriangle(x + 7, y + 2, x, y + 10, x + 14, y + 10, c); cv->fillRect(x + 5, y + 10, 4, 4, c); }

static void drawHeader(const String& title) {
  cv->fillRect(0, 0, TFT_W, 22, C_PANEL);
  txt(8, 16, fit(title, F_S, 200), F_S, C_ACC);
  int x = TFT_W - 8;
  if (batPct >= 0) {
    String b = String(batPct) + "%";
    txt(x, 16, b, F_S, batPct < 15 ? C_WARN : C_DIM, 2);
    x -= textW(b, F_S) + 10;
  }
  if (WiFi.status() == WL_CONNECTED) {
    int r = WiFi.RSSI();
    int lv = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
    for (int i = 0; i < 4; i++)
      cv->fillRect(x - 16 + i * 4, 18 - 3 * (i + 1), 3, 3 * (i + 1), i < lv ? C_OK : C_DIM);
  }
}

static void drawMarquee(const String& s, int baseY, const UFont* f, uint16_t col, int x0, int w) {
  static String mqText;
  static uint32_t mqStart = 0;
  int tw = textW(s, f);
  if (tw <= w) { txt(x0, baseY, s, f, col); return; }
  if (s != mqText) { mqText = s; mqStart = millis(); }
  const int gap = 48;
  int cyc = tw + gap;
  uint32_t period = (uint32_t)cyc * 25 + 1500;           // 40 px/s, 1.5 s pause at the start
  uint32_t p = (millis() - mqStart) % period;
  int off = p < 1500 ? 0 : (int)((p - 1500) / 25);
  txt(x0 - off, baseY, s, f, col);
  txt(x0 - off + cyc, baseY, s, f, col);
  cv->fillRect(0, baseY - 22, x0, 32, C_BG);                       // mask the edges
  cv->fillRect(x0 + w, baseY - 22, TFT_W - (x0 + w), 32, C_BG);
}

// =====================================================================
// screens
// =====================================================================
static void drawList(const String& title) {
  drawHeader(title);
  const int rowH = 28, y0 = 26, rows = 5;
  int n = items.size();
  if (sel >= n) sel = n ? n - 1 : 0;
  if (sel < topRow) topRow = sel;
  if (sel >= topRow + rows) topRow = sel - rows + 1;
  for (int r = 0; r < rows && topRow + r < n; r++) {
    int idx = topRow + r;
    const Item& it = items[idx];
    int y = y0 + r * rowH;
    bool s = idx == sel;
    if (s) cv->fillRoundRect(4, y, 304, rowH - 2, 6, C_SEL);
    int x = 14;
    uint16_t ic = s ? C_TXT : C_ACC;
    if (it.icon == 1) { iconDir(x, y + 6, ic); x += 24; }
    else if (it.icon == 2) { iconFile(x, y + 6, ic); x += 24; }
    else if (it.icon == 3) { iconUp(x, y + 6, ic); x += 24; }
    String right = fit(it.right, F_S, 120);
    int rw = right.length() ? textW(right, F_S) : 0;
    int avail = 298 - x - rw - (rw ? 10 : 0);
    txt(x, y + 19, fit(it.label, F_S, avail), F_S, C_TXT);
    if (rw) txt(298, y + 19, right, F_S, s ? C_TXT : C_DIM, 2);
  }
  if (n > rows) {                                  // scrollbar
    int th = rows * rowH;
    cv->fillRect(312, y0, 4, th, C_PANEL);
    int bh = max(10, th * rows / n);
    int by = y0 + (th - bh) * topRow / (n - rows);
    cv->fillRect(312, by, 4, bh, C_ACC);
  }
}

static String browserTitle() {
  String src = brSrc == SRC_SD ? "SD" : brSrc == SRC_FTP ? "FTP" : "Web";
  String d = brStack.empty() ? "" : brStack.back();
  if (d.isEmpty()) return src + ": stations";
  return src + ": " + cleanText(baseName(d).c_str());
}

static void drawPlayer() {
  PlayerMeta m;
  playerGetMeta(m);
  PlayState st = playerState();
  String hdr = plSrc == SRC_SD ? "SD card" : plSrc == SRC_FTP ? "FTP" : "Web";
  if (pl.size() > 1) hdr += "   " + String(plIdx + 1) + "/" + String(pl.size());
  drawHeader(hdr);

  String title = cleanText(m.title);
  if (title.isEmpty() && plIdx >= 0) title = cleanText(pl[plIdx].name.c_str());
  drawMarquee(title, 56, F_B, C_TXT, 10, 300);

  String sub = cleanText(m.artist);
  if (sub.isEmpty()) sub = cleanText(m.station);
  if (sub.length() && sub != title) txt(10, 80, fit(sub, F_S, 300), F_S, C_ACC);

  uint32_t br = playerBitrate(), sr = playerSampleRate();
  if ((st == PS_PLAYING || st == PS_PAUSED) && br > 0) {
    String info = String(br / 1000) + " kbps";
    if (sr) info += "  /  " + String(sr / 1000.0f, 1) + " kHz";
    txt(10, 101, info, F_S, C_DIM);
  }

  uint32_t pos = playerPos(), dur = playerDur();
  if (pos > dur && dur) pos = dur;
  cv->fillRoundRect(10, 112, 300, 6, 3, C_PANEL);
  if (dur > 0) cv->fillRoundRect(10, 112, max(6u, (unsigned)(300ULL * pos / dur)), 6, 3, C_ACC);
  txt(10, 136, fmtTime(pos), F_S, C_DIM);
  if (dur > 0) txt(310, 136, fmtTime(dur), F_S, C_DIM, 2);
  else if (st == PS_PLAYING || st == PS_PAUSED) txt(310, 136, "LIVE", F_S, C_OK, 2);

  int sx = 12, sy = 146;
  String label;
  switch (st) {
    case PS_PLAYING: cv->fillTriangle(sx, sy, sx, sy + 14, sx + 12, sy + 7, C_OK); label = "Playing"; break;
    case PS_PAUSED:  cv->fillRect(sx, sy, 4, 14, C_WARN); cv->fillRect(sx + 8, sy, 4, 14, C_WARN); label = "Paused"; break;
    case PS_LOADING: {
      drawSpinner(sx + 6, sy + 7, 6, 2);
      int p = playerLoadPct();
      label = p >= 0 ? "Downloading " + String(p) + "%" : "Connecting...";
      break;
    }
    case PS_ERROR:   cv->fillRect(sx + 4, sy, 4, 9, C_ERR); cv->fillRect(sx + 4, sy + 11, 4, 3, C_ERR); label = playerError(); break;
    default:         cv->fillRect(sx, sy + 1, 12, 12, C_DIM); label = "Stopped"; break;
  }
  txt(34, 158, fit(label, F_S, 170), F_S, st == PS_ERROR ? C_ERR : C_TXT);

  txt(222, 158, "Vol", F_S, C_DIM, 2);
  cv->drawRoundRect(228, 149, 82, 10, 3, C_DIM);
  cv->fillRoundRect(230, 151, (78 * g_set.volume) / VOLUME_MAX, 6, 2, C_ACC);
}

static void drawConnect() {
  drawHeader("Wi-Fi");
  drawSpinner(160, 70, 22, 4);
  txt(160, 118, "Connecting...", F_B, C_TXT, 1);
  txt(160, 142, fit(netSsid(), F_S, 280), F_S, C_DIM, 1);
  txt(160, 164, "click or BACK to cancel", F_S, C_DIM, 1);
}

static void drawBusy() {
  drawSpinner(160, 70, 22, 4);
  txt(160, 128, busyMsg, F_B, C_TXT, 1);
}

static void drawKeyboard() {
  drawHeader(String("Enter URL   [") + KB_PAGE_NAMES[kbPage] + "]");
  // text field (shows the tail of the string)
  cv->fillRoundRect(6, 25, 308, 24, 5, C_PANEL);
  String shown = cleanText(kbBuf.c_str());
  if ((millis() / 500) % 2 == 0) shown += "_";
  while (shown.length() > 1 && textW(shown, F_S) > 296) shown.remove(0, 1);
  txt(12, 43, shown, F_S, C_TXT);

  const int cw = 30, ch = 26, x0 = 10, y0 = 54;
  for (int i = 0; i < 30; i++) {
    int r = i / 10, c = i % 10;
    int x = x0 + c * cw, y = y0 + r * (ch + 2);
    bool s = i == kbIdx;
    cv->fillRoundRect(x + 1, y, cw - 2, ch, 4, s ? C_SEL : C_PANEL);
    if (s) cv->drawRoundRect(x + 1, y, cw - 2, ch, 4, C_ACC);
    char ch1[2] = {KB_PAGES[kbPage][r][c], 0};
    txt(x + cw / 2, y + 19, ch1, F_S, C_TXT, 1);
  }
  for (int k = 0; k < 5; k++) {
    int x = x0 + k * 60, y = y0 + 3 * (ch + 2);
    bool s = (30 + k) == kbIdx;
    cv->fillRoundRect(x + 1, y, 58, ch, 4, s ? C_SEL : C_PANEL);
    if (s) cv->drawRoundRect(x + 1, y, 58, ch, 4, C_ACC);
    uint16_t col = k == 3 ? C_OK : k == 4 ? C_WARN : C_TXT;
    txt(x + 30, y + 19, KB_CTRL[k], F_S, col, 1);
  }
}

static void drawToast() {
  if (millis() >= toastUntil) return;
  String t = fit(toastMsg, F_S, 280);
  int w = textW(t, F_S) + 24;
  int x = (TFT_W - w) / 2;
  cv->fillRoundRect(x, 130, w, 28, 8, C_PANEL);
  cv->drawRoundRect(x, 130, w, 28, 8, C_ACC);
  txt(TFT_W / 2, 149, t, F_S, C_TXT, 1);
}

static void flush() {
  const int W = TFT_W, H = TFT_H, STRIP = 34;      // push in strips so the audio task gets SPI time
  uint16_t* buf = cv->getBuffer();
  for (int y = 0; y < H; y += STRIP) {
    int h = min(STRIP, H - y);
    {
      SpiGuard g;
      tft.startWrite();
      tft.setAddrWindow(0, y, W, h);
      tft.writePixels(buf + y * W, W * h);
      tft.endWrite();
    }
    taskYIELD();
  }
}

static void render() {
  cv->fillScreen(C_BG);
  switch (scr) {
    case S_HOME:     drawList("T-Embed Player"); break;
    case S_BROWSER:  drawList(browserTitle()); break;
    case S_SETTINGS: drawList(setPage == 1 ? "Settings > Power" : "Settings"); break;
    case S_PLAYER:   drawPlayer(); break;
    case S_CONNECT:  drawConnect(); break;
    case S_LOADING:  drawBusy(); break;
    case S_KEYBOARD: drawKeyboard(); break;
  }
  drawToast();
  flush();
}

static void uiBusy(const char* msg) { busyMsg = msg; scr = S_LOADING; render(); }

// =====================================================================
// persistence of the last played track
// =====================================================================
static void loadLast() {
  Preferences p;
  p.begin("last", false);
  last.src = p.getUChar("src", SRC_NONE);
  last.path = p.getString("path", "");
  last.name = p.getString("name", "");
  last.stream = p.getBool("stream", false);
  p.end();
  if (last.path.isEmpty()) last.src = SRC_NONE;
}

static void saveLast(const Entry& e) {
  last.src = plSrc; last.path = e.path; last.name = e.name; last.stream = e.stream;
  Preferences p;
  p.begin("last", false);
  p.putUChar("src", plSrc);
  p.putString("path", e.path);
  p.putString("name", e.name);
  p.putBool("stream", e.stream);
  p.end();
}

// =====================================================================
// menus
// =====================================================================
static void addItem(const String& label, const String& right, int id, uint8_t icon = 0) {
  Item it; it.label = label; it.right = right; it.id = id; it.icon = icon;
  items.push_back(it);
}

static bool wifiConfigured() {
  if (g_cfg.wifi.empty()) { toast("Add Wi-Fi to /player/config.json"); return false; }
  return true;
}

static void buildHome() {
  items.clear();
  if (plIdx >= 0) addItem("Now playing", "", H_NOW);
  else if (last.src != SRC_NONE) addItem("Continue", cleanText(last.name.c_str()), H_CONT);
  addItem("SD card", sdOk() ? "" : "no card", H_SD);
  addItem("FTP server", g_cfg.ftpHost.length() ? g_cfg.ftpHost : String("not set"), H_FTP);
  addItem("Web radio / HTTP", String((unsigned)g_cfg.http.size()), H_HTTP);
  addItem("Settings", "", H_SET);
  addItem("Sleep", "", H_SLEEP);
}

static void goHome() {
  scr = S_HOME;
  buildHome();
  sel = 0; topRow = 0;
  dirty = true;
}

static void buildSettings() {
  items.clear();
  if (setPage == 0) {
    addItem("LEDs", g_set.ledOn ? "On" : "Off", SET_LED);
    addItem("LED animation", ledsAnimName(g_set.ledAnim), SET_ANIM);
    addItem("Screen brightness", String(g_set.brightness) + "%", SET_BRIGHT);
    addItem("Screen orientation", g_set.orientation ? "Flipped" : "Normal", SET_ORIENT);
    addItem("Wheel direction", g_set.wheelInvert ? "Reversed" : "Normal", SET_WHEEL);
    addItem("Shuffle", g_set.shuffle ? "On" : "Off", SET_SHUFFLE);
    addItem("Power options", ">", SET_POWER);
  } else {
    addItem("Battery", batPct >= 0 ? String(batPct) + "%" : String("n/a"), P_INFO);
    addItem("Sleep now", "", P_SLEEPNOW);
    addItem("Auto sleep (idle)", g_set.sleepMin ? String(g_set.sleepMin) + " min" : String("Off"), P_AUTOSLEEP);
    String t = "Off";
    if (sleepTimerAt) {
      int32_t left = (int32_t)(sleepTimerAt - millis());
      t = String((left > 0 ? left / 60000 : 0) + 1) + " min left";
    }
    addItem("Sleep timer", t, P_TIMER);
    addItem("Dim screen after", fmtSecs(g_set.dimAfter), P_DIMAFTER);
    addItem("Dimmed brightness", String(g_set.dimPct) + "%", P_DIMLEVEL);
    addItem("Screen off after", fmtSecs(g_set.offAfter), P_OFFAFTER);
    addItem("Sleep: cut power (risky)", g_set.cutPower ? "On" : "Off", P_CUTPOWER);
    addItem("Wi-Fi power save", g_set.wifiSave ? "On" : "Off", P_WIFISAVE);
  }
}

static void goSettings(int page = 0) {
  setPage = page;
  scr = S_SETTINGS;
  buildSettings();
  sel = 0; topRow = 0;
  dirty = true;
}

static void buildBrowserItems() {
  items.clear();
  addItem("..", "", -1, 3);
  if (brSrc == SRC_HTTP && brStack.size() == 1) addItem("+ Add URL...", "", -2, 0);
  for (size_t i = 0; i < brList.size(); i++)
    addItem(cleanText(brList[i].name.c_str()), "", (int)i, brList[i].isDir ? 1 : 2);
}

static bool loadBrowser() {
  String dir = brStack.back();
  uiBusy("Loading...");
  brList.clear();
  bool ok = false;
  if (brSrc == SRC_SD) ok = sdList(dir, brList);
  else if (brSrc == SRC_FTP) ok = ftpList(dir, brList);
  else if (brSrc == SRC_HTTP) {
    if (dir.isEmpty()) {
      for (auto& s : g_cfg.http) brList.push_back(Entry(s.name, s.url, s.url.endsWith("/"), !s.url.endsWith("/")));
      ok = true;
    } else {
      ok = httpList(dir, brList);
    }
  }
  if (!ok) { toast("Could not load list"); return false; }
  buildBrowserItems();
  sel = 0; topRow = 0;
  scr = S_BROWSER;
  dirty = true;
  return true;
}

static void openBrowser(SrcType s) {
  brSrc = s;
  brStack.clear();
  brSelStack.clear();
  if (s == SRC_SD) {
    brStack.push_back("/");
    if (sdDirExists("/music")) brStack.push_back("/music");
  } else if (s == SRC_FTP) {
    brStack.push_back(g_cfg.ftpPath.length() ? g_cfg.ftpPath : String("/"));
  } else {
    brStack.push_back("");
  }
  if (!loadBrowser()) { goHome(); toast("Could not open source"); return; }
  brValid = true;
}

static void browserUp() {
  if (brStack.size() > 1) {
    brStack.pop_back();
    int s = 0;
    if (!brSelStack.empty()) { s = brSelStack.back(); brSelStack.pop_back(); }
    if (loadBrowser()) sel = constrain(s, 0, (int)items.size() - 1);
    else goHome();
  } else {
    goHome();
  }
}

// ---------------------------------------------------------------------
// on-screen keyboard
// ---------------------------------------------------------------------
static void kbOpen() {
  kbReturnSel = sel;
  kbBuf = "http://";
  kbPage = 0;
  kbIdx = 0;
  scr = S_KEYBOARD;
  dirty = true;
}

static void kbCancel() {
  scr = S_BROWSER;
  buildBrowserItems();
  sel = constrain(kbReturnSel, 0, (int)items.size() - 1);
  dirty = true;
}

static void kbBackspace() { if (kbBuf.length()) kbBuf.remove(kbBuf.length() - 1); }

static void kbToggleScheme() {
  if (kbBuf.startsWith("https://")) kbBuf = "http://" + kbBuf.substring(8);
  else if (kbBuf.startsWith("http://")) kbBuf = "https://" + kbBuf.substring(7);
  else kbBuf = "http://" + kbBuf;
}

static void startStationPlaylist(const String& playUrl) {
  pl.clear();
  plSrc = SRC_HTTP;
  int idx = 0;
  for (auto& s : g_cfg.http) {
    if (s.url.endsWith("/")) continue;
    if (s.url == playUrl) idx = pl.size();
    pl.push_back(Entry(s.name, s.url, false, true));
  }
  plIdx = -1;
  playIndex(idx);
  scr = S_PLAYER;
  dirty = true;
}

static void kbDone() {
  String url = kbBuf;
  url.trim();
  if (!(url.startsWith("http://") || url.startsWith("https://")) || url.length() < 11) {
    toast("Enter a full http(s):// URL");
    return;
  }
  String name = url.substring(url.indexOf("://") + 3);
  if (name.endsWith("/")) name.remove(name.length() - 1);
  bool saved = cfgAddStation(name, url);
  brValid = true;
  if (url.endsWith("/")) {                    // directory index: show it in the browser
    brSrc = SRC_HTTP;
    brStack.clear(); brSelStack.clear();
    brStack.push_back("");
    brSelStack.push_back(0);
    brStack.push_back(url);
    if (!loadBrowser()) goHome();
    toast(saved ? "Saved to stations" : "Added (SD not saved)");
  } else {
    startStationPlaylist(url);
    toast(saved ? "Saved to stations" : "Playing (not saved: no SD)");
  }
}

static void kbKeys(const InputEvent& ev) {
  switch (ev.type) {
    case EV_ROTATE: kbIdx = ((kbIdx + ev.delta) % KB_KEYS + KB_KEYS) % KB_KEYS; break;
    case EV_PRESS_ROTATE: {                    // hold + turn = move a whole row
      int row = kbIdx < 30 ? kbIdx / 10 : 3;
      int col = kbIdx < 30 ? kbIdx % 10 : (kbIdx - 30) * 2;
      row = ((row + ev.delta) % 4 + 4) % 4;
      kbIdx = row < 3 ? row * 10 + col : 30 + min(col / 2, 4);
      break;
    }
    case EV_CLICK:
      if (kbIdx < 30) {
        if (kbBuf.length() < 200) kbBuf += KB_PAGES[kbPage][kbIdx / 10][kbIdx % 10];
      } else {
        switch (kbIdx - 30) {
          case 0: kbPage = (kbPage + 1) % 3; break;
          case 1: kbBackspace(); break;
          case 2: kbToggleScheme(); break;
          case 3: kbDone(); break;
          case 4: kbCancel(); break;
        }
      }
      break;
    case EV_LONG: kbBackspace(); break;
    default: break;
  }
}

static void browserSelect() {
  if (items.empty()) return;
  Item it = items[sel];
  if (it.id == -1) { browserUp(); return; }
  if (it.id == -2) { kbOpen(); return; }
  Entry e = brList[it.id];
  if (e.isDir) {
    brSelStack.push_back(sel);
    brStack.push_back(e.path);
    if (!loadBrowser()) {
      brStack.pop_back();
      brSelStack.pop_back();
      if (!loadBrowser()) goHome();
    }
    return;
  }
  brSel = sel;
  pl.clear();
  plSrc = brSrc;
  int idx = 0;
  for (size_t i = 0; i < brList.size(); i++) {
    if (brList[i].isDir) continue;
    if ((int)i == it.id) idx = pl.size();
    pl.push_back(brList[i]);
  }
  plIdx = -1;
  playIndex(idx);
  scr = S_PLAYER;
  dirty = true;
}

// =====================================================================
// playback control
// =====================================================================
static void playIndex(int i) {
  if (i < 0 || i >= (int)pl.size()) return;
  plIdx = i;
  retryCount = 0;
  trackStartMs = millis();
  const Entry& e = pl[i];
  playerPlay(plSrc, e.path.c_str(), e.stream, e.name.c_str());
  saveLast(e);
  dirty = true;
}

static void stepTrack(int dir) {
  int n = pl.size();
  if (n == 0) return;
  int i;
  if (g_set.shuffle && n > 1 && dir > 0) { do { i = random(n); } while (i == plIdx); }
  else i = ((plIdx < 0 ? 0 : plIdx) + dir + n) % n;
  playIndex(i);
}

static void togglePlay() {
  PlayState st = playerState();
  if (st == PS_PLAYING || st == PS_PAUSED) playerTogglePause();
  else if (st == PS_LOADING) playerStop();
  else if (plIdx >= 0) playIndex(plIdx);
}

static void onTrackEnded() {
  if (plIdx < 0 || plIdx >= (int)pl.size()) return;
  const Entry& e = pl[plIdx];
  if (e.stream) {                                   // live stream dropped -> reconnect a few times
    if (++retryCount <= 3) playerPlay(plSrc, e.path.c_str(), true, e.name.c_str());
    else toast("Stream lost");
    return;
  }
  badRun = (millis() - trackStartMs < 1500) ? badRun + 1 : 0;
  if (badRun >= 3) { badRun = 0; playerStop(); toast("Playback failed"); return; }
  stepTrack(1);                                     // repeat-all behaviour
}

static void resumeLast() {
  if (last.src == SRC_NONE) return;
  pl.clear();
  plSrc = (SrcType)last.src;
  int idx = 0;
  if (plSrc == SRC_SD && sdOk()) {
    std::vector<Entry> l;
    if (sdList(parentDir(last.path), l))
      for (auto& e : l) { if (e.isDir) continue; if (e.path == last.path) idx = pl.size(); pl.push_back(e); }
  } else if (plSrc == SRC_HTTP && last.stream) {
    for (auto& s : g_cfg.http) {
      if (s.url.endsWith("/")) continue;
      if (s.url == last.path) idx = pl.size();
      pl.push_back(Entry(s.name, s.url, false, true));
    }
  }
  if (pl.empty()) { pl.push_back(Entry(last.name, last.path, false, last.stream)); idx = 0; }
  plIdx = -1;
  playIndex(idx);
  scr = S_PLAYER;
  dirty = true;
}

static void startNet(SrcType s, uint8_t act) {
  if (!wifiConfigured()) return;
  pendingSrc = s; pendingAct = act;
  netStart();
  scr = S_CONNECT;
  dirty = true;
}

// =====================================================================
// input handling
// =====================================================================
static void moveSel(int d) {
  int n = items.size();
  if (!n) return;
  sel = ((sel + d) % n + n) % n;
}

static void homeSelect() {
  if (items.empty()) return;
  switch (items[sel].id) {
    case H_NOW:  scr = S_PLAYER; break;
    case H_CONT:
      if (last.src == SRC_SD) { if (sdOk()) resumeLast(); else toast("No SD card"); }
      else startNet((SrcType)last.src, PA_RESUME);
      break;
    case H_SD:   if (sdOk()) openBrowser(SRC_SD); else toast("No SD card"); break;
    case H_FTP:
      if (g_cfg.ftpHost.isEmpty()) toast("Set FTP in /player/config.json");
      else startNet(SRC_FTP, PA_BROWSE);
      break;
    case H_HTTP: startNet(SRC_HTTP, PA_BROWSE); break;
    case H_SET:  goSettings(0); break;
    case H_SLEEP: sleepReq = true; break;
  }
}

static void settingsSelect() {
  if (items.empty()) return;
  int id = items[sel].id;
  switch (id) {
    case SET_LED:     g_set.ledOn = !g_set.ledOn; ledsSetEnabled(g_set.ledOn); break;
    case SET_ANIM:    g_set.ledAnim = (g_set.ledAnim + 1) % LED_ANIM_COUNT; ledsSetAnim(g_set.ledAnim); break;
    case SET_BRIGHT:  g_set.brightness = nextOpt(BRIGHT_OPTS, g_set.brightness); curBl = 255; setBacklight(g_set.brightness); break;
    case SET_ORIENT:  g_set.orientation ^= 1; applyOrientation(); break;
    case SET_WHEEL:   g_set.wheelInvert = !g_set.wheelInvert; inputSetInvert(g_set.wheelInvert); break;
    case SET_SHUFFLE: g_set.shuffle = !g_set.shuffle; break;
    case SET_POWER:   goSettings(1); return;
    case P_INFO:      batPct = -1; lastBat = 0; break;    // force a re-read
    case P_SLEEPNOW:  sleepReq = true; return;
    case P_AUTOSLEEP: g_set.sleepMin = nextOpt(SLEEP_OPTS, g_set.sleepMin); break;
    case P_TIMER:
      sleepTimerMin = nextOpt(TIMER_OPTS, sleepTimerMin);
      sleepTimerAt = sleepTimerMin ? millis() + (uint32_t)sleepTimerMin * 60000UL : 0;
      toast(sleepTimerMin ? "Sleep in " + String(sleepTimerMin) + " min" : String("Sleep timer off"));
      break;
    case P_DIMAFTER:  g_set.dimAfter = nextOpt(DIM_OPTS, g_set.dimAfter); break;
    case P_DIMLEVEL:  g_set.dimPct = nextOpt(DIMLVL_OPTS, g_set.dimPct); break;
    case P_OFFAFTER:  g_set.offAfter = nextOpt(OFF_OPTS, g_set.offAfter); break;
    case P_CUTPOWER:
      g_set.cutPower = !g_set.cutPower;
      if (g_set.cutPower) toast("Risky: may not wake up. Keep Off", 4500);
      break;
    case P_WIFISAVE:
      g_set.wifiSave = !g_set.wifiSave;
      if (WiFi.status() == WL_CONNECTED) WiFi.setSleep(g_set.wifiSave);
      break;
  }
  int keep = sel;
  buildSettings();
  sel = keep;
  markSettingsDirty();
}

static void playerKeys(const InputEvent& ev) {
  switch (ev.type) {
    case EV_ROTATE: {
      int v = constrain((int)g_set.volume + ev.delta, 0, VOLUME_MAX);
      if (v != g_set.volume) {
        g_set.volume = v;
        playerSetVolume(v);
        ledsFlashVolume(v);
        markSettingsDirty();
      }
      break;
    }
    case EV_CLICK:  togglePlay(); break;
    case EV_DOUBLE: stepTrack(1); break;
    case EV_PRESS_ROTATE:
      if (millis() - lastSkip > 350) { lastSkip = millis(); stepTrack(ev.delta > 0 ? 1 : -1); }
      break;
    case EV_LONG:   goBack(); break;
    default: break;
  }
}

// "Back" (top USER key short-press, and long-press of the wheel in most screens)
static void goBack() {
  switch (scr) {
    case S_HOME:     if (plIdx >= 0) scr = S_PLAYER; break;
    case S_BROWSER:  browserUp(); break;
    case S_PLAYER:
      if (brValid) { buildBrowserItems(); sel = constrain(brSel, 0, (int)items.size() - 1); scr = S_BROWSER; }
      else goHome();
      break;
    case S_SETTINGS:
      if (setPage == 1) { goSettings(0); sel = 6; }      // back to the "Power options" row
      else goHome();
      break;
    case S_CONNECT:  netStop(); goHome(); break;
    case S_KEYBOARD: kbCancel(); break;
    default: break;
  }
  dirty = true;
}

void uiHandle(const InputEvent& ev) {
  dirty = true;
  if (ev.type == EV_USER_LONG) { sleepReq = true; return; }          // top button held -> deep sleep

  // while the screen is off the first press only wakes it up
  if (screenIsOff() && (ev.type == EV_CLICK || ev.type == EV_DOUBLE || ev.type == EV_LONG || ev.type == EV_USER_CLICK)) return;

  if (ev.type == EV_USER_CLICK) { goBack(); return; }
  if (scr == S_KEYBOARD) { kbKeys(ev); return; }
  if (ev.type == EV_ROTATE && (scr == S_HOME || scr == S_BROWSER || scr == S_SETTINGS)) { moveSel(ev.delta); return; }

  switch (scr) {
    case S_HOME:
      if (ev.type == EV_CLICK) homeSelect();
      else if (ev.type == EV_LONG && plIdx >= 0) scr = S_PLAYER;
      break;
    case S_BROWSER:
      if (ev.type == EV_CLICK) browserSelect();
      else if (ev.type == EV_LONG) goBack();
      break;
    case S_SETTINGS:
      if (ev.type == EV_CLICK) settingsSelect();
      else if (ev.type == EV_LONG) goBack();
      break;
    case S_PLAYER: playerKeys(ev); break;
    case S_CONNECT:
      if (ev.type == EV_CLICK || ev.type == EV_LONG) goBack();
      break;
    default: break;
  }
}

// =====================================================================
// battery (BQ27220 state-of-charge register)
// =====================================================================
static int readBattery() {
  Wire.beginTransmission(0x55);
  Wire.write(0x2C);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(0x55, 2) != 2) return -1;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  uint16_t v = lo | (hi << 8);
  return v <= 100 ? v : -1;
}

// =====================================================================
// public
// =====================================================================
void uiBegin() {
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_TFT_BL, 0);
  ledcWrite(0, 0);
  curBl = 0;

  tft.init(170, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(tftRotation());
  tft.fillScreen(0);

  cv = new GFXcanvas16(TFT_W, TFT_H);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  loadLast();

  cv->fillScreen(C_BG);
  txt(160, 82, "T-Embed Player", F_B, C_TXT, 1);
  txt(160, 110, "starting...", F_S, C_DIM, 1);
  flush();
  setBacklight(g_set.brightness);
}

void uiReady() {
  batPct = readBattery();
  lastBat = millis();
  goHome();
  render();
}

bool uiTick() {
  uint32_t now = millis();
  inputSetDoubleClick(scr == S_PLAYER);

  if (scr == S_CONNECT) {
    int st = netPoll();
    if (st == NET_OK) {
      if (pendingAct == PA_RESUME) resumeLast();
      else openBrowser(pendingSrc);
    } else if (st == NET_FAIL) {
      toast("Wi-Fi connection failed");
      goHome();
    }
  }

  if (playerPollEnded()) { onTrackEnded(); dirty = true; }

  static PlayState lastSt = PS_STOPPED;
  PlayState st = playerState();
  if (st != lastSt) { lastSt = st; dirty = true; }

  if (now - lastBat > 15000) { lastBat = now; batPct = readBattery(); dirty = true; }

  // ---- power: backlight dim / off on inactivity ----
  uint32_t idleMs = now - inputLastActivity();
  uint8_t target = g_set.brightness;
  if (g_set.offAfter && idleMs > (uint32_t)g_set.offAfter * 1000UL) target = 0;
  else if (g_set.dimAfter && idleMs > (uint32_t)g_set.dimAfter * 1000UL) target = min(g_set.dimPct, g_set.brightness);
  if (target != curBl) { setBacklight(target); dirty = true; }

  // ---- power: sleep timer ----
  if (sleepTimerAt && (int32_t)(now - sleepTimerAt) >= 0) { sleepTimerAt = 0; sleepTimerMin = 0; sleepReq = true; }

  // ---- power: deferred settings save (avoid writing NVS on every volume click) ----
  if (settingsDirtyAt && now - settingsDirtyAt > 3000) { settingsDirtyAt = 0; settingsSave(); }

  // ---- power: idle deep sleep (never while playing / connecting) ----
  bool busy = st == PS_PLAYING || st == PS_LOADING || scr == S_CONNECT;
  if (!busy && g_set.sleepMin && idleMs > (uint32_t)g_set.sleepMin * 60000UL) sleepReq = true;

  // ---- render (skipped entirely while the screen is off) ----
  static uint32_t lastDraw = 0;
  static bool toastWas = false;
  bool toastOn = now < toastUntil;
  if (toastOn != toastWas) { toastWas = toastOn; dirty = true; }
  bool anim = scr == S_PLAYER || scr == S_CONNECT || scr == S_LOADING || scr == S_KEYBOARD || toastOn;
  if (!screenIsOff() && (dirty || (anim && now - lastDraw >= 70))) {
    dirty = false;
    lastDraw = now;
    render();
  }

  ledsTick(st == PS_PLAYING, playerVU(), g_set.volume);
  return sleepReq;
}

void uiShowSleepScreen() {
  cv->fillScreen(C_BG);
  txt(160, 80, "Sleeping", F_B, C_TXT, 1);
  txt(160, 108, "press the top button to wake", F_S, C_DIM, 1);
  flush();
  curBl = 255;
  setBacklight(g_set.brightness);
}

void uiDisplayOff() {
  SpiGuard g;
  tft.sendCommand(0x28);   // DISPOFF
  tft.sendCommand(0x10);   // SLPIN
  ledcWrite(0, 0);
  curBl = 0;
}
