// Device name = DEVICE_NAME + "-" + last 4 hex digits of the ESP32's Bluetooth MAC, e.g. "CubiNav-A4F2".
// The MAC is burned into the chip, so the name is unique per unit and survives reflashing. The setup screen
// shows the same string, so call devname::get() from both places (it is built once and cached).
#pragma once
#include <Arduino.h>
#include "config.h"
#if defined(__has_include)
  #if __has_include(<esp_mac.h>)
    #include <esp_mac.h>          // arduino-esp32 3.x / IDF 5
  #endif
#endif
#include <esp_system.h>           // esp_read_mac() on older cores

namespace devname {
inline const char* get() {
  static char name[24] = {0};
  if (!name[0]) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);   // the BLE address is derived from this one
    snprintf(name, sizeof(name), "%s-%02X%02X", DEVICE_NAME, mac[4], mac[5]);
  }
  return name;
}
}  // namespace devname