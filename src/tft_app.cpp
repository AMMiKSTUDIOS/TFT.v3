// src/tft_app.cpp — ESP32 + ILI9488 (TFT_eSPI) — Darwin OpenLDBWS (SOAP 1.2)
// Minimal, flicker-free boot; persistent header; reliable first paint.
// + Perf/health beacons: heap snapshots, fragmentation guard, timing scopes.
// + File-backed endless ticker ribbon (LittleFS by default) to keep heap flat.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <vector>
#include <time.h>

#include "TFT.h"
#include "NationalRail.h"   // [TRAKKR] Use NationalRailTiny everywhere

// [TRAKKR-NOTE] Explicit FreeRTOS includes not required on ESP32 Arduino;
// Arduino.h pulls them in for us. We still use FreeRTOS APIs (tasks, semaphores).

// === [TRAKKR][BEGIN:fetch_guard] ===
static SemaphoreHandle_t gFetchSem = nullptr;
static uint32_t gLastFetchMs = 0;
static bool beginFetchGuard(uint32_t debounceMs = 800){
  if (!gFetchSem){
    gFetchSem = xSemaphoreCreateBinary();
    if (gFetchSem) xSemaphoreGive(gFetchSem);
  }
  if (!gFetchSem) return true;
  const uint32_t now = millis();
  if (now - gLastFetchMs < debounceMs) return false;
  if (xSemaphoreTake(gFetchSem, 0) != pdTRUE) return false;
  gLastFetchMs = now;
  return true;
}
static void endFetchGuard(){ if (gFetchSem) xSemaphoreGive(gFetchSem); }
struct FetchScope { bool active; explicit FetchScope(bool ok):active(ok){} ~FetchScope(){ if(active) endFetchGuard(); } };
// === [TRAKKR][END:fetch_guard] ===

