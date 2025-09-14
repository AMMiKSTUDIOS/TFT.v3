#ifndef TFT_H
#define TFT_H

//
// [TRAKKR] TFT wrapper for ESP32 + ILI9488
// [TRAKKR-NOTE] We enable FreeFonts here (no library edits).
//
#ifndef LOAD_GFXFF
  #define LOAD_GFXFF    // enables setFreeFont(const GFXfont*) in TFT_eSPI
#endif
// Optional: enable smooth .vlw fonts if you use them later
// #define SMOOTH_FONT

#include <TFT_eSPI.h>   // Will see defines from TFTSetup.h via -include
extern TFT_eSPI tft;    // global display instance

#endif
