#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "console_page.h"
// Initialize the web console logger:
// - registers HTTP routes: "/", "/logs", "/restart", "/clearlogs"
// - uses the provided HTML page as the main console page
void WebConsoleLogger_begin(WebServer &server, const char *htmlPage);

// Logging function to use instead of Serial.println():
void logPrintln(const String &msg);
