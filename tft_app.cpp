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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"



// === [TRAKKR][BEGIN:fetch_guard] ===
// Prevent overlapping Darwin POSTs and debounce rapid retriggers.
// Binary semaphore acts as a non-blocking lock.
// If a fetch is already running, we skip starting another one.
static SemaphoreHandle_t gFetchSem = nullptr;
static uint32_t gLastFetchMs = 0;

// Acquire guard if idle and not within the debounce window (default 800ms).
static bool beginFetchGuard(uint32_t debounceMs = 800){
  if (!gFetchSem){
    gFetchSem = xSemaphoreCreateBinary();
    if (gFetchSem) xSemaphoreGive(gFetchSem); // set to "available"
  }
  if (!gFetchSem){
    Serial.println("[NET][ERR] gFetchSem alloc failed");
    return true; // fail-open so the app still works
  }

  const uint32_t now   = millis();
  const uint32_t since = now - gLastFetchMs;
  if (since < debounceMs){
    Serial.printf("[NET][DEBOUNCE] last=%ums (<%ums); skipping fetch\n",
                  (unsigned)since, (unsigned)debounceMs);
    return false;
  }

  if (xSemaphoreTake(gFetchSem, 0) != pdTRUE){
    Serial.println("[NET][SKIP] fetch already running");
    return false;
  }

  gLastFetchMs = now;
  return true;
}

// Release guard.
static void endFetchGuard(){
  if (gFetchSem) xSemaphoreGive(gFetchSem);
}

// Scope helper to guarantee release on every code path.
struct FetchScope {
  bool active;
  explicit FetchScope(bool ok): active(ok) {}
  ~FetchScope(){ if (active) endFetchGuard(); }
};
// === [TRAKKR][END:fetch_guard] ===





