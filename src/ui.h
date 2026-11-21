// UI: boot screen, idle screen, navigation HUD and firmware update screen.
// All drawing goes through MonoTileRenderer. Text/icons are 1-bpp TFT_eSprites blitted as bitmaps.
#pragma once
#include <TFT_eSPI.h>
#include "MonoTileRenderer.h"
#include "nav_model.h"
#include "ota.h"

namespace ui {

enum Screen : uint8_t { SCR_IDLE = 0, SCR_NAV = 1, SCR_UPDATE = 2, SCR_SETUP = 3, SCR_WAITSETUP = 4 };

struct Hud {                 // per-frame data owned by main
  int      battPct;          // -1 = unknown
  bool     connected;
  bool     pairing;          // show passkey
  bool     showFps;          // CONFIG flag bit2
  uint32_t passkey;
  float    fps;
  int      kpx;              // average kilo-pixels sent to the panel per frame
  uint32_t nowMs;
};

// ---- Boot (drawn in BASE) ----
void initBoot(TFT_eSPI* tft, mtr::Renderer* r);   // logo + title sprites only (fast)
void initRest();                                  // all other sprites and icons
void bootDraw();                                  // logo, title, empty bar
void bootLabel(const char* s);
void bootEnd();                                   // boot finished: frees the animation buffer
void bootBar(int pct);                            // 0..100
void bootAnimTick(uint32_t nowMs);                // advance the boot animation (needs boot_frames.h)
bool bootAnimDone();                              // last frame shown (true when there is no animation)

// ---- Runtime screens ----
// Every screen change is the interlock reveal (emitReveal in ui.cpp; the Hud::pairing edge in idleFrame() and the update -> failed edge
// in updateFrame() start it too). The bespoke transitions described below are kept but OFF by default: -DUI_PAIR_TRANSITION=1,
// -DUI_OTA_TRANSITION=1, -DUI_OTA_CANCEL_TRANSITION=1, -DUI_OTA_FAIL_TRANSITION=1, -DUI_NAV_TRANSITION=1.
void enter(Screen s);                             // clears BASE; call on every screen change.
                                                  // enter(SCR_IDLE) while a running update is on screen = the update was cancelled:
                                                  // plays the cancel -> idle transition (see below). No protocol change needed.
bool transitionBusy(uint32_t nowMs);              // a transition into the current screen is still playing (idle -> update build-up,
                                                  // cancel -> idle sweep, idle <-> nav, or the interlock reveal). main keeps the RX queue held
                                                  // (no "go ahead" ack to the phone, nothing parsed) until it is false, so no traffic competes
                                                  // with the animation.
void shutdownTransition();                        // current screen -> blank (interlock), blocking, ~0.5 s (-DUI_SHUTDOWN_SPEED_DIV=<float>). Call right
                                                  // before a restart, from loop()'s task. Runs once. main.cpp hooks it to every esp_restart().
void idleFrame(const Hud& h);                     // phone not connected / no route / pairing
                                                  // Also plays the idle <-> pairing transition, driven by Hud edges:
                                                  //   pairing rises  -> marker pulls the wipe, pairing screen builds up (~2 s)
                                                  //   pairing falls  -> if Hud::connected is true at that moment: success (digits
                                                  //                     flash, wipe back, marker re-enters); else failure (damaged marker).
                                                  //   So clear `pairing` and set `connected` in the same frame. Disable with -DUI_PAIR_TRANSITION=0.
void navFrame(NavModel& m, const Hud& h);         // route + marker + HUD
                                                  // Also plays the idle -> nav transition (1.30 s designed: the scenery closes, the road swings
                                                  // round the parked marker until it points up, the dots swell into the route and the banner
                                                  // drops). It starts by itself when enter(SCR_NAV) is called right after a settled idle screen
                                                  // (not fresh, not pairing); from anywhere else the interlock reveal is used.
                                                  // Nav -> idle (1.30 s, played inside idleFrame() after enter(SCR_IDLE) from a steady nav
                                                  // screen): the same, backwards. It ends on the parked marker with the START ring grown, or,
                                                  // when Hud::connected is false, with the damaged marker and idle resuming in SEARCHING.
                                                  // No protocol change. Disable both with -DUI_NAV_TRANSITION=0.
void waitSetupFrame(const Hud& h);                // paired but not configured: app icon + "Continue your setup on the CubiNav Connect app" (language.cpp)
void setupFrame(const Hud& h);                    // first-boot setup: QR code to get the app + the device name to pick in it
                                                  // Shown by main while no phone is bonded (and no pairing is running). Pairing rising switches to
                                                  // the idle screen, which draws the pairing code; once bonded the device never comes back here.
                                                  // Static content goes to BASE once per enter(); only the pulse ring and the status bar are overlay.
                                                  // The change into / out of it is the interlock reveal like every other screen.
void updateFrame(const ota::Info& o, const Hud& h);   // firmware update: progress / done / failed
                                                  // Also plays the idle -> update transition (~1.85 s: scenery closes, the road
                                                  // flattens into the progress ruler, the update screen builds on it). It starts
                                                  // by itself when enter(SCR_UPDATE) is called right after the idle screen was shown;
                                                  // from any other screen the interlock reveal is used. Disable with -DUI_OTA_TRANSITION=0.
                                                  // Cancel -> idle (~1.15 s, played inside idleFrame() after enter(SCR_IDLE) from a running
                                                  // update): the bar and percent drain, "Update cancelled." holds, then the same curtain bar
                                                  // sweeps back and the marker lifts onto the road; idle resumes with the marker re-entering.
                                                  // Not played when done / verifying (plain interlock reveal). Disable with -DUI_OTA_CANCEL_TRANSITION=0.
                                                  // Failure: update -> "Update failed" (the progress screen closes, the failure screen opens, played
                                                  // inside updateFrame()), and failed -> idle after enter(SCR_IDLE) (the failure screen closes, then the
                                                  // curtain bar sweeps with the damaged marker). Disable both with -DUI_OTA_FAIL_TRANSITION=0.
// Transition Speed Dividers, one per transition and local to it: -D<name>=<float> (1.0 = designed speed, 2.0 takes twice as
// long, 0.5 half as long; must be > 0).
//   -DUI_PAIR_SPEED_DIV=<float>         idle <-> pairing
//   -DUI_OTA_SPEED_DIV=<float>          idle -> update
//   -DUI_OTA_CANCEL_SPEED_DIV=<float>   update cancelled -> idle
//   -DUI_OTA_FAIL_SPEED_DIV=<float>     update -> update failed
//   -DUI_OTA_FAIL_EXIT_SPEED_DIV=<float> update failed -> idle
//   -DUI_NAV_SPEED_DIV=<float>          idle -> nav
//   -DUI_NAV_EXIT_SPEED_DIV=<float>     nav -> idle
//   -DUI_REVEAL_SPEED_DIV=<float>       the interlock reveal, i.e. every screen change by default (designed 520 ms, default 2.0 = 1.04 s)
// An undefined one falls back to -DUI_TRANSITION_SPEED_DIV (default 2.0), except UI_REVEAL_SPEED_DIV, which has its own default (2.0).
}  // namespace ui