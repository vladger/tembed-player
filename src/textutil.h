#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include "ufont.h"

// Returns valid UTF-8: control characters removed and common mis-encodings repaired
// (Windows-1251 bytes, Windows-1251 mislabelled as Latin-1, UTF-8 double-encoded as Latin-1).
std::string textClean(const char* in);

uint32_t       utf8Next(const char*& s);                          // next code point, 0xFFFD on garbage
const UGlyph*  ufontGlyph(const UFont* f, uint32_t cp);           // nullptr if missing
int            ufontWidth(const UFont* f, const char* s);         // pixel width
size_t         ufontFitBytes(const UFont* f, const char* s, int maxW);   // longest prefix (bytes) that fits
