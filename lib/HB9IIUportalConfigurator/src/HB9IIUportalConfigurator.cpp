#include "HB9IIUportalConfigurator.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#include "config_page.h"  // const char index_html[] PROGMEM = "..."
#include "success_page.h" // const char html_success[] PROGMEM = "..."

namespace HB9IIUPortal
{
    // ───────── INTERNAL STATE ─────────
    static const byte DNS_PORT = 53;
    static DNSServer dnsServer;
    static IPAddress apIP(192, 168, 4, 1);

    static WebServer server(80);
    static Preferences prefs;

    static int scanCount = 0;
    static bool inAPmode = false;
    static bool connected = false;

    // ───────── INTERNAL PROTOTYPES ─────────
    static bool tryToConnectSavedWiFi();
    static void startConfigurationPortal();
    static void handleRootCaptivePortal();
    static void handleScanCaptivePortal();
    static void handleSaveCaptivePortal();

    // ───────── PUBLIC API ─────────
    void begin()
    {
        Serial.println(F("[HB9IIUPortal] begin()"));

        if (tryToConnectSavedWiFi())
        {
            inAPmode = false;
            connected = true;
            Serial.println(F("[HB9IIUPortal] Using saved WiFi, no captive portal needed."));
        }
        else
        {
            connected = false;
            inAPmode = true;
            startConfigurationPortal();
        }
    }

    void loop()
    {
        server.handleClient();

        if (inAPmode)
        {
            dnsServer.processNextRequest(); // important for captive portal
        }
    }

    void eraseAllPreferences()
    {
        const char *namespaces[] = {
            "wifi",      // SSID + password
            "config",    // API key, other config
            "iPhonetime" // phone time info
        };

        const size_t count = sizeof(namespaces) / sizeof(namespaces[0]);

        Serial.println("🧹 [HB9IIUPortal] Erasing all stored Preferences…");

        for (size_t i = 0; i < count; i++)
        {
            Serial.printf("   ➤ Clearing namespace \"%s\"…\n", namespaces[i]);
            prefs.begin(namespaces[i], false);
            prefs.clear();
            prefs.end();
        }

        Serial.println("✅ [HB9IIUPortal] All known Preferences namespaces cleared.");
    }

    bool isInAPMode()
    {
        return inAPmode;
    }

    bool isConnected()
    {
        return connected;
    }

    // ───────── INTERNAL IMPLEMENTATION ─────────

    static bool tryToConnectSavedWiFi()
    {
        Serial.println("🔍 [HB9IIUPortal] Attempting to load saved WiFi credentials...");

        if (!prefs.begin("wifi", true))
        {
            Serial.println("⚠️ [HB9IIUPortal] NVS namespace 'wifi' not found.");
            return false;
        }

        String ssid = prefs.getString("ssid", "");
        // If SSID looks like "NAME (-48 dBm)", strip the suffix
        int parenIndex = ssid.lastIndexOf('(');
        if (parenIndex > 0 && ssid.endsWith(" dBm)"))
        {
            ssid = ssid.substring(0, parenIndex);
            ssid.trim(); // remove trailing space
        }
        String pass = prefs.getString("pass", "");
        prefs.end();

        if (ssid.isEmpty() || pass.isEmpty())
        {
            Serial.println("⚠️ [HB9IIUPortal] No saved credentials found (empty values).");
            return false;
        }

        Serial.printf("📡 Found SSID: %s\n", ssid.c_str());
        Serial.printf("🔐 Found Password: %s\n", pass.c_str());

        Serial.printf("🔌 Connecting to WiFi: %s...\n", ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());

        for (int i = 0; i < 20; i++) // wait up to ~10s
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                Serial.println("✅ [HB9IIUPortal] Connected to WiFi!");
                Serial.print("📶 IP Address: ");
                Serial.println(WiFi.localIP());
                return true;
            }
            Serial.print(".");
            delay(500);
        }

