// TRAKKER - copyright (c)2025 AMMiKSTUDIOS
// All Rights Reserved
//
// TRAKKR is commercial software: you may not redistribute it and/or modify
// it without prior permission from AMMiKSTUDIOS.
// https://www.ammikstudios.com

#include <Arduino.h>
#include "Global.h"
#include "TFT.h"
#include <FS.h>
#include <LittleFS.h>
#include <JPEGDecoder.h>
#include "NationalRail.h"
#include "fonts_compat.h"
#include <WiFi.h>
#include <time.h>
#include "HttpServer.h"

extern void rail_setup();
extern void rail_loop();

// TFT dimensions for ILI9488 in landscape
static const int SCREEN_W = 480;
static const int SCREEN_H = 320;

// Time config
static const char* NTP_1 = "pool.ntp.org";                //  Primary NTP server
static const char* NTP_2 = "time.nist.gov";               //  Secondary NTP server
static const char* TZ_UK = "GMT0BST,M3.5.0/1,M10.5.0/2";  //  UK timezone with BST rules

// [TRAKKR] Match rail.cpp body background (#0b1020)
static inline uint16_t bodyBgMain() {
  return tft.color565(0x0b, 0x10, 0x20);
}

//
// [TRAKKR] Centralised Wi-Fi connect/reconnect
// Safe to call repeatedly; returns immediately if already connected.
//
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(Cfg::wifiSsid(), Cfg::wifiPass());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
    Serial.print('.');
  }
  Serial.println();
}

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
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.printf("[TRAKKR] File not found: %s\n", path); return false; }
  size_t sz = f.size(); f.close();
  if (sz < 1024) { Serial.printf("[TRAKKR] File too small (%u bytes): %s\n", (unsigned)sz, path); return false; }

  // [TRAKKR-NOTE] JPEGDecoder outputs big-endian RGB565; enable swap for TFT_eSPI.
  tft.setSwapBytes(true);

  if (!JpegDec.decodeFsFile(path)) {
    Serial.printf("[TRAKKR] JPEG decode failed: %s\n", path);
    tft.setSwapBytes(false);
    return false;
  }

  tft.startWrite();
  renderJPEG(x, y);
  tft.endWrite();

  tft.setSwapBytes(false);
  return true;
}

// [TRAKKR] Helper to show a centred message list
static void showSplash(const char* rows[], int rowCount, int holdMs) {
  tft.fillScreen(bodyBgMain());
  tft.setFreeFont(&NationalRailRegular);
  tft.setTextColor(TFT_WHITE, bodyBgMain());
  tft.setTextDatum(MC_DATUM);

  const int lineH  = tft.fontHeight();
  const int totalH = lineH * rowCount;
  int top = (SCREEN_H - totalH) / 2; if (top < 0) top = 0;

  const int cx = SCREEN_W / 2;
  for (int i = 0; i < rowCount; ++i) {
    int y = top + i * lineH + (lineH / 2);
    tft.drawString(rows[i], cx, y);
  }
  if (holdMs > 0) delay(holdMs);
}

// [TRAKKR] Animated Wi-Fi splash with success/fail outcome
static void splashWifiConnect(uint32_t timeoutMs = 15000) {
  const char* title = "Connecting to WiFi";
  const char* lines0[2] = { title, "" };
  showSplash(lines0, 2, 0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(Cfg::wifiSsid(), Cfg::wifiPass());

  uint32_t t0 = millis();
  uint8_t dots = 0;
  char line2[8] = {0};

  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    dots = (dots % 3) + 1;                      // 1..3 dots
    memset(line2, 0, sizeof(line2));
    for (uint8_t i = 0; i < dots; i++) line2[i] = '.';

    const char* lines[2] = { title, line2 };
    showSplash(lines, 2, 0);
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    String ok = String("WiFi connected: ") + ip;
    const char* linesOK[2] = { "Connected", ok.c_str() };
    showSplash(linesOK, 2, 3000);
    Serial.printf("[TRAKKR] Wi-Fi connected, IP: %s\n", ip.c_str());
  } else {
    const char* linesFail[2] = { "WiFi failed", "Check SSID / PASS" };
    showSplash(linesFail, 2, 3000);
    Serial.println("[TRAKKR] Wi-Fi failed");
  }
}

