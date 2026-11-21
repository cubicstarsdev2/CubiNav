#include "ble_nav.h"
#include <NimBLEDevice.h>
#include <esp_random.h>
#include "config.h"
#include "ota.h"
#include "device_name.h"
#include "startup.h"
#include "device_config.h"

using namespace proto;

// Serial log of the BLE link (connect, pairing, every message in/out except the high-rate POSE / chunk traffic). -DCUBINAV_BLE_LOG=0 to silence.
#ifndef CUBINAV_BLE_LOG
#define CUBINAV_BLE_LOG 1
#endif
#if CUBINAV_BLE_LOG
  #define BLOG(...) do { Serial.printf(__VA_ARGS__); Serial.print("\r\n"); } while (0)
#else
  #define BLOG(...) do {} while (0)
#endif

// Default = NimBLE-Arduino 1.x API. 2.x is used only if the library's version header says major >= 2,
// or if forced with -DCUBINAV_NIMBLE_V2=1 (force 1.x with -DCUBINAV_NIMBLE_V2=0).
#ifndef CUBINAV_NIMBLE_V2
  #if defined(__has_include)
    #if __has_include(<NimBLEVersion.h>)
      #include <NimBLEVersion.h>
    #endif
  #endif
  #if defined(NIMBLE_CPP_VERSION_MAJOR) && (NIMBLE_CPP_VERSION_MAJOR >= 2)
    #define CUBINAV_NIMBLE_V2 1
  #else
    #define CUBINAV_NIMBLE_V2 0
  #endif
#endif

namespace {
BleNav*               g_self = nullptr;
NimBLECharacteristic* g_tx   = nullptr;
NimBLECharacteristic* g_batt = nullptr;

#if CUBINAV_NIMBLE_V2
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    s->updateConnParams(ci.getConnHandle(), 12, 24, 0, 400);   // 15-30 ms, 4 s supervision timeout
    g_self->_onConnect();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override { g_self->_onDisconnect(); }
  uint32_t onPassKeyDisplay() override { return g_self->_onPasskey(); }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    if (!ci.isEncrypted()) { NimBLEDevice::getServer()->disconnect(ci.getConnHandle()); return; }
    g_self->_onAuthDone();
  }
};
class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    auto v = c->getValue();
    g_self->_onRx((const uint8_t*)v.data(), v.size());
  }
};
#else   // NimBLE-Arduino 1.x
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
    s->updateConnParams(d->conn_handle, 12, 24, 0, 400);
    g_self->_onConnect();
  }
  void onDisconnect(NimBLEServer*) override { g_self->_onDisconnect(); }
  uint32_t onPassKeyRequest() override { return g_self->_onPasskey(); }
  void onAuthenticationComplete(ble_gap_conn_desc* d) override {
    if (!d->sec_state.encrypted) { NimBLEDevice::getServer()->disconnect(d->conn_handle); return; }
    g_self->_onAuthDone();
  }
};
class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    auto v = c->getValue();                                  // std::string or NimBLEAttValue
    g_self->_onRx((const uint8_t*)v.data(), v.size());
  }
};
#endif

inline uint16_t rdU16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t rdU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline int32_t  rdI32(const uint8_t* p) { int32_t  v; memcpy(&v, p, 4); return v; }
inline void     wrU16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
inline void     wrU32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
}  // namespace

// ---------------------------------------------------------------------------------------------
void BleNav::begin(NavModel* m) {
  m_ = m; g_self = this;
  q_ = xQueueCreate(BLE_RXQ_DEPTH, sizeof(RxMsg));
  ota::setSender([](const uint8_t* m, size_t n) { g_self->send(m, n); });   // OTA_ACK goes out on the TX characteristic

  NimBLEDevice::init(devname::get());
  NimBLEDevice::setMTU(BLE_MTU);
  // First boot = no "setup done" key in NVS yet -> the SETUP screen shows. Once setup completes (first pairing, or first
  // connection on an open link) the key is saved and every later boot skips the screen. The key alone decides: old BLE bonds
  // left in flash do not count (startup.h).
#if CUBINAV_FORCE_SETUP
  startup::clearSetup();                                       // bench: every boot is a first boot (see config.h)
#endif
  configured_ = startup::setupDone();
#if BLE_REQUIRE_PAIRING
  NimBLEDevice::setSecurityAuth(true, true, true);             // bonding, MITM, LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);      // we show the passkey on the TFT
#endif

  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCb());
