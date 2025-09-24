#include "HttpServer.h"
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include "Api.h"       // your API glue (adds /api/* routes)

//
// [TRAKKR] Reboot scheduling (used by /api/settings and /reboot)
//
static volatile bool     sRebootPending = false;
static uint32_t          sRebootAtMs    = 0;

void scheduleReboot(uint32_t delayMs){
  sRebootPending = true;
  sRebootAtMs    = millis() + (delayMs ? delayMs : 1);
}

// [TRAKKR] Single global server on port 80
static WebServer server(80);

// ---- Minimal static file server (LittleFS) ----
static String contentType(const String& path){
  if (path.endsWith(".htm")  || path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg")  || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".woff2"))return "font/woff2";
  if (path.endsWith(".ttf"))  return "font/ttf";
  if (path.endsWith(".otf"))  return "font/otf";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".txt"))  return "text/plain";
  return "application/octet-stream";
}

static bool tryServeFile(const String& path){
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  server.streamFile(f, contentType(path));
  f.close();
  return true;
}

// ---- Routes ----
void http_setup(){
  // mDNS for http://trakkr.local
  if (MDNS.begin("trakkr")) {
    MDNS.addService("http", "tcp", 80);
  }

  // Static pages
  server.on("/", HTTP_GET, [](){
    if (!tryServeFile("/index.htm")) server.send(404, "text/plain", "index.htm not found");
  });
  server.on("/token", HTTP_GET, [](){
    if (!tryServeFile("/token.htm")) server.send(404, "text/plain", "token.htm not found");
  });

  // --- NEW: explicit reboot endpoint for the UI ---
  server.on("/reboot", HTTP_POST, [](){
    // [TRAKKR] Respond first so the browser sees success, then reboot shortly after
    server.send(200, "application/json", "{\"status\":\"rebooting\"}");
    scheduleReboot(200);
  });

  // Generic static loader (so /app.js, /styles.css, images, etc. work)
  server.onNotFound([](){
    String uri = server.uri();
    // Basic security: only serve from root; no parent dirs
    if (uri.indexOf("..") >= 0) { server.send(400, "text/plain", "Bad path"); return; }
    if (!uri.startsWith("/")) uri = String("/") + uri;
    if (!tryServeFile(uri)) server.send(404, "text/plain", "Not found");
  });

  // API routes (index.htm + token.htm talk to these)
  Api_attach(server);

  server.begin();
}

void http_loop(){
  server.handleClient();

  // [TRAKKR] Execute any scheduled reboot AFTER we've had a chance to send responses
  if (sRebootPending && (int32_t)(millis() - sRebootAtMs) >= 0){
    sRebootPending = false;
    Serial.println("[TRAKKR] Rebooting to apply settings…");
    Serial.flush();
    delay(50);
    ESP.restart();
  }
}
