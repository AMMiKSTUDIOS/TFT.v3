#include "Api.h"
#include <cstring>
#include <limits.h>   
#include <ctype.h>    
#include "HttpServer.h"   


static String jsonEscape(const char* s){
  String o; if (!s) return "\"\"";
  o.reserve(strlen(s) + 4);
  o += '"';
  for (const char* p = s; *p; ++p){
    char c = *p;
    if (c=='"' || c=='\\') { o+='\\'; o+=c; }
    else if (c=='\n') o+="\\n";
    else if (c=='\r') o+="\\r";
    else if (c=='\t') o+="\\t";
    else o+=c;
  }
  o += '"';
  return o;
}
static int findKey(const String& body, const char* key){
  return body.indexOf(String("\"")+key+"\"");
}
static String getJsonString(const String& body, const char* key){
  int k = findKey(body,key); if (k<0) return String();
  int c = body.indexOf(':', k); if (c<0) return String();
  int q1 = body.indexOf('"', c+1); if (q1<0) return String();
  int q2 = body.indexOf('"', q1+1); if (q2<0) return String();
  return body.substring(q1+1,q2);
}
static long getJsonInt(const String& body, const char* key){
  int k = findKey(body,key); if (k<0) return LONG_MIN;
  int c = body.indexOf(':', k); if (c<0) return LONG_MIN;
  int i=c+1; while (i<(int)body.length() && isspace((unsigned char)body[i])) ++i;
  int s=i; while (i<(int)body.length() && isdigit((unsigned char)body[i])) ++i;
  if (i<=s) return LONG_MIN;
  return body.substring(s,i).toInt();
}
static int getJsonBool(const String& body, const char* key){
  int k = findKey(body,key); if (k<0) return -1;
  int c = body.indexOf(':', k); if (c<0) return -1;
  int i=c+1; while (i<(int)body.length() && isspace((unsigned char)body[i])) ++i;
  if (body.startsWith("true", i))  return 1;
  if (body.startsWith("false", i)) return 0;
  return -1;
}

static String buildSettingsJSON(){
  const auto& s = Cfg::get();
  String j; j.reserve(512);
  j += '{';
  j += "\"source\":"       + jsonEscape(Cfg::source()) + ',';
  j += "\"station\":"       + jsonEscape(Cfg::crs())    + ',';   // station = CRS
  j += "\"nrBoardType\":"  + jsonEscape(Cfg::mode())   + ',';
  j += "\"callingAt\":"    + jsonEscape(Cfg::callingAt()) + ',';
  j += "\"includeBus\":"   + String(Cfg::includeBus() ? "true":"false") + ',';
  j += "\"includePass\":"  + String(Cfg::includePass()? "true":"false") + ',';
  j += "\"showDate\":"     + String(Cfg::showDate()?   "true":"false") + ',';
  j += "\"includeWeather\":"+ String(Cfg::includeWeather()? "true":"false") + ',';
  j += "\"autoUpdate\":"   + String(Cfg::autoUpdate()? "true":"false") + ',';
  j += "\"updateEvery\":"  + String((int)Cfg::updateEvery()) + ',';
  j += "\"ssStart\":"      + jsonEscape(Cfg::ssStart()) + ',';
  j += "\"ssEnd\":"        + jsonEscape(Cfg::ssEnd())   + ',';
  j += "\"line\":"         + jsonEscape(Cfg::tubeLine())+ ',';
  j += "\"direction\":"    + jsonEscape(Cfg::tubeDir())+ ',';
  // optional: expose wifi ssid (not pass)
  j += "\"wifi\":{\"ssid\":" + jsonEscape(Cfg::wifiSsid()) + "}";
  j += '}';
  return j;
}

