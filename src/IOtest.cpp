/*
  ESP32 I/O WIRING TESTER + ALT LED BLINK + WiFi OTA (PlatformIO)
  - 5x TTP223 touch inputs (active-HIGH, idle LOW): 18,19,21,22,23
  - 2x encoder click buttons (active-LOW with INPUT_PULLUP): 16,17
  - 3x quadrature encoders (A/B):
      MAIN   A=32, B=33
      FILTER A=25, B=26
      VOLUME A=27, B=14
  - 2x LEDs blink alternately (active-LOW wiring):
      GREEN on GPIO13, RED on GPIO4
  - Prints on changes only.
  - Adds Wi-Fi + ArduinoOTA so you can upload from PlatformIO over Wi-Fi.

  Serial: 115200
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

// ---- WiFi credentials (from user) ----
const char* WIFI_SSID = "NO WIFI FOR YOU!!!";
const char* WIFI_PASS = "Nestle2010Nestle";

// ---- mDNS Hostname shown as <hostname>.local ----
const char* OTA_HOSTNAME = "HB9IIU-FLEXCONTROL"; // set to whatever you like

// -------- Pins (change here if needed) --------
const int PIN_TOUCH1 = 18;
const int PIN_TOUCH2 = 19;
const int PIN_TOUCH3 = 21;
const int PIN_TOUCH4 = 22;
const int PIN_TOUCH5 = 23;

const int PIN_ENC_FREQ_SW = 16; // click buttons (active-LOW)
const int PIN_ENC_VOL_SW  = 17;

const int PIN_MAIN_A = 32, PIN_MAIN_B = 33; // main encoder A/B
const int PIN_FILT_A = 25, PIN_FILT_B = 26; // filter encoder A/B
const int PIN_VOL_A  = 27, PIN_VOL_B  = 14; // volume encoder A/B

// LEDs (active-LOW recommended, especially for GPIO4)
const int PIN_LED_GREEN = 13;   // safe GPIO
const int PIN_LED_RED   = 4;    // OK if not held LOW at boot
const bool LED_ACTIVE_LOW = true;
// ----------------------------------------------

// Touch debounce
const uint16_t DEBOUNCE_MS = 40;

// Quadrature lookup (Gray-code table)
static const int8_t QDEC_TAB[16] = {
  0,-1,+1,0, +1,0,0,-1, -1,0,0,+1, 0,+1,-1,0
};

// --- Simple state holding for touch & clicks ---
struct Debounce {
  int pin;
  bool last;
  uint32_t tlast;
};

Debounce TTP[5] = {
  {PIN_TOUCH1, false, 0},
  {PIN_TOUCH2, false, 0},
  {PIN_TOUCH3, false, 0},
  {PIN_TOUCH4, false, 0},
  {PIN_TOUCH5, false, 0},
};

Debounce CLICK_FREQ { PIN_ENC_FREQ_SW, false, 0 };
Debounce CLICK_VOL  { PIN_ENC_VOL_SW,  false, 0 };

// --- Simple polled quadrature decoder for each encoder ---
struct QDec {
  int pinA, pinB;
  uint8_t lastAB;   // last 2-bit state (A<<1 | B)
  int32_t edges;    // counts +-1 per transition
};

QDec ENC_MAIN { PIN_MAIN_A, PIN_MAIN_B, 0, 0 };
QDec ENC_FILT { PIN_FILT_A, PIN_FILT_B, 0, 0 };
QDec ENC_VOL  { PIN_VOL_A,  PIN_VOL_B,  0, 0 };

// Helpers
inline uint8_t readAB(int pa, int pb) {
  return (uint8_t(digitalRead(pa)) << 1) | uint8_t(digitalRead(pb));
}

inline void ledWrite(int pin, bool on) {
  if (LED_ACTIVE_LOW) digitalWrite(pin, on ? LOW : HIGH);
  else                digitalWrite(pin, on ? HIGH : LOW);
}

void setupPins() {
  // Touch inputs: active-HIGH, idle LOW -> use pulldown
  pinMode(PIN_TOUCH1, INPUT_PULLDOWN);
  pinMode(PIN_TOUCH2, INPUT_PULLDOWN);
  pinMode(PIN_TOUCH3, INPUT_PULLDOWN);
  pinMode(PIN_TOUCH4, INPUT_PULLDOWN);
  pinMode(PIN_TOUCH5, INPUT_PULLDOWN);

  // Click buttons: active-LOW -> use pullup
  pinMode(PIN_ENC_FREQ_SW, INPUT_PULLUP);
  pinMode(PIN_ENC_VOL_SW,  INPUT_PULLUP);

  // Encoders A/B: inputs (pullups typically fine)
  pinMode(PIN_MAIN_A, INPUT_PULLUP);
  pinMode(PIN_MAIN_B, INPUT_PULLUP);
  pinMode(PIN_FILT_A, INPUT_PULLUP);
  pinMode(PIN_FILT_B, INPUT_PULLUP);
  pinMode(PIN_VOL_A,  INPUT_PULLUP);
  pinMode(PIN_VOL_B,  INPUT_PULLUP);

  // LEDs
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED,   OUTPUT);
  ledWrite(PIN_LED_GREEN, false);
  ledWrite(PIN_LED_RED,   false);
}

void initBaselines() {
  uint32_t now = millis();

  for (int i=0;i<5;i++) {
    TTP[i].last = digitalRead(TTP[i].pin);
    TTP[i].tlast = now;
  }
  CLICK_FREQ.last = !digitalRead(CLICK_FREQ.pin); // pressed if LOW
  CLICK_VOL.last  = !digitalRead(CLICK_VOL.pin);
  CLICK_FREQ.tlast = CLICK_VOL.tlast = now;

  ENC_MAIN.lastAB = readAB(ENC_MAIN.pinA, ENC_MAIN.pinB);
  ENC_FILT.lastAB = readAB(ENC_FILT.pinA, ENC_FILT.pinB);
  ENC_VOL.lastAB  = readAB(ENC_VOL.pinA,  ENC_VOL.pinB);
}

// --- Wi-Fi + OTA helpers ---
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi: connecting to \"%s\" ...\n", WIFI_SSID);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
    if (millis() - t0 > 15000) {       // 15s timeout, then retry
      Serial.println("\nWiFi: retrying...");
      WiFi.disconnect(true);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      t0 = millis();
    }
  }
  Serial.printf("\nWiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  // Optional password (uncomment next two lines to enable)
  // ArduinoOTA.setPassword("YourOTAPassword");
  // Note: Add --auth=YourOTAPassword in platformio.ini upload_flags if you enable this.

  ArduinoOTA
    .onStart([]() {
      const char* type = ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem";
      Serial.printf("\nOTA Start (%s)\n", type);
      // indicate OTA with LEDs
      ledWrite(PIN_LED_GREEN, false);
      ledWrite(PIN_LED_RED,   true);
    })
    .onEnd([]() {
      Serial.println("\nOTA End");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      static uint8_t last = 255;
      uint8_t pct = (100U * progress) / total;
      if (pct != last) {
        Serial.printf("OTA Progress: %u%%\r", pct);
        last = pct;
      }
    })
    .onError([](ota_error_t error) {
      Serial.printf("\nOTA Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR)          Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR)    Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR)  Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR)  Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR)      Serial.println("End Failed");
    });

  ArduinoOTA.begin();
  Serial.printf("OTA ready: %s.local:3232\n", OTA_HOSTNAME);
}

// Print helpers
void printTouchEvent(int idx, bool pressed) {
  Serial.printf("[TTP%d] %s\n", idx+1, pressed ? "PRESSED" : "RELEASED");
}
void printClickEvent(const char* name, bool pressed) {
  Serial.printf("[CLICK %s] %s\n", name, pressed ? "PRESSED" : "RELEASED");
}
void printDetent(const char* name, int dir) {
  // dir: +1 = CW, -1 = CCW
  Serial.printf("[ENC %s] detent %s\n", name, dir > 0 ? "CW" : "CCW");
}

void pollTouch() {
  uint32_t t = millis();
  const int pins[5] = { PIN_TOUCH1, PIN_TOUCH2, PIN_TOUCH3, PIN_TOUCH4, PIN_TOUCH5 };
  for (int i=0;i<5;i++) {
    bool now = digitalRead(pins[i]);      // active-HIGH
    if (now != TTP[i].last && (t - TTP[i].tlast) >= DEBOUNCE_MS) {
      TTP[i].last = now;
      TTP[i].tlast = t;
      printTouchEvent(i, now);
    }
  }
}

void pollClicks() {
  uint32_t t = millis();
  bool cf = !digitalRead(CLICK_FREQ.pin); // pressed if LOW
  if (cf != CLICK_FREQ.last && (t - CLICK_FREQ.tlast) >= DEBOUNCE_MS) {
    CLICK_FREQ.last = cf; CLICK_FREQ.tlast = t;
    printClickEvent("FREQ", cf);
  }
  bool cv = !digitalRead(CLICK_VOL.pin);
  if (cv != CLICK_VOL.last && (t - CLICK_VOL.tlast) >= DEBOUNCE_MS) {
    CLICK_VOL.last = cv; CLICK_VOL.tlast = t;
    printClickEvent("VOL", cv);
  }
}

// Return +1/-1/0 for edge, and emit detent on 4 edges
void pollEncoder(QDec& e, const char* name) {
  uint8_t ab = readAB(e.pinA, e.pinB);
  if (ab == e.lastAB) return;
  uint8_t idx = (e.lastAB << 2) | ab;
  int8_t d = QDEC_TAB[idx];
  e.lastAB = ab;
  if (!d) return;

  e.edges += d;
  if (e.edges >= 4) { e.edges = 0; printDetent(name, +1); }
  else if (e.edges <= -4) { e.edges = 0; printDetent(name, -1); }
}

// --- Non-blocking alternate blink ---
const uint32_t BLINK_MS = 500;
bool ledPhase = false;      // false: GREEN on, RED off; true: RED on, GREEN off
uint32_t lastBlink = 0;

void blinkAlternate() {
  uint32_t now = millis();
  if (now - lastBlink < BLINK_MS) return;
  lastBlink = now;
  ledPhase = !ledPhase;
  ledWrite(PIN_LED_GREEN, !ledPhase); // GREEN on when phase==false
  ledWrite(PIN_LED_RED,    ledPhase); // RED   on when phase==true
}
void printNetworkInfo() {
  IPAddress ip   = WiFi.localIP();
  IPAddress gw   = WiFi.gatewayIP();
  IPAddress mask = WiFi.subnetMask();
  String mac     = WiFi.macAddress();

  Serial.println("=== Network Info ===");
  Serial.printf("SSID : %s\n", WiFi.SSID().c_str());
  Serial.printf("IP   : %s\n", ip.toString().c_str());
  Serial.printf("GW   : %s\n", gw.toString().c_str());
  Serial.printf("MASK : %s\n", mask.toString().c_str());
  Serial.printf("MAC  : %s\n", mac.c_str());
  Serial.printf("mDNS : %s.local:3232\n", OTA_HOSTNAME);  // if using ArduinoOTA
  Serial.println("====================");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 I/O WIRING TESTER + ALT LED BLINK + OTA ===");

  setupPins();
  initBaselines();

  Serial.println("Touch: 18/19/21/22/23 (pressed=HIGH)");
  Serial.println("Clicks: 16/17 (pressed=LOW)");
  Serial.println("Encoders: MAIN 32/33, FILTER 25/26, VOLUME 27/14");
  Serial.printf("LEDs: GREEN=%d, RED=%d (active-%s)\n",
      PIN_LED_GREEN, PIN_LED_RED, LED_ACTIVE_LOW ? "LOW" : "HIGH");
  Serial.println("-----------------------------------------");

  // --- WiFi + OTA ---
  connectWiFi();
  printNetworkInfo();
  setupOTA();
}

void loop() {
  // OTA service (non-blocking)
  ArduinoOTA.handle();

  // Touch inputs
  pollTouch();

  // Encoder clicks
  pollClicks();

  // Encoders (detents)
  pollEncoder(ENC_MAIN, "MAIN");
  pollEncoder(ENC_FILT, "FILTER");
  pollEncoder(ENC_VOL,  "VOLUME");

  // Alternate blink
  blinkAlternate();

  // Keep it breathable
  delay(1);

  // (Optional) basic WiFi reconnect watchdog without blocking OTA
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if (now - lastCheck > 10000) { // every 10s
    lastCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi: lost connection, reconnecting...");
      WiFi.disconnect(true);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
}

/*
  LED wiring (active-LOW recommended for GPIO4):
  3.3V -> 330Ω -> LED anode -> LED cathode -> GPIO pin
  (drive pin LOW to turn LED ON). Start-up lines are HIGH, so LEDs are OFF at boot.
*/
