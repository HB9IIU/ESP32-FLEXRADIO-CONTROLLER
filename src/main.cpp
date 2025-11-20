/*
 * FlexControl WiFi – ESP32 SmartSDR Tuning Knob
 *
 * Author: Daniel (HB9IIU)
 * First release: November 2025
 *
 * I'm an amateur coder, so forgive the mess :)
 *
 * Contact: hb9iiu@gmail.com
 * (Replies can be very slow – I still have a day job and this is just a hobby.)
 *
 * You are free to use, modify and redistribute this code,
 * but please do NOT sell it. If you improve it, a mention
 * or pull request on GitHub would be very welcome.
 *
 * GitHub: https://github.com/HB9IIU/ESP32-FLEXRADIO-CONTROLLER
 */


#include <Arduino.h>
#include "HB9IIUWebConsoleLogger.h"
#include <HB9IIUportalConfigurator.h>
#include "HB9IIUOtaHelper.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// --- LEDS ---
const int PIN_LED_GREEN = 13;
const int PIN_LED_RED = 4;

// ---- VFO ENCODER (frequency) ----
const int PIN_ENC_A = 32;
const int PIN_ENC_B = 33;

// ---- FILTER ENCODER (preset 0..7) ----
const int PIN_FILT_A = 26;
const int PIN_FILT_B = 25;

// ---- VOLUME ENCODER ----
const int PIN_VOL_A = 14;
const int PIN_VOL_B = 27;

// ---- ENCODER CLICK BUTTONS (active-LOW with internal pull-ups) ----
const int PIN_ENC_BW_SW = 16;  // click of main (frequency) encoder
const int PIN_ENC_VOL_SW = 17; // click of volume encoder

// ---- HOLD DOWN FACTORY RESET -----
const int PIN_LED_RED_RESET = 4;
const int PIN_FACTORY_RESET_SW = 17;

const char *OTA_HOSTNAME = "flexcontroller"; // should match with platformio.ini

// When true, send debug output to the web terminal as well as Serial.
// Disable (set to false) if the app starts behaving unreliably.
bool webDebug = true;
String debugMessage;

const uint16_t CAT_PORT = 5002;

// --- discovery timeouts (fast) ---
const uint32_t TCP_CONNECT_TIMEOUT_MS = 150;
WebServer server(80);

// ===== LED BLINK TASK STUFF =====
TaskHandle_t ledBlinkTaskHandle = nullptr;
volatile bool ledBlinkActive = false;

// ---- FT8 frequencies (Hz) ----
const uint32_t FT8_40M_HZ = 7077000UL;  // 7.074 MHz normally but i wat to hear LSB (just to see if teher is activity)
const uint32_t FT8_20M_HZ = 14074000UL; // 14.074 MHz

// --- prototypes ----------------------------------------------------

// Blink task
void ledBlinkTask(void *parameter);
// Read CAT-line
bool readLine(String &out, uint32_t waitMs);
// Send VFO
bool sendFA(uint32_t hz);
// Send filter
bool sendFilterPreset(uint8_t idx);
// Read filter
void readFilterPresetOnce();
// Set volume
bool setVolumeA(uint8_t lvl);
// Read volume
int readVolumeA();
// Sync VFO
bool initialSyncFromRadio();
// Pump CAT
void pumpIncoming();
// Connect host
bool tryConnectHost(const IPAddress &host);
// Connect CAT
bool catConnect();
// Save CAT-host
void saveCurrentHostIfNeeded();
// Close network
void cleanCloseNet();
// Set frequency
void setFrequencyHz(uint32_t hz);
// FT8 40m
inline void setFT8_40m() { setFrequencyHz(FT8_40M_HZ); }
// FT8 20m
inline void setFT8_20m() { setFrequencyHz(FT8_20M_HZ); }
// Set mode
bool setMode(const String &mode);
// Get mode
int getMode(); // returns MD code or -1 on fail
// Set mode-code
bool setModeCode(int code); // send MDn; directly
// Set PTT
bool setPTT(bool on);
// Set RF-power
bool setPowerPct(uint8_t pct); // ZZPC 000..100
// Get RF-power
int getPowerPct(); // 0..100 or -1
// Cycle modes
void cycleModeSequence(); // USB -> LSB -> CW -> FM -> ...
// Start tune
void startTune(uint16_t ms = 1200, uint8_t tunePower = 10, const String &tuneMode = "AM");
// Service tune
void serviceTune();
// Startup banner
void printStartupHeader();
// Mute toggle
void muteUnmute();
// Update GREEN
void updateGreenLed();
// Reboot ESP
void rebootESP();

//---------------------------------------------------------------------------------------------------------------------

