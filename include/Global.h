#pragma once
#include <Arduino.h>

//
// [TRAKKR] Global settings (persisted via ESP32 NVS Preferences)
// [TRAKKR-NOTE] No ArduinoJson; fixed-size buffers; zero heap churn.
//
namespace Cfg {
  // --- Compile-time defaults (used if NVS blank) ---
  constexpr const char* DEF_WIFI_SSID    = "alterra";
  constexpr const char* DEF_WIFI_PASS    = "Hewer035!!";
  constexpr const char* DEF_DARWIN_TOKEN = "9a6c3c95-ca8e-411f-8d5b-f32564d0928d";
  constexpr const char* DEF_TFL_TOKEN    = "";       // none by default
  constexpr const char* DEF_WX_TOKEN     = "";       // none by default

  constexpr const char* DEF_MODE         = "departures";   // or "arrivals"
  constexpr const char* DEF_CRS          = "WAT";          // 3 letters
  constexpr uint32_t    DEF_TICKER_MS    = 7000;           // 7s

  // Control Panel (index.htm) defaults
  constexpr const char* DEF_SOURCE       = "rail";         // "rail" | "tube"
  constexpr const char* DEF_CALLING_AT   = "";             // comma separated
  constexpr bool        DEF_INCLUDE_BUS  = false;
  constexpr bool        DEF_INCLUDE_PASS = true;
  constexpr bool        DEF_SHOW_DATE    = true;
  constexpr bool        DEF_INCLUDE_WX   = false;
  constexpr bool        DEF_AUTO_UPDATE  = true;
  constexpr uint16_t    DEF_UPDATE_EVERY = 30;             // seconds (min 5)
  constexpr const char* DEF_SS_START     = "23:00";        // "HH:MM"
  constexpr const char* DEF_SS_END       = "06:00";        // "HH:MM"
  constexpr const char* DEF_TUBE_LINE    = "";             // e.g. "Victoria"
  constexpr const char* DEF_TUBE_DIR     = "";             // "inbound"/"outbound"/"northbound" etc.

  struct Settings {
    // Wi-Fi
    char     wifi_ssid[33];
    char     wifi_pass[65];

    // Tokens
    char     darwin_token[40];   // GUID + NUL
    char     tfl_token[72];      // TfL API token
    char     wx_token[56];       // OpenWeather token

    // NR/Ticker
    char     mode[12];           // "arrivals"/"departures"
    char     crs[4];             // 3 + NUL
    uint32_t ticker_ms;

    // Control Panel (index.htm)
    char     source[8];          // "rail"/"tube"
    char     calling_at[128];    // comma-separated filter
    bool     include_bus;
    bool     include_pass;
    bool     show_date;
    bool     include_weather;
    bool     auto_update;
    uint16_t update_every;       // seconds
    char     ss_start[6];        // HH:MM
    char     ss_end[6];          // HH:MM
    char     tube_line[28];
    char     tube_dir[16];
  };

  // Lifecycle
  void begin();
  const Settings& get();
  Settings& edit();

  // Accessors
  const char*  wifiSsid();
  const char*  wifiPass();
  const char*  darwinToken();
  const char*  tflToken();
  const char*  weatherToken();
  const char*  mode();
  const char*  crs();
  uint32_t     tickerMs();

  // Control Panel accessors (read)
  const char*  source();
  const char*  callingAt();
  bool         includeBus();
  bool         includePass();
  bool         showDate();
  bool         includeWeather();
  bool         autoUpdate();
  uint16_t     updateEvery();
  const char*  ssStart();
  const char*  ssEnd();
  const char*  tubeLine();
  const char*  tubeDir();

  // Setters (validate + persist)
  bool setWifi(const char* ssid, const char* pass);
  bool setDarwinToken(const char* token);
  bool setTflToken(const char* token);
  bool setWeatherToken(const char* token);
  bool setMode(const char* m);              // arrivals/departures
  bool setCRS(const char* three);           // 3 letters
  bool setTickerMs(uint32_t ms);

  // Control Panel setters
  bool setSource(const char* s);            // rail/tube
  bool setCallingAt(const char* list);      // arbitrary text (truncated)
  bool setIncludeBus(bool v);
  bool setIncludePass(bool v);
  bool setShowDate(bool v);
  bool setIncludeWeather(bool v);
  bool setAutoUpdate(bool v);
  bool setUpdateEvery(uint16_t sec);        // clamp ≥5
  bool setScreensaver(const char* startHHMM, const char* endHHMM); // "HH:MM"
  bool setTubeLine(const char* line);
  bool setTubeDir(const char* dir);

  // Bulk persist / reset
  bool save();
  void resetToDefaults();
} // namespace Cfg
