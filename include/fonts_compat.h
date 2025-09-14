//
// [TRAKKR] FreeFont type compat shim for TFT_eSPI / Adafruit_GFX
// Ensures GFXglyph / GFXfont are defined before including custom fonts.
// Safe to include anywhere; it only defines types if the official header
// hasn't already been included.
//
// [TRAKKR-NOTE] We DO NOT modify library files. This header avoids that by
// conditionally including gfxfont.h from whichever GFX stack is present,
// then providing a minimal fallback definition if neither is found.
//

#pragma once

// Arduino core (for PROGMEM, etc.)
#if defined(ARDUINO)
  #include <Arduino.h>
#endif

// Try to include the official GFX font header from common locations
#if __has_include(<gfxfont.h>)
  #include <gfxfont.h>
#elif __has_include(<Fonts/gfxfont.h>)
  #include <Fonts/gfxfont.h>
#elif __has_include(<TFT_eSPI.h>)
  // If TFT_eSPI is present, pull it in (it may pull its own gfxfont)
  #include <TFT_eSPI.h>
#endif

// If, after the above, the guard used by Adafruit's gfxfont.h is not set,
// and the types are still missing, provide a minimal compatible definition.
#ifndef _GFXFONT_H_
  // Avoid redefinition if another header already brought these in.
  #ifndef TRAKKR_MIN_GFXFONT_DEFINED
  #define TRAKKR_MIN_GFXFONT_DEFINED

  typedef struct {        // Matches Adafruit_GFX layout
    uint16_t bitmapOffset;
    uint8_t  width;
    uint8_t  height;
    uint8_t  xAdvance;
    int8_t   xOffset;
    int8_t   yOffset;
  } GFXglyph;

  typedef struct {        // Matches Adafruit_GFX layout
    uint8_t  *bitmap;     // Glyph bitmaps, concatenated
    GFXglyph *glyph;      // Glyph array
    uint16_t  first;      // ASCII extents (first)
    uint16_t  last;       // ASCII extents (last)
    uint8_t   yAdvance;   // Newline distance
  } GFXfont;

  #endif // TRAKKR_MIN_GFXFONT_DEFINED
#endif // _GFXFONT_H_

//
// [TRAKKR-TODO] If you later add more custom fonts, include this header
// before their font headers to guarantee the types exist.
//