void ledBlinkTask(void *parameter)
{
  pinMode(PIN_LED_RED_RESET, OUTPUT);
  bool state = false;

  for (;;)
  {
    if (ledBlinkActive)
    {
      digitalWrite(PIN_LED_RED_RESET, state ? HIGH : LOW);
      state = !state;
      vTaskDelay(pdMS_TO_TICKS(200)); // blink every 200 ms
    }
    else
    {
      // make sure LED is off when not active
      digitalWrite(PIN_LED_RED_RESET, LOW);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

//-------------------------------------------------------------------

// ---- VOLUME STEP (percent per detent) ----
const int VOLUME_STEP = 5; // change to 2/5/10 as you like

#define ENC_INPUT_MODE INPUT_PULLUP

// Frequency step/behavior
const int32_t STEP_HZ = 1;
const uint32_t SEND_INTERVAL_MS = 60;
const uint32_t RESYNC_MS = 800;
const uint32_t ACCEL_T1_MS = 35;
const uint32_t ACCEL_T2_MS = 80;

// ---- TTP223 TOUCH PINS (active-HIGH, idle LOW) ----
const int PIN_TOUCH1 = 23;
const int PIN_TOUCH2 = 22;
const int PIN_TOUCH3 = 21;
const int PIN_TOUCH4 = 19;
const int PIN_TOUCH5 = 18;

// Simple debounce
const uint16_t TOUCH_DEBOUNCE_MS = 100;
bool touchLast1 = false, touchLast2 = false, touchLast3 = false, touchLast4 = false, touchLast5 = false;
uint32_t touchT1 = 0, touchT2 = 0, touchT3 = 0, touchT4 = 0, touchT5 = 0;

// Encoder click debounce (active-LOW)
const uint16_t CLICK_DEBOUNCE_MS = 50;
bool clickLastBW = false, clickLastVol = false; // "pressed" state (LOW) after inversion
uint32_t clickTBW = 0, clickTVol = 0;

WiFiClient cat;
Preferences prefs;
IPAddress currentHost;

uint32_t vfoHz = 14110000, lastSentHz = vfoHz;

// ---------- Quadrature decoder (ISR) : MAIN ----------
static const int8_t QDEC_TAB[16] = {
    0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0};
volatile uint8_t vfo_q_last = 0;
volatile int32_t q_edges = 0;
volatile uint32_t lastDetentMs = 0;
volatile int32_t detentPending = 0;
volatile bool needResetEncoderBaseline = false;

inline uint8_t fastReadAB()
{
  return (uint8_t(digitalRead(PIN_ENC_A)) << 1) | uint8_t(digitalRead(PIN_ENC_B));
}

void IRAM_ATTR encISR()
{
  uint8_t now = fastReadAB();
  uint8_t idx = (vfo_q_last << 2) | now;
  int8_t d = QDEC_TAB[idx];
  if (d)
  {
    q_edges += d;
    detentPending += d;
    if (detentPending >= 4 || detentPending <= -4)
    {
      lastDetentMs = millis();
      detentPending = 0;
    }
  }
  vfo_q_last = now;
}

// ---------- FILTER ENCODER ----------
volatile uint8_t f_q_last = 0;
volatile int32_t f_edges = 0;
int8_t filterIdx = 0; // 0..7

inline uint8_t fastReadAB_filt()
{
  return (uint8_t(digitalRead(PIN_FILT_A)) << 1) | uint8_t(digitalRead(PIN_FILT_B));
}

void IRAM_ATTR filtISR()
{
  uint8_t now = fastReadAB_filt();
  uint8_t idx = (f_q_last << 2) | now;
  int8_t d = QDEC_TAB[idx];
  if (d)
    f_edges += d;
  f_q_last = now;
}
// ---- LED helpers ----
inline void ledsOff()
{
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED, LOW);
}
inline void ledGreenSolid()
{
  digitalWrite(PIN_LED_GREEN, HIGH);
  digitalWrite(PIN_LED_RED, LOW);
}
inline void ledRedSolid()
{
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED, HIGH);
}

// Blink RED/GREEN alternately for durationMs (used instead of blocking delays)
void blinkAlt(uint32_t durationMs, uint16_t periodMs = 150)
{
  uint32_t t0 = millis();
  bool phase = false;
  while (millis() - t0 < durationMs)
  {
    // phase=false -> RED on, GREEN off; phase=true -> GREEN on, RED off
    digitalWrite(PIN_LED_RED, phase ? LOW : HIGH);
    digitalWrite(PIN_LED_GREEN, phase ? HIGH : LOW);
    phase = !phase;
    delay(periodMs); // replaces the old blocking delay(...)
  }
  ledsOff(); // stop blinking after the timed window
}

// --- non-blocking GREEN flash state ---
bool redFlashActive = false;
uint32_t redFlashUntil = 0;

// --- non-blocking GREEN mute-blink state ---
bool greenBlinkOn = false;
uint32_t greenBlinkLastToggle = 0;
const uint16_t GREEN_BLINK_PERIOD_MS = 400; // blink period for mute indication

// Call this to flash RED for ms (default ~120 ms)
inline void flashRedLed(uint16_t ms = 120)
{
  redFlashActive = true;
  redFlashUntil = millis() + ms;
  digitalWrite(PIN_LED_RED, HIGH); // start flash
}

// ---------- VOLUME ENCODER ----------
volatile uint8_t v_q_last = 0;
volatile int32_t v_edges = 0;
int16_t volumePct = 50; // 0..100
bool isMuted = false;
int16_t muteRestoreVolume = 50; // last non-zero volume to restore

inline uint8_t fastReadAB_vol()
{
  return (uint8_t(digitalRead(PIN_VOL_A)) << 1) | uint8_t(digitalRead(PIN_VOL_B));
}

void IRAM_ATTR volISR()
{
  uint8_t now = fastReadAB_vol();
  uint8_t idx = (v_q_last << 2) | now;
  int8_t d = QDEC_TAB[idx];
  if (d)
    v_edges += d;
  v_q_last = now;
}

// ---------- Discovery helpers ----------
static bool tryConnectQuick(IPAddress host)
{
  WiFiClient probe;
  bool ok = probe.connect(host, CAT_PORT, TCP_CONNECT_TIMEOUT_MS);
  probe.stop();
  return ok;
}
static bool scanFirstOpen(IPAddress &found)
{
  IPAddress me = WiFi.localIP();
  IPAddress net(me[0], me[1], me[2], 0);
  delay(100);
  for (int last = 1; last <= 254; last++)
  {
    if (last == (int)me[3])
      continue;
    IPAddress ip(net[0], net[1], net[2], last);
    Serial.print("Trying IP: ");
    Serial.println(ip);
    digitalWrite(PIN_LED_RED, HIGH);
    digitalWrite(PIN_LED_GREEN, LOW);
    if (tryConnectQuick(ip))
    {
      found = ip;
      return true;
      ledsOff();
    }
    delay(50);
    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_GREEN, HIGH);
    delay(80);
  }
  ledsOff();
  return false;
}
// ---------- FlexRadio Discovery ----------
bool tryConnectHost(const IPAddress &host)
{
  if (cat.connected())
    cat.stop();
  delay(120);

  const uint16_t backoff[] = {600, 1200, 2000, 3500};

  for (uint8_t i = 0; i < 4; i++)
  {
    // show a quick alternating blink to indicate an active attempt
    digitalWrite(PIN_LED_RED, HIGH);
    digitalWrite(PIN_LED_GREEN, LOW);
    delay(120);
    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_GREEN, HIGH);
    delay(120);
    ledsOff();

    Serial.printf("[CAT] Connecting %s:%u (try %u/4)\n",
                  host.toString().c_str(), CAT_PORT, i + 1);

    if (cat.connect(host, CAT_PORT, TCP_CONNECT_TIMEOUT_MS))
    {
      cat.setNoDelay(true);
      cat.setTimeout(1200);
      Serial.println("[CAT] Connected.");
      ledGreenSolid(); // ✅ solid green when CAT is up
      return true;
    }

    // connection failed -> alternate-blink during backoff window
    blinkAlt(backoff[i]); // ⏳ replaces delay(backoff[i])
  }

  // all tries failed
  ledRedSolid(); // ❌ solid red (you reboot after this anyway)
  return false;
}
bool catConnect()
{
  String cached = prefs.getString("host", "");
  if (cached.length())
  {
    IPAddress ip;
    if (ip.fromString(cached))
    {
      Serial.printf("[CACHE] Trying cached host: %s\n", cached.c_str());

      if (tryConnectHost(ip))
      {
        currentHost = ip;
        return true;
      }
      Serial.println("[CACHE] Cached host failed.");
    }
  }

  Serial.println("[SCAN] Scanning subnet for CAT (TCP 5002) ...");
  IPAddress found;
  if (scanFirstOpen(found))
  {
    Serial.printf("[SCAN] Found CAT at %s\n", found.toString().c_str());
    if (tryConnectHost(found))
    {
      currentHost = found;
      return true;
    }
  }
  Serial.println("[SCAN] No CAT found.");
  return false;
}
void saveCurrentHostIfNeeded()
{
  String cached = prefs.getString("host", "");
  String nowStr = currentHost.toString();
  if (cached != nowStr)
  {
    prefs.putString("host", nowStr);
    Serial.printf("[SAVE] Stored CAT host: %s\n", nowStr.c_str());
  }
}
// ---------------------------------------

bool readLine(String &out, uint32_t waitMs)
{
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs)
  {
    if (cat.available())
    {
      String s = cat.readStringUntil(';');
      if (s.length())
      {
        out = s + ';';
        return true;
      }
    }
    delay(2);
    yield();
  }
  return false;
}

