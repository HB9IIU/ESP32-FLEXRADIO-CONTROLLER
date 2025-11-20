#include "HB9IIUWebConsoleLogger.h"

// ================== INTERNAL STATE ===================
static WebServer *g_server = nullptr;
static const char *g_consoleHTML = nullptr;

static const int LOG_LINES = 100;
static String logBuffer[LOG_LINES];
static int logIndex = 0;

// -------- Internal helpers --------
static void addLogLine(const String &line) {
  logBuffer[logIndex] = line;
  logIndex = (logIndex + 1) % LOG_LINES;
}

// Public logging function
void logPrintln(const String &msg) {
  Serial.println(msg);
  addLogLine(msg);
}

// ============= HTTP HANDLERS =====================
static void handleRoot() {
  if (!g_server || !g_consoleHTML) return;
  g_server->send(200, "text/html", g_consoleHTML);
}

static void handleLogs() {
  if (!g_server) return;

  String text;
  int idx = logIndex;
  for (int i = 0; i < LOG_LINES; i++) {
    int pos = (idx + i) % LOG_LINES;
    if (logBuffer[pos].length() > 0) {
      text += logBuffer[pos] + "\n";
    }
  }
  g_server->send(200, "text/plain", text);
}

static void handleRestart() {
  logPrintln("Web request: restart ESP");
  if (g_server) {
    g_server->send(200, "text/plain", "Restarting...");
  }
  delay(100);
  ESP.restart();
}

static void handleClearLogs() {
  logPrintln("Web request: clear logs");
  for (int i = 0; i < LOG_LINES; i++) {
    logBuffer[i] = "";
  }
  logIndex = 0;
  if (g_server) {
    g_server->send(200, "text/plain", "Logs cleared");
  }
}

static void handleNotFound() {
  if (!g_server) return;
  g_server->send(404, "text/plain", "Not found");
}

// ============= PUBLIC INIT FUNCTION ==============
void WebConsoleLogger_begin(WebServer &server, const char *htmlPage) {
  g_server = &server;
  g_consoleHTML = htmlPage;

  server.on("/", handleRoot);
  server.on("/logs", handleLogs);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/clearlogs", HTTP_POST, handleClearLogs);
  server.onNotFound(handleNotFound);
}
