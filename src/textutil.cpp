#include "textutil.h"
#include <vector>

// ---------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------
uint32_t utf8Next(const char*& s) {
  uint8_t c = (uint8_t)*s;
  if (c < 0x80) { s++; return c; }
  if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    uint32_t cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    s += 2; return cp;
  }
  if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    uint32_t cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    s += 3; return cp;
  }
  if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
    uint32_t cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    s += 4; return cp;
  }
  s++;
  return 0xFFFD;
}

static void appendUtf8(std::string& o, uint32_t cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
  else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

// strict decode: returns false if the input is not valid UTF-8
static bool decodeStrict(const char* s, std::vector<uint32_t>& out) {
  while (*s) {
    const char* before = s;
    uint32_t cp = utf8Next(s);
    if (cp == 0xFFFD && (uint8_t)*before != 0xEF) return false;   // garbage byte (a real U+FFFD starts with 0xEF)
    out.push_back(cp);
  }
  return true;
}

// ---------------------------------------------------------------------
// Windows-1251 -> Unicode
// ---------------------------------------------------------------------
static uint32_t cp1251(uint8_t b) {
  if (b < 0x80) return b;
  if (b >= 0xC0) return 0x410 + (b - 0xC0);
  switch (b) {
    case 0xA8: return 0x401;  case 0xB8: return 0x451;   // Ё ё
    case 0xAA: return 0x404;  case 0xBA: return 0x454;   // Є є
    case 0xAF: return 0x407;  case 0xBF: return 0x457;   // Ї ї
    case 0xB2: return 0x406;  case 0xB3: return 0x456;   // І і
    case 0xA5: return 0x490;  case 0xB4: return 0x491;   // Ґ ґ
    case 0x96: return 0x2013; case 0x97: return 0x2014;
    case 0x91: return 0x2018; case 0x92: return 0x2019;
    case 0x93: return 0x201C; case 0x94: return 0x201D;
    case 0x85: return 0x2026; case 0x95: return 0x2022;
    case 0xB9: return 0x2116; case 0xAB: return 0xAB; case 0xBB: return 0xBB;
    case 0xA0: return 0xA0;   case 0xB0: return 0xB0;
  }
  return '?';
}

static bool isCyrLatin1Letter(uint32_t cp) { return (cp >= 0xC0 && cp <= 0xFF) || cp == 0xA8 || cp == 0xB8; }
static bool isAsciiLetter(uint32_t cp) { return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'); }

std::string textClean(const char* in) {
  // fast path: plain ASCII
  const char* p = in;
  bool ascii = true;
  for (; *p; p++) if ((uint8_t)*p >= 0x80 || (uint8_t)*p < 0x20) { ascii = false; break; }
  if (ascii) return std::string(in);

  std::vector<uint32_t> cps;
  if (!decodeStrict(in, cps)) {
    // not UTF-8 at all -> legacy single-byte text, assume Windows-1251
    cps.clear();
    for (const uint8_t* q = (const uint8_t*)in; *q; q++) cps.push_back(cp1251(*q));
  } else {
    bool allLatin1 = true, anyHigh = false;
    int hi = 0, asciiL = 0;
    for (uint32_t cp : cps) {
      if (cp > 0xFF) { allLatin1 = false; break; }
      if (cp >= 0x80) anyHigh = true;
      if (isCyrLatin1Letter(cp)) hi++;
      else if (isAsciiLetter(cp)) asciiL++;
    }
    if (allLatin1 && anyHigh) {
      std::string bytes;
      for (uint32_t cp : cps) bytes += (char)cp;
      std::vector<uint32_t> inner;
      if (decodeStrict(bytes.c_str(), inner)) {
        cps = inner;                                         // UTF-8 that was double-encoded
      } else if (hi >= 2 && hi * 10 >= (hi + asciiL) * 6) {  // Windows-1251 labelled as Latin-1
        for (auto& cp : cps) cp = cp1251((uint8_t)cp);
      }
    }
  }
  std::string out;
  for (uint32_t cp : cps) {
    if (cp == '\t') cp = ' ';
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) continue;
    appendUtf8(out, cp);
  }
  return out;
}

// ---------------------------------------------------------------------
// font metrics
// ---------------------------------------------------------------------
const UGlyph* ufontGlyph(const UFont* f, uint32_t cp) {
  int lo = 0, hi = (int)f->count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t c = f->glyphs[mid].cp;
    if (c == cp) return &f->glyphs[mid];
    if (c < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

static const UGlyph* glyphOrFallback(const UFont* f, uint32_t cp) {
  const UGlyph* g = cp <= 0xFFFF ? ufontGlyph(f, cp) : nullptr;
  return g ? g : ufontGlyph(f, '?');
}

int ufontWidth(const UFont* f, const char* s) {
  int w = 0;
  while (*s) {
    const UGlyph* g = glyphOrFallback(f, utf8Next(s));
    if (g) w += g->adv;
  }
  return w;
}

size_t ufontFitBytes(const UFont* f, const char* s, int maxW) {
  int w = 0;
  const char* start = s;
  while (*s) {
    const char* before = s;
    const UGlyph* g = glyphOrFallback(f, utf8Next(s));
    int a = g ? g->adv : 0;
    if (w + a > maxW) return before - start;
    w += a;
  }
  return s - start;
}