bool sendFA(uint32_t hz)
{
  if (!cat.connected())
    return false;
  char digits[16];
  snprintf(digits, sizeof(digits), "%011u", hz);
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "FA%s;", digits);
  Serial.print(">> ");
  Serial.println(cmd);
  if (webDebug)
  {
    String debugMessage = String(">> ") + cmd;
    logPrintln(debugMessage);
  }
  return cat.print(cmd) == (int)strlen(cmd);
}

// ----- Filter preset (ZZFI) -----
bool sendFilterPreset(uint8_t idx)
{
  if (!cat.connected())
  {
    Serial.printf("[FILT] Cannot set filter preset to %u – CAT not connected.\n", idx);
    if (webDebug)
    {
      String msg = "[FILT] Cannot set filter preset to ";
      msg += String(idx);
      msg += " – CAT not connected.";
      logPrintln(msg);
    }
    return false;
  }

  uint8_t requested = idx;
  if (idx > 7)
    idx = 7;

  if (idx != requested)
  {
    Serial.printf("[FILT] Requested preset %u, clamped to %u (valid range 0–7).\n", requested, idx);
    if (webDebug)
    {
      String msg = "[FILT] Requested preset ";
      msg += String(requested);
      msg += ", clamped to ";
      msg += String(idx);
      msg += " (valid range 0–7).";
      logPrintln(msg);
    }
  }

  char cmd[16];
  snprintf(cmd, sizeof(cmd), "ZZFI%02u;", idx);

  // High-level intent
  Serial.printf("[FILT] Setting filter preset index to %u (%s)\n", idx, cmd);
  if (webDebug)
  {
    String msg = "[FILT] Setting filter preset index to ";
    msg += String(idx);
    msg += " (";
    msg += cmd;
    msg += ")";
    logPrintln(msg);
  }

  // Raw CAT command
  Serial.print(">> ");
  Serial.println(cmd);
  if (webDebug)
  {
    String debugMessage = ">> ";
    debugMessage += cmd;
    logPrintln(debugMessage);
  }

  bool ok = (cat.print(cmd) == (int)strlen(cmd));
  if (!ok)
  {
    Serial.println("[FILT] ERROR: Failed to send ZZFI command over CAT.");
    if (webDebug)
      logPrintln("[FILT] ERROR: Failed to send ZZFI command over CAT.");
  }
  else
  {
    Serial.println("[FILT] Filter preset command sent successfully.");
    if (webDebug)
      logPrintln("[FILT] Filter preset command sent successfully.");
  }

  return ok;
}

void readFilterPresetOnce()
{
  if (!cat.connected())
    return;
  cat.print("ZZFI;");
  String line;
  if (readLine(line, 800) && line.startsWith("ZZFI") && line.endsWith(";") && line.length() >= 6)
  {
    filterIdx = line.substring(4, line.length() - 1).toInt();
    if (filterIdx < 0)
      filterIdx = 0;
    if (filterIdx > 7)
      filterIdx = 7;
    Serial.printf("[FILTER] Current preset = %d\n", filterIdx);
    if (webDebug)
    {
      String debugMessage = "[FILTER] Current preset = " + String(filterIdx) + "\n";
      logPrintln(debugMessage);
    }
  }
  else
  {
    Serial.println("[FILTER] No reply; defaulting to 0");

    if (webDebug)
    {
      logPrintln("[FILTER] No reply; defaulting to 0");
    }

    filterIdx = 0;
  }
}

// ----- Volume (Flex ZZAGnnn; 000..100) -----
bool setVolumeA(uint8_t lvl)
{
  if (!cat.connected())
  {
    Serial.printf("[VOL] Cannot set volume to %u%% – CAT not connected.\n", lvl);
    if (webDebug)
    {
      String msg = "[VOL] Cannot set volume to ";
      msg += String(lvl);
      msg += "% – CAT not connected.";
      logPrintln(msg);
    }
    return false;
  }

  uint8_t requested = lvl;
  if (lvl > 100)
    lvl = 100;

  if (lvl != requested)
  {
    Serial.printf("[VOL] Requested %u%%, clamped to %u%%.\n", requested, lvl);
    if (webDebug)
    {
      String msg = "[VOL] Requested ";
      msg += String(requested);
      msg += "%, clamped to ";
      msg += String(lvl);
      msg += "%.";
      logPrintln(msg);
    }
  }

  char cmd[16];
  snprintf(cmd, sizeof(cmd), "ZZAG%03u;", lvl);

  // High-level intent
  Serial.printf("[VOL] Setting AF gain to %u%% (%s)\n", lvl, cmd);
  if (webDebug)
  {
    String msg = "[VOL] Setting AF gain to ";
    msg += String(lvl);
    msg += "% (";
    msg += cmd;
    msg += ")";
    logPrintln(msg);
  }

  // Raw CAT command
  Serial.print(">> ");
  Serial.println(cmd);
  if (webDebug)
  {
    String debugMessage = ">> ";
    debugMessage += cmd;
    logPrintln(debugMessage);
  }

  bool ok = (cat.print(cmd) == (int)strlen(cmd));
  if (!ok)
  {
    Serial.println("[VOL] ERROR: Failed to send ZZAG command over CAT.");
    if (webDebug)
      logPrintln("[VOL] ERROR: Failed to send ZZAG command over CAT.");
  }
  else
  {
    Serial.println("[VOL] Volume command sent successfully.");
    if (webDebug)
      logPrintln("[VOL] Volume command sent successfully.");
  }

  return ok;
}

int readVolumeA()
{
  // returns 0..100 or -1 on fail
  if (!cat.connected())
  {
    Serial.println("[VOL] Cannot read volume – CAT not connected.");
    if (webDebug)
      logPrintln("[VOL] Cannot read volume – CAT not connected.");
    return -1;
  }

  // Log query
  Serial.println("[VOL] Querying current AF gain (ZZAG;)");
  if (webDebug)
    logPrintln(">> ZZAG;");

  // Send query
  cat.print("ZZAG;");

  String line;
  if (!readLine(line, 800))
  {
    Serial.println("[VOL] No reply to ZZAG; within 800 ms.");
    if (webDebug)
      logPrintln("[VOL] No reply to ZZAG; within 800 ms.");
    return -1; // timeout
  }

  // Log raw reply
  Serial.print("[VOL] Raw reply: ");
  Serial.println(line);
  if (webDebug)
  {
    String debugMessage = "<< " + line;
    logPrintln(debugMessage);
  }

  // Expect "ZZAGnnn;"
  if (!line.startsWith("ZZAG") || !line.endsWith(";"))
  {
    Serial.println("[VOL] Unexpected reply format (expected 'ZZAGnnn;').");
    if (webDebug)
      logPrintln("[VOL] Unexpected reply format (expected 'ZZAGnnn;').");
    return -1;
  }

  String numStr = line.substring(4, line.length() - 1);
  int value = numStr.toInt();

  if (value < 0 || value > 100)
  {
    Serial.printf("[VOL] Parsed volume out of range: %d (from '%s')\n", value, numStr.c_str());
    if (webDebug)
    {
      String msg = "[VOL] Parsed volume out of range: ";
      msg += String(value);
      msg += " (from '";
      msg += numStr;
      msg += "')";
      logPrintln(msg);
    }
    return -1;
  }

  Serial.printf("[VOL] Parsed current AF gain: %d%%\n", value);
  if (webDebug)
  {
    String msg = "[VOL] Parsed current AF gain: ";
    msg += String(value);
    msg += "%";
    logPrintln(msg);
  }

  return value;
}

