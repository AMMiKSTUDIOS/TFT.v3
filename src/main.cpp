

#include <Arduino.h>
#include "TFT.h"


// Load FONTS
#include "NationalRail.h"
#include "fonts_compat.h"


// [TRAKKR-NOTE] TFT dimensions for ILI9488 in landscape
static const int SCREEN_W = 480;
static const int SCREEN_H = 320;

void setup() {
  Serial.begin(115200);

  // ---- Init display ----
  tft.init();
  tft.setRotation(1);                 // landscape  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFreeFont(&NationalRailSmall); 


  // ---- Prepare 12 labels ----
  const char* rows[12] = {
    "One","Two","11:17  London Euston               1  Cancelled","11:34  London Euston               1  ETA 11:36","11:51  London Euston               1  On Time","Six",
    "Seven","Eight","Nine","Ten","Eleven","Twelve"
  };

  // ---- Layout: evenly distribute 12 rows vertically, centred horizontally ----
  const int ROWS = 12;
  const int lineH = tft.fontHeight();                 // yAdvance for current FreeFont
  const int totalH = lineH * ROWS;
  int top = (SCREEN_H - totalH) / 2;                  // top padding so block is vertically centred
  if (top < 0) top = 0;                                // clamp if font is too large

  tft.setTextDatum(MC_DATUM);                         // middle-centre for easy centring
  const int cx = SCREEN_W / 2;

  for (int i = 0; i < ROWS; ++i) {
    // Middle of each row band = top + i*lineH + lineH/2
    int y = top + i * lineH + (lineH / 2);
    tft.drawString(rows[i], cx, y);
  }

  Serial.println("[TRAKKR] Success — drew 12 rows (One..Twelve) using NationalRail ~20pt");
}

void loop() {
  // Idle
}
