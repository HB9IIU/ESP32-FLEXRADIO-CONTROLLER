#pragma once
#include <Arduino.h>

namespace OtaHelper {
  // Initialize OTA with a given hostname
  void begin(const char *hostname);

  // Call this regularly in loop()
  void handle();
}
