#include "ota.h"
#include <Arduino.h>
#include <Update.h>
#include <string.h>
#include "battery.h"
#include "language.h"
#include <esp_heap_caps.h>

namespace ota {
namespace {

using namespace proto;

SendFn   sendFn    = nullptr;
Info     in;
uint8_t  letter_   = 0;          // target version letter from OTA_BEGIN (optional trailing byte): 0 = none, 1 = A ... 26 = Z
uint32_t size_     = 0;          // total image size
uint32_t next_     = 0;          // next expected byte offset
char     md5hex_[33] = {0};
uint8_t  sinceAck_ = 0;          // good chunks since the last OTA_ACK
bool     gapSent_  = false;      // a GAP ack is already out for the current gap (no spamming)
uint32_t lastRxMs_ = 0;
uint32_t lastAckMs_ = 0;         // last OTA_ACK sent (any kind)
uint32_t lastGapMs_ = 0;         // last GAP ack sent
uint32_t stateMs_  = 0;          // when ERROR / DONE was entered

// Update.begin() mallocs a 4 KB sector buffer and fails with "malloc failed" when the heap has no contiguous block that big.
// Update.abort() frees that buffer, but the BLE stack / UI grab the hole before the next OTA_BEGIN. So keep a same-sized block
// parked here: it is handed back right before Update.begin() (so Update's malloc takes the same hole) and taken again as soon
// as the update is aborted / finished / failed.
// The block is made BIGGER than 4 KB (slack): the BLE task can allocate between our free() and Update's malloc(), and a block of
// exactly 4096 would then be left too small (seen: maxBlock=4084). With slack, Update's 4 KB still fits after such a bite.
void*  reserve_ = nullptr;
size_t reserveSz_ = 0;
void reserveTake() {
  if (reserve_) return;
  static const size_t sizes[] = { 8192, 6144, 5120, 4608 };
  for (size_t sz : sizes) {
    reserve_ = malloc(sz);
    if (reserve_) { reserveSz_ = sz; return; }
  }
  reserveSz_ = 0;
}
inline void reserveRelease() { if (reserve_) { free(reserve_); reserve_ = nullptr; } }

// ms elapsed since `t`, clamped to 0. `now` is sampled at the top of loop() and the stamps below are taken with
// millis() while ble.poll() runs, i.e. they can be NEWER than `now`: a plain `now - t` would wrap to ~4e9.
inline uint32_t since(uint32_t now, uint32_t t) { return (int32_t)(now - t) > 0 ? now - t : 0; }

inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }   // little-endian host
inline uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

// OTA_ACK: [0x83][status][state][next u32][window][ackEvery]
void sendAck(uint8_t status) {
  lastAckMs_ = millis();
  if (!sendFn) return;
  uint8_t m[9];
  m[0] = OTA_ACK; m[1] = status; m[2] = in.state;
  memcpy(m + 3, &next_, 4);
  m[7] = OTA_WINDOW; m[8] = OTA_ACK_EVERY;
  sendFn(m, sizeof(m));
}

// Transfer failed: drop the partial image, show the error screen, tell the phone.
void enterError(uint8_t err, bool notify) {
  Update.abort();
  reserveTake();
  Serial.printf("OTA error %u at %u/%u\n", (unsigned)err, (unsigned)next_, (unsigned)size_);
  in.state = OTA_ERROR; in.err = err;
  stateMs_ = millis();
  if (notify) sendAck(err);
}

uint8_t flashErr() {
  return Update.getError() == UPDATE_ERROR_MAGIC_BYTE ? OTA_E_IMAGE : OTA_E_FLASH;
}

// ---- OTA_BEGIN: size u32 | md5[16] | version u16 | letter u8 (optional) ---------------------------------------------------
void doBegin(const uint8_t* p, size_t len, bool secure) {
#if BLE_REQUIRE_PAIRING
  if (!secure) { sendAck(OTA_E_NOT_SECURE); return; }
#else
  (void)secure;
#endif
  if (len < 22) { sendAck(OTA_E_ARGS); return; }
  if (in.state == OTA_VERIFYING || in.state == OTA_DONE) { sendAck(OTA_E_STATE); return; }

  const uint32_t size = rd32(p);
  if (size < 4096) { sendAck(OTA_E_SIZE); return; }

  const int batt = battery::percent();
  if (batt >= 0 && batt < OTA_MIN_BATT_PCT) { sendAck(OTA_E_BATTERY); return; }

  if (in.state == OTA_RECEIVING) { Update.abort(); in.state = OTA_IDLE; }   // phone restarted: start over
  reserveTake();                                              // make sure the hole exists (no-op if already held)

  static const char* DIGITS = "0123456789abcdef";             // (not HEX: Arduino defines that as a macro)
  for (int i = 0; i < 16; i++) { md5hex_[2 * i] = DIGITS[p[4 + i] >> 4]; md5hex_[2 * i + 1] = DIGITS[p[4 + i] & 15]; }
  md5hex_[32] = 0;

  const size_t held = reserveSz_ ? (reserve_ ? reserveSz_ : 0) : 0;
  reserveRelease();                                           // Update.begin() mallocs its 4 KB buffer from this hole
  if (!Update.begin(size)) {                                  // fails if it does not fit / no OTA partition
    Serial.printf("Update.begin failed (err %u) reserve=%u free=%u maxBlock=%u\n", (unsigned)Update.getError(), (unsigned)held,
                  (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    reserveTake();
    sendAck(Update.getError() == UPDATE_ERROR_NO_PARTITION ? OTA_E_FLASH : OTA_E_SIZE);
    return;
  }
  Update.setMD5(md5hex_);                                     // Update.end() checks the whole image against it

  size_ = size; next_ = 0; sinceAck_ = 0; gapSent_ = false;
  lastRxMs_ = millis();
  in.state = OTA_RECEIVING; in.err = OTA_OK;
  in.done = 0; in.total = size; in.version = rd16(p + 20);
  letter_ = (len >= 23 && p[22] >= 1 && p[22] <= 26) ? p[22] : 0;   // older phones omit it
  sendAck(OTA_OK);                                            // carries window + ackEvery for the phone
}

// ---- OTA_CHUNK: offset u32 | data[1..239] ----------------------------------------------------------
void doChunk(const uint8_t* p, size_t len) {
  if (in.state != OTA_RECEIVING || len < 5) return;           // late chunks after an error: ignore
  const uint32_t off = rd32(p);
  const uint8_t* d = p + 4;
  const size_t   n = len - 4;
  lastRxMs_ = millis();

  if (off != next_) {                                         // lost / duplicated chunk: tell the phone
    const uint32_t t = millis();                              // (repeated every 500 ms: a single lost GAP ack must not hang the transfer)
    if (!gapSent_ || t - lastGapMs_ > 500) { gapSent_ = true; lastGapMs_ = t; sendAck(OTA_E_GAP); }
    return;
  }
  if (n > size_ - next_) { enterError(OTA_E_SIZE, true); return; }

  if (Update.write((uint8_t*)d, n) != n) { enterError(flashErr(), true); return; }

  next_ += n; in.done = next_;
  gapSent_ = false;
  if (++sinceAck_ >= OTA_ACK_EVERY || next_ == size_) { sinceAck_ = 0; sendAck(OTA_OK); }
}

// ---- OTA_END ---------------------------------------------------------------------------------------------
void doEnd() {
  if (in.state == OTA_DONE) { sendAck(OTA_OK); return; }       // phone re-sent END: idempotent
  if (in.state != OTA_RECEIVING) { sendAck(OTA_E_STATE); return; }
  if (next_ != size_) { sendAck(OTA_E_GAP); return; }          // `next` tells the phone where to resume

  in.state = OTA_VERIFYING;
  if (!Update.end()) {                                         // size + MD5 check, then switches the boot partition
    enterError(Update.getError() == UPDATE_ERROR_MD5 ? OTA_E_MD5 : flashErr(), true);
    return;
  }
  in.state = OTA_DONE; stateMs_ = millis();
  sendAck(OTA_OK);                                             // update() reboots OTA_REBOOT_DELAY_MS later
}

// ---- OTA_ABORT (user cancelled in the app) ------------------------------------------------------------
void doAbort() {
  if (in.state == OTA_RECEIVING || in.state == OTA_VERIFYING) { Update.abort(); reserveTake(); }
  if (in.state != OTA_DONE) { in.state = OTA_IDLE; in.err = OTA_OK; }
  sendAck(OTA_E_ABORTED);
}

}  // namespace

void setSender(SendFn fn) { sendFn = fn; }

void reserveHeap() {
  reserveTake();
  Serial.printf("OTA reserve: %u bytes, maxBlock=%u\n", (unsigned)(reserve_ ? reserveSz_ : 0), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// main calls this every frame while it is holding the RX queue: the transfer is alive, just paused on our side.
void keepAlive() { if (in.state == OTA_RECEIVING) lastRxMs_ = millis(); }

bool onMessage(uint8_t type, const uint8_t* p, size_t len, bool secure) {
  switch (type) {
    case OTA_BEGIN: doBegin(p, len, secure); return true;
    case OTA_CHUNK: doChunk(p, len);         return true;
    case OTA_END:   doEnd();                 return true;
    case OTA_ABORT: doAbort();               return true;
  }
  return false;
}

void onDisconnect() {
  if (in.state == OTA_RECEIVING || in.state == OTA_VERIFYING) enterError(OTA_E_ABORTED, false);  // no link to notify
}

void update(uint32_t now) {
  if (!reserve_ && in.state == OTA_IDLE) reserveTake();       // missed earlier (heap was too fragmented then): retry while idle
  switch (in.state) {
    case OTA_RECEIVING:
      if (since(now, lastRxMs_) > OTA_IDLE_TIMEOUT_MS) enterError(OTA_E_TIMEOUT, true);
      else if (since(now, lastRxMs_) > 1000 && since(now, lastAckMs_) > 1000) {
        // Nothing arrived for a second: re-announce where we are, so a lost ACK / GAP ack cannot deadlock the phone.
        gapSent_ = false;
        sendAck(OTA_OK);                                      // carries next_ = resume point
      }
      break;
    case OTA_ERROR:
      if (since(now, stateMs_) > OTA_ERROR_SHOW_MS) { in.state = OTA_IDLE; in.err = OTA_OK; }
      break;
    case OTA_DONE:
      if (since(now, stateMs_) > OTA_REBOOT_DELAY_MS) ESP.restart();
      break;
  }
}

bool        active() { return in.state != OTA_IDLE; }
const Info& info()   { return in; }
uint8_t     letter() { return letter_; }   // target version letter (0 = none); declared in ui.cpp, add it to ota.h if you prefer

const char* errorText(uint8_t s) {
  switch (s) {
    case OTA_E_STATE:      return lang::tr(lang::S_OTA_E_STATE);
    case OTA_E_SIZE:       return lang::tr(lang::S_OTA_E_SIZE);
    case OTA_E_GAP:        return lang::tr(lang::S_OTA_E_GAP);
    case OTA_E_FLASH:      return lang::tr(lang::S_OTA_E_FLASH);
    case OTA_E_MD5:        return lang::tr(lang::S_OTA_E_MD5);
    case OTA_E_BATTERY:    return lang::tr(lang::S_OTA_E_BATTERY);
    case OTA_E_TIMEOUT:    return lang::tr(lang::S_OTA_E_TIMEOUT);
    case OTA_E_ARGS:       return lang::tr(lang::S_OTA_E_ARGS);
    case OTA_E_ABORTED:    return lang::tr(lang::S_OTA_E_ABORTED);
    case OTA_E_NOT_SECURE: return lang::tr(lang::S_OTA_E_NOT_SECURE);
    case OTA_E_IMAGE:      return lang::tr(lang::S_OTA_E_IMAGE);
    default:               return lang::tr(lang::S_OTA_E_UNKNOWN);
  }
}

}  // namespace ota