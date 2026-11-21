// Persistent start-up status: has the first-time setup (pairing with the phone) ever completed?
//
// Stored in NVS (namespace "cubinav", key "setup", one byte), not in a SPIFFS partition: NVS is already initialised by the Arduino
// core and used by NimBLE for its bonds, needs no partition-table change (the OTA slots stay as they are) and costs no static RAM
// (the Preferences handle lives on the stack, only for the duration of each call).
//
// First boot: the key does not exist, so the SETUP screen shows (QR code + name of the device). When setup completes - the first
// successful pairing, or the first connection when BLE_REQUIRE_PAIRING is 0 - the key is written, and every later boot skips the
// SETUP screen and goes straight to IDLE. The key alone decides: bonds already stored by NimBLE (e.g. from earlier test builds on the
// same board) are ignored, and a wiped bond list does not bring the screen back.
// NVS survives re-flashing the firmware, so a bench unit shows the setup screen again only after `pio run -t erase` (then flash again).
#pragma once
#include <Preferences.h>

namespace startup {

constexpr const char* NS  = "cubinav";
constexpr const char* KEY = "setup";

// true once setup has completed. Opened read/write on purpose: a read-only open of a namespace that does not exist yet fails and
// logs an error on every first boot.
inline bool setupDone() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  const bool v = p.getUChar(KEY, 0) == 1;
  p.end();
  return v;
}

// Flash is only written when the value actually changes.
inline void markSetupDone() {
  Preferences p;
  if (!p.begin(NS, false)) return;
  if (p.getUChar(KEY, 0) != 1) p.putUChar(KEY, 1);
  p.end();
}

// Bench only (CUBINAV_FORCE_SETUP): forget that setup was done, so the next boot is a first boot again.
inline void clearSetup() {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.remove(KEY);
  p.end();
}

}  // namespace startup