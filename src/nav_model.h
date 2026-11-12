// NavModel - everything the BLE parser writes and the UI reads. Only touched from loop().
#pragma once
#include "route_ring.h"
#include "pose_filter.h"

struct NavConfig {
  uint16_t zoom = 60;              // px per 100 m
  bool headingUp = true, invert = false, showFps = false, imperial = false;
};

struct NavModel {
  RouteRing  ringA, ringB;
  RouteRing* active = &ringA;      // drawn
  RouteRing* shadow = &ringB;      // being uploaded
  PoseFilter pose;
  NavConfig  cfg;

  uint8_t  navState = proto::NAV_IDLE;
  uint8_t  icon = 0;
  char     street[41] = {0};
  uint16_t distToTurn = 0xFFFF;
  uint8_t  speedLimitKmh = 0;      // posted limit, km/h on the wire. 0 = none known yet. Only replaced by a new non-zero
                                   // value (POSE byte 16), so the last known limit stays when a road has no data
  uint32_t cursor = 0;             // global index of the route point nearest to the marker
  uint32_t routeRev = 0;           // bumped whenever the ACTIVE route changes (forces a map redraw)
  uint32_t badMsgs = 0, droppedMsgs = 0;
  char     alertText[128] = {0};    // last AT:"..." message ("" = dismiss)
  uint32_t alertRev = 0;           // bumped on every AT: command (main forwards it to the UI)

  bool hasRoute() const { return active->count() >= 2; }
  void resetSession();             // HELLO / disconnect: forget everything rebuildable
  void commitShadow();             // ROUTE_COMMIT: swap shadow -> active
  void update(float dt, uint32_t nowMs);   // pose filter, cursor, trimming
  bool handleAt(const uint8_t* d, size_t n);   // AT:"msg" -> alertText/alertRev; true if it was an AT: command
 private:
  void pickNearestStart();
};