static SemaphoreHandle_t gTftMutex = nullptr;
static void drawTicker_FS();
static void tickerTask(void*){
  for(;;){
    if (xSemaphoreTake(gTftMutex, portMAX_DELAY) == pdTRUE){
      drawTicker_FS();                    // scroll or static, then push the sprite
      xSemaphoreGive(gTftMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(33));        // ~30 fps
  }
}

//
// FILESYSTEM SELECT
// choose LittleFS or SD for ticker storage
//
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

//
// PERFORMANCE / HEALTH
// heap/timing beacons and guardrails
//
#include <esp_heap_caps.h>
#include <esp_system.h>

static const bool     PERF_VERBOSE           = true;
static const uint32_t PERF_PERIOD_MS         = 15000;
static const size_t   PERF_WARN_LARGEST_MIN  = 12 * 1024;

static void logMem(const char* where){
  if (!PERF_VERBOSE) return;
  size_t freeHeap = ESP.getFreeHeap();
  size_t minHeap  = ESP.getMinFreeHeap();
  size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t freePS   = ESP.getFreePsram();
  size_t minPS    = ESP.getMinFreePsram();
  size_t largestPS= heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  Serial.printf("[MEM] %-18s | heap: free=%uB min=%uB largest=%uB | psram: free=%uB min=%uB largest=%uB\n",
                where, (unsigned)freeHeap, (unsigned)minHeap, (unsigned)largest,
                (unsigned)freePS, (unsigned)minPS, (unsigned)largestPS);
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
struct ScopeTimer {
  const char* name; uint32_t t0;
  ScopeTimer(const char* n) : name(n), t0(millis()) {}
  ~ScopeTimer(){ if (PERF_VERBOSE) Serial.printf("[TIME] %-18s %lums\n", name, (unsigned long)(millis()-t0)); }
};

//
// CONFIG
// network, Darwin SOAP, board parameters
//
static const char* WIFI_SSID   = "alterra";
static const char* WIFI_PASS   = "Hewer035!!";
static const char* DARWIN_HOST = "lite.realtime.nationalrail.co.uk";
static const char* DARWIN_PATH = "/OpenLDBWS/ldb9.asmx";
static const char* DARWIN_TOKEN= "9a6c3c95-ca8e-411f-8d5b-f32564d0928d";

static const char* SOAP12_NS   = "http://www.w3.org/2003/05/soap-envelope";
static const char* LDB_NS      = "http://thalesgroup.com/RTTI/2016-02-16/ldb/";
static const char* TOK_NS      = "http://thalesgroup.com/RTTI/2013-11-28/Token/types";

static const char* MODE = "departures";   // "arrivals" for arrivals
static const char* CRS  = "STP";
static const int   ROWS = 8;
static const int   TIME_WINDOW_MINS = 120;

static const uint32_t POLL_MS_OK  = 15000;
static const uint32_t POLL_MS_ERR = 2000;
static const uint32_t TICKER_MS   = 7000;

static const bool   DEBUG_NET       = true;
static const bool   DEBUG_BODY_SNIP = false;  // keep serial clean
static const size_t BODY_SNIP_N     = 700;

static const char* POWERED_MSG = "Powered by National Rail";

//
// LAYOUT
// geometry, columns and ticker height
//
static const int W=480, H=320, PAD=8;
static const int HEADER_H=48, COLBAR_H=24;
static const int COLBAR_Y=HEADER_H, ROW_TOP=COLBAR_Y+COLBAR_H;

// Columns
static const int X_STD=PAD, X_TO=50, X_ETD=250, X_PLAT=310, X_OPER=335;
static const int CH_TIME=5, CH_TO=28, CH_ETD=10, CH_PLAT=3, CH_OPER=21;

static const int  TICKER_H=28, TICKER_SPEED=2;

//
// STATE
// runtime models and timers
//
struct Svc { String time, place, est, plat, oper; };
std::vector<Svc>   services;
std::vector<String> nrccMsgs;

static String stationTitle="Board";
static uint32_t nextPoll=0, nextClockTick=0;
static uint32_t nextPerfBeat=0;



// --- clock/title geometry (FONT2) ---
static int  clockX=0, clockBaseY=28, clockBoxX=0, clockBoxY=0, clockBoxW=0, clockBoxH=0;
static char lastClock[16] = {0};

// boot box rect
static int bootX=0, bootY=0, bootW=300, bootH=110;

//
// COLORS
// palette helpers
//
static uint16_t bodyBg()   { return tft.color565(0x0b,0x10,0x20); }
static uint16_t headBg()   { return tft.color565(0x13,0x1a,0x33); }
static uint16_t headBr()   { return tft.color565(0x24,0x30,0x59); }
static uint16_t rowAlt()   { return tft.color565(0x0d,0x12,0x30); }
static uint16_t warnCol()  { return tft.color565(0xff,0xd1,0x66); }
static uint16_t badCol()   { return tft.color565(0xff,0x5d,0x5d); }

//
// UTILS
// small helpers (strings, wifi)
//
static String ellipsize(const String& s, int maxChars){
  if ((int)s.length()<=maxChars) return s;
  if (maxChars<=1) return "…";
  return s.substring(0,maxChars-1)+"…";
}

static void ensureWiFi(){
  if (WiFi.status()==WL_CONNECTED) return;
  ScopeTimer T("WiFi connect");
  logMem("pre-WiFi");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  if (DEBUG_NET){ Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID); }
  uint32_t t0=millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-t0<15000){ delay(200); Serial.print('.'); }
  Serial.println();
  if (WiFi.status()==WL_CONNECTED){
    delay(150);
    IPAddress ip; WiFi.hostByName(DARWIN_HOST, ip);
    if (DEBUG_NET){ Serial.printf("[WiFi] OK IP=%s\n", WiFi.localIP().toString().c_str()); }
  }else if (DEBUG_NET){ Serial.println("[WiFi] FAILED"); }
  logMem("post-WiFi");
  checkHeap("post-WiFi");
}

