

#include <Arduino.h>
#include "TFT.h"


// Load FONTS
#include "NationalRail.h"
#include "fonts_compat.h"
extern void tft_app_setup();
extern void tft_app_loop();

// [TRAKKR-NOTE] TFT dimensions for ILI9488 in landscape
static const int SCREEN_W = 480;
static const int SCREEN_H = 320;

void setup() {
  Serial.begin(115200);

  // ---- Init display ----
  tft.init();
  tft.setRotation(1);                 // landscape
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFreeFont(&NationalRailRegular); 
  tft.fillScreen(TFT_BLACK);  // <<---- clears everything

  // ---- Prepare 2 labels ----
  const char* rows[2] = {
    "TRAKKR", 
    "Your Desktop Station"
  };

  // ---- Layout: evenly distribute rows vertically, centred horizontally ----
  const int ROWS = 2;
  const int lineH = tft.fontHeight();
  const int totalH = lineH * ROWS;
  int top = (SCREEN_H - totalH) / 2;
  if (top < 0) top = 0;

  tft.setTextDatum(MC_DATUM);
  const int cx = SCREEN_W / 2;

  for (int i = 0; i < ROWS; ++i) {
    int y = top + i * lineH + (lineH / 2);
    tft.drawString(rows[i], cx, y);
  }

  Serial.println("[TRAKKR] Success — drew title");
  delay(5000);

  // ---- Clear before handing off ----
  Serial.println("[TRAKKR] Clearing screen");
  tft.fillScreen(TFT_BLACK);  // <<---- clears everything

  // ---- Hand off to main app ----
  tft_app_setup();
}


void loop() {
  tft_app_loop();
}