// ----- Sync VFO from radio -----
bool initialSyncFromRadio()
{
  if (!cat.connected())
    return false;
  cat.print("FA;");
  String line;
  if (!readLine(line, 1500))
    return false;
  if (!line.startsWith("FA") || line.length() < 14)
    return false;
  String d = line.substring(2, 13);
  vfoHz = (uint32_t)d.toInt();
  lastSentHz = vfoHz;
  noInterrupts();
  q_edges = 0;
  detentPending = 0;
  vfo_q_last = fastReadAB();
  interrupts();
  Serial.printf("[SYNC] Start at %.6f MHz\n", vfoHz / 1e6);

  if (webDebug)
  {
    String debugMessage = "[SYNC] Start at ";
    debugMessage += String(vfoHz / 1e6, 6); // 6 decimal places
    debugMessage += " MHz";
    logPrintln(debugMessage); // assumes it adds newline
  }

  return true;
}

// ----- Incoming CAT pump -----
void pumpIncoming()
{
  while (cat.connected() && cat.available())
  {
    String s = cat.readStringUntil(';');
    if (!s.length())
      break;

    s += ';'; // restore the ';' so we see the full CAT command

    if (s == "?;")
    {
      // Ignore but still show/log it
      Serial.println("<< ?; (ignored)");
      if (webDebug)
      {
        logPrintln("<< ?; (ignored)");
      }
    }
    else if (s.startsWith("FA") && s.length() >= 14)
    {
      // FA + 11 digits + ';'
      String d = s.substring(2, 13); // frequency digits only
      uint32_t rxHz = (uint32_t)d.toInt();

      // Log the raw CAT line
      Serial.print("<< ");
      Serial.println(s);
      if (webDebug)
      {
        String debugMessage = "<< " + s;
        logPrintln(debugMessage);
      }

      if (rxHz != vfoHz)
      {
        vfoHz = rxHz;
        lastSentHz = rxHz;
        needResetEncoderBaseline = true;

        Serial.printf("[EXT] Radio → %.6f MHz (sync)\n", vfoHz / 1e6);
        if (webDebug)
        {
          String debugMessage = "[EXT] Radio → ";
          debugMessage += String(vfoHz / 1e6, 6); // 6 decimal places, MHz
          debugMessage += " MHz (sync)";
          logPrintln(debugMessage);
        }
      }
    }
    else
    {
      // Any other CAT line
      Serial.print("<< ");
      Serial.println(s);

      if (webDebug)
      {
        String debugMessage = "<< " + s;
        logPrintln(debugMessage);
      }
    }
  }
}

// ====== SIMPLE ACTIONS ======
void setFrequencyHz(uint32_t hz)
{
  vfoHz = hz;

  if (cat.connected())
  {
    if (sendFA(vfoHz))
    {
      lastSentHz = vfoHz;
    }
    else
    {
      Serial.println("[CAT] Send failed; stopping socket.");
      if (webDebug)
      {
        logPrintln("[CAT] Send failed; stopping socket.");
      }
      cat.stop();
    }
  }

  needResetEncoderBaseline = true;

  Serial.printf("[ACTION] VFO set to %.6f MHz\n", vfoHz / 1e6);
  if (webDebug)
  {
    String debugMessage = "[ACTION] VFO set to ";
    debugMessage += String(vfoHz / 1e6, 6); // 6 decimal places, MHz
    debugMessage += " MHz";
    logPrintln(debugMessage); // newline added by logPrintln()
  }
}

// --- implementation (place with your CAT helpers)
static int mdCodeFromString(String m)
{
  m.toUpperCase();
  if (m == "LSB")
    return 1;
  if (m == "USB")
    return 2;
  if (m == "CW")
    return 3;
  if (m == "FM")
    return 4; // NFM/DFM/FDV family
  if (m == "AM" || m == "SAM")
    return 5;
  if (m == "DIGL" || m == "RTTY")
    return 6;
  if (m == "DIGU")
    return 9;
  return -1;
}
bool setMode(const String &mode)
{
  // 1) Guard: CAT must be connected
  if (!cat.connected())
  {
    Serial.printf("[MD] Cannot set mode to '%s' – CAT not connected.\n", mode.c_str());
    if (webDebug)
    {
      String msg = "[MD] Cannot set mode to '";
      msg += mode;
      msg += "' – CAT not connected.";
      logPrintln(msg);
    }
    return false;
  }

  // 2) Map string (USB/LSB/CW/FM/...) -> MD code
  int code = mdCodeFromString(mode);
  if (code < 0)
  {
    Serial.printf("[MD] Requested mode '%s' is not mapped to any MD code. Ignoring.\n", mode.c_str());
    if (webDebug)
    {
      String msg = "[MD] Requested mode '";
      msg += mode;
      msg += "' is not mapped to any MD code. Ignoring.";
      logPrintln(msg);
    }
    return false;
  }

  // 3) Build CAT command (MDn;)
  char cmd[12];
  snprintf(cmd, sizeof(cmd), "MD%d;", code);

  // 4) Log intent *before* sending
  Serial.printf("[MD] Setting mode to '%s' (MD%d)\n", mode.c_str(), code);
  Serial.print(">> ");
  Serial.println(cmd);

  if (webDebug)
  {
    String msg = "[MD] Setting mode to '";
    msg += mode;
    msg += "' (MD";
    msg += String(code);
    msg += ")";
    logPrintln(msg);

    String raw = ">> ";
    raw += cmd;
    logPrintln(raw); // raw CAT command line
  }

  // 5) Send and log result
  bool ok = (cat.print(cmd) == (int)strlen(cmd));
  if (!ok)
  {
    Serial.println("[MD] ERROR: Failed to send MD command over CAT.");
    if (webDebug)
      logPrintln("[MD] ERROR: Failed to send MD command over CAT.");
  }
  else
  {
    Serial.println("[MD] Mode command sent successfully.");
    if (webDebug)
      logPrintln("[MD] Mode command sent successfully.");
  }

  return ok;
}

int getMode()
{ // reads current mode (MDn) -> n
  if (!cat.connected())
    return -1;

  // Optional: log the query when webDebug is on
  if (webDebug)
  {
    logPrintln(">> MD;");
  }

  cat.print("MD;");
  String line;
  if (!readLine(line, 800))
    return -1; // expect "MDn;"

  if (!line.startsWith("MD") || !line.endsWith(";"))
    return -1;

  if (webDebug)
  {
    String debugMessage = "<< " + line;
    logPrintln(debugMessage);
  }

  return line.substring(2, line.length() - 1).toInt();
}