static SemaphoreHandle_t gTftMutex = nullptr;
static void drawTicker_FS();
static void tickerTask(void*){
  for(;;){
    if (xSemaphoreTake(gTftMutex, portMAX_DELAY) == pdTRUE){
      drawTicker_FS();
      xSemaphoreGive(gTftMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(33)); // ~30 fps
  }
}

// ===== FILESYSTEM SELECT =====
#define USE_SD_TICKER 0
#if USE_SD_TICKER
  #include <FS.h>
  #include <SD.h>
  #define FSNS SD
  static bool fsBegin(){ return SD.begin(); }
  static const char* kFSName = "SD";
#else
  #include <FS.h>
  #include <LittleFS.h>
  #define FSNS LittleFS
  static bool fsBegin(){ return LittleFS.begin(true); } // format on fail
  static const char* kFSName = "LittleFS";
#endif

// ===== PERFORMANCE / HEALTH =====
#include <esp_heap_caps.h>
#include <esp_system.h>
static const bool     PERF_VERBOSE           = true;
static const uint32_t PERF_PERIOD_MS         = 15000;
static const size_t   PERF_WARN_LARGEST_MIN  = 12 * 1024;
static void logMem(const char* where){
  if (!PERF_VERBOSE) return;
  Serial.printf("[MEM] %-18s | heap: free=%uB min=%uB largest=%uB | psram: free=%uB min=%uB largest=%uB\n",
                where, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getMinFreePsram(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
static bool checkHeap(const char* where){
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest < PERF_WARN_LARGEST_MIN){
    Serial.printf("[MEM][WARN] Largest 8-bit block low at %-18s => %uB (< %uB)\n",
                  where, (unsigned)largest, (unsigned)PERF_WARN_LARGEST_MIN);
    return false;
  }
  return true;
}
struct ScopeTimer { const char* n; uint32_t t0; ScopeTimer(const char* s):n(s),t0(millis()){} ~ScopeTimer(){ if(PERF_VERBOSE) Serial.printf("[TIME] %-18s %lums\n", n, (unsigned long)(millis()-t0)); } };

// ===== CONFIG =====
static const char* WIFI_SSID   = "alterra";
static const char* WIFI_PASS   = "Hewer035!!";
static const char* DARWIN_HOST = "lite.realtime.nationalrail.co.uk";
static const char* DARWIN_PATH = "/OpenLDBWS/ldb9.asmx";
static const char* DARWIN_TOKEN= "9a6c3c95-ca8e-411f-8d5b-f32564d0928d";

static const char* SOAP12_NS   = "http://www.w3.org/2003/05/soap-envelope";
static const char* LDB_NS      = "http://thalesgroup.com/RTTI/2016-02-16/ldb/";
static const char* TOK_NS      = "http://thalesgroup.com/RTTI/2013-11-28/Token/types";

// [TRAKKR] Switch to arrivals mode as requested
static const char* MODE = "departures";    // "arrivals" or "departures"
static const char* CRS  = "TAM";
static const int   ROWS = 8;
static const int   TIME_WINDOW_MINS = 120;

static const uint32_t POLL_MS_OK  = 15000;
static const uint32_t POLL_MS_ERR = 2000;
static const uint32_t TICKER_MS   = 7000;

static const bool   DEBUG_NET       = true;
static const bool   DEBUG_BODY_SNIP = false;
static const size_t BODY_SNIP_N     = 700;

static const char* POWERED_MSG = "Powered by National Rail";

// ===== LAYOUT =====
static const int W=480, H=320, PAD=8;
static const int HEADER_H=48, COLBAR_H=32;
static const int COLBAR_Y=HEADER_H, ROW_TOP=COLBAR_Y+COLBAR_H;
static const int X_STD=PAD, X_TO=55, X_ETD=245, X_PLAT=310, X_OPER=335;
static const int CH_TIME=5, CH_TO=28, CH_ETD=10, CH_PLAT=3, CH_OPER=21;
static const int  TICKER_H=28, TICKER_SPEED=2;
static const int  ROW_VPAD = 6; 

// ===== STATE =====
struct Svc { String time, place, est, plat, oper; };
std::vector<Svc>   services;
std::vector<String> nrccMsgs;

static String stationTitle="Board";
static uint32_t nextPoll=0, nextClockTick=0;
static uint32_t nextPerfBeat=0;

// header metrics (using NationalRailTiny)
static int  clockX=0, clockBaseY=28, clockBoxX=0, clockBoxY=0, clockBoxW=0, clockBoxH=0;
static char lastClock[16] = {0};
static int bootX=0, bootY=0, bootW=300, bootH=110;

// ===== COLOURS =====
static uint16_t bodyBg()   { return tft.color565(0x0b,0x10,0x20); }
static uint16_t headBg()   { return tft.color565(0x13,0x1a,0x33); }
static uint16_t headBr()   { return tft.color565(0x24,0x30,0x59); }
static uint16_t rowAlt()   { return tft.color565(0x0d,0x12,0x30); }
static uint16_t warnCol()  { return tft.color565(0xff,0xd1,0x66); }
static uint16_t badCol()   { return tft.color565(0xff,0x5d,0x5d); }

// ===== UTILS =====
static String ellipsize(const String& s, int m){ if((int)s.length()<=m) return s; if(m<=1) return "…"; return s.substring(0,m-1)+"…"; }
static void ensureWiFi(){
  if (WiFi.status()==WL_CONNECTED) return;
  ScopeTimer T("WiFi connect");
  logMem("pre-WiFi");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0=millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-t0<15000){ delay(200); Serial.print('.'); }
  Serial.println();
  logMem("post-WiFi");
  checkHeap("post-WiFi");
}

// [TRAKKR] Keep only the first sentence of an NRCC message.
static String keepFirstSentence(const String& in){
  int idx = in.indexOf('.');
  if (idx < 0) return in;
  String out = in.substring(0, idx + 1);
  out.trim();
  return out;
}

// [TRAKKR] Draw legible text: 1-px drop shadow (no background fill).
static void drawShadowed(const String& s, int x, int y, uint16_t fg, uint8_t datum){
  tft.setTextDatum(datum);
  tft.setTextColor(TFT_BLACK);  tft.drawString(s, x+1, y+1);   // shadow
  tft.setTextColor(fg);         tft.drawString(s, x,   y);     // main
  tft.setTextDatum(TL_DATUM);
}

// ===== BOOT BOX =====
static void bootInit(){
  bootW=300; bootH=110; bootX=(W-bootW)/2; bootY=(H-bootH)/2;
  tft.fillScreen(bodyBg());
  tft.fillRect(0,0,W,HEADER_H, headBg()); tft.drawRect(0,0,W,HEADER_H, headBr());
}
static void bootShow(const char* line){
  uint16_t panel = tft.color565(0x0d,0x12,0x30);
  tft.fillRect(bootX,bootY,bootW,bootH,panel);
  tft.drawRect(bootX,bootY,bootW,bootH, headBr());
  tft.setFreeFont(&NationalRailTiny);
  tft.setTextColor(TFT_WHITE,panel);
  tft.setCursor(bootX+12, bootY+28);
  tft.print(line?line:"");
}
static void bootHide(){ tft.fillRect(0, HEADER_H, W, H-HEADER_H, bodyBg()); }

// ===== CLOCK =====
static void setupClockTZ(){
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2",
               "pool.ntp.org","time.google.com","time.cloudflare.com");
}
static bool timeValid(){ time_t t=time(nullptr); struct tm tm{}; localtime_r(&t,&tm); return tm.tm_year>=120; }
static bool waitForTime(uint32_t ms=6000){ ScopeTimer T("NTP settle"); uint32_t t0=millis(); while(!timeValid() && millis()-t0<ms){ delay(100);} return timeValid(); }
static void nowHHMM(char* out,size_t n){ if(!timeValid()){ strncpy(out,"--:--",n); return; } time_t t=time(nullptr); struct tm tm{}; localtime_r(&t,&tm); strftime(out,n,"%H:%M",&tm); }
static void headerInit(){
  tft.setFreeFont(&NationalRailSmall);
  int ww = tft.textWidth("88:88");
  int hh = tft.fontHeight();
  if (ww <= 0) ww = 60; if (hh <= 0) hh = 16;
  clockX = W - PAD - ww;
  int topPad = (HEADER_H - hh) / 2;
  clockBoxX  = clockX - 3; clockBoxY=topPad - 2; clockBoxW=ww + 10; clockBoxH=hh + 4;
}

// [TRAKKR] Draw clock with the SAME vertical centring as the title
static void drawClockIfChanged(){
  static char last[8] = "";
  char buf[8]; nowHHMM(buf, sizeof(buf));
  if (strcmp(buf,last)==0) return;

  tft.setFreeFont(&NationalRailSmall);
  // Compute the header’s top for this font and use TL_DATUM at that Y (no baseline fudge)
  int fh = (int)tft.fontHeight(); if (fh < 16) fh = 16;
  const int yTop = (HEADER_H - fh) / 2;

  // Clear the clock box area then redraw
  tft.fillRect(clockBoxX, clockBoxY, clockBoxW, clockBoxH, headBg());

  // Small left padding inside the box; align vertically with header mid
  const int xPad = 7;
  drawShadowed(String(buf), clockBoxX + xPad, yTop, TFT_WHITE, TL_DATUM);

  strncpy(last,buf,sizeof(last)-1); last[sizeof(last)-1]='\0';
}

static void scheduleNextMinute(){
  if(!timeValid()){ nextClockTick=millis()+1000; return; }
  time_t t=time(nullptr); struct tm tm{}; localtime_r(&t,&tm);
  uint32_t ms=(60-tm.tm_sec)*1000u; nextClockTick=millis()+(ms?ms:60000);
}

// ===== HEADER TITLE =====
static void setTitle(const String& station){
  // [TRAKKR] Centre the title vertically in the header so it lines up with the clock
  tft.setFreeFont(&NationalRailSmall);
  int fh = (int)tft.fontHeight(); if (fh < 16) fh = 16;
  const int yTop  = (HEADER_H - fh) / 2;  // midway between top and base

  // [TRAKKR] Clear the title band up to just before the clock box
  const int clearX = 1;
  const int clearY = 1;
  const int stopX  = (clockBoxX > 0 ? clockBoxX : (W - PAD - 60));
  const int clearW = ((stopX - 3 - clearX) > 0) ? (stopX - 3 - clearX) : 0;
  const int clearH = HEADER_H - 2;
  tft.fillRect(clearX, clearY, clearW, clearH, headBg());

  // Compose and trim to fit available pixels
  String want = station + String(" ") + (MODE[0]=='a' ? "Arrivals" : "Departures");
  const int maxPx = ((stopX - PAD - 6) > 20) ? (stopX - PAD - 6) : 20;

  String out = want;
  while (out.length() && tft.textWidth(out + "…") > maxPx) out.remove(out.length()-1);
  if (out.length() && out != want) out += "…";

  // Draw title using the same vertical reference as the clock
  drawShadowed(out, PAD, yTop, TFT_WHITE, TL_DATUM);
}





// ===== COLUMNS & ROWS =====
static const int rowH=26;
static void drawColHeader(){
  uint16_t bg=rowAlt();
  tft.fillRect(0, COLBAR_Y, W, COLBAR_H, bg);
  tft.setFreeFont(&NationalRailTiny);
  int y = COLBAR_Y + COLBAR_H/2;
  drawShadowed("STA",      X_STD,  y, tft.color565(0x9f,0xb3,0xff), ML_DATUM);
  drawShadowed("From",     X_TO,   y, tft.color565(0x9f,0xb3,0xff), ML_DATUM);
  drawShadowed("ETA",      X_ETD,  y, tft.color565(0x9f,0xb3,0xff), ML_DATUM);
  drawShadowed("Plt",      X_PLAT, y, tft.color565(0x9f,0xb3,0xff), ML_DATUM);
  drawShadowed("Operator", X_OPER, y, tft.color565(0x9f,0xb3,0xff), ML_DATUM);
}
static String normalizeOper(String op){
  op.trim();
  if (op == "London North Eastern Railway")   return "LNER";
  if (op == "Great Western Railway")          return "Great Western";
  if (op == "West Midlands Trains")           return "West Midlands";
  return op;
}

static void drawRows() {
  ScopeTimer T("drawRows");

  // [TRAKKR] Compute dynamic row height for 7 rows, padded so descenders aren't clipped
  tft.setFreeFont(&NationalRailTiny);
  // ADD/REPLACE the row-height maths below

  // REPLACE these original lines:
  //   const int fh      = tft.fontHeight();
  //   const int autoH   = (H - ROW_TOP - TICKER_H) / max(1, ROWS);
  //   const int rowH    = max(fh + ROW_VPAD, autoH);
  //   const int maxVis  = min(ROWS, (H - ROW_TOP - TICKER_H) / rowH);
  //   const int painted = min((int)services.size(), maxVis);

  // WITH THIS:
  const int fh = (int)tft.fontHeight();

  // Available height for rows
  const int availH  = H - ROW_TOP - TICKER_H;
  const int autoH   = availH / max(1, ROWS);

  // [TRAKKR] Tight spacing caps:
  // - Keep at least 3px padding above & below glyph box (no clipped heads/tails).
  // - Avoid stretched rows by capping extra leading to ~8px total.
  const int minRowH = fh + 6;   // safe minimum (3px top + 3px bottom)
  const int maxRowH = fh + 8;   // compact target; allows a touch of breathing room

  int rowH = autoH;
  if (rowH > maxRowH) rowH = maxRowH;
  if (rowH < minRowH) rowH = minRowH;

  // How many rows we can actually paint
  const int maxVis  = min(ROWS, availH / rowH);
  const int painted = min((int)services.size(), maxVis);

  // [TRAKKR-NOTE] Keep your existing loop/body that fills backgrounds and draws text.
  // The usual "by" calculation still works; no change needed:
  //   int by = ROW_TOP + i*rowH + rowH/2;

  for (int i = 0; i < painted; i++) {
    const auto& s = services[i];
    uint16_t bg   = (i % 2 == 0) ? bodyBg() : rowAlt();

    // clear row background
    tft.fillRect(0, ROW_TOP + i*rowH, W, rowH, bg);

    // vertical middle of row for ML_DATUM text baseline
    int by = ROW_TOP + i*rowH + rowH/2;

    // --- STA / From ---
    drawShadowed(ellipsize(s.time,  CH_TIME),  X_STD,  by, TFT_YELLOW, ML_DATUM);

    // Place column
    drawShadowed(ellipsize(s.place, CH_TO),    X_TO,   by, TFT_WHITE,  ML_DATUM);

    // --- ETA (warn/cancel colouring) ---
    String low = s.est; low.toLowerCase();
    uint16_t c = TFT_WHITE;
    if (low.indexOf("cancel") >= 0 || low.indexOf("delay") >= 0) c = badCol();
    else if (low.indexOf("late") >= 0 || low.indexOf(':') >= 0)   c = warnCol();
    drawShadowed(ellipsize(s.est, CH_ETD),     X_ETD,  by, c,       ML_DATUM);

    // --- Plat / Operator ---
    drawShadowed(ellipsize(s.plat, CH_PLAT),   X_PLAT, by, TFT_WHITE, ML_DATUM);
    drawShadowed(ellipsize(s.oper, CH_OPER),   X_OPER, by, TFT_WHITE, ML_DATUM);
  }

  // clear any unpainted rows
  if (painted < maxVis) {
    int y = ROW_TOP + painted*rowH;
    int h = (maxVis - painted)*rowH;
    tft.fillRect(0, y, W, h, bodyBg());
  }

  checkHeap("after drawRows");
}

// ===== TICKER =====
static const char* kTickerPath = "/ticker.txt";
static const char* kMetaPath   = "/ticker.meta";
static const char* kSep        = "   |   ";

static TFT_eSprite tickSpr(&tft);
static File        tickFile;
static size_t      tickSize = 0;
static size_t fileOffset = 0;
static int    scrollPx   = 0;
static volatile bool gTickerHasNRCC    = false;
static volatile bool gTickerStaticDirty= true;

static inline void tickerSetHasNRCC(bool has){
  if (gTickerHasNRCC != has){
    gTickerHasNRCC = has;
    gTickerStaticDirty = true;
    if (has){ scrollPx = 0; }
  }
}

static uint32_t fnv1a32(const uint8_t* d, size_t n, uint32_t h=2166136261u){ for(size_t i=0;i<n;i++){ h^=d[i]; h*=16777619u; } return h; }
static uint32_t hashMessages(const std::vector<String>& msgs){
  uint32_t h=2166136261u;
  for (auto &m : msgs){ h = fnv1a32((const uint8_t*)m.c_str(), m.length(), h); h = fnv1a32((const uint8_t*)kSep, strlen(kSep), h); }
  const char* tail = POWERED_MSG; h = fnv1a32((const uint8_t*)tail, strlen(tail), h); h = fnv1a32((const uint8_t*)kSep, strlen(kSep), h);
  return h;
}
static bool readMeta(uint32_t& out){ File f=FSNS.open(kMetaPath,"r"); if(!f) return false; uint32_t v=0; int n=f.read((uint8_t*)&v,sizeof(v)); f.close(); if(n!=(int)sizeof(v)) return false; out=v; return true; }
static void writeMeta(uint32_t v){ File f=FSNS.open(kMetaPath,"w"); if(!f) return; f.write((const uint8_t*)&v,sizeof(v)); f.close(); }

static bool writeTickerFileIfChanged(){
  // Build message list (trim empties)
  std::vector<String> list;
  list.reserve(nrccMsgs.size()+1);
  for (auto &m : nrccMsgs){ String v=m; v.trim(); if (v.length()) list.push_back(v); }
  list.push_back(String(POWERED_MSG));

  uint32_t want = hashMessages(list);
  uint32_t have = 0; bool haveMeta = readMeta(have);
  if (haveMeta && have==want){
    if (DEBUG_NET) Serial.println("[TICK] content unchanged; not rewriting file");
    return false;
  }

  // Write to temp → rename for atomicity
  File tmp = FSNS.open("/ticker.tmp", "w");
  if (!tmp){ Serial.println("[TICK][ERR] open tmp failed"); return false; }

  const size_t BUF = 512;
  char buf[BUF];
  for (size_t i=0;i<list.size();++i){
    String s = list[i];
    // Normalize whitespace a bit
    for (int p=0; p+1<(int)s.length();){ if (s[p]==' ' && s[p+1]==' ') s.remove(p,1); else ++p; }
    s.trim();
    // Write chunked to avoid large temporary allocations
    size_t off=0, n=s.length();
    while (off<n){
      size_t take = min(BUF, n-off);
      memcpy(buf, s.c_str()+off, take);
      tmp.write((const uint8_t*)buf, take); off+=take;
    }
    tmp.write((const uint8_t*)kSep, strlen(kSep));
  }
  tmp.flush(); tmp.close();

  FSNS.remove(kTickerPath);
  if (!FSNS.rename("/ticker.tmp", kTickerPath)){
    Serial.println("[TICK][ERR] rename failed");
    return false;
  }
  writeMeta(want);
  if (DEBUG_NET) Serial.println("[TICK] ticker.txt rewritten");
  return true;
}

static bool openTicker(){ if(tickFile) tickFile.close(); tickFile=FSNS.open(kTickerPath,"r"); if(!tickFile){ Serial.println("[TICK][ERR] open ticker.txt failed"); return false; } tickSize=tickFile.size(); return true; }
static int  readByteAt(size_t off){ if(!tickFile||!tickSize) return -1; off%=tickSize; tickFile.seek(off); return tickFile.read(); }

// -------------------- TICKER RENDERER --------------------
static void drawTicker_FS(){
  // [TRAKKR] Bottom ticker band
  const int y        = H - TICKER_H;
  const int availPx  = W - 2*PAD;

  // Font + vertical placement
  tickSpr.setFreeFont(&NationalRailTiny);
  const int fh    = (int)tickSpr.fontHeight() > 0 ? (int)tickSpr.fontHeight() : 1;
  const int baseY = TICKER_H - 2;                         // ~2 px above bottom edge (baseline)

  // -------------------- STATIC MODE (no NRCC) --------------------
  if (!gTickerHasNRCC){
    if (gTickerStaticDirty){
      tickSpr.fillSprite(headBg());
      tickSpr.setTextWrap(false);
      tickSpr.setTextDatum(MC_DATUM);
      // light shadow for legibility
      tickSpr.setTextColor(TFT_BLACK, headBg());  tickSpr.drawString(POWERED_MSG, W/2+1, TICKER_H/2+1);
      tickSpr.setTextColor(TFT_WHITE, headBg());  tickSpr.drawString(POWERED_MSG, W/2,   TICKER_H/2);
      gTickerStaticDirty = false;
    }
    tickSpr.pushSprite(0, y);
    tft.drawRect(0, y, W, TICKER_H, headBr());
    return;
  }

  // -------------------- SCROLLING MODE (NRCC) --------------------
  // Cache: raw buffer (with '|'), rendered text (without '|'), pixel width, and separator positions in pixels.
  static String sBuf;             // raw NRCC text (with '|')
  static String sRender;          // sBuf with '|' stripped (what we actually draw as text)
  static int    sRenderPx = 0;    // pixel width of sRender
  static std::vector<int> sSepPx; // pixel offsets of each '|' relative to start of sRender
  static bool   sInit = false;

  if (gTickerStaticDirty || !sInit){
    // (1) Read full ticker file to RAM (once per content change)
    sBuf = "";
    sBuf.reserve(2048);
    if (tickFile){
      tickFile.seek(0);
      while (tickFile.available()){
        char c = (char)tickFile.read();
        if (c == '\r' || c == '\n') c = ' ';
        sBuf += c;
      }
    }
    if (sBuf.length() == 0) sBuf = POWERED_MSG;

    // Repeat short strings so we always have enough width to tile smoothly
    if (sBuf.length() < 64) sBuf += String("   |   ") + sBuf;

    // (2) Build the render string (no pipes) and measure once
    sRender = sBuf; sRender.replace("|", "");
    tickSpr.setTextDatum(BL_DATUM);
    tickSpr.setTextWrap(false);
    sRenderPx = tickSpr.textWidth(sRender);
    if (sRenderPx <= 0) sRenderPx = 1;

    // (3) Precompute pixel positions for each '|' separator (for bullets)
    sSepPx.clear();
    int searchFrom = 0;
    while (true){
      int idx = sBuf.indexOf('|', searchFrom);
      if (idx < 0) break;
      searchFrom = idx + 1;
      String upTo = sBuf.substring(0, idx); upTo.replace("|", "");
      int px = tickSpr.textWidth(upTo);
      sSepPx.push_back(px);
    }

    // Reset scroll position
    scrollPx = 0;
    gTickerStaticDirty = false;
    sInit = true;
  }

  // (4) Compose the sprite for this frame (no re-layout; just tile by pixels)
  tickSpr.fillSprite(headBg());
  tickSpr.setTextDatum(BL_DATUM);
  tickSpr.setTextWrap(false);

  // Pixel-scrolling only: wrap by the render width
  const int modScroll = (sRenderPx > 0) ? (scrollPx % sRenderPx) : 0;
  const int x0 = PAD - modScroll;

  // Draw the text strip enough times to fully cover the band width (shadow + main)
  for (int tileX = x0; tileX < PAD + availPx; tileX += sRenderPx){
    tickSpr.setTextColor(TFT_BLACK, headBg()); tickSpr.drawString(sRender, tileX+1, baseY+1);
    tickSpr.setTextColor(TFT_WHITE, headBg()); tickSpr.drawString(sRender, tileX,   baseY);
  }

  // Draw bullets at each separator position, tiled the same way
  auto drawDiamondAt = [&](int px){
    // [TRAKKR] Smaller diamond and real spacing (gap) either side
    const int sz  = max(5, min(9, fh - 7));           // size
    const int pad = max(3, min(8, (fh/5) + 2));       // horizontal gap either side
    const int cx  = px;
    const int cy  = TICKER_H / 2;

    // Clear a narrow band behind the diamond to produce visible spacing
    int clearW = sz + 2 * pad;
    int clearH = fh + 6;
    int clearX = cx - clearW / 2;
    int clearY = (TICKER_H - clearH) / 2;
    if (clearY < 0) clearY = 0;
    if (clearX < 0) { clearW += clearX; clearX = 0; }
    if (clearX + clearW > W) clearW = W - clearX;
    if (clearW > 0) tickSpr.fillRect(clearX, clearY, clearW, clearH, headBg());

    // Draw the diamond
    tickSpr.fillTriangle(cx, cy - sz/2,  cx - sz/2, cy,  cx, cy + sz/2, TFT_WHITE);
    tickSpr.fillTriangle(cx, cy - sz/2,  cx + sz/2, cy,  cx, cy + sz/2, TFT_WHITE);
  };

  for (int k = 0; k < 3; ++k){ // usually 2 tiles suffice; 3 is safe
    const int tileBase = x0 + k * sRenderPx;
    if (tileBase > PAD + availPx) break;
    for (int px : sSepPx){
      int iconX = tileBase + px;
      if (iconX >= PAD && iconX < PAD + availPx) drawDiamondAt(iconX);
    }
  }

  // Push to screen and outline the band
  tickSpr.pushSprite(0, y);
  tft.drawRect(0, y, W, TICKER_H, headBr());

  // (5) Advance the smooth pixel scroll
  scrollPx += TICKER_SPEED;
  if (scrollPx >= sRenderPx) scrollPx -= sRenderPx;
}

static void tickerRefreshFilesAndOpen(){
  bool changed = writeTickerFileIfChanged();
  if (!tickFile || changed){ if(tickFile) tickFile.close(); if(openTicker()){ tickSize=tickFile.size(); fileOffset=0; scrollPx=0; } }
}

// ===== SOAP/XML HELPERS =====
static String get1ns(const String& xml, const String& tag, int from=0){
  for(int pos=from;;){
    int a=xml.indexOf('<',pos); if(a<0) break;
    int b=xml.indexOf('>',a+1); if(b<0) break;
    String head=xml.substring(a+1,b);
    int sp=head.indexOf(' '); if(sp>=0) head=head.substring(0,sp);
    int col=head.indexOf(':'); String bare=(col>=0)?head.substring(col+1):head;
    if (bare==tag){
      String pref=(col>=0)?head.substring(0,col+1):"";
      String close="</"+pref+tag+">";
      int cend=xml.indexOf(close,b+1); if(cend>=0) return xml.substring(b+1,cend);
    }
    pos=b+1;
  }
  return "";
}
static bool nextTagNS(const String& xml, const String& tag, int& pos, String& inner){
  const int N=xml.length();
  for(int i=pos;i<N;){
    int a=xml.indexOf('<',i); if(a<0) return false;
    if (a+1<N && xml[a+1]=='/'){ i=a+1; continue; }
    int b=xml.indexOf('>',a+1); if(b<0) return false;
    String head=xml.substring(a+1,b);
    int sp=head.indexOf(' '); if(sp>=0) head=head.substring(0,sp);
    int col=head.indexOf(':'); String bare=(col>=0)?head.substring(col+1):head;
    if (bare==tag){
      String pref=(col>=0)?head.substring(0,col+1):"";
      String close="</"+pref+tag+">";
      int cend=xml.indexOf(close,b+1); if(cend<0){ i=b+1; continue; }
      inner=xml.substring(b+1,cend); pos=cend+close.length(); return true;
    }
    i=b+1;
  }
  return false;
}

// ===== SOAP POST / FETCH / PARSE =====
static bool postSoapOnce(String& outBody, int& outCode, const char* method, const char* reqTag){
  ScopeTimer T("HTTP POST+recv"); logMem("pre-POST");
  String soap;
  soap.reserve(1600);
  soap += "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
          "<soap:Envelope xmlns:soap=\""; soap+=SOAP12_NS; soap+="\""
          " xmlns:typ=\""; soap+=TOK_NS; soap+="\""
          " xmlns:ldb=\""; soap+=LDB_NS; soap+="\">"
          "<soap:Header><typ:AccessToken><typ:TokenValue>";
  soap += DARWIN_TOKEN;
  soap += "</typ:TokenValue></typ:AccessToken></soap:Header>"
          "<soap:Body><ldb:"; soap+=reqTag; soap+=">"
          "<ldb:numRows>"; soap+=String(ROWS); soap+="</ldb:numRows>"
          "<ldb:crs>"; soap+=CRS; soap+="</ldb:crs>"
          "<ldb:timeOffset>0</ldb:timeOffset>"
          "<ldb:timeWindow>"; soap+=String(TIME_WINDOW_MINS); soap+="</ldb:timeWindow>"
          "</ldb:"; soap+=reqTag; soap+=">"
          "</soap:Body></soap:Envelope>";

  WiFiClientSecure client; client.setInsecure(); client.setTimeout(12000);
  HTTPClient http; http.setReuse(false);
  String url=String("https://")+DARWIN_HOST+DARWIN_PATH;
  http.setConnectTimeout(12000);
  if(!http.begin(client,url)){ outCode=-1; Serial.println("[NET] http.begin() failed"); checkHeap("http.begin fail"); return false; }

  String action=String(LDB_NS)+method;
  http.addHeader("Content-Type", String("application/soap+xml; charset=utf-8; action=\"")+action+"\"");
  http.addHeader("Accept","text/xml"); http.addHeader("Connection","close");

  if (DEBUG_NET){
    Serial.println("\n===== Darwin POST =====");
    Serial.printf("Method: %s  CRS:%s  Rows:%d\n", method, CRS, ROWS);
  }

  outCode=http.POST((uint8_t*)soap.c_str(), soap.length());
  outBody=http.getString(); http.end();

  if (DEBUG_NET){ Serial.printf("[NET] HTTP %d  body=%uB\n", outCode, (unsigned)outBody.length()); }
  logMem("post-POST"); checkHeap("post-POST");
  return outCode==200;
}
static String extractFault(const String& body){
  String s=get1ns(body,"faultstring"); if(s.length()) return s;
  String reason=get1ns(body,"Reason"); String text=get1ns(reason,"Text");
  return text;
}
static bool fetchDarwinBoard(){
  bool okToRun = beginFetchGuard(800);
  if (!okToRun) return false;
  FetchScope guard(okToRun);

  ScopeTimer T("fetch+parse");
  services.clear(); nrccMsgs.clear();
  const bool dep = (MODE[0] != 'a');
  const char* method = dep ? "GetDepartureBoard"        : "GetArrivalBoard";
  const char* reqTag = dep ? "GetDepartureBoardRequest" : "GetArrivalBoardRequest";

  String body; int code=0;
  { ScopeTimer Tpost("SOAP roundtrip"); if (!postSoapOnce(body, code, method, reqTag)){
      String fault = extractFault(body);
      if (DEBUG_NET){ Serial.printf("[SOAP] FAIL code=%d fault=\"%s\"\n", code, fault.c_str()); }
      return false;
    } }

  { ScopeTimer Tparse("parse XML");
    String loc = get1ns(body,"locationName"); stationTitle = loc.length()?loc:String(CRS);

    String ts = get1ns(body,"trainServices");
    if (ts.length()){
      int pos=0; String svc;
      while ((int)services.size() < ROWS && nextTagNS(ts,"service",pos,svc)){
        Svc v;
        v.time = get1ns(svc, dep ? "std" : "sta");
        v.est  = get1ns(svc, dep ? "etd" : "eta"); if(!v.est.length()) v.est="On time";
        v.plat = get1ns(svc,"platform");
        v.oper = normalizeOper(get1ns(svc,"operator"));
        String endBlk = get1ns(svc, dep ? "destination" : "origin");
        String first  = get1ns(endBlk,"location");
        v.place = get1ns(first,"locationName");
        if (v.time.length() || v.place.length()) services.push_back(v);
      }
    }

    String ms = get1ns(body, "nrccMessages");
    if (ms.length()){
      int pos = 0; String inner;
      while (nextTagNS(ms, "message", pos, inner)){
        String txt = get1ns(inner,"text"); if (!txt.length()) txt = inner;
        // HTML entity unescape
        txt.replace("&nbsp;"," "); txt.replace("&amp;","&"); txt.replace("&lt;","<");
        txt.replace("&gt;",">"); txt.replace("&quot;","\""); txt.replace("&apos;","'");
        // Strip any HTML tags
        for (;;){
          int lt=txt.indexOf('<'); if(lt<0) break;
          int gt=txt.indexOf('>',lt+1); if(gt<0){ txt.remove(lt); break;}
          txt.remove(lt,gt-lt+1);
        }
        // Collapse spaces and trim
        for (int i=0;i+1<(int)txt.length();){ if (txt[i]==' ' && txt[i+1]==' ') txt.remove(i,1); else ++i; }
        txt.trim();

        // [TRAKKR] Keep only the first sentence
        txt = keepFirstSentence(txt);

        if (txt.length()) nrccMsgs.push_back(txt);
      }
    }
  }

  tickerSetHasNRCC(!nrccMsgs.empty());
  tickerRefreshFilesAndOpen();

  if (DEBUG_NET){ Serial.printf("[PARSE] %s  services=%u  nrcc=%u\n",
    stationTitle.c_str(), (unsigned)services.size(), (unsigned)nrccMsgs.size()); }
  return true;
}

// ===== APP SETUP / LOOP =====
static void app_setup_impl(){
  Serial.begin(115200); delay(30);
  Serial.println("\n[BOOT] tft_app starting…");
  logMem("boot");

  tft.init();
  tft.setRotation(1);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&NationalRailTiny);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  bootInit();
  headerInit();
  setTitle(String(CRS));

  bootShow("Mounting FS…");
  if (!fsBegin()) { bootShow("FS mount failed"); Serial.printf("[FS][ERR] %s mount failed\n", kFSName); }
  else { Serial.printf("[FS] %s mounted\n", kFSName); }

  bootShow("Connecting to Wi-Fi…");
  ensureWiFi();
  bootShow(WiFi.status()==WL_CONNECTED ? "Wi-Fi connected" : "Wi-Fi failed");

  bootShow("Setting time…");
  setupClockTZ(); waitForTime(6000); drawClockIfChanged(); scheduleNextMinute();

  { size_t before=ESP.getFreeHeap();
    tickSpr.setColorDepth(16);
    bool ok = tickSpr.createSprite(W, TICKER_H);
    size_t after=ESP.getFreeHeap();
    Serial.printf("[SPRITE] ticker %s | size=%dx%d x 2Bpp ~= %uB | heap delta=%ldB\n",
                  ok?"OK":"FAIL", W, TICKER_H, (unsigned)(W*TICKER_H*2), (long)(after-before));
    checkHeap("after sprite alloc");
  }

  bootShow("Loading station data…");
  bool okFetch = (WiFi.status()==WL_CONNECTED) ? fetchDarwinBoard() : false;
  if (!tickFile) openTicker();
  bootHide();

  if (!gTftMutex) gTftMutex = xSemaphoreCreateMutex();
  if (xSemaphoreTake(gTftMutex, pdMS_TO_TICKS(200))){
    ScopeTimer Tpaint("first paint");
    setTitle(stationTitle);
    tft.fillRect(0, HEADER_H, W, H-HEADER_H, bodyBg());
    drawColHeader();
    drawRows();
    xSemaphoreGive(gTftMutex);
  }

  xTaskCreatePinnedToCore(tickerTask, "ticker", 4096, nullptr, 1, nullptr, 1);
  nextPoll     = millis() + (okFetch ? POLL_MS_OK : POLL_MS_ERR);
  nextPerfBeat = millis() + PERF_PERIOD_MS;

  logMem("after first paint");
  Serial.println("[BOOT] setup complete.");
}