// [TRAKKR] Check if system time is valid (i.e. has been set via NTP)
static bool timeIsValid() {
  time_t now = time(nullptr);
  return (now >= 1704067200UL); // 2024-01-01 00:00:00 UTC
}

// [TRAKKR] Ensure time is set via NTP; safe to call repeatedly
void ensureTime() {
  if (timeIsValid()) return;
  configTime(0, 0, NTP_1, NTP_2);     // use NTP servers
  setenv("TZ", TZ_UK, 1); tzset();    // apply local timezone
}

// [TRAKKR] Animated "Setting clock…" splash with success/fail outcome
static void splashTimeSync(uint32_t timeoutMs = 15000) {
  const char* title = "Setting clock";
  const char* lines0[2] = { title, "" };
  showSplash(lines0, 2, 0);

  // Begin sync (idempotent)
  ensureTime();

  // Animate dots while waiting for valid time
  uint32_t t0 = millis();
  uint8_t dots = 0;
  char line2[8] = {0};

  while (!timeIsValid() && (millis() - t0) < timeoutMs) {
    dots = (dots % 3) + 1;    // 1..3 dots
    memset(line2, 0, sizeof(line2));
    for (uint8_t i = 0; i < dots; i++) line2[i] = '.';
    const char* lines[2] = { title, line2 };
    showSplash(lines, 2, 0);
    delay(250);
  }

  if (timeIsValid()) {
    // Format local time for display
    time_t now = time(nullptr);
    struct tm lt; localtime_r(&now, &lt);
    char buf[48];
    strftime(buf, sizeof(buf), "%d %b %Y %H:%M", &lt);
    String msg = String("Time: ") + buf;
    const char* ok[2] = { "Clock set", msg.c_str() };
    showSplash(ok, 2, 2000);
    Serial.printf("[TRAKKR] Clock set to %s\n", buf);
  } else {
    const char* fail[2] = { "Time sync failed", "Will retry later" };
    showSplash(fail, 2, 1500);
    Serial.println("[TRAKKR] Time sync failed");
  }
}

// -----------------------------------------------------------------------------
// Main application entry points
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // [TRAKKR] Load NVS-backed config (Wi-Fi, CRS, mode, tokens, etc.)
  Cfg::begin();

  // ---- Init display ----
  tft.init();
  tft.setRotation(1); // landscape
  tft.fillScreen(bodyBgMain());

  // ---- Init filesystem ----
  if (!LittleFS.begin()) {
    Serial.println("[TRAKKR] LittleFS mount failed!");
  } else {
    listFS();

    // Try to show splash image if present
    if (LittleFS.exists("/TRAKKR.jpg")) {
      if (drawJpgFile("/TRAKKR.jpg", 0, 0)) {
        Serial.println("[TRAKKR] Splash loaded OK");
        delay(5000);
      } else {
        Serial.println("[TRAKKR] Splash image present but failed to draw");
      }
    } else {
      Serial.println("[TRAKKR] Splash image not found, skipping");
    }
  }
/*
  // Optional text splash sequence
  const char* scr1[] = { "Welcome to TRAKKR", "from AMMiKSTUDIOS" };
  showSplash(scr1, 2, 3000);

  const char* scr2[] = { "Powered by National Rail, TfL Open Data", "and OpenWeather" };
  showSplash(scr2, 2, 3000);

  const char* scr3[] = { "Copyright (c)2025 AMMiKSTUDIOS:", "All Rights Reserved" };
  showSplash(scr3, 2, 3000);
*/
  // Wi-Fi splash (animated)
  Serial.println("[TRAKKR] Connecting Wi-Fi from main.cpp…");
  splashWifiConnect();

  // Time splash
  Serial.println("[TRAKKR] Setting Time from main.cpp…");
  splashTimeSync();
/*
  // Control Panel info
  const char* scr5[] = { "Control Panel", "http://trakkr.local" };
  showSplash(scr5, 2, 3000);
*/
  // Clear screen to avoid ghosting from splash
  tft.fillScreen(bodyBgMain());

  // ---- Hand-off to main app ----
  http_setup();
  rail_setup();
}

void loop() {
  http_loop();
  rail_loop();
}
