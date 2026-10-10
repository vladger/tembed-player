#include "sources.h"
#include "config.h"
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>

Config g_cfg;
static bool s_sd = false;

// =====================================================================
// helpers
// =====================================================================
bool isAudioName(const String& n) {
  int i = n.lastIndexOf('.');
  if (i < 0) return false;
  String e = n.substring(i + 1);
  e.toLowerCase();
  return e == "mp3" || e == "aac" || e == "m4a" || e == "wav" || e == "flac" || e == "ogg" || e == "opus";
}

String baseName(const String& p) {
  String s = p;
  while (s.length() > 1 && s.endsWith("/")) s.remove(s.length() - 1);
  int i = s.lastIndexOf('/');
  return i >= 0 ? s.substring(i + 1) : s;
}

String parentDir(const String& p) {
  int i = p.lastIndexOf('/');
  if (i <= 0) return "/";
  return p.substring(0, i);
}

static String joinPath(const String& d, const String& n) {
  return d.endsWith("/") ? d + n : d + "/" + n;
}

static String urlDecode(const String& s) {
  String o;
  o.reserve(s.length());
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '%' && i + 2 < s.length() + 0 && isxdigit(s[i + 1]) && isxdigit(s[i + 2])) {
      char h[3] = {s[i + 1], s[i + 2], 0};
      o += (char)strtol(h, nullptr, 16);
      i += 2;
    } else if (c == '+') {
      o += ' ';
    } else {
      o += c;
    }
  }
  return o;
}

static void sortEntries(std::vector<Entry>& v) {
  std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

static void mergeSorted(std::vector<Entry>& dirs, std::vector<Entry>& files, std::vector<Entry>& out) {
  sortEntries(dirs);
  sortEntries(files);
  out.clear();
  for (auto& e : dirs) out.push_back(e);
  for (auto& e : files) out.push_back(e);
}

// =====================================================================
// SD card
// =====================================================================
bool sdBegin() {
  SpiGuard g;
  s_sd = SD.begin(PIN_SD_CS, SPI, 20000000);
  return s_sd;
}
bool sdOk() { return s_sd; }
void sdEnd() { SpiGuard g; SD.end(); s_sd = false; }

bool sdDirExists(const char* path) {
  if (!s_sd) return false;
  SpiGuard g;
  File f = SD.open(path);
  bool ok = f && f.isDirectory();
  if (f) f.close();
  return ok;
}

void sdCleanCache() {
  if (!s_sd) return;
  SpiGuard g;
  for (int guard = 0; guard < 16; guard++) {
    File d = SD.open(CACHE_DIR);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    File f = d.openNextFile();
    if (!f) { d.close(); return; }
    String p = joinPath(CACHE_DIR, f.name());
    f.close();
    d.close();
    SD.remove(p);
  }
}

bool sdList(const String& dir, std::vector<Entry>& out) {
  if (!s_sd) return false;
  SpiGuard g;
  File d = SD.open(dir.isEmpty() ? "/" : dir.c_str());
  if (!d || !d.isDirectory()) { if (d) d.close(); return false; }
  std::vector<Entry> dirs, files;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name();
    bool isDir = f.isDirectory();
    f.close();
    if (name.startsWith(".") || name == "System Volume Information") continue;
    if (isDir) dirs.push_back(Entry(name, joinPath(dir, name), true));
    else if (isAudioName(name)) files.push_back(Entry(name, joinPath(dir, name), false));
    if (dirs.size() + files.size() >= MAX_LIST_ENTRIES) break;
  }
  d.close();
  mergeSorted(dirs, files, out);
  return true;
}

// =====================================================================
// config
// =====================================================================
static const char* CFG_TEMPLATE = R"JSON({
  "wifi": [
    { "ssid": "YourWiFi", "pass": "YourPassword" }
  ],
  "ftp": {
    "host": "",
    "port": 21,
    "user": "anonymous",
    "pass": "",
    "path": "/"
  },
  "http": [
    { "name": "SomaFM Groove Salad", "url": "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "name": "SomaFM DEF CON Radio", "url": "http://ice1.somafm.com/defcon-128-mp3" },
    { "name": "Radio Paradise", "url": "http://stream.radioparadise.com/mp3-128" }
  ]
}
)JSON";