#if CUBINAV_NIMBLE_V2
  srv->advertiseOnDisconnect(true);
#endif

  NimBLEService* svc = srv->createService(BLE_SERVICE_UUID);
  uint32_t rxProps = NIMBLE_PROPERTY::WRITE_NR;
#if BLE_REQUIRE_PAIRING
  rxProps |= NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;   // writing triggers pairing
#endif
  NimBLECharacteristic* rx = svc->createCharacteristic(BLE_RX_UUID, rxProps);
  rx->setCallbacks(new RxCb());
  g_tx = svc->createCharacteristic(BLE_TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  NimBLEService* bat = srv->createService("180F");
  g_batt = bat->createCharacteristic("2A19", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  uint8_t lvl = 100; g_batt->setValue(&lvl, 1);

#if CUBINAV_NIMBLE_V2
  srv->start();
#else
  svc->start();                                              // 1.x: services are started individually
  bat->start();
#endif

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
#if CUBINAV_NIMBLE_V2
  adv->setName(devname::get());
  adv->enableScanResponse(true);
#else
  adv->setScanResponse(true);                                // name comes from NimBLEDevice::init()
#endif
  // Advertising does NOT start here: main calls startAdvertising() once the boot sequence is over and the first screen has been
  // chosen. A phone with an old bond reconnects within a second or two; if it could do that during the boot animation, setup would
  // be marked done before the SETUP screen was ever picked and the unit would boot straight to IDLE.
}

void BleNav::startAdvertising() {
  NimBLEDevice::getAdvertising()->start();
}

void BleNav::setBatteryLevel(uint8_t pct) {
  if (!g_batt) return;
  g_batt->setValue(&pct, 1);
  if (connected_) g_batt->notify();
}

// ---- callbacks (NimBLE task) -----------------------------------------------------------------
void BleNav::_onConnect() {
  connected_ = true; if (q_) xQueueReset(q_);
  BLOG("[ble] connected (configured=%d)", (int)configured_);
#if !BLE_REQUIRE_PAIRING
  authed_ = true;                          // open link (bench): there is no pairing
#endif
}
void BleNav::_onDisconnect() { BLOG("[ble] disconnected (authed=%d)", (int)authed_); connected_ = false; pairing_ = false; authed_ = false; evtDisc_ = true; if (q_) xQueueReset(q_); }
uint32_t BleNav::_onPasskey() { BLOG("[ble] passkey requested"); passkey_ = esp_random() % 1000000UL; pairing_ = true; return passkey_; }
void BleNav::_onAuthDone()   { BLOG("[ble] authenticated + encrypted"); pairing_ = false; authed_ = true; }   // pairing is only authentication; setup completes with SETUP_CONFIG   // only called on an encrypted link (see ServerCb)
void BleNav::_onRx(const uint8_t* d, size_t n) {
  if (!q_ || n == 0 || n > BLE_MAX_MSG) { BLOG("[ble] RX rejected len=%u", (unsigned)n); if (m_) m_->badMsgs++; return; }
  RxMsg msg; msg.len = (uint8_t)n; memcpy(msg.data, d, n);
  if (xQueueSend(q_, &msg, 0) != pdTRUE && m_) { m_->droppedMsgs++; BLOG("[ble] RX queue full, 0x%02X dropped", d[0]); }   // route gaps recover via ACK
}

// ---- loop() side -----------------------------------------------------------------------------
void BleNav::poll(uint32_t now) {
  if (!q_) return;
  if (restartAtMs_ && (int32_t)(now - restartAtMs_) >= 0) { Serial.println("[setup] restarting"); Serial.flush(); ESP.restart(); }
  if (evtDisc_) { evtDisc_ = false; rxPaused_ = false; ota::onDisconnect(); m_->resetSession(); }
  RxMsg msg;
  while (!rxPaused_ && xQueueReceive(q_, &msg, 0) == pdTRUE) handle(msg.data, msg.len, now);

  // The phone only learns about free slots from ACKs, and trimming frees them silently:
  // report when at least 64 more slots became free (checked once per second).
  RouteRing* a = m_->active;
  if (connected_ && a->valid() && now - lastTopUpMs_ >= 1000) {
    lastTopUpMs_ = now;
    if (a->freeSlots() >= lastFree_ + 64) ackFor(*a, ACK_OK);
  }
}

void BleNav::send(const uint8_t* d, size_t n) {
  if (!(connected_ && g_tx)) { BLOG("[ble] TX 0x%02X dropped: not connected", d[0]); return; }
  g_tx->notify(d, n);                                                  // returns void in this NimBLE version: no delivery result
  if (d[0] != ROUTE_ACK) BLOG("[ble] TX 0x%02X %uB sent", d[0], (unsigned)n);
}

void BleNav::sendHelloAck() {
  uint8_t b[12];
  b[0] = HELLO_ACK; b[1] = PROTO_VERSION;
  wrU16(b + 2, (uint16_t)(FW_VERSION & 0xFFFF)); wrU16(b + 4, SCREEN_W); wrU16(b + 6, SCREEN_H); wrU16(b + 8, RouteRing::CAP);   // bytes 2-3: major/minor/patch only (old apps read just this)
  BLOG("[ble] HELLO -> HELLO_ACK fw=0x%06X configured=%d", (unsigned)FW_VERSION, (int)configured_);
  b[10] = configured_ ? 1 : 0;                          // optional byte 10 (FW 1.5.0+): 0 = first-time setup pending
  b[11] = (uint8_t)FW_VERSION_LETTER;                   // optional byte 11: version letter, 0 = none, 1 = A ... 26 = Z (1.0.0R -> 18)
  send(b, sizeof(b));
}

void BleNav::sendAck(uint8_t id, uint8_t status, uint32_t expected, uint16_t freeSlots) {
  uint8_t b[9];
  b[0] = ROUTE_ACK; b[1] = id; b[2] = status;
  wrU32(b + 3, expected); wrU16(b + 7, freeSlots);
  send(b, sizeof(b));
  lastFree_ = freeSlots;
}

// Messages shorter than expected are ignored and counted; extra trailing bytes are tolerated
// (new fields are only ever appended - README 6.8). Unknown types are ignored.
void BleNav::handle(const uint8_t* d, size_t n, uint32_t now) {
  NavModel& m = *m_;
  if (n < 1) return;
  if (d[0] != POSE && d[0] != ROUTE_CHUNK && d[0] != OTA_CHUNK) BLOG("[ble] RX 0x%02X %uB", d[0], (unsigned)n);
  if (d[0] == 'A' && m.handleAt(d, n)) return;          // AT:"message" -> flashing panel ('A' is not a protocol id)
  switch (d[0]) {
    case HELLO:
      if (n < 2) { m.badMsgs++; break; }
      m.resetSession();
      sendHelloAck();                                   // phone checks proto and stops on mismatch
      break;

    case REBASE:
      if (n < 9) { m.badMsgs++; break; }
      {
        int32_t dx = rdI32(d + 1), dy = rdI32(d + 5);
        m.active->rebase(dx, dy); m.shadow->rebase(dx, dy); m.pose.rebase(dx, dy); m.routeRev++;
      }
      break;

    case POSE:
      if (n < 16) { m.badMsgs++; break; }
      m.pose.onPose(rdI32(d + 1), rdI32(d + 5), rdU16(d + 9), rdU16(d + 11), (d[15] & 1) != 0, now);
      m.distToTurn = rdU16(d + 13);
      // Optional byte 16 (v1.2.0+): posted speed limit in km/h. 0 = unknown -> keep the previous one.
      if (n >= 17 && d[16] != 0 && d[16] <= SPEED_LIMIT_MAX_KMH) m.speedLimitKmh = d[16];
      Serial.printf("POSE n=%u lim=%u\n", (unsigned)n, n >= 17 ? d[16] : 0);
      break;

    case ROUTE_BEGIN:
      if (n < 14) { m.badMsgs++; break; }
      m.shadow->begin(d[1], rdU32(d + 2), rdI32(d + 6), rdI32(d + 10));
      chunksSinceAck_ = 0;
      ackFor(*m.shadow, ACK_OK);
      break;

    case ROUTE_CHUNK: {
      if (n < 7) { m.badMsgs++; break; }
      uint8_t id = d[1], cnt = d[6];
      if (n < (size_t)(7 + 4 * cnt)) { m.badMsgs++; break; }
      RouteRing* r = (m.shadow->valid() && m.shadow->id() == id) ? m.shadow
                   : (m.active->valid() && m.active->id() == id) ? m.active : nullptr;
      if (!r) { sendAck(id, ACK_UNKNOWN, 0, 0); break; }
      uint8_t st = r->append(rdU32(d + 2), d + 7, cnt);
      if (st == ACK_OK && r == m.active) m.routeRev++;
      if (st != ACK_OK || ++chunksSinceAck_ >= 4) { ackFor(*r, st); chunksSinceAck_ = 0; }
      break;
    }

    case ROUTE_COMMIT:
      if (n < 2) { m.badMsgs++; break; }
      if (m.shadow->valid() && m.shadow->id() == d[1]) { m.commitShadow(); ackFor(*m.active, ACK_OK); }
      else if (!(m.active->valid() && m.active->id() == d[1])) sendAck(d[1], ACK_UNKNOWN, 0, 0);
      break;

    case ROUTE_CLEAR:
      if (n < 2) { m.badMsgs++; break; }
      if (m.shadow->valid() && m.shadow->id() == d[1]) m.shadow->clear();
      if (m.active->valid() && m.active->id() == d[1]) { m.active->clear(); m.routeRev++; m.speedLimitKmh = 0; }
      break;

    case MANEUVER: {
      if (n < 2) { m.badMsgs++; break; }
      m.icon = d[1] <= 10 ? d[1] : 0;
      size_t k = 0;
      for (; k < 40 && 2 + k < n && d[2 + k]; k++) {
        uint8_t c = d[2 + k];
        m.street[k] = (c >= 32 && c < 127) ? (char)c : '?';   // device font is ASCII only
      }
      m.street[k] = 0;
      break;
    }

    case CONFIG:
      if (n < 4) { m.badMsgs++; break; }
      {
        uint16_t z = rdU16(d + 1);
        if (z >= 5 && z <= 2000) m.cfg.zoom = z;
        uint8_t f = d[3];
        m.cfg.headingUp = f & CFG_HEADING_UP;
        m.cfg.invert    = f & CFG_INVERT;
        m.cfg.showFps   = f & CFG_SHOW_FPS;
        m.cfg.imperial  = f & CFG_IMPERIAL;
      }
      break;

    case SETUP_CONFIG: {                                // persistent first-time setup (PROTOCOL_SETUP.md); NVS is written here, never in a NimBLE callback
      if (restartAtMs_) break;                          // already applied, restarting
      uint8_t st = (n == 6) ? devcfg::validate(d[1], d[2], d[3], rdU16(d + 4)) : (uint8_t)SETUP_E_ARGS;
      if (st == SETUP_OK) {
        devcfg::Config c; c.language = d[2]; c.flags = d[3]; c.zoom = rdU16(d + 4);
        if (!devcfg::save(c)) st = SETUP_E_NVS;
      }
      BLOG("[setup] lang=%u flags=0x%02X zoom=%u len=%u -> status %u", n > 2 ? d[2] : 0, n > 3 ? d[3] : 0, n >= 6 ? rdU16(d + 4) : 0, (unsigned)n, (unsigned)st);
      const uint8_t a[2] = { SETUP_ACK, st };
      send(a, sizeof(a));
      if (st == SETUP_OK) restartAtMs_ = (now + 600) | 1;   // ACK is sent only after the verified save; give it time to leave, then restart
      break;
    }

    case NAV_STATE:
      if (n < 2) { m.badMsgs++; break; }
      if (d[1] <= NAV_NO_GPS) m.navState = d[1];
      if (d[1] == NAV_IDLE) m.speedLimitKmh = 0;           // trip over: do not carry the limit into the next one
      break;

    case OTA_BEGIN: case OTA_CHUNK: case OTA_END: case OTA_ABORT:
      // With BLE_REQUIRE_PAIRING the RX characteristic only accepts writes on an encrypted link,
      // so a message that got here is already secure.
      ota::onMessage(d[0], d + 1, n - 1, true);
      if (d[0] == OTA_BEGIN && ota::active()) rxPaused_ = true;   // chunks wait in the queue until the update screen is up (main)
      break;

    default: break;                                     // unknown type: ignore
  }
}