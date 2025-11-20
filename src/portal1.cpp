#include <HB9IIUportalConfigurator.h>
#include <Preferences.h>
Preferences prefs;

void eraseAllPreferences();






void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\nBooting…");
    // Optional factory reset:
    //HB9IIUPortal::eraseAllPreferences();

    HB9IIUPortal::begin();   // connect if possible, else start captive portal
}

void loop()
{
    HB9IIUPortal::loop();    // MUST be called every loop()

    if (!HB9IIUPortal::isInAPMode())
    {
        // ✅ Normal application code here – WiFi connected
        // e.g. fetch NTP, HTTP stuff, etc.
    }
    else
    {
        // We are in captive-portal mode; you can keep this empty if you want.
    }
}



void eraseAllPreferences()
{
    // List all namespaces you use in this project
    const char *namespaces[] = {
        "wifi",      // SSID + password
        "config",    // API key, other config
        "iPhonetime" // phone time info
        // add more here if you create new namespaces later
    };

    const size_t count = sizeof(namespaces) / sizeof(namespaces[0]);

    Serial.println("🧹 Erasing all stored Preferences…");

    for (size_t i = 0; i < count; i++)
    {
        Serial.printf("   ➤ Clearing namespace \"%s\"…\n", namespaces[i]);
        prefs.begin(namespaces[i], false); // RW mode
        prefs.clear();                     // erase all keys in this namespace
        prefs.end();
    }

    Serial.println("✅ All known Preferences namespaces cleared.");
}

