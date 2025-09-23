#include "Global.h"
#include <Preferences.h>
#include <cstring>
#include <cctype>

namespace {
  Preferences prefs;
  Cfg::Settings g;
  constexpr const char* NS = "trakkrcfg";

  inline void copySafe(char* dst, size_t cap, const char* src, const char* fallback=""){
    if (!dst || cap == 0) return;
    const char* s = (src && *src) ? src : (fallback ? fallback : "");
    size_t n = cap - 1;
    std::strncpy(dst, s, n);
    dst[n] = '\0';
  }
  inline bool isAlpha3(const char* s){
    if (!s) return false;
    if (std::strlen(s) != 3) return false;
    for (int i=0;i<3;i++) if (!std::isalpha((unsigned char)s[i])) return false;
    return true;
  }
  inline bool ieq(const char* a, const char* b){
    if (!a || !b) return false;
    while (*a && *b){
      if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
      ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
  }
  inline bool isHHMM(const char* s){
    if (!s || std::strlen(s)!=5 || s[2]!=':') return false;
    return std::isdigit((unsigned char)s[0]) && std::isdigit((unsigned char)s[1]) &&
           std::isdigit((unsigned char)s[3]) && std::isdigit((unsigned char)s[4]);
  }
}

void Cfg::begin(){
  prefs.begin(NS, false);

  // Wi-Fi
  copySafe(g.wifi_ssid, sizeof(g.wifi_ssid), prefs.getString("ssid", DEF_WIFI_SSID).c_str(), DEF_WIFI_SSID);
  copySafe(g.wifi_pass, sizeof(g.wifi_pass), prefs.getString("pass", DEF_WIFI_PASS).c_str(), DEF_WIFI_PASS);

  // Tokens
  copySafe(g.darwin_token, sizeof(g.darwin_token), prefs.getString("drw", DEF_DARWIN_TOKEN).c_str(), DEF_DARWIN_TOKEN);
  copySafe(g.tfl_token,    sizeof(g.tfl_token),    prefs.getString("tfl", DEF_TFL_TOKEN).c_str(), DEF_TFL_TOKEN);
  copySafe(g.wx_token,     sizeof(g.wx_token),     prefs.getString("owm", DEF_WX_TOKEN).c_str(),  DEF_WX_TOKEN);

  // Rail basics
  copySafe(g.mode, sizeof(g.mode), prefs.getString("mode", DEF_MODE).c_str(), DEF_MODE);
  copySafe(g.crs,  sizeof(g.crs),  prefs.getString("crs",  DEF_CRS ).c_str(), DEF_CRS);
  for (int i=0;i<3 && g.crs[i];++i) g.crs[i] = (char)std::toupper((unsigned char)g.crs[i]);
  g.crs[3]='\0';
  g.ticker_ms = prefs.getUInt("tms", DEF_TICKER_MS);

  // Control Panel (index.htm)
  copySafe(g.source,      sizeof(g.source),      prefs.getString("src", DEF_SOURCE).c_str(), DEF_SOURCE);
  copySafe(g.calling_at,  sizeof(g.calling_at),  prefs.getString("call",DEF_CALLING_AT).c_str(), DEF_CALLING_AT);
  g.include_bus     = prefs.getBool("bus",  DEF_INCLUDE_BUS);
  g.include_pass    = prefs.getBool("passX",DEF_INCLUDE_PASS); // avoid key clash with wifi pass
  g.show_date       = prefs.getBool("date", DEF_SHOW_DATE);
  g.include_weather = prefs.getBool("wx",   DEF_INCLUDE_WX);
  g.auto_update     = prefs.getBool("auto", DEF_AUTO_UPDATE);
  g.update_every    = prefs.getUShort("upd", DEF_UPDATE_EVERY);
  copySafe(g.ss_start,    sizeof(g.ss_start),    prefs.getString("ss1", DEF_SS_START).c_str(), DEF_SS_START);
  copySafe(g.ss_end,      sizeof(g.ss_end),      prefs.getString("ss2", DEF_SS_END  ).c_str(), DEF_SS_END);
  copySafe(g.tube_line,   sizeof(g.tube_line),   prefs.getString("line",DEF_TUBE_LINE).c_str(), DEF_TUBE_LINE);
  copySafe(g.tube_dir,    sizeof(g.tube_dir),    prefs.getString("dir", DEF_TUBE_DIR ).c_str(), DEF_TUBE_DIR);
}

const Cfg::Settings& Cfg::get(){ return g; }
Cfg::Settings&       Cfg::edit(){ return g; }

// Accessors
const char* Cfg::wifiSsid()    { return g.wifi_ssid; }
const char* Cfg::wifiPass()    { return g.wifi_pass; }
const char* Cfg::darwinToken() { return g.darwin_token; }
const char* Cfg::tflToken()    { return g.tfl_token; }
const char* Cfg::weatherToken(){ return g.wx_token; }
const char* Cfg::mode()        { return g.mode; }
const char* Cfg::crs()         { return g.crs; }
uint32_t    Cfg::tickerMs()    { return g.ticker_ms; }

const char* Cfg::source()      { return g.source; }
const char* Cfg::callingAt()   { return g.calling_at; }
bool        Cfg::includeBus()  { return g.include_bus; }
bool        Cfg::includePass() { return g.include_pass; }
bool        Cfg::showDate()    { return g.show_date; }
bool        Cfg::includeWeather(){ return g.include_weather; }
bool        Cfg::autoUpdate()  { return g.auto_update; }
uint16_t    Cfg::updateEvery() { return g.update_every; }
const char* Cfg::ssStart()     { return g.ss_start; }
const char* Cfg::ssEnd()       { return g.ss_end; }
const char* Cfg::tubeLine()    { return g.tube_line; }
const char* Cfg::tubeDir()     { return g.tube_dir; }

// Setters
bool Cfg::setWifi(const char* ssid, const char* pass){
  if (!ssid || !*ssid) return false;
  copySafe(g.wifi_ssid, sizeof(g.wifi_ssid), ssid);
  copySafe(g.wifi_pass, sizeof(g.wifi_pass), pass?pass:"");
  bool ok=true;
  ok &= prefs.putString("ssid", g.wifi_ssid) > 0;
  ok &= prefs.putString("pass", g.wifi_pass) > 0;
  return ok;
}
bool Cfg::setDarwinToken(const char* token){
  if (!token || !*token){ g.darwin_token[0]='\0'; prefs.remove("drw"); return true; }
  copySafe(g.darwin_token, sizeof(g.darwin_token), token);
  return prefs.putString("drw", g.darwin_token) >= 0;
}
bool Cfg::setTflToken(const char* token){
  if (!token || !*token){ g.tfl_token[0]='\0'; prefs.remove("tfl"); return true; }
  copySafe(g.tfl_token, sizeof(g.tfl_token), token);
  return prefs.putString("tfl", g.tfl_token) >= 0;
}
bool Cfg::setWeatherToken(const char* token){
  if (!token || !*token){ g.wx_token[0]='\0'; prefs.remove("owm"); return true; }
  copySafe(g.wx_token, sizeof(g.wx_token), token);
  return prefs.putString("owm", g.wx_token) >= 0;
}
bool Cfg::setMode(const char* m){
  std::strncpy(g.mode, (ieq(m,"arrivals") ? "arrivals" : "departures"), sizeof(g.mode)-1);
  g.mode[sizeof(g.mode)-1]='\0';
  return prefs.putString("mode", g.mode) > 0;
}
bool Cfg::setCRS(const char* three){
  if (!three) return false;
  char up[4] = {0,0,0,0}; copySafe(up, sizeof(up), three);
  for (int i = 0; i < 3 && up[i]; ++i) up[i] = (char)std::toupper((unsigned char)up[i]);
  if (!isAlpha3(up)) return false;
  copySafe(g.crs, sizeof(g.crs), up);
  return prefs.putString("crs", g.crs) > 0;
}
bool Cfg::setTickerMs(uint32_t ms){
  if (ms < 1000) ms = 1000;
  g.ticker_ms = ms;
  return prefs.putUInt("tms", g.ticker_ms) > 0;
}

bool Cfg::setSource(const char* s){
  const char* v = (ieq(s,"tube") ? "tube" : "rail");
  copySafe(g.source, sizeof(g.source), v);
  return prefs.putString("src", g.source) > 0;
}
bool Cfg::setCallingAt(const char* list){
  copySafe(g.calling_at, sizeof(g.calling_at), list?list:"");
  return prefs.putString("call", g.calling_at) >= 0;
}
bool Cfg::setIncludeBus(bool v){ g.include_bus=v; return prefs.putBool("bus", v); }
bool Cfg::setIncludePass(bool v){ g.include_pass=v; return prefs.putBool("passX", v); }
bool Cfg::setShowDate(bool v){ g.show_date=v; return prefs.putBool("date", v); }
bool Cfg::setIncludeWeather(bool v){ g.include_weather=v; return prefs.putBool("wx", v); }
bool Cfg::setAutoUpdate(bool v){ g.auto_update=v; return prefs.putBool("auto", v); }
bool Cfg::setUpdateEvery(uint16_t sec){
  if (sec < 5) sec = 5;
  g.update_every = sec;
  return prefs.putUShort("upd", sec);
}
bool Cfg::setScreensaver(const char* startHHMM, const char* endHHMM){
  if (startHHMM && isHHMM(startHHMM)) copySafe(g.ss_start, sizeof(g.ss_start), startHHMM);
  if (endHHMM   && isHHMM(endHHMM))   copySafe(g.ss_end,   sizeof(g.ss_end),   endHHMM);
  bool ok=true;
  ok &= prefs.putString("ss1", g.ss_start) > 0;
  ok &= prefs.putString("ss2", g.ss_end)   > 0;
  return ok;
}
bool Cfg::setTubeLine(const char* line){
  copySafe(g.tube_line, sizeof(g.tube_line), line?line:"");
  return prefs.putString("line", g.tube_line) >= 0;
}
bool Cfg::setTubeDir(const char* dir){
  copySafe(g.tube_dir, sizeof(g.tube_dir), dir?dir:"");
  return prefs.putString("dir", g.tube_dir) >= 0;
}

bool Cfg::save(){
  bool ok=true;
  ok &= prefs.putString("ssid", g.wifi_ssid) > 0;
  ok &= prefs.putString("pass", g.wifi_pass) > 0;
  ok &= prefs.putString("drw",  g.darwin_token) >= 0;
  ok &= prefs.putString("tfl",  g.tfl_token)    >= 0;
  ok &= prefs.putString("owm",  g.wx_token)     >= 0;
  ok &= prefs.putString("mode", g.mode) > 0;
  ok &= prefs.putString("crs",  g.crs)  > 0;
  ok &= prefs.putUInt  ("tms",  g.ticker_ms) > 0;
  ok &= prefs.putString("src",  g.source) > 0;
  ok &= prefs.putString("call", g.calling_at) >= 0;
  ok &= prefs.putBool  ("bus",  g.include_bus);
  ok &= prefs.putBool  ("passX",g.include_pass);
  ok &= prefs.putBool  ("date", g.show_date);
  ok &= prefs.putBool  ("wx",   g.include_weather);
  ok &= prefs.putBool  ("auto", g.auto_update);
  ok &= prefs.putUShort("upd",  g.update_every);
  ok &= prefs.putString("ss1",  g.ss_start) > 0;
  ok &= prefs.putString("ss2",  g.ss_end)   > 0;
  ok &= prefs.putString("line", g.tube_line) >= 0;
  ok &= prefs.putString("dir",  g.tube_dir)  >= 0;
  return ok;
}

void Cfg::resetToDefaults(){
  copySafe(g.wifi_ssid, sizeof(g.wifi_ssid), DEF_WIFI_SSID);
  copySafe(g.wifi_pass, sizeof(g.wifi_pass), DEF_WIFI_PASS);
  copySafe(g.darwin_token, sizeof(g.darwin_token), DEF_DARWIN_TOKEN);
  copySafe(g.tfl_token,    sizeof(g.tfl_token),    DEF_TFL_TOKEN);
  copySafe(g.wx_token,     sizeof(g.wx_token),     DEF_WX_TOKEN);
  copySafe(g.mode, sizeof(g.mode), DEF_MODE);
  copySafe(g.crs,  sizeof(g.crs),  DEF_CRS);
  for (int i=0;i<3 && g.crs[i];++i) g.crs[i]=(char)std::toupper((unsigned char)g.crs[i]);
  g.ticker_ms = DEF_TICKER_MS;

  copySafe(g.source,      sizeof(g.source),      DEF_SOURCE);
  copySafe(g.calling_at,  sizeof(g.calling_at),  DEF_CALLING_AT);
  g.include_bus     = DEF_INCLUDE_BUS;
  g.include_pass    = DEF_INCLUDE_PASS;
  g.show_date       = DEF_SHOW_DATE;
  g.include_weather = DEF_INCLUDE_WX;
  g.auto_update     = DEF_AUTO_UPDATE;
  g.update_every    = DEF_UPDATE_EVERY;
  copySafe(g.ss_start,    sizeof(g.ss_start),    DEF_SS_START);
  copySafe(g.ss_end,      sizeof(g.ss_end),      DEF_SS_END);
  copySafe(g.tube_line,   sizeof(g.tube_line),   DEF_TUBE_LINE);
  copySafe(g.tube_dir,    sizeof(g.tube_dir),    DEF_TUBE_DIR);
  save();
}
