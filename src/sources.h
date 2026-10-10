#pragma once
#include <Arduino.h>
#include <vector>

struct Entry {
  String name;
  String path;       // SD path | FTP path | URL
  bool   isDir  = false;
  bool   stream = false;   // live web stream (HTTP station)
  Entry() {}
  Entry(const String& n, const String& p, bool d, bool s = false) : name(n), path(p), isDir(d), stream(s) {}
};

struct WifiCred { String ssid, pass; };
struct Station  { String name, url; };

struct Config {
  std::vector<WifiCred> wifi;
  String   ftpHost;
  uint16_t ftpPort = 21;
  String   ftpUser = "anonymous";
  String   ftpPass;
  String   ftpPath = "/";
  std::vector<Station> http;
};
extern Config g_cfg;

// ---- SD ----
bool sdBegin();
bool sdOk();
void sdEnd();
void sdCleanCache();
bool sdDirExists(const char* path);
bool sdList(const String& dir, std::vector<Entry>& out);
bool cfgAddStation(const String& name, const String& url);   // appends to /player/config.json
void loadConfig();                       // reads /player/config.json (creates a template if missing)

// ---- FTP ----
bool ftpList(const String& dir, std::vector<Entry>& out);
bool ftpDownload(const String& remote, const String& localPath, volatile int& pct, volatile bool& cancel);

// ---- HTTP directory index (Apache / nginx autoindex style) ----
bool httpList(const String& url, std::vector<Entry>& out);

// ---- helpers ----
bool   isAudioName(const String& n);
String baseName(const String& path);
String parentDir(const String& path);

// ---- Wi-Fi (non-blocking, tries every configured network in turn) ----
enum NetState { NET_IDLE = 0, NET_CONNECTING, NET_OK, NET_FAIL };
void   netStart();
int    netPoll();
void   netStop();
String netSsid();
