// BleNav - NimBLE GATT server + protocol parser (README section 6).
// BLE callbacks only copy bytes into a FreeRTOS queue; everything is parsed in poll(),
// which runs from loop(), so NavModel and the renderer never see another thread.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "nav_model.h"

class BleNav {
 public:
  void begin(NavModel* model);               // sets up the GATT server; does not advertise yet
  void startAdvertising();                   // call once the boot sequence is over (main, after the first screen is chosen)
  void poll(uint32_t nowMs);                 // drain RX queue, send housekeeping ACKs
  bool connected() const { return connected_; }
  bool pairing()   const { return pairing_; }       // passkey is being displayed
  bool configured() const { return configured_; }   // a valid SETUP_CONFIG was saved (NVS key, startup.h). Pairing does NOT set it.
  bool authed()     const { return authed_; }       // connected AND paired/encrypted (or open link): waiting for / using the app
  uint32_t passkey() const { return passkey_; }
  void setBatteryLevel(uint8_t pct);         // standard Battery Service (0x180F)
  // poll() stops draining the RX queue right after an OTA_BEGIN, so the update screen can build up without flash writes in
  // between. main calls resumeRx() once the transition is over. Cleared on disconnect.
  void resumeRx()        { rxPaused_ = false; }
  bool rxPaused() const  { return rxPaused_; }

  // --- internal: called from NimBLE callbacks (other task) ---
  void _onConnect();
  void _onDisconnect();
  void _onRx(const uint8_t* d, size_t n);
  uint32_t _onPasskey();
  void _onAuthDone();

 private:
  struct RxMsg { uint8_t len; uint8_t data[244]; };
  void handle(const uint8_t* d, size_t n, uint32_t now);
  void send(const uint8_t* d, size_t n);
  void sendHelloAck();
  void sendAck(uint8_t id, uint8_t status, uint32_t expected, uint16_t freeSlots);
  void ackFor(const RouteRing& r, uint8_t status) { sendAck(r.id(), status, r.head(), r.freeSlots()); }

  NavModel* m_ = nullptr;
  QueueHandle_t q_ = nullptr;
  volatile bool connected_ = false, pairing_ = false, evtDisc_ = false, rxPaused_ = false, configured_ = false, authed_ = false;
  uint32_t restartAtMs_ = 0;                 // != 0: restart at this time (after a verified SETUP_CONFIG, once the ACK has left)
  volatile uint32_t passkey_ = 0;
  uint8_t  chunksSinceAck_ = 0;
  uint16_t lastFree_ = 0;
  uint32_t lastTopUpMs_ = 0;
};