bool setPTT(bool on)
{
  if (!cat.connected())
  {
    Serial.printf("[PTT] Cannot set PTT %s – CAT not connected.\n", on ? "ON" : "OFF");
    if (webDebug)
    {
      String msg = "[PTT] Cannot set PTT ";
      msg += on ? "ON" : "OFF";
      msg += " – CAT not connected.";
      logPrintln(msg);
    }
    return false;
  }

  const char *cmd = on ? "ZZTX1;" : "ZZTX0;";

  // High-level intent log
  Serial.printf("[PTT] Setting PTT %s (%s)\n", on ? "ON" : "OFF", cmd);
  if (webDebug)
  {
    String msg = "[PTT] Setting PTT ";
    msg += on ? "ON" : "OFF";
    msg += " (";
    msg += cmd;
    msg += ")";
    logPrintln(msg);
  }

  // Raw CAT command log
  Serial.print(">> ");
  Serial.println(cmd);
  if (webDebug)
  {
    String debugMessage = ">> ";
    debugMessage += cmd;
    logPrintln(debugMessage); // logPrintln adds newline
  }

  bool ok = (cat.print(cmd) == (int)strlen(cmd));
  if (!ok)
  {
    Serial.println("[PTT] ERROR: Failed to send ZZTX command over CAT.");
    if (webDebug)
      logPrintln("[PTT] ERROR: Failed to send ZZTX command over CAT.");
  }
  else
  {
    Serial.println("[PTT] PTT command sent successfully.");
    if (webDebug)
      logPrintln("[PTT] PTT command sent successfully.");
  }

  return ok;
}

// Cleanly close CAT + Wi-Fi before rebooting (very small + safe)
inline void cleanCloseNet()
{
  if (cat.connected())
  {
    cat.stop(); // closes TCP with FIN
    delay(50);
  }
  WiFi.disconnect(true, true); // drop STA and forget current link
  delay(100);
}

// --- RF Power via ZZPC (SmartSDR CAT)
bool setPowerPct(uint8_t pct)
{
  if (!cat.connected())
  {
    Serial.printf("[PWR] Cannot set power to %u%% – CAT not connected.\n", pct);
    if (webDebug)
    {
      String msg = "[PWR] Cannot set power to ";
      msg += String(pct);
      msg += "% – CAT not connected.";
      logPrintln(msg);
    }
    return false;
  }

  // Clamp to valid range 0..100
  uint8_t requested = pct;
  if (pct > 100)
    pct = 100;

  if (pct != requested)
  {
    Serial.printf("[PWR] Requested %u%%, clamped to %u%%.\n", requested, pct);
    if (webDebug)
    {
      String msg = "[PWR] Requested ";
      msg += String(requested);
      msg += "%, clamped to ";
      msg += String(pct);
      msg += "%.";
      logPrintln(msg);
    }
  }

  // Build command ZZPCnnn;
  char cmd[12];
  snprintf(cmd, sizeof(cmd), "ZZPC%03u;", pct);

  // High-level intent
  Serial.printf("[PWR] Setting RF power to %u%% (%s)\n", pct, cmd);
  if (webDebug)
  {
    String msg = "[PWR] Setting RF power to ";
    msg += String(pct);
    msg += "% (";
    msg += cmd;
    msg += ")";
    logPrintln(msg);
  }

  // Raw CAT command
  Serial.print(">> ");
  Serial.println(cmd);
  if (webDebug)
  {
    String debugMessage = ">> ";
    debugMessage += cmd;
    logPrintln(debugMessage); // logPrintln adds newline
  }

  bool ok = (cat.print(cmd) == (int)strlen(cmd));
  if (!ok)
  {
    Serial.println("[PWR] ERROR: Failed to send ZZPC command over CAT.");
    if (webDebug)
      logPrintln("[PWR] ERROR: Failed to send ZZPC command over CAT.");
  }
  else
  {
    Serial.println("[PWR] Power command sent successfully.");
    if (webDebug)
      logPrintln("[PWR] Power command sent successfully.");
  }

  return ok;
}

int getPowerPct()
{
  // returns 0..100 or -1 on fail
  if (!cat.connected())
  {
    Serial.println("[PWR] Cannot read power – CAT not connected.");
    if (webDebug)
      logPrintln("[PWR] Cannot read power – CAT not connected.");
    return -1;
  }

  // Log query
  Serial.println("[PWR] Querying current RF power (ZZPC;)");
  if (webDebug)
    logPrintln(">> ZZPC;");

  // Send query
  cat.print("ZZPC;");

  String line;
  if (!readLine(line, 800))
  {
    Serial.println("[PWR] No reply to ZZPC; within 800 ms.");
    if (webDebug)
      logPrintln("[PWR] No reply to ZZPC; within 800 ms.");
    return -1; // timeout
  }

  // Log raw reply
  Serial.print("[PWR] Raw reply: ");
  Serial.println(line);
  if (webDebug)
  {
    String debugMessage = "<< " + line;
    logPrintln(debugMessage);
  }

  // Expect "ZZPCnnn;"
  if (!line.startsWith("ZZPC") || !line.endsWith(";"))
  {
    Serial.println("[PWR] Unexpected reply format (expected 'ZZPCnnn;').");
    if (webDebug)
      logPrintln("[PWR] Unexpected reply format (expected 'ZZPCnnn;').");
    return -1;
  }

  String numStr = line.substring(4, line.length() - 1);
  int value = numStr.toInt();

  if (value < 0 || value > 100)
  {
    Serial.printf("[PWR] Parsed power value out of range: %d (from '%s')\n", value, numStr.c_str());
    if (webDebug)
    {
      String msg = "[PWR] Parsed power value out of range: ";
      msg += String(value);
      msg += " (from '";
      msg += numStr;
      msg += "')";
      logPrintln(msg);
    }
    // still return the raw value so caller can see the problem
    return -1;
  }

  Serial.printf("[PWR] Parsed current RF power: %d%%\n", value);
  if (webDebug)
  {
    String msg = "[PWR] Parsed current RF power: ";
    msg += String(value);
    msg += "%";
    logPrintln(msg);
  }

  return value;
}