//
// BOOT BOX
// centered panel used during staged boot
//
static void bootInit(){
  bootW=300; bootH=110; bootX=(W-bootW)/2; bootY=(H-bootH)/2;
  tft.fillScreen(bodyBg());
  tft.fillRect(0,0,W,HEADER_H, headBg()); tft.drawRect(0,0,W,HEADER_H, headBr());
}
static void bootShow(const char* line){
  uint16_t panel = tft.color565(0x0d,0x12,0x30);
  tft.fillRect(bootX,bootY,bootW,bootH,panel);
  tft.drawRect(bootX,bootY,bootW,bootH, headBr());
  tft.setTextFont(2); tft.setTextColor(TFT_WHITE,panel);
  tft.setCursor(bootX+12, bootY+22); tft.print(line?line:"");
}
static void bootHide(){ tft.fillRect(bootX-1, bootY-1, bootW+2, bootH+2, bodyBg()); }

//
// CLOCK/TIME
// timezone, validity and rendering cadence
//
static void setupClockTZ(){
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2",
               "pool.ntp.org","time.google.com","time.cloudflare.com");
}
static bool timeValid(){
  time_t t=time(nullptr); struct tm tm{}; localtime_r(&t,&tm); return tm.tm_year>=120;
}
static bool waitForTime(uint32_t ms=6000){
  ScopeTimer T("NTP settle");
  uint32_t t0=millis(); while(!timeValid() && millis()-t0<ms){ delay(100);} return timeValid();
}
static void nowHHMM(char* out,size_t n){
  if(!timeValid()){ strncpy(out,"--:--",n); return; }
  time_t t=time(nullptr); struct tm tm{}; localtime_r(&t,&tm); strftime(out,n,"%H:%M",&tm);
}

//
// HEADER GEOMETRY
// compute positions for clock box
//
static void headerInit(){
  tft.setTextFont(2); tft.setTextSize(1);
  int ww = tft.textWidth("88:88"); int hh = tft.fontHeight();
  if (ww <= 0) ww = 60; if (hh <= 0) hh = 16;
  clockX = W - PAD - ww;
  int topPad = (HEADER_H - hh) / 2;
  clockBaseY = topPad;
  clockBoxX  = clockX - 3; clockBoxY=topPad - 2; clockBoxW=ww + 10; clockBoxH=hh + 4;
}
static void drawClockIfChanged(){
  char buf[16]; nowHHMM(buf, sizeof(buf));
  if (strcmp(buf, lastClock) == 0) return;
  tft.setTextFont(2); tft.setTextSize(1);
  tft.fillRect(clockBoxX, clockBoxY, clockBoxW, clockBoxH, headBg());
  tft.setTextColor(TFT_WHITE, headBg());
  tft.setCursor(clockX, clockBaseY);
  tft.print(buf);
  strncpy(lastClock, buf, sizeof(lastClock)-1);
  lastClock[sizeof(lastClock)-1] = '\0';
}
static void scheduleNextMinute(){
  if(!timeValid()){ nextClockTick=millis()+1000; return; }
  time_t t=time(nullptr); struct tm tm{}; localtime_r(&t,&tm);
  uint32_t ms=(60-tm.tm_sec)*1000u; nextClockTick=millis()+(ms?ms:60000);
}

//
// HEADER TITLE
// station title + mode in the header bar
//
static void setTitle(const String& station){
  tft.setTextFont(2); tft.setTextSize(1);
  int hh = tft.fontHeight(); if (hh <= 0) hh = 16;
  int topPad = (HEADER_H - hh) / 2; int baseY = topPad;
  String want = station + " " + (MODE[0]=='a' ? "Arrivals" : "Departures");
  int clockRight = (clockX > 0 && clockX < W) ? clockX : (W - PAD - 60);
  int maxPx = clockRight - 6 - PAD; if (maxPx < 20) maxPx = 20;
  String out = want;
  while (out.length() && tft.textWidth(out + "…") > maxPx) out.remove(out.length()-1);
  if (out.length() && out != want) out += "…";
  tft.fillRect(PAD, topPad - 2, maxPx, hh + 4, headBg());
  tft.setTextColor(TFT_WHITE, headBg());
  tft.setCursor(PAD, baseY); tft.print(out);
}