void loadConfig() {
  g_cfg = Config();
  if (s_sd) {
    SpiGuard g;
    if (!SD.exists(CFG_PATH)) {
      SD.mkdir("/player");
      File w = SD.open(CFG_PATH, FILE_WRITE);
      if (w) { w.print(CFG_TEMPLATE); w.close(); }
    }
    File f = SD.open(CFG_PATH, FILE_READ);
    if (f) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if (!err) {
        for (JsonObject w : doc["wifi"].as<JsonArray>()) {
          WifiCred c;
          c.ssid = w["ssid"].as<String>();
          c.pass = w["pass"].as<String>();
          if (c.ssid.length() && c.ssid != "YourWiFi") g_cfg.wifi.push_back(c);
        }
        JsonObject ftp = doc["ftp"];
        g_cfg.ftpHost = ftp["host"].as<String>();
        g_cfg.ftpPort = ftp["port"] | 21;
        if (ftp["user"].is<const char*>()) g_cfg.ftpUser = ftp["user"].as<String>();
        g_cfg.ftpPass = ftp["pass"].as<String>();
        if (ftp["path"].is<const char*>() && ftp["path"].as<String>().length()) g_cfg.ftpPath = ftp["path"].as<String>();
        for (JsonObject h : doc["http"].as<JsonArray>()) {
          Station s;
          s.name = h["name"].as<String>();
          s.url = h["url"].as<String>();
          if (s.url.length()) {
            if (s.name.isEmpty()) s.name = s.url;
            g_cfg.http.push_back(s);
          }
        }
      }
    }
  }
  if (g_cfg.http.empty()) {
    g_cfg.http.push_back({"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"});
    g_cfg.http.push_back({"Radio Paradise", "http://stream.radioparadise.com/mp3-128"});
  }
}

bool cfgAddStation(const String& name, const String& url) {
  Station st; st.name = name; st.url = url;
  g_cfg.http.push_back(st);
  if (!s_sd) return false;
  SpiGuard g;
  JsonDocument doc;
  File f = SD.open(CFG_PATH, FILE_READ);
  if (f) { deserializeJson(doc, f); f.close(); }
  JsonArray arr = doc["http"].is<JsonArray>() ? doc["http"].as<JsonArray>() : doc["http"].to<JsonArray>();
  JsonObject o = arr.add<JsonObject>();
  o["name"] = name;
  o["url"] = url;
  SD.mkdir("/player");
  SD.remove(CFG_PATH);
  File w = SD.open(CFG_PATH, FILE_WRITE);
  if (!w) return false;
  serializeJsonPretty(doc, w);
  w.close();
  return true;
}

// =====================================================================
// Wi-Fi
// =====================================================================
static int      s_netState = NET_IDLE;
static size_t   s_netIdx = 0;
static uint32_t s_netT = 0;

static void netBeginCurrent() {
  const WifiCred& c = g_cfg.wifi[s_netIdx];
  WiFi.disconnect();
  WiFi.begin(c.ssid.c_str(), c.pass.c_str());
  s_netT = millis();
}

void netStart() {
  if (WiFi.status() == WL_CONNECTED) { s_netState = NET_OK; return; }
  if (g_cfg.wifi.empty()) { s_netState = NET_FAIL; return; }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(g_set.wifiSave); // modem sleep only if the power option is on
  WiFi.setAutoReconnect(true);
  s_netIdx = 0;
  netBeginCurrent();
  s_netState = NET_CONNECTING;
}

int netPoll() {
  if (s_netState == NET_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      s_netState = NET_OK;
    } else if (millis() - s_netT > 9000) {
      if (++s_netIdx >= g_cfg.wifi.size()) { s_netState = NET_FAIL; WiFi.disconnect(); }
      else netBeginCurrent();
    }
  }
  return s_netState;
}