// --- Mode by code (MDn;) so we can restore without mapping back to a string
bool setModeCode(int code)
{
  if (!cat.connected())
  {
    Serial.printf("[MD] Cannot set mode code MD%d – CAT not connected.\n", code);
    if (webDebug)
    {
      String msg = "[MD] Cannot set mode code MD";
      msg += String(code);
      msg += " – CAT not connected.";
      logPrintln(msg);
    }
    return false;
  }

  // Optional: map known codes to human-readable names
  const char *name = nullptr;
  switch (code)
  {
  case 1:
    name = "LSB";
    break;
  case 2:
    name = "USB";
    break;
  case 3:
    name = "CW";
    break;
  case 4:
    name = "FM";
    break;
  case 5:
    name = "AM";
    break;
  case 6:
    name = "DIGL";
    break;
  case 9:
    name = "DIGU";
    break;
  default:
    name = "UNKNOWN";
    break;
  }

  char cmd[12];
  snprintf(cmd, sizeof(cmd), "MD%d;", code);

  // High-level intent log
  if (name && strcmp(name, "UNKNOWN") != 0)
    Serial.printf("[MD] Setting mode by code: MD%d (%s)\n", code, name);
  else
    Serial.printf("[MD] Setting mode by code: MD%d (name unknown)\n", code);

  if (webDebug)
  {
    String msg = "[MD] Setting mode by code: MD";
    msg += String(code);
    msg += " (";
    msg += name;
    msg += ")";
    logPrintln(msg);
  }

  // Raw CAT command log
  Serial.print(">> ");
  Serial.println(cmd);
  if (webDebug)
  {
    String debugMessage = ">> ";
    debugMessage += cmd;
    logPrintln(debugMessage); // logPrintln adds newline
  }

  bool ok = (cat.print(cmd) == (int)strlen(cmd));
  if (!ok)
  {
    Serial.println("[MD] ERROR: Failed to send MDn command over CAT.");
    if (webDebug)
      logPrintln("[MD] ERROR: Failed to send MDn command over CAT.");
  }
  else
  {
    Serial.println("[MD] Mode code command sent successfully.");
    if (webDebug)
      logPrintln("[MD] Mode code command sent successfully.");
  }

  return ok;
}

void cycleModeSequence()
{
  if (!cat.connected())
  {
    Serial.println("[MODE] Cycle ignored (CAT not connected)");
    if (webDebug)
      logPrintln("[MODE] Cycle ignored (CAT not connected)");
    return;
  }

  // Mode cycle order: USB -> LSB - ...
  const int modes[] = {1,2}; // MD codes
  const char *names[] = {"USB", "LSB"};
  const size_t numModes = sizeof(modes) / sizeof(modes[0]);

  // --- Read current mode from radio ---
  int currentCode = getMode(); // MDn;
  size_t idx = 0;

  if (currentCode > 0)
  {
    bool found = false;
    for (size_t i = 0; i < numModes; ++i)
    {
      if (modes[i] == currentCode)
      {
        idx = i;
        found = true;
        break;
      }
    }
    if (!found)
    {
      // If current mode is not in our list, start from first (USB)
      idx = 0;
    }
  }
  else
  {
    // If getMode() failed, start from first in sequence
    idx = 0;
  }

  size_t nextIdx = (idx + 1) % numModes;
  int nextCode = modes[nextIdx];

  // --- Actually change mode on the radio ---
  if (setModeCode(nextCode))
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "[MODE] Cycle -> %s (MD%d)", names[nextIdx], nextCode);
    Serial.println(buf);
    if (webDebug)
      logPrintln(String(buf));

    // 🛑 SAFETY BELT: force RX AFTER the mode change has taken effect
    // Give SmartSDR a moment to do its internal shenanigans, then send ZZTX0;
    delay(120); // small, but explicit — you *will* see it in timing/logs

    Serial.println("[MODE/PTT] Forcing RX after mode change (ZZTX0;)");
    if (webDebug)
      logPrintln("[MODE/PTT] Forcing RX after mode change (ZZTX0;)");

    setPTT(false); // sends ZZTX0; and logs [PTT]... if it actually runs
  }
  else
  {
    Serial.println("[MODE] Failed to set mode in cycle");
    if (webDebug)
      logPrintln("[MODE] Failed to set mode in cycle");
  }
}

// ---- TUNE state ----
bool tuneActive = false;
uint32_t tuneUntilMs = 0;
int savedModeCode = -1;
int savedPowerPct = -1;

void startTune(uint16_t ms, uint8_t tunePower, const String &tuneMode)
{
  if (tuneActive || !cat.connected())
    return;

  // snapshot current settings
  savedModeCode = getMode();     // MD?;
  savedPowerPct = getPowerPct(); // ZZPC?;
  if (savedModeCode < 0)
    savedModeCode = -1;
  if (savedPowerPct < 0)
    savedPowerPct = -1;

  // prep carrier: set AM (or your choice) and low drive
  setMode(tuneMode);
  setPowerPct(tunePower);

  // key MOX
  setPTT(true);
  digitalWrite(PIN_LED_RED, HIGH); // show TX

  tuneUntilMs = millis() + ms;
  tuneActive = true;
}

void serviceTune()
{
  if (!tuneActive)
    return;
  if ((int32_t)(millis() - tuneUntilMs) < 0)
    return;

  // time’s up: unkey and restore
  setPTT(false);
  if (savedModeCode >= 0)
    setModeCode(savedModeCode);
  if (savedPowerPct >= 0)
    setPowerPct((uint8_t)savedPowerPct);
  digitalWrite(PIN_LED_RED, LOW);
  tuneActive = false;
}
void printStartupHeader()
{

  logPrintln(F("+------------------------------------------------------------+"));
  logPrintln(F("|                                                            |"));
  logPrintln(F("|                ESP32 SmartSDR Tuning Knob                  |"));
  logPrintln(F("|                            by                              |"));
  logPrintln(F("|                          HB9IIU                            |"));
  logPrintln(F("|                      November 2025                         |"));
  logPrintln(F("|                                                            |"));
  logPrintln(F("+------------------------------------------------------------+"));
}

void muteUnmute()
{
  if (!isMuted)
  {
    // going to MUTE
    if (volumePct > 0)
      muteRestoreVolume = volumePct; // remember last useful volume

    volumePct = 0;
    setVolumeA(0); // send ZZAG000;
    isMuted = true;

    Serial.println("[MUTE] ON");
    if (webDebug)
      logPrintln("[MUTE] ON");
  }
  else
  {
    // going to UNMUTE
    volumePct = muteRestoreVolume;
    if (volumePct < 0)
      volumePct = 0;
    if (volumePct > 100)
      volumePct = 100;

    setVolumeA((uint8_t)volumePct);
    isMuted = false;

    Serial.printf("[MUTE] OFF -> %d%%\n", volumePct);
    if (webDebug)
    {
      String msg = "[MUTE] OFF -> " + String(volumePct) + "%";
      logPrintln(msg);
    }
  }
}

void updateGreenLed()
{
  // Only GREEN LED behaviour based on CAT & mute state
  if (cat.connected())
  {
    if (isMuted)
    {
      // Blink GREEN when muted
      uint32_t now = millis();
      if (now - greenBlinkLastToggle >= GREEN_BLINK_PERIOD_MS)
      {
        greenBlinkLastToggle = now;
        greenBlinkOn = !greenBlinkOn;
        digitalWrite(PIN_LED_GREEN, greenBlinkOn ? HIGH : LOW);
      }
    }
    else
    {
      // Solid GREEN when connected and not muted
      greenBlinkOn = false;
      digitalWrite(PIN_LED_GREEN, HIGH);
    }
  }
  else
  {
    // No CAT -> GREEN off
    greenBlinkOn = false;
    digitalWrite(PIN_LED_GREEN, LOW);
  }
}

void rebootESP()
{
  cleanCloseNet();
  // fast blink RED 5 times (about 0.8s total)
  for (int i = 0; i < 5; ++i)
  {
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
    delay(80);
    digitalWrite(PIN_LED_RED, LOW);
    delay(80);
  }
  ESP.restart();
}

