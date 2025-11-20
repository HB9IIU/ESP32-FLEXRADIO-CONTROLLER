#pragma once
#include <Arduino.h>

namespace HB9IIUPortal
{
    // Call once in setup()
    void begin();                 // tries to connect to saved WiFi, else starts captive portal

    // Call every loop()
    void loop();                  // handles WebServer + DNS when in AP mode

    // Optional factory reset
    void eraseAllPreferences();   // clears wifi/config/iPhonetime NVS namespaces

    // State helpers
    bool isInAPMode();            // true when captive portal is running
    bool isConnected();           // true when WiFi.status() == WL_CONNECTED
}
