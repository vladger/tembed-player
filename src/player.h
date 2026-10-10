#pragma once
#include <Arduino.h>

enum SrcType : uint8_t { SRC_NONE = 0, SRC_SD, SRC_FTP, SRC_HTTP };
enum PlayState : uint8_t { PS_STOPPED = 0, PS_LOADING, PS_PLAYING, PS_PAUSED, PS_ERROR };

struct PlayerMeta {
  char title[128];
  char artist[80];
  char station[80];
};

void        playerBegin();
// path = SD path | FTP remote path | http(s) URL. stream = live web stream (no duration).
void        playerPlay(SrcType src, const char* path, bool stream, const char* name);
void        playerTogglePause();
void        playerStop();
void        playerSetVolume(uint8_t v);          // 0..21

PlayState   playerState();
uint32_t    playerPos();                         // seconds
uint32_t    playerDur();                         // seconds, 0 = unknown / live
uint32_t    playerBitrate();                     // bit/s
uint32_t    playerSampleRate();
uint8_t     playerVU();                          // 0..255 (raw, roughly)
int         playerLoadPct();                     // FTP download progress, -1 = n/a
const char* playerError();
bool        playerPollEnded();                   // true once when a track/stream finished
void        playerGetMeta(PlayerMeta& m);