//
// COLUMNS & ROWS
// board layout and row painting
//
static void drawColHeader(){
  uint16_t bg=rowAlt();
  tft.fillRect(0, COLBAR_Y, W, COLBAR_H, bg);
  tft.setTextFont(2); tft.setTextColor(tft.color565(0x9f,0xb3,0xff), bg);
  int y=COLBAR_Y+4;
  tft.setCursor(X_STD,y);  tft.print(MODE[0]=='a'?"STA":"STD");
  tft.setCursor(X_TO,y);   tft.print(MODE[0]=='a'?"From":"To");
  tft.setCursor(X_ETD,y);  tft.print(MODE[0]=='a'?"ETA":"ETD");
  tft.setCursor(X_PLAT,y); tft.print("Plt");
  tft.setCursor(X_OPER,y); tft.print("Operator");
}
static String normalizeOper(String op){
  op.trim();
  if (op=="London North Eastern Railway") return "LNER";
  op.trim();
  return op;
}
static void drawRows(){
  ScopeTimer T("drawRows");
  const int rowH=26;
  const int maxVis=min(ROWS,(H-ROW_TOP-TICKER_H)/rowH);
  int painted=min((int)services.size(), maxVis);
  for(int i=0;i<painted;i++){
    const auto& s=services[i];
    uint16_t bg=(i%2==0)?bodyBg():rowAlt();
    tft.fillRect(0, ROW_TOP+i*rowH, W, rowH, bg);
    int by=ROW_TOP+i*rowH+6;

    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE,bg);
    tft.setCursor(X_STD,by);  tft.print(ellipsize(s.time,CH_TIME));
    tft.setCursor(X_TO, by);  tft.print(ellipsize(s.place,CH_TO));

    String low = s.est;  low.toLowerCase();
    uint16_t c = TFT_WHITE;
    if (low.indexOf("cancel") >= 0 || low.indexOf("delay") >= 0) c = badCol();
    else if (low.indexOf("late") >= 0 || low.indexOf(':') >= 0)   c = warnCol();
    tft.setTextColor(c, bg);
    tft.setCursor(X_ETD, by); tft.print(ellipsize(s.est, CH_ETD));

    tft.setTextColor(TFT_WHITE,bg);
    tft.setCursor(X_PLAT,by); tft.print(ellipsize(s.plat,CH_PLAT));
    tft.setCursor(X_OPER,by); tft.print(ellipsize(s.oper,CH_OPER));
  }
  if (painted<maxVis){
    int y=ROW_TOP+painted*rowH, h=(maxVis-painted)*rowH;
    tft.fillRect(0,y,W,h, bodyBg());
  }
  checkHeap("after drawRows");
}

//
// TICKER CONFIG & STATE
// file-backed ribbon + mode flags for static fallback
//
static const char* kTickerPath = "/ticker.txt";
static const char* kMetaPath   = "/ticker.meta";
static const char* kSep        = "   |   ";   // ASCII separator (single-byte, UTF-8 safe)

// Streaming state for endless ribbon
static TFT_eSprite tickSpr(&tft);
static File        tickFile;
static size_t      tickSize = 0;

// Endless-ribbon cursors
static size_t fileOffset = 0;   // byte offset into ticker.txt
static int    scrollPx   = 0;   // pixel scroll within the lead character

// Mode flags: when no NRCC => show static "Powered by National Rail"
static volatile bool gTickerHasNRCC    = false;
static volatile bool gTickerStaticDirty= true;

static inline void tickerSetHasNRCC(bool has){
  if (gTickerHasNRCC != has){
    gTickerHasNRCC = has;
    gTickerStaticDirty = true;  // rebuild static sprite or resume scroll cleanly
    // Optionally reset scroll when re-entering scroll mode
    if (has){ scrollPx = 0; }
  }
}

