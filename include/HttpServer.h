#pragma once
#include <Arduino.h>

void http_setup();   // [TRAKKR] Start Web + mDNS (requires Wi-Fi connected)
void http_loop();    // [TRAKKR] Service incoming HTTP requests

// ADD THIS:
void scheduleReboot(uint32_t delayMs = 1200);  // [TRAKKR] Request a delayed reboot