// ================== SETUP ========================
void setup()
{
  Serial.begin(115200);
  printStartupHeader();
  // Factory Reset Pins Configuration
  pinMode(PIN_FACTORY_RESET_SW, INPUT_PULLUP);
  pinMode(PIN_LED_RED_RESET, OUTPUT);
  digitalWrite(PIN_LED_RED_RESET, LOW);
  // Factory reset may erase NVS and reboot
  HB9IIUPortal::checkFactoryReset(PIN_FACTORY_RESET_SW, PIN_LED_RED_RESET);

  // --- Start LED blink task (for the rest of setup) ---
  xTaskCreatePinnedToCore(
      ledBlinkTask,        // task function
      "LEDBlink",          // name
      2048,                // stack size
      nullptr,             // parameter
      1,                   // priority
      &ledBlinkTaskHandle, // handle
      1);                  // core (0 or 1, doesn’t matter much here)

  ledBlinkActive = true; // start blinking

  // Connect if possible, else start captive portal
  HB9IIUPortal::begin();

  // If we reach here: WiFi is configured (STA or AP mode is decided)
  if (!HB9IIUPortal::isInAPMode())
  {
    logPrintln("[Setup] WiFi connected.");
    logPrintln("[Setup] IP address: " + WiFi.localIP().toString());

    // Init web console logger (routes + handlers)
    WebConsoleLogger_begin(server, consoleHTML);

    // Start HTTP server
    server.begin();
    logPrintln("HTTP server started.");

    // Setup OTA
    OtaHelper::begin(OTA_HOSTNAME);

    // --- Stop LED blinking at end of setup() ---
    ledBlinkActive = false;
    if (ledBlinkTaskHandle != nullptr)
    {
      vTaskDelete(ledBlinkTaskHandle);
      ledBlinkTaskHandle = nullptr;
    }
    digitalWrite(PIN_LED_RED_RESET, LOW); // ensure off

    // we are now ready and connected, here comes the real stuff

    prefs.begin("cat", false);

    // VFO encoder
    pinMode(PIN_ENC_A, ENC_INPUT_MODE);
    pinMode(PIN_ENC_B, ENC_INPUT_MODE);
    vfo_q_last = fastReadAB();
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

    // Filter encoder
    pinMode(PIN_FILT_A, ENC_INPUT_MODE);
    pinMode(PIN_FILT_B, ENC_INPUT_MODE);
    f_q_last = fastReadAB_filt();
    attachInterrupt(digitalPinToInterrupt(PIN_FILT_A), filtISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_FILT_B), filtISR, CHANGE);

    // Volume encoder
    pinMode(PIN_VOL_A, ENC_INPUT_MODE);
    pinMode(PIN_VOL_B, ENC_INPUT_MODE);
    v_q_last = fastReadAB_vol();
    attachInterrupt(digitalPinToInterrupt(PIN_VOL_A), volISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_VOL_B), volISR, CHANGE);

    // Encoder click buttons (active-LOW with pull-ups)
    pinMode(PIN_ENC_BW_SW, INPUT_PULLUP);
    pinMode(PIN_ENC_VOL_SW, INPUT_PULLUP);
    clickLastBW = !digitalRead(PIN_ENC_BW_SW); // pressed = LOW -> !LOW = true
    clickLastVol = !digitalRead(PIN_ENC_VOL_SW);

    // TTP223 touch pins: INPUT_PULLDOWN (idle LOW, touch = HIGH)
    pinMode(PIN_TOUCH1, INPUT_PULLDOWN);
    pinMode(PIN_TOUCH2, INPUT_PULLDOWN);
    pinMode(PIN_TOUCH3, INPUT_PULLDOWN);
    pinMode(PIN_TOUCH4, INPUT_PULLDOWN);
    pinMode(PIN_TOUCH5, INPUT_PULLDOWN);

    // LEDs
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    ledsOff();

    // Init debounce baselines
    touchLast1 = digitalRead(PIN_TOUCH1);
    touchLast2 = digitalRead(PIN_TOUCH2);
    touchLast3 = digitalRead(PIN_TOUCH3);
    touchLast4 = digitalRead(PIN_TOUCH4);
    touchLast5 = digitalRead(PIN_TOUCH5);
    uint32_t now = millis();
    touchT1 = touchT2 = touchT3 = touchT4 = touchT5 = now;
    clickTBW = clickTVol = now;

    if (!catConnect())
    {
      Serial.println("[CAT] Could not connect to CAT. Rebooting...");
      logPrintln("[CAT] Could not connect to CAT. Rebooting...");
      delay(500);
      rebootESP();
    }

    saveCurrentHostIfNeeded();

    if (!initialSyncFromRadio())
    {
      Serial.println("[SYNC] No FA reply; pushing local once.");
      sendFA(vfoHz);
      lastSentHz = vfoHz;
    }

    // Init filter + volume from radio
    readFilterPresetOnce();
    int v = readVolumeA();
    if (v >= 0)
    {
      volumePct = v;
      muteRestoreVolume = v; // remember for unmute
      Serial.printf("[VOL] %d%%\n", volumePct);
    }
  }
}