//
// TICKER FILES
// hashing, meta, and atomic write of ticker content
//
// Hash: 32-bit FNV-1a (simple + robust)
static uint32_t fnv1a32(const uint8_t* d, size_t n, uint32_t h=2166136261u){
  for(size_t i=0;i<n;i++){ h ^= d[i]; h *= 16777619u; }
  return h;
}
static uint32_t hashMessages(const std::vector<String>& msgs){
  uint32_t h=2166136261u;
  for (auto &m : msgs){
    h = fnv1a32((const uint8_t*)m.c_str(), m.length(), h);
    h = fnv1a32((const uint8_t*)kSep, strlen(kSep), h);
  }
  // Include the powered tail to keep the visual stream deterministic
  const char* tail = POWERED_MSG;
  h = fnv1a32((const uint8_t*)tail, strlen(tail), h);
  h = fnv1a32((const uint8_t*)kSep, strlen(kSep), h);
  return h;
}
static bool readMeta(uint32_t& out){
  File f = FSNS.open(kMetaPath, "r");
  if (!f) return false;
  uint32_t v=0; int n = f.read((uint8_t*)&v, sizeof(v)); f.close();
  if (n != (int)sizeof(v)) return false;
  out = v; return true;
}
static void writeMeta(uint32_t v){
  File f = FSNS.open(kMetaPath, "w");
  if (!f) return;
  f.write((const uint8_t*)&v, sizeof(v)); f.close();
}
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

// Helpers for file-backed ribbon
static bool openTicker(){
  if (tickFile) tickFile.close();
  tickFile = FSNS.open(kTickerPath, "r");
  if (!tickFile){ Serial.println("[TICK][ERR] open ticker.txt failed"); return false; }
  tickSize = tickFile.size();
  return true;
}
static int readByteAt(size_t off){
  if (!tickFile) return -1;
  if (tickSize == 0) return -1;
  off %= tickSize;
  tickFile.seek(off);
  return tickFile.read();
}