Serial.println("\n❌ [HB9IIUPortal] Failed to connect to saved WiFi.");
WiFi.disconnect(true);           // optional, but nice to be clean

Serial.println("🔁 Rebooting ESP32 in 2 seconds...");
delay(2000);                     // give time for the message to flush

ESP.restart();                   // Arduino-style restart
    }

    static void startConfigurationPortal()
    {
        Serial.println("🌐 [HB9IIUPortal] Starting Captive Portal...");

        // Start AP + STA mode
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("HB9IIUSetup");
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

        Serial.print("📶 AP IP: ");
        Serial.println(WiFi.softAPIP());

        // DNS: redirect all domains to our AP IP
        dnsServer.start(DNS_PORT, "*", apIP);

        // Scan Wi-Fi networks
        Serial.println("📡 Scanning for networks...");
        WiFi.scanDelete();
        scanCount = WiFi.scanNetworks(false, false, false, 120, 0);
        Serial.printf("📶 Found %d networks\n", scanCount);

        // Pretty scan dump at startup
        Serial.println();
        Serial.printf("[HB9IIUPortal] Scan result at startup:\n");
        Serial.println(F("────────────────────────────────────────────────────────"));
        Serial.println(F(" #   RSSI  Auth         SSID"));
        Serial.println(F("────────────────────────────────────────────────────────"));

        for (int i = 0; i < scanCount; i++)
        {
            String ssid = WiFi.SSID(i);
            int32_t rssi = WiFi.RSSI(i);
            wifi_auth_mode_t auth = WiFi.encryptionType(i);

            const char *authStr = "";
            switch (auth)
            {
            case WIFI_AUTH_OPEN:
                authStr = "OPEN      ";
                break;
            case WIFI_AUTH_WEP:
                authStr = "WEP       ";
                break;
            case WIFI_AUTH_WPA_PSK:
                authStr = "WPA_PSK   ";
                break;
            case WIFI_AUTH_WPA2_PSK:
                authStr = "WPA2_PSK  ";
                break;
            case WIFI_AUTH_WPA_WPA2_PSK:
                authStr = "WPA/WPA2  ";
                break;
            case WIFI_AUTH_WPA2_ENTERPRISE:
                authStr = "WPA2_ENT  ";
                break;
            case WIFI_AUTH_WPA3_PSK:
                authStr = "WPA3_PSK  ";
                break;
            case WIFI_AUTH_WPA2_WPA3_PSK:
                authStr = "WPA2/3    ";
                break;
            case WIFI_AUTH_WAPI_PSK:
                authStr = "WAPI_PSK  ";
                break;
            default:
                authStr = "UNKNOWN   ";
                break;
            }

            Serial.printf(" %2d  %4d  %s  %s\n",
                          i + 1,
                          rssi,
                          authStr,
                          ssid.c_str());
        }
        Serial.println(F("────────────────────────────────────────────────────────"));

        // HTTP routes
        server.on("/hotspot-detect.html", HTTP_ANY, handleRootCaptivePortal);
        server.on("/generate_204", HTTP_ANY, handleRootCaptivePortal);
        server.on("/ncsi.txt", HTTP_ANY, handleRootCaptivePortal);
        server.on("/connecttest.txt", HTTP_ANY, handleRootCaptivePortal);

        server.on("/", handleRootCaptivePortal);
        server.on("/scan", handleScanCaptivePortal);
        server.on("/save", HTTP_POST, handleSaveCaptivePortal);

        server.onNotFound([]()
                          {
        Serial.print("[HB9IIUPortal] Unknown request: ");
        Serial.println(server.uri());
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", ""); });

        server.begin();
        Serial.println("🚀 [HB9IIUPortal] Web server started (captive portal).");
    }

    static void handleRootCaptivePortal()
    {
        server.send_P(200, "text/html", index_html);
    }

    static void handleScanCaptivePortal()
    {
        Serial.println();
        Serial.printf("[HB9IIUPortal] Returning scan list for %d network(s):\n", scanCount);
        Serial.println(F("────────────────────────────────────────────────────────"));
        Serial.println(F(" #   RSSI  Auth         SSID"));
        Serial.println(F("────────────────────────────────────────────────────────"));

        String json = "[";

        for (int i = 0; i < scanCount; i++)
        {
            if (i > 0)
                json += ",";

            String ssid = WiFi.SSID(i);
            ssid.replace("\"", "\\\""); // just in case there is a quote
            int32_t rssi = WiFi.RSSI(i);
            wifi_auth_mode_t auth = WiFi.encryptionType(i);

            // Build label: "MyWifi (-63 dBm)"
            String label = ssid + " (" + String(rssi) + " dBm)";

            // Add to JSON
            json += "\"";
            json += label;
            json += "\"";

            // Pretty auth string
            const char *authStr = "";
            switch (auth)
            {
            case WIFI_AUTH_OPEN:
                authStr = "OPEN      ";
                break;
            case WIFI_AUTH_WEP:
                authStr = "WEP       ";
                break;
            case WIFI_AUTH_WPA_PSK:
                authStr = "WPA_PSK   ";
                break;
            case WIFI_AUTH_WPA2_PSK:
                authStr = "WPA2_PSK  ";
                break;
            case WIFI_AUTH_WPA_WPA2_PSK:
                authStr = "WPA/WPA2  ";
                break;
            case WIFI_AUTH_WPA2_ENTERPRISE:
                authStr = "WPA2_ENT  ";
                break;
            case WIFI_AUTH_WPA3_PSK:
                authStr = "WPA3_PSK  ";
                break;
            case WIFI_AUTH_WPA2_WPA3_PSK:
                authStr = "WPA2/3    ";
                break;
            case WIFI_AUTH_WAPI_PSK:
                authStr = "WAPI_PSK  ";
                break;
            default:
                authStr = "UNKNOWN   ";
                break;
            }

            // Serial debug line
            Serial.printf(" %2d  %4d  %s  %s\n",
                          i + 1,
                          rssi,
                          authStr,
                          ssid.c_str());
        }

        Serial.println(F("────────────────────────────────────────────────────────"));
        json += "]";
        server.send(200, "application/json", json);
    }

    static void handleSaveCaptivePortal()
    {
        Serial.println("[HB9IIUPortal] Saving from captive portal…");

        if (server.hasArg("ssid") && server.hasArg("password") && server.hasArg("time"))
        {
            String ssid = server.arg("ssid");
            String pass = server.arg("password");
            String timeStr = server.arg("time"); // JSON string: {"iso":"...","unix":...,"offset":...}

            // --- Save WiFi credentials ---
            prefs.begin("wifi", false);
            prefs.putString("ssid", ssid);
            prefs.putString("pass", pass);
            prefs.end();

            // --- Parse JSON "time" blob from phone ---
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, timeStr);

            if (!err)
            {
                String isoTime = doc["iso"].as<String>();
                unsigned long long unixMillis = doc["unix"].as<unsigned long long>();
                int offsetMinutes = doc["offset"].as<int>();

                prefs.begin("iPhonetime", false);
                prefs.putString("iso", isoTime);
                prefs.putLong64("unix", unixMillis);
                prefs.putInt("offsetMinutes", offsetMinutes);
                prefs.end();

                Serial.printf("✅ Saved Phone Time:\n   ISO: %s\n   Unix: %llu\n   Offset: %d minutes\n",
                              isoTime.c_str(), unixMillis, offsetMinutes);
            }
            else
            {
                Serial.println("⚠️ Failed to parse time JSON, saving raw string instead.");
                prefs.begin("iPhonetime", false);
                prefs.putString("localTime", timeStr);
                prefs.end();
            }

            server.send_P(200, "text/html", html_success);
            delay(500);
            ESP.restart();
        }
        else
        {
            server.send(400, "text/plain", "Missing fields.");
        }
    }

} // namespace HB9IIUPortal