static bool applySettingsFromJSON(const String& body){
  bool ok = true;
  String v;

  // source, station, mode
  v = getJsonString(body,"source");       if (v.length()) ok &= Cfg::setSource(v.c_str());
  v = getJsonString(body,"station");      if (v.length()==3) ok &= Cfg::setCRS(v.c_str());
  v = getJsonString(body,"nrBoardType");  if (v.length()) ok &= Cfg::setMode(v.c_str());

  // callingAt
  v = getJsonString(body,"callingAt");    if (v.length() || findKey(body,"callingAt")>=0) ok &= Cfg::setCallingAt(v.c_str());

  // booleans
  int b;
  b = getJsonBool(body,"includeBus");     if (b!=-1) ok &= Cfg::setIncludeBus(!!b);
  b = getJsonBool(body,"includePass");    if (b!=-1) ok &= Cfg::setIncludePass(!!b);
  b = getJsonBool(body,"showDate");       if (b!=-1) ok &= Cfg::setShowDate(!!b);
  b = getJsonBool(body,"includeWeather"); if (b!=-1) ok &= Cfg::setIncludeWeather(!!b);
  b = getJsonBool(body,"autoUpdate");     if (b!=-1) ok &= Cfg::setAutoUpdate(!!b);

  // numbers
  long n;
  n = getJsonInt(body,"updateEvery");     if (n!=LONG_MIN) ok &= Cfg::setUpdateEvery((uint16_t)n);
  n = getJsonInt(body,"tickerMs");        if (n!=LONG_MIN) ok &= Cfg::setTickerMs((uint32_t)n);

  // screensaver
  String s1 = getJsonString(body,"ssStart");
  String s2 = getJsonString(body,"ssEnd");
  if (s1.length() || s2.length()) ok &= Cfg::setScreensaver(s1.c_str(), s2.c_str());

  // tube
  v = getJsonString(body,"line");         if (v.length() || findKey(body,"line")>=0) ok &= Cfg::setTubeLine(v.c_str());
  v = getJsonString(body,"direction");    if (v.length() || findKey(body,"direction")>=0) ok &= Cfg::setTubeDir(v.c_str());

  // Optional nested wifi { wifi: { ssid, pass } }
  if (findKey(body,"wifi")>=0){
    String ssid = getJsonString(body,"ssid");
    String pass = getJsonString(body,"pass");
    if (ssid.length()) ok &= Cfg::setWifi(ssid.c_str(), pass.c_str());
  }
  return ok;
}

static String buildTokenJSON(const char* token){
  String j("{\"token\":"); j += jsonEscape(token); j += '}';
  return j;
}