// ================== LOOP =========================
void loop()
{
  HB9IIUPortal::loop(); // MUST be called every loop()
  OtaHelper::handle();  // Handle OTA updates
  if (!HB9IIUPortal::isInAPMode())
  {
    // ✅ Normal application code here – CAT connected
    server.handleClient(); // Handle web requests

    if (cat.connected() && cat.available())
      pumpIncoming();

    // reconnect if needed
    static uint32_t lastTry = 0;
    if (!cat.connected() && millis() - lastTry > 1200)
    {
      lastTry = millis();
      if (!tryConnectHost(currentHost))
      {
        if (!catConnect())
        {
          Serial.println("[CAT] Reconnect failed; rebooting...");
          delay(500);
          ESP.restart();
        }
      }
      saveCurrentHostIfNeeded();
      if (!initialSyncFromRadio())
      {
        sendFA(vfoHz);
        lastSentHz = vfoHz;
      }
      readFilterPresetOnce();
      int v = readVolumeA();
      if (v >= 0)
      {
        volumePct = v;
        muteRestoreVolume = v; // keep restore value in sync after reconnect
      }
    }

    // External change sync baseline
    static int32_t lastEdges = 0;
    if (needResetEncoderBaseline)
    {
      noInterrupts();
      q_edges = 0;
      detentPending = 0;
      interrupts();
      needResetEncoderBaseline = false;
      noInterrupts();
      lastEdges = q_edges;
      interrupts();
    }

    // VFO ENCODER: freq detents + accel
    int32_t edges;
    noInterrupts();
    edges = q_edges;
    interrupts();
    int32_t deltaEdges = edges - lastEdges;
    int32_t detents = deltaEdges / 4;
    if (detents != 0)
    {
      lastEdges += detents * 4;
      uint32_t nowMs = millis();
      uint32_t dt = nowMs - lastDetentMs;
      int accel = 1;
      if (dt < ACCEL_T1_MS)
        accel = 4;
      else if (dt < ACCEL_T2_MS)
        accel = 2;
      long next = (long)vfoHz + (long)detents * STEP_HZ * accel;
      if (next < 0)
        next = 0;
      vfoHz = (uint32_t)next;
    }

    // Rate-limited FA
    static uint32_t lastSend = 0;
    if (cat.connected() && millis() - lastSend >= SEND_INTERVAL_MS)
    {
      lastSend = millis();
      if (vfoHz != lastSentHz)
      {
        if (sendFA(vfoHz))
          lastSentHz = vfoHz;
        else if (cat.connected())
          cat.stop();
      }
    }

    // Light periodic FA;
    static uint32_t lastFAq = 0;
    if (RESYNC_MS > 0 && cat.connected() && millis() - lastFAq > RESYNC_MS)
    {
      lastFAq = millis();
      cat.print("FA;");
    }

    // FILTER ENCODER: 4 edges = 1 detent; step 0..7
    static int32_t f_lastEdges = 0;
    int32_t fe;
    noInterrupts();
    fe = f_edges;
    interrupts();
    int32_t f_delta = fe - f_lastEdges;
    int32_t f_detents = f_delta / 4;
    if (f_detents != 0)
    {
      f_lastEdges += f_detents * 4;
      int8_t dir = (f_detents > 0) ? +1 : -1;
      int8_t newIdx = filterIdx + dir;
      if (newIdx < 0)
        newIdx = 0;
      if (newIdx > 7)
        newIdx = 7;
      if (newIdx != filterIdx)
      {
        filterIdx = newIdx;
        sendFilterPreset((uint8_t)filterIdx);
      }
    }

    // VOLUME ENCODER: each detent = VOLUME_STEP %, clamp 0..100, throttle sends
    static int32_t v_lastEdges = 0;
    static uint32_t lastVolSend = 0;
    static int16_t lastVolSent = -1;

    int32_t ve;
    noInterrupts();
    ve = v_edges;
    interrupts();
    int32_t v_delta = ve - v_lastEdges;
    int32_t v_detents = v_delta / 4;

    if (v_detents != 0)
    {
      v_lastEdges += v_detents * 4;

      // 🔊 If user turns the knob while muted -> auto-unmute
      if (isMuted)
      {
        Serial.println("[VOL] Encoder rotated while muted -> auto-unmute");
        if (webDebug)
          logPrintln("[VOL] Encoder rotated while muted -> auto-unmute");

        isMuted = false;

        // If we are at 0 but have a remembered volume, restore it first
        if (volumePct == 0 && muteRestoreVolume > 0)
        {
          volumePct = muteRestoreVolume;
        }
      }

      // Apply detent change
      int16_t newVol = volumePct + (int16_t)v_detents * VOLUME_STEP;
      if (newVol < 0)
        newVol = 0;
      if (newVol > 100)
        newVol = 100;
      volumePct = newVol;

      // Keep a good "last non-zero volume" for future mute/unmute
      if (!isMuted && volumePct > 0)
      {
        muteRestoreVolume = volumePct;
      }
    }

    if (cat.connected() && volumePct != lastVolSent && millis() - lastVolSend >= 120)
    {
      if (setVolumeA((uint8_t)volumePct))
      {
        lastVolSent = volumePct;
        lastVolSend = millis();
      }
    }

    // ===== SIMPLE TOUCH HANDLING (active-HIGH, debounced) =====
    uint32_t t = millis();

    bool r1 = digitalRead(PIN_TOUCH1);
    if (r1 != touchLast1 && (t - touchT1) >= TOUCH_DEBOUNCE_MS)
    {
      touchLast1 = r1;
      touchT1 = t;
      if (r1)
      {
        Serial.println("[TTP] 1 -> FT8 40m; mode: LSB");
        flashRedLed();
        setFT8_40m();
        setMode("LSB");
      }
    }
    bool r2 = digitalRead(PIN_TOUCH2);
    if (r2 != touchLast2 && (t - touchT2) >= TOUCH_DEBOUNCE_MS)
    {
      touchLast2 = r2;
      touchT2 = t;
      if (r2)
      {
        Serial.println("[TTP] 2 -> FT8 20m; mode: USB");
        flashRedLed();
        setFT8_20m();
        setMode("USB");
      }
    }
    bool r3 = digitalRead(PIN_TOUCH3);
    if (r3 != touchLast3 && (t - touchT3) >= TOUCH_DEBOUNCE_MS)
    {
      touchLast3 = r3;
      touchT3 = t;
      if (r3)
      { // finger down
        Serial.println("[TTP] 3 -> PTT ON");
        setPTT(true);
        digitalWrite(PIN_LED_RED, HIGH);
      }
      else
      { // finger up
        Serial.println("[TTP] 3 -> PTT OFF");
        setPTT(false);
        digitalWrite(PIN_LED_RED, LOW);
      }
    }
    bool r4 = digitalRead(PIN_TOUCH4);
    if (r4 != touchLast4 && (t - touchT4) >= TOUCH_DEBOUNCE_MS)
    {
      touchLast4 = r4;
      touchT4 = t;
      if (r4)
      {
        Serial.println("[TTP] 4 pressed -> TUNE");
        startTune(/*ms*/ 1200, /*power%*/ 10, /*mode*/ "FM");
      }
    }
    bool r5 = digitalRead(PIN_TOUCH5);
    if (r5 != touchLast5 && (t - touchT5) >= TOUCH_DEBOUNCE_MS)
    {
      touchLast5 = r5;
      touchT5 = t;
      if (r5)
      {
        Serial.println("[TTP] 5 pressed -> MODE CYCLE (USB/LSB/CW/FM)");
        flashRedLed();
        cycleModeSequence();
      }
    }

    // ===== ENCODER CLICK HANDLING (active-LOW, debounced) =====
    bool cfRaw = !digitalRead(PIN_ENC_BW_SW); // pressed if LOW
    if (cfRaw != clickLastBW && (t - clickTBW) >= CLICK_DEBOUNCE_MS)
    {
      clickLastBW = cfRaw;
      clickTBW = t;
      if (cfRaw)
      {
        Serial.println("[CLICK] BW");
        if (webDebug)
        {
          logPrintln("[CLICK] BW ->> Rebooting");
          server.handleClient(); // Handle web requests
          delay(1000);
        }
        rebootESP();
      }
    }

    bool cvRaw = !digitalRead(PIN_ENC_VOL_SW); // pressed if LOW
    if (cvRaw != clickLastVol && (t - clickTVol) >= CLICK_DEBOUNCE_MS)
    {
      clickLastVol = cvRaw;
      clickTVol = t;
      if (cvRaw)
      {
        Serial.println("[CLICK] Vol -> Mute/Unmute");
        muteUnmute();
      }
    }
    if (redFlashActive && (int32_t)(millis() - redFlashUntil) >= 0)
    {
      redFlashActive = false;
      digitalWrite(PIN_LED_RED, LOW); // end flash
    }
    updateGreenLed(); // enforce GREEN LED: solid vs blink vs off
    serviceTune();

    delay(1);
  }
  else
  {
    // Captive-portal mode
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 1000)
    {
      lastLog = millis();
      logPrintln("Waiting in CAPTIVE PORTAL mode for Wifi Credentials: " + String(millis() / 1000) + " seconds");
    }
  }
}