//
// TICKER DRAW
// static fallback when no NRCC, otherwise smooth file-backed scroll
//
static void drawTicker_FS(){
  const int y       = H - TICKER_H;
  const int availPx = W - 2*PAD;

  // --- Static fallback when there are no NRCC messages ---
  if (!gTickerHasNRCC){
    if (gTickerStaticDirty){
      tickSpr.fillSprite(headBg());
      tickSpr.setTextFont(2);
      tickSpr.setTextSize(1);
      tickSpr.setTextColor(TFT_WHITE, headBg());
      tickSpr.setTextWrap(false);

      // Perfect centre inside the same PAD band the scroller uses
      const int bandMidX = PAD + ((W - 2*PAD) / 2);
      const int midY     = TICKER_H / 2;

      tickSpr.setTextDatum(MC_DATUM);
      tickSpr.drawString(POWERED_MSG, bandMidX, midY);
      tickSpr.setTextDatum(TL_DATUM);   // restore for scrolling path

      gTickerStaticDirty = false;
    }

    tickSpr.pushSprite(0, y);
    tft.drawRect(0, y, W, TICKER_H, headBr());
    return; // skip scrolling path
  }

  // --- Scrolling path (with custom separator icon) ---
  tickSpr.fillSprite(headBg());
  tickSpr.setTextDatum(TL_DATUM);
  tickSpr.setTextFont(2);
  tickSpr.setTextSize(1);
  tickSpr.setTextColor(TFT_WHITE, headBg());
  tickSpr.setTextWrap(false);

  // Build a small read-ahead window starting at fileOffset
  String window;
  window.reserve(512);
  int windowPx = 0;
  const int needPx = availPx + scrollPx + 40; // 40px safety

  for (size_t i = 0; windowPx < needPx && i < 512; ++i){
    int b = readByteAt(fileOffset + i);
    if (b < 0) break;
    char c = (char)b;
    if (c=='\r' || c=='\n') c=' ';
    window += c;
    windowPx = tickSpr.textWidth(window);
  }

  // If file is empty or read failed, draw frame & bail safely
  if (window.length() == 0){
    tickSpr.pushSprite(0, y);
    tft.drawRect(0, y, W, TICKER_H, headBr());
    return;
  }

  // Rendered text: remove the '|' completely so it doesn't add any extra space
  String render = window;
  render.replace("|", "");
  const int renderPx = tickSpr.textWidth(render);
  const int tileStep = max(1, renderPx);   // step for both text and icons

  // Tile the window across the visible band for an endless ribbon
  int x0 = PAD - scrollPx;

  // 1) Print text (no pipe glyphs)
  for (int tileX = x0; tileX < PAD + availPx; tileX += tileStep){
    tickSpr.setCursor(tileX, 6);
    tickSpr.print(render);
  }

  // 2) Draw a custom icon where each '|' was
  auto drawDiamondAt = [&](int px){
    int h  = tickSpr.fontHeight();
    int sz = max(8, min(12, h - 4));   // keep it balanced
    int cx = px;
    int cy = TICKER_H / 2;
    uint16_t col = TFT_WHITE;
    tickSpr.fillTriangle(cx, cy - sz/2,  cx - sz/2, cy,  cx, cy + sz/2, col);
    tickSpr.fillTriangle(cx, cy - sz/2,  cx + sz/2, cy,  cx, cy + sz/2, col);
  };

  // For each occurrence of '|' compute its pixel x in the *rendered* string
  int searchFrom = 0;
  while (true){
    int idx = window.indexOf('|', searchFrom);
    if (idx < 0) break;
    searchFrom = idx + 1;

    // Width up to this separator, but measured on the rendered text (no pipes)
    String upTo = window.substring(0, idx);
    upTo.replace("|", "");                       // match what we actually printed
    int sepPx = tickSpr.textWidth(upTo);

    for (int iconX = x0 + sepPx; iconX < PAD + availPx; iconX += tileStep){
      if (iconX >= PAD) drawDiamondAt(iconX);
    }
  }

  // Frame + push
  tickSpr.pushSprite(0, y);
  tft.drawRect(0, y, W, TICKER_H, headBr());

  // Advance smooth pixel scroll
  scrollPx += TICKER_SPEED;

  // Consume the leading character when it has fully scrolled off
  String firstChar; firstChar += window[0];
  int cw = tickSpr.textWidth(firstChar);
  if (cw > 0 && scrollPx >= cw){
    scrollPx   -= cw;
    fileOffset  = (fileOffset + 1) % (tickSize ? tickSize : 1);
  }
}

//
// TICKER REFRESH
// rebuild files if NRCC changed, then (re)open stream
//
static void tickerRefreshFilesAndOpen(){
  bool changed = writeTickerFileIfChanged(); // writes only if hash changed
  if (!tickFile || changed) {
    if (tickFile) tickFile.close();
    if (openTicker()) {
      tickSize   = tickFile.size();
      fileOffset = 0;
      scrollPx   = 0;
    }
  }
}

//
// SOAP/XML HELPERS
// small namespace-aware tag helpers
//
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

//
// SOAP POST
// builds and performs one SOAP call
//
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
  if (DEBUG_BODY_SNIP){ Serial.println(outBody.substring(0,BODY_SNIP_N)); }

  logMem("post-POST");
  checkHeap("post-POST");
  return outCode==200;
}

static String extractFault(const String& body){
  String s=get1ns(body,"faultstring"); if(s.length()) return s;
  String reason=get1ns(body,"Reason"); String text=get1ns(reason,"Text");
  return text;
}