// =================== Arduino WebServer =============================
#if __has_include(<WebServer.h>)
#include <WebServer.h>
static void attachCommon(WebServer& srv){
  // Settings
  srv.on("/api/settings", HTTP_GET, [&](){
    srv.send(200, "application/json", buildSettingsJSON());
  });
  srv.on("/api/settings", HTTP_POST, [&](){
    bool ok = applySettingsFromJSON(srv.arg("plain"));

    // Respond first so the browser sees "saved"
    String body = ok ? buildSettingsJSON() : String("{\"err\":\"bad json\"}");
    int code = ok ? 200 : 400;
    srv.send(code, "application/json", body);

    // Then schedule a soft reboot shortly after
    if (ok) scheduleReboot(1200);
  });

  // Version (lightweight)
  srv.on("/api/version", HTTP_GET, [&](){
    srv.send(200, "application/json", "{\"version\":\"TRAKKR\",\"build\":1}");
  });

  // Tokens — Darwin (Rail), TfL, Weather (OpenWeather)
// --- WebServer token endpoints (explicit, no C++14) ---
  // Darwin (Rail)
  srv.on("/api/rail/token", HTTP_GET,  [&](){
    srv.send(200, "application/json", buildTokenJSON(Cfg::darwinToken()));
  });
  srv.on("/api/rail/token", HTTP_POST, [&](){
    String body = srv.arg("plain");
    int k = body.indexOf("\"token\""); int c = body.indexOf(':',k);
    int q1 = body.indexOf('"', c+1), q2 = body.indexOf('"', q1+1);
    String tok = (k<0||c<0||q1<0||q2<0) ? String() : body.substring(q1+1,q2);
    bool ok = Cfg::setDarwinToken(tok.c_str());
    srv.send(ok?200:400, "application/json", ok? buildTokenJSON(Cfg::darwinToken()) : "{\"err\":\"bad json\"}");
  });
  srv.on("/api/rail/token", HTTP_DELETE, [&](){
    Cfg::setDarwinToken("");
    srv.send(200, "application/json", buildTokenJSON(Cfg::darwinToken()));
  });

  // TfL
  srv.on("/api/tfl/token", HTTP_GET,  [&](){
    srv.send(200, "application/json", buildTokenJSON(Cfg::tflToken()));
  });
  srv.on("/api/tfl/token", HTTP_POST, [&](){
    String body = srv.arg("plain");
    int k = body.indexOf("\"token\""); int c = body.indexOf(':',k);
    int q1 = body.indexOf('"', c+1), q2 = body.indexOf('"', q1+1);
    String tok = (k<0||c<0||q1<0||q2<0) ? String() : body.substring(q1+1,q2);
    bool ok = Cfg::setTflToken(tok.c_str());
    srv.send(ok?200:400, "application/json", ok? buildTokenJSON(Cfg::tflToken()) : "{\"err\":\"bad json\"}");
  });
  srv.on("/api/tfl/token", HTTP_DELETE, [&](){
    Cfg::setTflToken("");
    srv.send(200, "application/json", buildTokenJSON(Cfg::tflToken()));
  });

  // OpenWeather
  srv.on("/api/weather/token", HTTP_GET,  [&](){
    srv.send(200, "application/json", buildTokenJSON(Cfg::weatherToken()));
  });
  srv.on("/api/weather/token", HTTP_POST, [&](){
    String body = srv.arg("plain");
    int k = body.indexOf("\"token\""); int c = body.indexOf(':',k);
    int q1 = body.indexOf('"', c+1), q2 = body.indexOf('"', q1+1);
    String tok = (k<0||c<0||q1<0||q2<0) ? String() : body.substring(q1+1,q2);
    bool ok = Cfg::setWeatherToken(tok.c_str());
    srv.send(ok?200:400, "application/json", ok? buildTokenJSON(Cfg::weatherToken()) : "{\"err\":\"bad json\"}");
  });
  srv.on("/api/weather/token", HTTP_DELETE, [&](){
    Cfg::setWeatherToken("");
    srv.send(200, "application/json", buildTokenJSON(Cfg::weatherToken()));
  });


  // Aliases / legacy
  srv.on("/api/token", HTTP_GET,  [&](){ srv.send(200,"application/json", buildTokenJSON(Cfg::darwinToken())); });
  srv.on("/api/token", HTTP_POST, [&](){ String b=srv.arg("plain"); int k=b.indexOf("\"token\""); int c=b.indexOf(':',k);
    int q1=b.indexOf('"',c+1), q2=b.indexOf('"',q1+1); String t=(k<0||c<0||q1<0||q2<0)?String():b.substring(q1+1,q2);
    bool ok=Cfg::setDarwinToken(t.c_str()); srv.send(ok?200:400,"application/json", ok? buildTokenJSON(Cfg::darwinToken()):"{\"err\":\"bad json\"}"); });

  // Stubs (so pages don't error)
  srv.on("/api/firmware/check", HTTP_POST, [&](){ srv.send(200,"application/json","{\"status\":\"noop\"}"); });
  srv.on("/api/reset-wifi",     HTTP_POST, [&](){ srv.send(200,"application/json","{\"status\":\"queued\"}"); });
  srv.on("/api/factory-reset",  HTTP_POST, [&](){ Cfg::resetToDefaults(); srv.send(200,"application/json","{\"status\":\"ok\"}"); });

}
void Api_attach(WebServer& srv){ attachCommon(srv); }
#endif

