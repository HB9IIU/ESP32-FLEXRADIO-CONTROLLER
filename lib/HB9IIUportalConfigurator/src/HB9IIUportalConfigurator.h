#pragma once
#include <Arduino.h>

namespace HB9IIUPortal
{
    // Call once in setup()
    void begin();                 // tries to connect to saved WiFi, else starts captive portal

    // Call every loop()
    void loop();                  // handles WebServer + DNS when in AP mode

    //  factory reset
    void eraseAllPreferencesAndRestart();   // clears wifi/config/iPhonetime NVS namespaces

    // check factory reset at boot using an already-configured button + LED
    bool checkFactoryReset(uint8_t buttonPin, uint8_t ledPin);

    // State helpers
    bool isInAPMode();            // true when captive portal is running
    bool isConnected();           // true when WiFi.status() == WL_CONNECTED
}
