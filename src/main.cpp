#include <Arduino.h>
#include "TFT.h"

#include <FS.h>
#include <LittleFS.h>
#include <JPEGDecoder.h>

// Load FONTS
#include "NationalRail.h"
#include "fonts_compat.h"
extern void tft_app_setup();
extern void tft_app_loop();

// [TRAKKR-NOTE] TFT dimensions for ILI9488 in landscape
static const int SCREEN_W = 480;
static const int SCREEN_H = 320;

// -----------------------------------------------------------------------------
// [TRAKKR] Utilities
// -----------------------------------------------------------------------------
static void listFS() {
  Serial.println("[TRAKKR] Listing LittleFS contents:");
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) { Serial.println("  <no root dir>"); return; }
  File file = root.openNextFile();
  while (file) {
    Serial.printf("  %s  (%u bytes)\n", file.name(), (unsigned)file.size());
    file = root.openNextFile();
  }
}

// Render one JPEG MCU block at the requested position
static void renderJPEG(int xpos, int ypos) {
  while (JpegDec.read()) {
    int mcu_x = JpegDec.MCUx * JpegDec.MCUWidth  + xpos;
    int mcu_y = JpegDec.MCUy * JpegDec.MCUHeight + ypos;

    int win_w = (mcu_x + JpegDec.MCUWidth  <= xpos + JpegDec.width ) ? JpegDec.MCUWidth  : (JpegDec.width  % JpegDec.MCUWidth);
    int win_h = (mcu_y + JpegDec.MCUHeight <= ypos + JpegDec.height) ? JpegDec.MCUHeight : (JpegDec.height % JpegDec.MCUHeight);

    if (win_w && win_h) {
      tft.pushImage(mcu_x, mcu_y, win_w, win_h, JpegDec.pImage);
    }
  }
}

// Decode and draw a JPG from LittleFS at (x,y)
static bool drawJpgFile(const char *path, int x, int y) {
  // basic sanity: exist + not tiny
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.printf("[TRAKKR] File not found: %s\n", path); return false; }
  size_t sz = f.size(); f.close();
  if (sz < 1024) { Serial.printf("[TRAKKR] File too small (%u bytes): %s\n", (unsigned)sz, path); return false; }

  // IMPORTANT: JPEGDecoder outputs RGB565 in big-endian.
  // ILI9488 (TFT_eSPI) expects little-endian unless swapBytes is enabled.
  // Enable byte swap for correct colours (prevents the orange/pink cast).
  tft.setSwapBytes(true);

  if (!JpegDec.decodeFsFile(path)) {
    Serial.printf("[TRAKKR] JPEG decode failed: %s\n", path);
    tft.setSwapBytes(false);
    return false;
  }

  tft.startWrite();
  renderJPEG(x, y);
  tft.endWrite();

  // reset (optional)
  tft.setSwapBytes(false);
  return true;
}

// [TRAKKR] Helper to show a centred message list
static void showSplash(const char* rows[], int rowCount, int holdMs) {
  tft.fillScreen(TFT_BLACK);
  tft.setFreeFont(&NationalRailRegular);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  const int lineH  = tft.fontHeight();
  const int totalH = lineH * rowCount;
  int top = (SCREEN_H - totalH) / 2; if (top < 0) top = 0;

  const int cx = SCREEN_W / 2;
  for (int i = 0; i < rowCount; ++i) {
    int y = top + i * lineH + (lineH / 2);
    tft.drawString(rows[i], cx, y);
  }
  delay(holdMs);
}

void setup() {
  Serial.begin(115200);

  // ---- Init display ----
  tft.init();
  tft.setRotation(1); // landscape
  tft.fillScreen(TFT_BLACK);

  // ---- Init filesystem ----
  if (!LittleFS.begin()) {
    Serial.println("[TRAKKR] LittleFS mount failed!");
  } else {
    listFS();

    // Try to show splash image if present
    if (LittleFS.exists("/TRAKKR.jpg")) {
      if (drawJpgFile("/TRAKKR.jpg", 0, 0)) {
        Serial.println("[TRAKKR] Splash loaded OK");
        delay(10000);
      } else {
        Serial.println("[TRAKKR] Splash image present but failed to draw");
      }
    } else {
      Serial.println("[TRAKKR] Splash image not found, skipping");
    }
  }

  // Optional text splash sequence (kept from your original)
  const char* scr1[] = { "TRAKKR", "from AMMiKSTUDIOS" };
  showSplash(scr1, 2, 1500);

  const char* scr2[] = { "Powered by National Rail, TfL Open Data", "and Underground Weather" };
  showSplash(scr2, 2, 1500);

  const char* scr3[] = { "Copyright (c)2025 AMMiKSTUDIOS:", "All Rights Reserved" };
  showSplash(scr3, 2, 1500);

  const char* scr4[] = { "Control Panel", "http://trakkr.local" };
  showSplash(scr4, 2, 1500);

  Serial.println("[TRAKKR] Splash sequence complete — clearing");
  tft.fillScreen(TFT_BLACK);

  // ---- Hand off to main app ----
  tft_app_setup();
}

void loop() {
  tft_app_loop();
}