//
// FETCH & PARSE
// SOAP roundtrip + extraction into models (services, nrcc)
//
static bool fetchDarwinBoard(){
  // Guard: prevent overlapping POSTs + debounce rapid retriggers.
  // If a fetch is already running or we’re inside the debounce window, skip.
  bool okToRun = beginFetchGuard(800);   // debounce window in ms
  if (!okToRun) return false;
  FetchScope guard(okToRun);            // auto-release on all exits

  ScopeTimer T("fetch+parse");
  services.clear();
  nrccMsgs.clear();
  checkHeap("after clear vectors");

  const bool dep = (MODE[0] != 'a');
  const char* method = dep ? "GetDepartureBoard"        : "GetArrivalBoard";
  const char* reqTag = dep ? "GetDepartureBoardRequest" : "GetArrivalBoardRequest";

  String body; int code = 0;
  {
    ScopeTimer Tpost("SOAP roundtrip");
    bool ok = postSoapOnce(body, code, method, reqTag);
    if (!ok){
      String fault = extractFault(body);
      if (DEBUG_NET){
        Serial.printf("[SOAP] FAIL code=%d fault=\"%s\"\n",
                      code, fault.length() ? fault.c_str() : "");
      }
      return false;
    }
  }

  {
    ScopeTimer Tparse("parse XML");
    String loc = get1ns(body,"locationName");
    stationTitle = loc.length()?loc:String(CRS);

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

        // Decode common entities
        txt.replace("&nbsp;"," ");
        txt.replace("&amp;","&");
        txt.replace("&lt;","<");
        txt.replace("&gt;",">");
        txt.replace("&quot;","\"");
        txt.replace("&apos;","'");

        // Strip tags
        for (;;){
          int lt = txt.indexOf('<'); if (lt < 0) break;
          int gt = txt.indexOf('>', lt+1);
          if (gt < 0) { txt.remove(lt); break; }
          txt.remove(lt, gt - lt + 1);
        }

        // Collapse whitespace
        for (int i=0; i+1 < (int)txt.length(); ){
          if (txt[i]==' ' && txt[i+1]==' ') txt.remove(i,1);
          else ++i;
        }
        txt.trim();

        // Keep only first sentence
        int pDot = txt.indexOf('.');
        int pExc = txt.indexOf('!');
        int pQue = txt.indexOf('?');
        int cut  = -1;
        if (pDot >= 0) cut = pDot;
        if (pExc >= 0 && (cut < 0 || pExc < cut)) cut = pExc;
        if (pQue >= 0 && (cut < 0 || pQue < cut)) cut = pQue;
        if (cut >= 0) txt = txt.substring(0, cut + 1);
        txt.trim();

        if (txt.length()) nrccMsgs.push_back(txt);
      }
    }
  }

  // Ticker mode + files
  tickerSetHasNRCC(!nrccMsgs.empty());
  tickerRefreshFilesAndOpen();

  if (DEBUG_NET){
    Serial.printf("[PARSE] %s  services=%u  nrcc=%u\n",
      stationTitle.c_str(), (unsigned)services.size(), (unsigned)nrccMsgs.size());
  }
  checkHeap("after parse");
  return true;
}


static bool fetchWithRetry(int tries=3, uint32_t backoff=250){
  for(int i=0;i<tries;i++){
    if (fetchDarwinBoard()) return true;
    if (DEBUG_NET) Serial.printf("[NET] First fetch failed (try %d/%d)\n", i+1, tries);
    delay(backoff); backoff += 250;
  }
  return false;
}

