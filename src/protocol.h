// cubiNav BLE protocol v1 - message ids and enums (see README section 6).
#pragma once
#include <stdint.h>

#define PROTO_VERSION 1

// POSE byte 16 (optional, appended in FW 1.2.0): posted speed limit in km/h, 0 = unknown (device keeps the previous one).
#define SPEED_LIMIT_MAX_KMH 250

// ---- Firmware version --------------------------------------------------------------------------------
// >>> To release a new firmware, edit ONLY this string ("major.minor.patch" + optional letter) and put the same
// >>> "## x.y.z" line at the top of README.md (the phone reads the README to decide whether to update).
// No build script needed: the string is parsed at compile time. Limits: major 0-255, minor 0-15, patch 0-15,
// then at most ONE letter (e.g. "1.0.0R"; case-insensitive, shown upper-case).
// Can still be overridden from the build flags:  -DFW_VERSION_STRING=\"1.2.3\"
#ifndef FW_VERSION_STRING
#define FW_VERSION_STRING "1.0.2R"
#endif

// Wire format (HELLO_ACK / OTA_BEGIN, u32): (letter << 16) | (major << 8) | (minor << 4) | patch.
// letter: 0 = none, 1 = A ... 26 = Z.  1.0.0 == 0x00000100, 1.0.0R == 0x00120100.
// (C++11 constexpr: single-expression recursive functions only.)
namespace fwv {
constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }
constexpr bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr int  num(const char* s, int i, int acc = 0) { return isDigit(s[i]) ? num(s, i + 1, acc * 10 + (s[i] - '0')) : acc; }
constexpr int  skipDigits(const char* s, int i) { return isDigit(s[i]) ? skipDigits(s, i + 1) : i; }
constexpr int  nextPart(const char* s, int i) { return s[i] == '.' ? i + 1 : (s[i] == 0 ? i : nextPart(s, i + 1)); }
constexpr int  major(const char* s) { return num(s, 0); }
constexpr int  minor(const char* s) { return num(s, nextPart(s, 0)); }
constexpr int  patch(const char* s) { return num(s, nextPart(s, nextPart(s, 0))); }
// Optional single letter right after the patch digits: 0 = none, 1 = A ... 26 = Z.
constexpr int  letterAt(const char* s, int i) { return s[i] == 0 ? 0 : ((s[i] >= 'a' && s[i] <= 'z') ? (s[i] - 'a' + 1) : (s[i] - 'A' + 1)); }
constexpr int  letter(const char* s) { return letterAt(s, skipDigits(s, nextPart(s, nextPart(s, 0)))); }
// "<digits>.<digits>.<digits>" optionally followed by ONE letter (e.g. "1.0.0R")
constexpr bool endOrLetter(const char* s, int i) { return s[i] == 0 || (isAlpha(s[i]) && s[i + 1] == 0); }
constexpr int  dotAfterDigits(const char* s, int i) { return (isDigit(s[i]) && s[skipDigits(s, i)] == '.') ? skipDigits(s, i) + 1 : -1; }
constexpr bool tail(const char* s, int i)  { return isDigit(s[i]) && endOrLetter(s, skipDigits(s, i)); }
constexpr bool part3(const char* s, int i) { return i >= 0 && tail(s, i); }
constexpr bool part2(const char* s, int i) { return i >= 0 && part3(s, dotAfterDigits(s, i)); }
constexpr bool valid(const char* s)        { return part2(s, dotAfterDigits(s, 0)); }
}  // namespace fwv

static_assert(fwv::valid(FW_VERSION_STRING), "FW_VERSION_STRING must look like \"1.2.3\" or \"1.2.3R\" (at most one letter, after the last digit)");
#define FW_VERSION_MAJOR  (fwv::major(FW_VERSION_STRING))
#define FW_VERSION_MINOR  (fwv::minor(FW_VERSION_STRING))
#define FW_VERSION_PATCH  (fwv::patch(FW_VERSION_STRING))
#define FW_VERSION_LETTER (fwv::letter(FW_VERSION_STRING))
static_assert(FW_VERSION_MAJOR >= 0 && FW_VERSION_MAJOR <= 255 &&
              FW_VERSION_MINOR >= 0 && FW_VERSION_MINOR <= 15 &&
              FW_VERSION_PATCH >= 0 && FW_VERSION_PATCH <= 15,
              "Version must fit the code: major 0-255, minor 0-15, patch 0-15");
static_assert(FW_VERSION_LETTER >= 0 && FW_VERSION_LETTER <= 26, "Version letter must be A-Z");
#define FW_VERSION    ((uint32_t)(((uint32_t)FW_VERSION_LETTER << 16) | (FW_VERSION_MAJOR << 8) | (FW_VERSION_MINOR << 4) | FW_VERSION_PATCH))
// Split a version code back into its parts (for display).
#define FW_V_MAJOR(v)  (((v) >> 8) & 0xFF)
#define FW_V_MINOR(v)  (((v) >> 4) & 0x0F)
#define FW_V_PATCH(v)  ((v) & 0x0F)
#define FW_V_LETTER(v) (((v) >> 16) & 0x1F)   // 0 = none, 1 = A ... 26 = Z

namespace proto {
enum Msg : uint8_t {
  HELLO = 0x01, REBASE = 0x03, POSE = 0x10,
  ROUTE_BEGIN = 0x20, ROUTE_CHUNK = 0x21, ROUTE_COMMIT = 0x22, ROUTE_CLEAR = 0x23,
  MANEUVER = 0x30, CONFIG = 0x40, SETUP_CONFIG = 0x41, NAV_STATE = 0x50,
  HELLO_ACK = 0x81, ROUTE_ACK = 0x82, SETUP_ACK = 0x84,
  // ---- OTA firmware update (see OTA_PROTOCOL.md) ----
  OTA_BEGIN = 0x60, OTA_CHUNK = 0x61, OTA_END = 0x62, OTA_ABORT = 0x63,   // phone -> device
  OTA_ACK = 0x83                                                           // device -> phone
};
enum OtaState : uint8_t { OTA_IDLE = 0, OTA_RECEIVING = 1, OTA_VERIFYING = 2, OTA_DONE = 3, OTA_ERROR = 4 };
enum OtaStatus : uint8_t {
  OTA_OK = 0, OTA_E_STATE = 1, OTA_E_SIZE = 2, OTA_E_GAP = 3, OTA_E_FLASH = 4, OTA_E_MD5 = 5,
  OTA_E_BATTERY = 6, OTA_E_TIMEOUT = 7, OTA_E_ARGS = 8, OTA_E_ABORTED = 9, OTA_E_NOT_SECURE = 10, OTA_E_IMAGE = 11
};
// SETUP_ACK status (see PROTOCOL_SETUP.md)
enum SetupStatus : uint8_t { SETUP_OK = 0, SETUP_E_SCHEMA = 1, SETUP_E_LANG = 2, SETUP_E_ARGS = 3, SETUP_E_NVS = 4 };
enum Ack : uint8_t { ACK_OK = 0, ACK_GAP = 1, ACK_FULL = 2, ACK_UNKNOWN = 3 };
enum NavStateId : uint8_t { NAV_IDLE = 0, NAV_NAVIGATING = 1, NAV_REROUTING = 2, NAV_ARRIVED = 3, NAV_NO_GPS = 4 };
enum CfgFlag : uint8_t { CFG_HEADING_UP = 1, CFG_INVERT = 2, CFG_SHOW_FPS = 4, CFG_IMPERIAL = 8 };
}  // namespace proto