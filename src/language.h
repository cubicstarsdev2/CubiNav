// Device UI language (ids match the app and PROTOCOL_SETUP.md). English is the default and the fallback for every missing string.
// Text is ASCII only (the TFT fonts have no accents; B12/FB11/B46/B56 are even upper-case or letter-limited): ES is written without accents.
// PT/FR/DE are accepted by the protocol but fall back to English until their columns are added in language.cpp.
#pragma once
#include <stdint.h>

namespace lang {
enum Id : uint8_t { EN = 0, ES = 1, PT = 2, FR = 3, DE = 4, COUNT = 5 };
enum Str : uint8_t {
  S_SETUP_L1, S_SETUP_L2, S_SETUP_WAIT, S_DISCONNECTED, S_CONN_LOST, S_SEARCHING, S_NEXT_TRIP, S_CONNECTED, S_PICK_DEST, S_ARRIVED, S_REROUTING, S_REROUTE_BIG, S_START, S_PAIR_TAG, S_PAIR_LABEL, S_PAIR_H1, S_PAIR_H2, S_NO_GPS, S_FW_UPDATE, S_UPD_FAILED, S_FW_KEPT, S_KEEP_OPEN, S_UPD_CANCELLED, S_RESTARTING, S_VERIFYING, S_BOOT_LOADING, S_BOOT_BATTERY, S_BOOT_BT, S_BOOT_READY, S_SU_TAG, S_SU_L1, S_SU_L2, S_SU_L3, S_SU_H1, S_SU_H2, S_DIR0, S_DIR1, S_DIR2, S_DIR3, S_DIR4, S_DIR5, S_DIR6, S_DIR7,
  S_OTA_E_STATE, S_OTA_E_SIZE, S_OTA_E_GAP, S_OTA_E_FLASH, S_OTA_E_MD5, S_OTA_E_BATTERY, S_OTA_E_TIMEOUT, S_OTA_E_ARGS, S_OTA_E_ABORTED, S_OTA_E_NOT_SECURE, S_OTA_E_IMAGE, S_OTA_E_UNKNOWN,
  S_COUNT
};
inline bool valid(uint8_t id) { return id < COUNT; }
void        set(uint8_t id);        // unknown id -> English
uint8_t     get();
const char* tr(Str s);              // never null; falls back to English
const char* cardinal(int idx8);     // 0..7 = N NE E SE S SW W NW
}  // namespace lang