//
// APP SETUP
// one-time init, staged boot, first paint, start ticker task
//
static void app_setup_impl(){
  Serial.begin(115200); delay(30);
  Serial.println("\n[BOOT] tft_app starting…");
  logMem("boot");

  tft.init();
  tft.setRotation(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // (optional) backlight PWM — set your BL pin here
  // ledcAttachPin(32, 1);
  // ledcSetup(1, 5000, 8);
  // ledcWrite(1, 200);

  bootInit();
  headerInit();
  setTitle(String(CRS));

  // --- FS ---
  bootShow("Mounting FS…");
  if (!fsBegin()) {
    bootShow("FS mount failed");
    Serial.printf("[FS][ERR] %s mount failed\n", kFSName);
  } else {
    Serial.printf("[FS] %s mounted\n", kFSName);
  }

  // --- Wi-Fi ---
  bootShow("Connecting to Wi-Fi…");
  ensureWiFi();
  bootShow(WiFi.status()==WL_CONNECTED ? "Wi-Fi connected" : "Wi-Fi failed");

  // --- Time ---
  bootShow("Setting time…");
  setupClockTZ();
  waitForTime(6000);
  drawClockIfChanged();
  scheduleNextMinute();

  // --- Ticker sprite (once) ---
  {
    size_t before = ESP.getFreeHeap();
    tickSpr.setColorDepth(16);
    bool ok = tickSpr.createSprite(W, TICKER_H);
    size_t after  = ESP.getFreeHeap();
    Serial.printf("[SPRITE] ticker %s | size=%dx%d x 2Bpp ~= %uB | heap delta=%ldB\n",
                  ok?"OK":"FAIL", W, TICKER_H, (unsigned)(W*TICKER_H*2), (long)(after - before));
    checkHeap("after sprite alloc");
  }

  // --- First data load (quick retry) ---
  bootShow("Loading station data…");
  bool okFetch = (WiFi.status()==WL_CONNECTED) ? fetchWithRetry(3,250) : false;

  // Ensure ticker file is ready (even if NRCC empty)
  if (!tickFile) openTicker();

  bootHide();

  // --- Create mutex and do the first paint under lock ---
  if (!gTftMutex) gTftMutex = xSemaphoreCreateMutex();

  if (xSemaphoreTake(gTftMutex, pdMS_TO_TICKS(200))){
    ScopeTimer Tpaint("first paint");
    setTitle(stationTitle);
    tft.fillRect(0, HEADER_H, W, H-HEADER_H, bodyBg());
    drawColHeader();
    drawRows();
    // ticker task handles ticker drawing separately
    xSemaphoreGive(gTftMutex);
  }

  // --- Start the dedicated ticker task (keeps scrolling during polls) ---
  xTaskCreatePinnedToCore(tickerTask, "ticker", 4096, nullptr, 1, nullptr, 1);

  // --- Schedules ---
  nextPoll     = millis() + (okFetch ? POLL_MS_OK : POLL_MS_ERR);
  nextPerfBeat = millis() + PERF_PERIOD_MS;

  logMem("after first paint");
  Serial.println("[BOOT] setup complete.");
}

//
// APP LOOP
// polling cadence, repaint, and clock ticks
//
static void app_loop_impl(){
  uint32_t now = millis();

  // ---- HEALTH HEARTBEAT ----
  if (PERF_VERBOSE && now >= nextPerfBeat){
    logMem("heartbeat"); checkHeap("heartbeat");
    nextPerfBeat = now + PERF_PERIOD_MS;
  }

  // ---- DATA POLL (boards + NRCC) ----
  if (now >= nextPoll){
    ScopeTimer Tp("poll+repaint");
    ensureWiFi();
    bool ok = (WiFi.status() == WL_CONNECTED) ? fetchDarwinBoard() : false;

    // All TFT writes under the mutex
    if (xSemaphoreTake(gTftMutex, portMAX_DELAY) == pdTRUE){
      setTitle(stationTitle);
      drawColHeader();
      drawRows();
      // DO NOT call drawTicker_FS() here — ticker task handles it
      xSemaphoreGive(gTftMutex);
    }

    nextPoll = now + (ok ? POLL_MS_OK : POLL_MS_ERR);
    logMem(ok ? "post-poll OK" : "post-poll ERR");
  }

  // ---- CLOCK ----
  if (now >= nextClockTick){
    if (xSemaphoreTake(gTftMutex, pdMS_TO_TICKS(50)) == pdTRUE){
      drawClockIfChanged();
      xSemaphoreGive(gTftMutex);
    }
    scheduleNextMinute();
  }

  delay(3);
}

// expose both spellings
void tft_app_setup(){ app_setup_impl(); }
void tft_app_loop(){  app_loop_impl();  }
void tfl_app_setup(){ app_setup_impl(); }
void tfl_app_loop(){  app_loop_impl();  }
