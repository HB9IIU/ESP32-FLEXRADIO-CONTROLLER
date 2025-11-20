#include "HB9IIUOtaHelper.h"
#include <ArduinoOTA.h>
#include "HB9IIUWebConsoleLogger.h"  // for logPrintln()

namespace OtaHelper {

void begin(const char *hostname) {
  ArduinoOTA.setHostname(hostname);

  // Optional: password
  // ArduinoOTA.setPassword("my_ota_password");

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else { // U_SPIFFS / FS
        type = "filesystem";
      }
      logPrintln("OTA Start updating " + type);
    })
    .onEnd([]() {
      logPrintln("OTA End");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      char buf[32];
      snprintf(buf, sizeof(buf), "OTA Progress: %u%%", (progress * 100) / total);
      logPrintln(String(buf));
    })
    .onError([](ota_error_t error) {
      String msg = "OTA Error[" + String(error) + "]: ";
      if (error == OTA_AUTH_ERROR)       msg += "Auth Failed";
      else if (error == OTA_BEGIN_ERROR) msg += "Begin Failed";
      else if (error == OTA_CONNECT_ERROR) msg += "Connect Failed";
      else if (error == OTA_RECEIVE_ERROR) msg += "Receive Failed";
      else if (error == OTA_END_ERROR)   msg += "End Failed";
      logPrintln(msg);
    });

  ArduinoOTA.begin();
  logPrintln("OTA ready. Hostname: " + String(hostname));
}

void handle() {
  ArduinoOTA.handle();
}

} // namespace OtaHelper
