// Persistent device configuration written by SETUP_CONFIG (PROTOCOL_SETUP.md). NVS namespace "cubinav".
//   "cfg"   blob, 5 bytes [schema][language][flags][zoom lo][zoom hi]  - one key, so a power loss never leaves a mix of old and new values
//   "setup" u8 = 1  - written LAST, only after the blob has been read back and verified. It alone means "configured".
// Devices that already have "setup" = 1 but no "cfg" (firmware < 1.5.0) are configured and get the defaults (English).
#pragma once
#include <Preferences.h>
#include <string.h>
#include "protocol.h"
#include "nav_model.h"
#include "language.h"
#include "startup.h"

namespace devcfg {

constexpr uint8_t  SCHEMA    = 1;
constexpr uint16_t ZOOM_MIN  = 5, ZOOM_MAX = 2000;
constexpr uint8_t  FLAGS_ALL = proto::CFG_HEADING_UP | proto::CFG_INVERT | proto::CFG_SHOW_FPS | proto::CFG_IMPERIAL;
constexpr const char* KEY_CFG = "cfg";

struct Config { uint8_t language = lang::EN; uint8_t flags = proto::CFG_HEADING_UP; uint16_t zoom = 60; };

// SETUP_ACK status for these values (SETUP_OK if valid).
inline uint8_t validate(uint8_t schema, uint8_t language, uint8_t flags, uint16_t zoom) {
  if (schema != SCHEMA)                    return proto::SETUP_E_SCHEMA;
  if (!lang::valid(language))              return proto::SETUP_E_LANG;
  if ((flags & ~FLAGS_ALL) || zoom < ZOOM_MIN || zoom > ZOOM_MAX) return proto::SETUP_E_ARGS;
  return proto::SETUP_OK;
}

// Missing or invalid record -> defaults (English). Always returns usable values.
inline void load(Config& c) {
  c = Config();
  Preferences p;
  if (!p.begin(startup::NS, false)) return;
  uint8_t b[5];
  if (p.getBytes(KEY_CFG, b, sizeof(b)) == sizeof(b)) {
    const uint16_t z = (uint16_t)(b[3] | (b[4] << 8));
    if (validate(b[0], b[1], b[2], z) == proto::SETUP_OK) { c.language = b[1]; c.flags = b[2]; c.zoom = z; }
  }
  p.end();
}

// Write, read back, then set the setup marker. false = nothing was marked as configured.
inline bool save(const Config& c) {
  const uint8_t b[5] = { SCHEMA, c.language, c.flags, (uint8_t)(c.zoom & 0xFF), (uint8_t)(c.zoom >> 8) };
  Preferences p;
  if (!p.begin(startup::NS, false)) return false;
  bool ok = p.putBytes(KEY_CFG, b, sizeof(b)) == sizeof(b);
  uint8_t r[5];
  ok = ok && p.getBytes(KEY_CFG, r, sizeof(r)) == sizeof(r) && memcmp(b, r, sizeof(b)) == 0;
  if (ok) { p.putUChar(startup::KEY, 1); ok = p.getUChar(startup::KEY, 0) == 1; }
  p.end();
  return ok;
}

inline void apply(const Config& c, NavConfig& n) {
  n.zoom      = c.zoom;
  n.headingUp = c.flags & proto::CFG_HEADING_UP;
  n.invert    = c.flags & proto::CFG_INVERT;
  n.showFps   = c.flags & proto::CFG_SHOW_FPS;
  n.imperial  = c.flags & proto::CFG_IMPERIAL;
}

}  // namespace devcfg