void netStop() { WiFi.disconnect(); s_netState = NET_IDLE; }

String netSsid() {
  if (s_netIdx < g_cfg.wifi.size()) return g_cfg.wifi[s_netIdx].ssid;
  return "";
}

// =====================================================================
// FTP (minimal passive-mode client)
// =====================================================================
static int ftpRead(WiFiClient& c, String* last = nullptr) {
  uint32_t t = millis();
  while (millis() - t < 8000) {
    if (!c.connected() && !c.available()) return -1;
    if (!c.available()) { delay(2); continue; }
    String line = c.readStringUntil('\n');
    line.trim();
    t = millis();
    if (line.length() < 3 || !isdigit((unsigned char)line[0])) continue;
    if (line.length() == 3 || line[3] == ' ') {       // final line of a (multi-line) reply
      if (last) *last = line;
      return line.substring(0, 3).toInt();
    }
  }
  return -1;
}

static int ftpCmd(WiFiClient& c, const String& cmd, String* last = nullptr) {
  c.print(cmd);
  c.print("\r\n");
  return ftpRead(c, last);
}

static bool ftpOpen(WiFiClient& c) {
  if (g_cfg.ftpHost.isEmpty()) return false;
  if (!c.connect(g_cfg.ftpHost.c_str(), g_cfg.ftpPort, 6000)) return false;
  if (ftpRead(c) != 220) return false;
  int r = ftpCmd(c, "USER " + g_cfg.ftpUser);
  if (r == 331) r = ftpCmd(c, "PASS " + g_cfg.ftpPass);
  if (r != 230 && r != 202) return false;
  ftpCmd(c, "TYPE I");
  return true;
}

