#pragma once
// Project-local TFT_eSPI setup for TRAKKR (ESP32-S3 + ILI9488 parallel 8-bit)

// [TRAKKR] Force-include: make TFT_eSPI use this setup (ignore library defaults)
#ifndef USER_SETUP_LOADED
  #define USER_SETUP_LOADED
#endif

// ---- Choose driver ----
#define ILI9488_DRIVER

// ---- Bus / interface ----
#define TFT_PARALLEL_8_BIT

// ---- Control pins (match your wiring) ----
// Place immediately after this existing line:
// #define TFT_PARALLEL_8_BIT
// ADD THESE LINES:
#define TFT_CS   16   // LCD_CS -> IO16
#define TFT_DC   17   // LCD_RS -> IO17 (D/C)
#define TFT_WR   4    // LCD_WR -> IO4
#define TFT_RD   -1   // LCD_RD tied HIGH to 3.3V
#define TFT_RST  -1   // LCD_RST tied HIGH to 3.3V

// ---- 8-bit data bus (ESP32-S3 available GPIOs) ----
// Place before fonts section
#define TFT_D0   8
#define TFT_D1   9
#define TFT_D2   10
#define TFT_D3   11
#define TFT_D4   12
#define TFT_D5   13
#define TFT_D6   14
#define TFT_D7   15

// ---- Touch / SD (not used) ----
// [TRAKKR-NOTE] Do NOT define TOUCH_CS in 8/16-bit parallel mode — it forces
// TFT_eSPI to compile Touch.cpp, which is unsupported and triggers a build error.
// Simply leave it undefined to exclude touch support entirely.
// #define TOUCH_CS -1   // <-- keep commented / absent
// #define TFT_SD_CS -1  // leave commented; we’re not using the TFT’s SD slot

// ---- Fonts / options ----
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_GFXFF   // [TRAKKR-NOTE] Needed for setFreeFont(&GFXfont) support
// (intentionally no SMOOTH_FONT)

// Optional performance tweaks (not applicable for parallel bus SPI clocks)
// #define SUPPORT_TRANSACTIONS
