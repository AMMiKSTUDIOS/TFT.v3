#pragma once
#include <Arduino.h>
#include "Global.h"

#if __has_include(<WebServer.h>)
  #include <WebServer.h>
  void Api_attach(WebServer& srv);
#endif