static bool ftpPasv(WiFiClient& c, WiFiClient& d) {
  String resp;
  if (ftpCmd(c, "PASV", &resp) != 227) return false;
  int a = resp.indexOf('(');
  if (a < 0) return false;
  int v[6];
  if (sscanf(resp.c_str() + a + 1, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  uint16_t port = (uint16_t)(v[4] * 256 + v[5]);
  // connect to the control-connection host (works behind NAT / odd PASV addresses)
  return d.connect(g_cfg.ftpHost.c_str(), port, 6000);
}

static const char* skipTok(const char* p) {
  while (*p && *p != ' ' && *p != '\t') p++;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

static bool parseListLine(String line, const String& dir, Entry& e) {
  line.trim();
  if (line.length() < 2 || line.startsWith("total")) return false;
  const char* s = line.c_str();
  bool isDir = false;
  String name;
  if (isdigit((unsigned char)s[0])) {                 // DOS/IIS style: "10-09-26 03:12PM <DIR> name"
    const char* p3 = skipTok(skipTok(s));
    const char* p4 = skipTok(p3);
    isDir = strncmp(p3, "<DIR>", 5) == 0;
    name = p4;
  } else if (s[0] == 'd' || s[0] == '-' || s[0] == 'l') {   // Unix style
    const char* p = s;
    for (int k = 0; k < 8; k++) p = skipTok(p);
    isDir = s[0] == 'd';
    name = p;
    int arrow = name.indexOf(" -> ");
    if (arrow > 0) name.remove(arrow);
  } else {
    return false;
  }
  name.trim();
  if (name.isEmpty() || name == "." || name == "..") return false;
  e = Entry(name, joinPath(dir, name), isDir);
  return true;
}

bool ftpList(const String& dir, std::vector<Entry>& out) {
  WiFiClient c, d;
  if (!ftpOpen(c)) return false;
  bool ok = false;
  if (ftpCmd(c, "CWD " + dir) == 250 && ftpPasv(c, d)) {
    int r = ftpCmd(c, "LIST");
    if (r == 150 || r == 125) {
      String buf;
      uint8_t tmp[512];
      uint32_t t = millis();
      while ((d.connected() || d.available()) && millis() - t < 8000 && buf.length() < 60000) {
        int n = d.read(tmp, sizeof(tmp));
        if (n > 0) { buf.concat((const char*)tmp, n); t = millis(); }
        else delay(2);
      }
      d.stop();
      ftpRead(c);   // 226
      std::vector<Entry> dirs, files;
      int pos = 0;
      while (pos < (int)buf.length() && dirs.size() + files.size() < MAX_LIST_ENTRIES) {
        int nl = buf.indexOf('\n', pos);
        if (nl < 0) nl = buf.length();
        Entry e;
        if (parseListLine(buf.substring(pos, nl), dir, e)) {
          if (e.isDir) dirs.push_back(e);
          else if (isAudioName(e.name)) files.push_back(e);
        }
        pos = nl + 1;
      }
      mergeSorted(dirs, files, out);
      ok = true;
    }
  }
  d.stop();
  c.print("QUIT\r\n");
  c.stop();
  return ok;
}

bool ftpDownload(const String& remote, const String& localPath, volatile int& pct, volatile bool& cancel) {
  WiFiClient c, d;
  if (!ftpOpen(c)) return false;

  long size = -1;
  { String l; if (ftpCmd(c, "SIZE " + remote, &l) == 213) size = atol(l.c_str() + 4); }

  if (!ftpPasv(c, d)) { c.stop(); return false; }
  int r = ftpCmd(c, "RETR " + remote);
  if (r != 150 && r != 125) { d.stop(); c.stop(); return false; }

  File f;
  { SpiGuard g; SD.remove(localPath); f = SD.open(localPath, FILE_WRITE); }
  if (!f) { d.stop(); c.stop(); return false; }

  const size_t BUF = 8192;
  uint8_t* buf = (uint8_t*)malloc(BUF);
  if (!buf) { SpiGuard g; f.close(); d.stop(); c.stop(); return false; }

  long total = 0;
  uint32_t t = millis();
  while ((d.connected() || d.available()) && !cancel) {
    int n = d.read(buf, BUF);
    if (n > 0) {
      { SpiGuard g; f.write(buf, n); }
      total += n;
      t = millis();
      if (size > 0) pct = (int)(total * 100L / size);
    } else {
      if (millis() - t > 10000) break;
      delay(1);
    }
  }
  free(buf);
  { SpiGuard g; f.close(); }
  d.stop();
  if (!cancel) ftpRead(c);
  c.print("QUIT\r\n");
  c.stop();
  if (cancel) { SpiGuard g; SD.remove(localPath); return false; }
  return total > 0 && (size < 0 || total == size);
}

// =====================================================================
// HTTP directory index
// =====================================================================
bool httpList(const String& url, std::vector<Entry>& out) {
  HTTPClient http;
  WiFiClientSecure sec;
  WiFiClient plain;
  if (url.startsWith("https")) { sec.setInsecure(); http.begin(sec, url); }
  else http.begin(plain, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200 || http.getSize() > 150000) { http.end(); return false; }
  String body = http.getString();
  http.end();

  String base = url;
  if (!base.endsWith("/")) base += "/";

  std::vector<Entry> dirs, files;
  int pos = 0;
  while (dirs.size() + files.size() < MAX_LIST_ENTRIES) {
    int h = body.indexOf("href=", pos);
    if (h < 0) break;
    char q = body[h + 5];
    if (q != '"' && q != '\'') { pos = h + 5; continue; }
    int e = body.indexOf(q, h + 6);
    if (e < 0) break;
    String href = body.substring(h + 6, e);
    pos = e + 1;
    if (href.isEmpty() || href[0] == '?' || href[0] == '#' || href[0] == '/' || href.startsWith("..") ||
        href.indexOf("://") >= 0 || href.startsWith("mailto")) continue;
    bool isDir = href.endsWith("/");
    String name = urlDecode(href);
    if (isDir) name.remove(name.length() - 1);
    if (!isDir && !isAudioName(name)) continue;
    (isDir ? dirs : files).push_back(Entry(name, base + href, isDir));
  }
  mergeSorted(dirs, files, out);
  return true;
}