static void app_loop_impl(){
  uint32_t now = millis();
  if (PERF_VERBOSE && now >= nextPerfBeat){ logMem("heartbeat"); checkHeap("heartbeat"); nextPerfBeat = now + PERF_PERIOD_MS; }

  if (now >= nextPoll){
    ScopeTimer Tp("poll+repaint");
    ensureWiFi();
    bool ok = (WiFi.status()==WL_CONNECTED) ? fetchDarwinBoard() : false;
    if (xSemaphoreTake(gTftMutex, portMAX_DELAY) == pdTRUE){
      setTitle(stationTitle);
      drawColHeader();
      drawRows();
      xSemaphoreGive(gTftMutex);
    }
    nextPoll = now + (ok ? POLL_MS_OK : POLL_MS_ERR);
    logMem(ok ? "post-poll OK" : "post-poll ERR");
  }

  if (now >= nextClockTick){
    if (xSemaphoreTake(gTftMutex, pdMS_TO_TICKS(50)) == pdTRUE){
      drawClockIfChanged();
      xSemaphoreGive(gTftMutex);
    }
    scheduleNextMinute();
  }

  delay(3);
}

// expose for main.cpp
void tft_app_setup(){ app_setup_impl(); }
void tft_app_loop(){  app_loop_impl();  }
void tfl_app_setup(){ app_setup_impl(); }
void tfl_app_loop(){  app_loop_impl();  }
