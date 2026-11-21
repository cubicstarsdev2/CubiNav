// CubiNav UI - boot, idle/pairing, navigation HUD and firmware update screens.
// Design language (from CubiNav_UI.html): pure black on white, Inter Tight type, big numbers,
// a solid black turn banner, a thin status bar, 2 px rules and tracked small caps.
// Everything goes through MonoTileRenderer: text/icons are 1-bpp sprites blitted as bitmaps.
#include "ui.h"
#include "language.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "tv_fonts.h"
#include "tv_fonts_setup.h"   // BN24: full-ASCII bold for the device name
#include "device_name.h"
#include "qr_app.h"

// Optional boot animation: put boot_frames.h (from png2frames.py) next to the sources to enable it.
#if defined(__has_include)
  #if __has_include("boot_frames.h")
    #include "boot_frames.h"
    #define HAVE_BOOT_ANIM 1
  #endif
#endif
#ifndef HAVE_BOOT_ANIM
#define HAVE_BOOT_ANIM 0
#endif
#ifndef BOOT_FRAME_FORMAT
#define BOOT_FRAME_FORMAT 0        // 0 = 1-bpp BOOT_FRAMES (png2frames.py), 1 = alpha4 RLE (png2frames.py --format alpha4)
#endif
#if HAVE_BOOT_ANIM && BOOT_FRAME_FORMAT == 1
  #include "boot_alpha.h"
#endif
#ifndef BOOT_ANIM_SCALE
#define BOOT_ANIM_SCALE ((SCREEN_H - 32) / BOOT_FRAME_H)   // auto: largest integer scale with a 16 px margin (64 px -> x4, 128 px -> x2 = 256 px)
#endif

// Optional mascot for the "continue your setup" screen: setup_mascot_start.h (plays once) + setup_mascot_loop.h (then repeats), both
// made by png2frames.py --format alpha4 (64x64, 24 fps). Without them the screen shows the static app icon instead.
// Both files define the same names as boot_frames.h (BOOT_FRAME_*, BOOT_ALPHA_*): each one is included under its own array names,
// its constants are copied into setupmascot::, and the BOOT_* macros are put back as they were (push_macro / pop_macro), so the
// boot animation is not affected.
#if defined(__has_include)
  #if __has_include("setup_mascot_start.h") && __has_include("setup_mascot_loop.h")
    #define HAVE_SETUP_MASCOT 1
  #endif
#endif
#ifndef HAVE_SETUP_MASCOT
#define HAVE_SETUP_MASCOT 0
#endif
#if HAVE_SETUP_MASCOT
  #include "boot_alpha.h"
  #pragma push_macro("BOOT_FRAME_FORMAT")
  #pragma push_macro("BOOT_FRAME_W")
  #pragma push_macro("BOOT_FRAME_H")
  #pragma push_macro("BOOT_FRAME_COUNT")
  #pragma push_macro("BOOT_FRAME_FPS")
  #pragma push_macro("BOOT_ALPHA_OFFSETS")
  #pragma push_macro("BOOT_ALPHA_DATA")
  #undef BOOT_FRAME_FORMAT
  #undef BOOT_FRAME_W
  #undef BOOT_FRAME_H
  #undef BOOT_FRAME_COUNT
  #undef BOOT_FRAME_FPS
  #define BOOT_ALPHA_OFFSETS MASCOT_START_OFFSETS
  #define BOOT_ALPHA_DATA    MASCOT_START_DATA
  #include "setup_mascot_start.h"
  namespace setupmascot { constexpr int SRC = BOOT_FRAME_W, START_N = BOOT_FRAME_COUNT, START_FPS = BOOT_FRAME_FPS; }
  static_assert(BOOT_FRAME_W == BOOT_FRAME_H && BOOT_FRAME_W <= 256, "setup mascot frames must be square, at most 256 px");
  #undef BOOT_FRAME_FORMAT
  #undef BOOT_FRAME_W
  #undef BOOT_FRAME_H
  #undef BOOT_FRAME_COUNT
  #undef BOOT_FRAME_FPS
  #undef BOOT_ALPHA_OFFSETS
  #undef BOOT_ALPHA_DATA
  #define BOOT_ALPHA_OFFSETS MASCOT_LOOP_OFFSETS
  #define BOOT_ALPHA_DATA    MASCOT_LOOP_DATA
  #include "setup_mascot_loop.h"
  namespace setupmascot { constexpr int LOOP_N = BOOT_FRAME_COUNT, LOOP_FPS = BOOT_FRAME_FPS; }
  static_assert(BOOT_FRAME_W == setupmascot::SRC && BOOT_FRAME_H == setupmascot::SRC, "start and loop mascot frames must have the same size");
  #undef BOOT_FRAME_FORMAT
  #undef BOOT_FRAME_W
  #undef BOOT_FRAME_H
  #undef BOOT_FRAME_COUNT
  #undef BOOT_FRAME_FPS
  #undef BOOT_ALPHA_OFFSETS
  #undef BOOT_ALPHA_DATA
  #pragma pop_macro("BOOT_FRAME_FORMAT")
  #pragma pop_macro("BOOT_FRAME_W")
  #pragma pop_macro("BOOT_FRAME_H")
  #pragma pop_macro("BOOT_FRAME_COUNT")
  #pragma pop_macro("BOOT_FRAME_FPS")
  #pragma pop_macro("BOOT_ALPHA_OFFSETS")
  #pragma pop_macro("BOOT_ALPHA_DATA")
#endif

using namespace mtr;

namespace ui {
namespace {

using BF = fonts::Font;
using namespace fonts;

// ---- Layout (480x320) ---------------------------------------------------------
const int BAN_H = 96;                                   // turn banner
const int BOT_H = 36, BOT_Y = SCREEN_H - BOT_H;         // status bar
const int ANCHOR_X = SCREEN_W / 2, ANCHOR_Y = 215;      // vehicle marker: low on screen, more road ahead
const int ROUTE_W = 10, ROUTE_W2 = 6;                   // route up to the next turn / after it
const float CULL_R = 300.f;                             // px around the marker
const int BACK_MAX = 24, FWD_MAX = 200;

const int IC = 72;                                      // banner icon size
const int LOGO_BIG = 88, LOGO_SMALL = 26;
const int BAR_X = 120, BAR_Y = 238, BAR_W = 240;        // boot progress

const Mode M_SET = SET, M_CLR = CLEAR;

TFT_eSPI* tft = nullptr;
Renderer* R   = nullptr;

// ---- Animations ----------------------------------------------------------------
#ifndef UI_ANIMATIONS
#define UI_ANIMATIONS 1
#endif
// Map redraw throttle. A base redraw dirties every tile and, in heading-up mode, a rotation changes most of the
// map's pixels, so the map is only redrawn when the view moved/rotated by a visible step AND at most every
// UI_MAP_MIN_MS. Raise the three values for fewer tile updates, lower them for a smoother map.
#ifndef UI_MAP_POS_Q_PX
#define UI_MAP_POS_Q_PX  2.0f   // snap map position to this many pixels
#endif
#ifndef UI_MAP_HDG_Q_DEG
#define UI_MAP_HDG_Q_DEG 3.0f   // snap map heading to this many degrees
#endif
#ifndef UI_MAP_MIN_MS
#define UI_MAP_MIN_MS    100    // max 10 map redraws per second
#endif
// The map is redrawn in steps (above), but the vehicle marker is an overlay: it is moved every frame by the distance
// travelled since the last map redraw (the map then catches up and the marker returns to the anchor, seamlessly).
#ifndef UI_MARKER_LERP
#define UI_MARKER_LERP   1
#endif
#ifndef UI_MARKER_MAX_PX
#define UI_MARKER_MAX_PX 32.f   // never drift further than this from the anchor
#endif
// Interlock reveal: the transition of every screen change (see emitReveal). UI_REVEAL_SPEED_DIV: 1.0 = designed speed (REVEAL_MS),
// 2.0 (default) takes twice as long, 1.04 s. It does not use UI_TRANSITION_SPEED_DIV, so changing that one leaves it alone.
#ifndef UI_REVEAL_SPEED_DIV
#define UI_REVEAL_SPEED_DIV 2.0f
#endif
static_assert(UI_REVEAL_SPEED_DIV > 0, "UI_REVEAL_SPEED_DIV must be > 0");
// Shutdown transition: current screen -> blank screen, right before a restart (see shutdownTransition()). Same interlock effect as the
// reveal, own speed: 1.0 = designed speed (0.52 s). It delays the restart by that long.
#ifndef UI_SHUTDOWN_SPEED_DIV
#define UI_SHUTDOWN_SPEED_DIV 1.0f
#endif
static_assert(UI_SHUTDOWN_SPEED_DIV > 0, "UI_SHUTDOWN_SPEED_DIV must be > 0");
const uint32_t REVEAL_MS  = 520;       // screen change: interlock reveal, at designed speed
const int      IL_COLS    = 12;        // interlock: columns (SCREEN_W / 12 = 40 px)
const float    IL_STAGGER = .03f;      // ...each one starts this fraction of REVEAL_MS after the previous one (30 ms at designed speed)
const float    IL_SPAN    = .62f;      // ...and takes this fraction of REVEAL_MS to cross the screen
const int      IL_EDGE    = 3;         // ...behind a leading edge this many px thick
const uint32_t BURST_MS   = 700;       // idle: ring burst on connect
const uint32_t POP_MS     = 260;       // idle: check badge pops in
const uint32_t ICON_IN_MS = 260;       // nav: icon slides in on maneuver change
const uint16_t TURN_NEAR_M = 120;      // nav: icon bobs when the turn is this close
const uint16_t TURN_BAR_M  = 400;      // nav: proximity bar fills over the last 400 m
// Idle <-> pairing transition (see "Idle <-> pairing transition" below). Set UI_PAIR_TRANSITION to 0 for a hard cut.
#ifndef UI_PAIR_TRANSITION
#define UI_PAIR_TRANSITION 0   // off by default: this screen change is the interlock reveal (1 = bespoke transition)
#endif
const uint32_t PT_WIPE_MS       = 1000;  // curtain wipe, linear, the marker rides its edge
const uint32_t PT_STAGE_MS      = 500;   // the pairing items run on their own clock, starting this long after the wipe began
const uint32_t PT_IN_MS         = 2050;  // whole transition into pairing (wipe + bars + code roll + hints)
const uint32_t PT_FLASH_MS      = 360;   // pairing accepted: digits flash inverted twice before the wipe back
const uint32_t PT_FLASH_STEP_MS = 90;
// Idle -> firmware update transition (see "Idle -> OTA transition" below). Set UI_OTA_TRANSITION to 0 for the plain curtain wipe.
#ifndef UI_OTA_TRANSITION
#define UI_OTA_TRANSITION 0   // off by default: this screen change is the interlock reveal (1 = bespoke transition)
#endif
// Update -> idle transition when the update is cancelled (see "Update cancelled -> idle" below). 0 = plain curtain wipe.
#ifndef UI_OTA_CANCEL_TRANSITION
#define UI_OTA_CANCEL_TRANSITION 0   // off by default: this screen change is the interlock reveal (1 = bespoke transition)
#endif
// Update -> lang::tr(lang::S_UPD_FAILED) and "Update failed" -> idle (see "Update failed" below). 0 = hard cut / plain curtain wipe.
#ifndef UI_OTA_FAIL_TRANSITION
#define UI_OTA_FAIL_TRANSITION 0   // off by default: this screen change is the interlock reveal (1 = bespoke transition)
#endif
// Transition Speed Dividers, one per transition, each local to its own transition: it runs that many times SLOWER.
// 1.0 = designed speed, 2.0 = twice as long, 0.5 = twice as fast. Must be > 0.
//   UI_PAIR_SPEED_DIV         idle <-> pairing
//   UI_OTA_SPEED_DIV          idle -> update
//   UI_OTA_CANCEL_SPEED_DIV   update cancelled -> idle
//   UI_OTA_FAIL_SPEED_DIV     update -> update failed
//   UI_OTA_FAIL_EXIT_SPEED_DIV  update failed -> idle
// Override with e.g. -DUI_OTA_SPEED_DIV=1.5f. Any left undefined takes UI_TRANSITION_SPEED_DIV (default 2.0f), kept only as a
// shared fallback for older build flags.
#ifndef UI_TRANSITION_SPEED_DIV
#define UI_TRANSITION_SPEED_DIV 8.0f
#endif
#ifndef UI_PAIR_SPEED_DIV
#define UI_PAIR_SPEED_DIV UI_TRANSITION_SPEED_DIV
#endif
#ifndef UI_OTA_SPEED_DIV
#define UI_OTA_SPEED_DIV UI_TRANSITION_SPEED_DIV
#endif
#ifndef UI_OTA_CANCEL_SPEED_DIV
#define UI_OTA_CANCEL_SPEED_DIV UI_TRANSITION_SPEED_DIV
#endif
#ifndef UI_OTA_FAIL_SPEED_DIV
#define UI_OTA_FAIL_SPEED_DIV UI_TRANSITION_SPEED_DIV
#endif
#ifndef UI_OTA_FAIL_EXIT_SPEED_DIV
#define UI_OTA_FAIL_EXIT_SPEED_DIV UI_TRANSITION_SPEED_DIV
#endif
static_assert(UI_OTA_FAIL_SPEED_DIV > 0, "UI_OTA_FAIL_SPEED_DIV must be > 0");
static_assert(UI_OTA_FAIL_EXIT_SPEED_DIV > 0, "UI_OTA_FAIL_EXIT_SPEED_DIV must be > 0");
static_assert(UI_PAIR_SPEED_DIV > 0, "UI_PAIR_SPEED_DIV must be > 0");
static_assert(UI_OTA_SPEED_DIV > 0, "UI_OTA_SPEED_DIV must be > 0");
static_assert(UI_OTA_CANCEL_SPEED_DIV > 0, "UI_OTA_CANCEL_SPEED_DIV must be > 0");
// Idle <-> navigation transitions (see "Idle <-> navigation transition" below). Set UI_NAV_TRANSITION to 0 for the plain curtain wipe.
//   UI_NAV_SPEED_DIV        idle -> nav
//   UI_NAV_EXIT_SPEED_DIV   nav -> idle
#ifndef UI_NAV_TRANSITION
#define UI_NAV_TRANSITION 0   // off by default: this screen change is the interlock reveal (1 = bespoke transition)
#endif
#ifndef UI_NAV_SPEED_DIV
#define UI_NAV_SPEED_DIV UI_TRANSITION_SPEED_DIV
#endif
#ifndef UI_NAV_EXIT_SPEED_DIV
#define UI_NAV_EXIT_SPEED_DIV UI_TRANSITION_SPEED_DIV
#endif
static_assert(UI_NAV_SPEED_DIV > 0, "UI_NAV_SPEED_DIV must be > 0");
static_assert(UI_NAV_EXIT_SPEED_DIV > 0, "UI_NAV_EXIT_SPEED_DIV must be > 0");
// Milliseconds since `start`, divided by that transition's own speed divider: transition code works on this scaled clock.
inline uint32_t trElapsed(uint32_t now, uint32_t start, float div) { return (uint32_t)((float)(now - start) / div); }

inline float easeOut(float p) { p = p < 0 ? 0 : (p > 1 ? 1 : p); return 1.f - (1.f - p) * (1.f - p); }
inline float easeIO(float p) { p = p < 0 ? 0 : (p > 1 ? 1 : p); return p < .5f ? 2.f * p * p : 1.f - 2.f * (1.f - p) * (1.f - p); }
inline float tri(uint32_t now, uint32_t period) {
  float p = (now % period) / (float)period;
  return p < 0.5f ? p * 2.f : (1.f - p) * 2.f;
}

bool     revealPending = false;
uint32_t revealStart   = 0;

// Interlock reveal: call LAST in a frame (it covers everything emitted before it). Port of "Interlock" in CubiNav_Curtain_Alternatives.html.
// The new screen is already emitted underneath. The screen is cut into IL_COLS columns that start IL_STAGGER of REVEAL_MS apart, left to
// right. Each column slides the new screen in vertically over IL_SPAN of REVEAL_MS (ease in-out): even columns from the top, odd ones from
// the bottom, behind a IL_EDGE px black leading edge. The part of a column the new screen has not reached yet keeps showing the OLD screen:
// ovRestore() puts back what the panel shows (the renderer's own shadow buffer), so nothing is copied and no RAM is used.
// That only works if nothing is drawn INTO the not-yet-reached part, so the leading edge sits on the new screen's side of the line (the
// HTML draws it on the old side). A finished column is left alone.
// Cost: at most one ovRestore for the columns not started, plus an ovRestore and an edge per moving column (2 * IL_COLS + 1 commands).
void emitReveal(uint32_t now, float div = (float)UI_REVEAL_SPEED_DIV) {
#if UI_ANIMATIONS
  if (revealPending) { revealPending = false; revealStart = now; }
  const uint32_t el = trElapsed(now, revealStart, div);
  if (el >= REVEAL_MS) return;
  const float t = (float)el / (float)REVEAL_MS;
  const int   cw = SCREEN_W / IL_COLS;                                         // column width
  int k = 0;                                                                   // columns [k, IL_COLS) have not started: the old screen
  while (k < IL_COLS && t - k * IL_STAGGER > 0.f) k++;
  if (k < IL_COLS) R->ovRestore(k * cw, 0, SCREEN_W - k * cw, SCREEN_H);
  for (int i = 0; i < k; i++) {                                                // columns [0, k) have started
    const float p = (t - i * IL_STAGGER) / IL_SPAN;
    if (p >= 1.f) continue;                                                    // finished: the new screen shows as it is
    const float q = easeIO(p);
    const int   x = i * cw;
    if ((i & 1) == 0) {                                                        // from the top: new screen above y, old screen below it
      const int y = (int)lroundf(SCREEN_H * q);
      if (y < SCREEN_H) R->ovRestore(x, y, cw, SCREEN_H - y);
      const int ey = y < IL_EDGE ? 0 : y - IL_EDGE;
      if (y > ey) R->ovFillRect(x, ey, cw, y - ey, M_SET);                     // leading edge, just above the old screen
    } else {                                                                   // from the bottom: old screen above y, new screen below it
      const int y = (int)lroundf(SCREEN_H * (1.f - q));
      if (y > 0) R->ovRestore(x, 0, cw, y);
      const int eh = SCREEN_H - y < IL_EDGE ? SCREEN_H - y : IL_EDGE;
      if (eh > 0) R->ovFillRect(x, y, cw, eh, M_SET);                          // leading edge, just below the old screen
    }
  }
#else
  (void)now;
#endif
}

// ---- Raw 1-bpp helpers (TFT_eSprite 1 bpp = MSB first, rows padded to bytes) ----------------
inline uint8_t* rawBits(TFT_eSprite* s) { return s ? (uint8_t*)s->getPointer() : nullptr; }
inline const uint8_t* ptr(TFT_eSprite* s) { return rawBits(s); }
inline void setBit(uint8_t* p, int w, int h, int x, int y) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
  p[y * ((w + 7) >> 3) + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}
inline bool getBit(const uint8_t* p, int w, int h, int x, int y) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return false;
  return (p[y * ((w + 7) >> 3) + (x >> 3)] >> (7 - (x & 7))) & 1;
}

void mkSprite(TFT_eSprite*& sp, int w, int h) {
  sp = new TFT_eSprite(tft);
  sp->setColorDepth(1);
  sp->createSprite(w, h);
  sp->fillSprite(0);
}

// ---- Text sprites ---------------------------------------------------------------------------
struct TextSpr {
  TFT_eSprite* sp = nullptr;
  TFT_eSprite* halo = nullptr;       // optional white halo (text dilated by 2 px)
  int x = 0, y = 0, w = 0, h = 0;
  int i0 = 0, i1 = 0;                // columns [i0, i1) that hold ink (set by finish); lets the clipper skip empty sprite area
  char cache[64] = {0};
};
enum Al : uint8_t { AL_L, AL_C, AL_R };

TextSpr tBootT, tBootL, tBootV;                              // boot
TextSpr tIdleT, tStat, tHint;                                // idle
TextSpr tPTitle, tPTag, tPLbl, tPDig, tPH1, tPH2;            // pairing
TextSpr tBatt, tBtLbl, tVerLbl, tFps;                        // status bar
TextSpr tDist, tStreet, tBig, tSpeed, tUnit, tDir, tPill, tLimit;    // navigation
TextSpr tUpLbl, tUpPct, tUpKB, tUpVer, tUpHint, tFailT, tFailE, tFailH;   // firmware update
TextSpr tWA1, tWA2, tWA3;                                       // "continue your setup": headline line 1 / 2, "on the ... app" line
TextSpr tSTag, tSL1, tSL2, tSL3, tSN1, tSN2, tSN3, tSH1, tSH2, tName;      // setup

TFT_eSprite* spLogoBig = nullptr;
TFT_eSprite* spLogoSmall = nullptr;
TFT_eSprite* spIcon[11];
TFT_eSprite* spSpin[8];
TFT_eSprite* spChev = nullptr;       // vehicle chevron (rotated on demand)
TFT_eSprite* spCompass = nullptr;
TFT_eSprite* spCheck = nullptr;      // white disc with check (arrived)
TFT_eSprite* spDash = nullptr;       // dashed accuracy ring (no GPS)
TFT_eSprite* spHatch = nullptr;      // hatched strip under the banner
TFT_eSprite* spLock = nullptr;
TFT_eSprite* spQr = nullptr;         // setup: QR code, QR_APP_N modules x 5 px
TFT_eSprite* spAppIcon = nullptr;    // "continue your setup": app icon, rounded square with the kite knocked out
#if HAVE_SETUP_MASCOT
uint8_t*      maBuf = nullptr;       // "continue your setup": decoded + dithered mascot frame, 1 bpp
#endif

void mk(TextSpr& t, int x, int y, int w, int h) { mkSprite(t.sp, w, h); t.x = x; t.y = y; t.w = w; t.h = h; }
void mkHalo(TextSpr& t) { mkSprite(t.halo, t.w, t.h); }

const Glyph* gly(const BF& f, unsigned char c) {
  if (c < f.first || c > f.last) return nullptr;
  const Glyph* g = &f.g[c - f.first];
  return (g->adv || g->w) ? g : nullptr;
}
// Width of `s`, tracking applied between glyphs only.
int textW(const BF& f, const char* s, int tr) {
  int w = 0, n = 0;
  for (; *s; s++) { const Glyph* g = gly(f, (unsigned char)*s); if (!g) continue; w += g->adv + tr; n++; }
  return n ? w - tr : 0;
}
// Draw one run into the sprite; returns the x after the last glyph (tracking included).
int put(TextSpr& t, const BF& f, const char* s, int x, int base, int tr) {
  uint8_t* p = rawBits(t.sp);
  if (!p) return x;
  for (; *s; s++) {
    const Glyph* g = gly(f, (unsigned char)*s);
    if (!g) continue;
    const uint8_t* b = f.bmp + g->off;
    int bit = 0;
    for (int r = 0; r < g->h; r++)
      for (int c = 0; c < g->w; c++, bit++)
        if (b[bit >> 3] & (0x80 >> (bit & 7))) setBit(p, t.w, t.h, x + g->xo + c, base + g->yo + r);
    x += g->adv + tr;
  }
  return x;
}

void rebuildHalo(TextSpr& t) {
  if (!t.halo) return;
  t.halo->fillSprite(0);
  const uint8_t* s = rawBits(t.sp);
  uint8_t* d = rawBits(t.halo);
  for (int y = 0; y < t.h; y++)
    for (int x = 0; x < t.w; x++)
      if (getBit(s, t.w, t.h, x, y))
        for (int dy = -2; dy <= 2; dy++)
          for (int dx = -2; dx <= 2; dx++) setBit(d, t.w, t.h, x + dx, y + dy);
}

// Start / finish a re-render. The renderer compares commands, not bitmap contents, so a changed
// sprite has to be flagged by hand.
bool begin(TextSpr& t, const char* key) {
  if (!t.sp || strncmp(t.cache, key, sizeof(t.cache) - 1) == 0) return false;
  strncpy(t.cache, key, sizeof(t.cache) - 1); t.cache[sizeof(t.cache) - 1] = 0;
  t.sp->fillSprite(0);
  return true;
}
// Column range holding ink, so wide, mostly empty text sprites (480 px for a 110 px label) clip cheaply.
void inkRange(TextSpr& t) {
  t.i0 = t.i1 = 0;
  const uint8_t* p = rawBits(t.sp);
  if (!p) return;
  const int ss = (t.w + 7) >> 3;
  int lo = t.w, hi = -1;
  for (int bx = 0; bx < ss; bx++) {
    uint8_t acc = 0;
    for (int y = 0; y < t.h; y++) acc |= p[y * ss + bx];
    for (int k = 0; k < 8 && acc; k++)
      if (acc & (0x80 >> k)) { const int c = bx * 8 + k; if (c < lo) lo = c; if (c > hi) hi = c; }
  }
  if (hi >= 0) { t.i0 = lo; t.i1 = hi + 1; }
}
void finish(TextSpr& t) {
  rebuildHalo(t);
  inkRange(t);
  R->markDirtyRect(t.x, t.y, t.x + t.w - 1, t.y + t.h - 1);
}

// Single-run text. `ox` is a margin from the aligned edge.
bool setText(TextSpr& t, const char* s, const BF& f, Al a, int base, int tr = 0, int ox = 0) {
  if (!begin(t, s)) return false;
  int w = textW(f, s, tr);
  int x = a == AL_L ? ox : (a == AL_C ? (t.w - w) / 2 : t.w - w - ox);
  put(t, f, s, x, base, tr);
  finish(t);
  return true;
}

// Never overflows the sprite: big font, then small, then "..." truncation (small).
bool setFit(TextSpr& t, const char* s, const BF& big, const BF& small, int base) {
  if (!begin(t, s)) return false;
  char buf[64];
  strncpy(buf, s, sizeof(buf) - 4); buf[sizeof(buf) - 4] = 0;
  const BF* f = &big;
  if (textW(big, buf, 0) > t.w) {
    f = &small;
    if (textW(small, buf, 0) > t.w) {
      size_t n = strlen(buf);
      while (n > 1) {
        n--;
        char tmp[64];
        memcpy(tmp, buf, n); strcpy(tmp + n, "...");
        if (textW(small, tmp, 0) <= t.w) { strcpy(buf, tmp); break; }
      }
    }
  }
  put(t, *f, buf, 0, base, 0);
  finish(t);
  return true;
}

void blit(const TextSpr& t, Mode m) {
  const uint8_t* p = ptr(t.sp);
  if (p) R->ovBitmap(t.x, t.y, t.w, t.h, p, m);
}
void blitHalo(const TextSpr& t) {          // white halo, then black text
  const uint8_t* ph = ptr(t.halo);
  if (ph) R->ovBitmap(t.x, t.y, t.w, t.h, ph, M_CLR);
  blit(t, M_SET);
}

// ---- Clipped drawing (screen transitions) --------------------------------------------------------------------
// A transition shows two screens side by side, cut by a vertical line, so every element must be limited to a screen
// column window [x0, x1). Rects and small dots clip trivially. A bitmap that crosses the window edge is copied
// column-wise into a per-frame pool and drawn from there; one that is fully visible is drawn straight from its sprite
// (so a full window, i.e. normal operation, issues exactly the commands it always did).
// The renderer compares commands, not pixels, so every pooled copy marks its rect dirty by hand.
const int CLIP_POOL = 6144;                                  // worst case seen in the transitions is about 4 KB
uint8_t clipPool[CLIP_POOL];
int     clipUsed = 0;
inline void clipBegin() { clipUsed = 0; }                    // call once per frame

struct Src { const uint8_t* p; int x, y, w, h, i0, i1; };    // a 1-bpp bitmap placed on screen; ink columns [i0, i1)
inline Src srcOf(const TextSpr& t) { return Src{ptr(t.sp), t.x, t.y, t.w, t.h, t.i0, t.i1}; }
inline Src srcOf(TFT_eSprite* s, int x, int y, int w, int h) { return Src{ptr(s), x, y, w, h, 0, w}; }

// Draw `s` (shifted down by dy) limited to screen columns [x0, x1). [cs0, cs1) >= 0 draws only that slice of its columns.
void blitClip(const Src& s, int x0, int x1, Mode m, int dy = 0, int cs0 = -1, int cs1 = -1) {
  if (!s.p) return;
  const bool slice = cs0 >= 0;
  const int fa = s.x + (slice ? cs0 : s.i0), fb = s.x + (slice ? cs1 : s.i1);   // columns of interest, on screen
  int a = fa, b = fb;
  if (a < x0) a = x0;
  if (b > x1) b = x1;
  if (b <= a) return;
  const int y0 = s.y + dy;
  const int r0 = y0 < 0 ? -y0 : 0;                           // rows that stick out above the screen are dropped (banner sliding in/out)
  const int rn = s.h - r0, y = y0 + r0;
  if (rn <= 0) return;
  const int ss = (s.w + 7) >> 3;
  if (!slice && a == fa && b == fb) { R->ovBitmap(s.x, y, s.w, rn, s.p + r0 * ss, m); return; }   // fully visible: no copy
  const int bw = b - a, ds = (bw + 7) >> 3, s0 = a - s.x, sb = s0 >> 3, sh = s0 & 7;
  if (clipUsed + ds * rn > CLIP_POOL) return;                // pool exhausted: skip the element rather than corrupt memory
  uint8_t* d = clipPool + clipUsed;
  clipUsed += ds * rn;
  const uint8_t tail = (bw & 7) ? (uint8_t)(0xFF << (8 - (bw & 7))) : 0xFF;
  for (int r = 0; r < rn; r++) {
    const uint8_t* row = s.p + (r0 + r) * ss;
    uint8_t* o = d + r * ds;
    for (int j = 0; j < ds; j++) {
      const unsigned hi = row[sb + j], lo = (sb + j + 1 < ss) ? row[sb + j + 1] : 0;
      o[j] = (uint8_t)((((hi << 8) | lo) << sh) >> 8);
    }
    o[ds - 1] &= tail;
  }
  R->ovBitmap(a, y, bw, rn, d, m);
  R->markDirtyRect(a, y, b - 1, y + rn - 1);
}
inline void blitC(const TextSpr& t, int x0, int x1, Mode m, int dy = 0) { blitClip(srcOf(t), x0, x1, m, dy); }

inline void rectC(int x, int y, int w, int h, int x0, int x1, Mode m) {
  const int a = x < x0 ? x0 : x, b = (x + w > x1) ? x1 : x + w;
  if (b > a) R->ovFillRect(a, y, b - a, h, m);
}
// Small dots: kept whole when their centre is inside the window (the curtain bar hides the 1-2 px difference).
inline void dotC(int cx, int cy, int r, int x0, int x1, Mode m) {
  if ((x0 <= 0 && x1 >= SCREEN_W) || (cx >= x0 && cx < x1)) R->ovFillCircle(cx, cy, r, m);
}

// ---- Rounded rect (rect + 4 corner discs) --------------------------------------------------------
void fillRR(int x, int y, int w, int h, int r, Mode m) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r < 1) { R->ovFillRect(x, y, w, h, m); return; }
  R->ovFillRect(x + r, y, w - 2 * r, h, m);
  R->ovFillRect(x, y + r, w, h - 2 * r, m);
  R->ovFillCircle(x + r, y + r, r, m);
  R->ovFillCircle(x + w - 1 - r, y + r, r, m);
  R->ovFillCircle(x + r, y + h - 1 - r, r, m);
  R->ovFillCircle(x + w - 1 - r, y + h - 1 - r, r, m);
}

// ---- Icon drawing (sprites, authored on a 72 grid, stroke 7 px, round caps) ----------------------
void thickLine(TFT_eSprite* s, float x0, float y0, float x1, float y1, int r) {
  float dx = x1 - x0, dy = y1 - y0;
  int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
  if (n < 1) n = 1;
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n;
    s->fillCircle(lroundf(x0 + dx * t), lroundf(y0 + dy * t), r, 1);
  }
}

struct IconCtx {
  TFT_eSprite* s; float ca, sa; float px[96], py[96]; int n; float lx, ly;
  IconCtx(TFT_eSprite* sp, float rotDeg = 0) : s(sp), n(0), lx(0), ly(0) {
    float a = rotDeg * 0.017453292f; ca = cosf(a); sa = sinf(a);
  }
  void add(float x, float y) {
    lx = x; ly = y;
    if (n >= 96) return;
    float dx = x - 36, dy = y - 36;
    px[n] = 36 + dx * ca - dy * sa; py[n] = 36 + dx * sa + dy * ca; n++;
  }
  void mv(float x, float y) { stroke(); add(x, y); }
  void ln(float x, float y) { add(x, y); }
  void quad(float cx, float cy, float x, float y) {
    float x0 = lx, y0 = ly;
    for (int i = 1; i <= 8; i++) {
      float t = i / 8.f, u = 1 - t;
      add(u * u * x0 + 2 * u * t * cx + t * t * x, u * u * y0 + 2 * u * t * cy + t * t * y);
    }
  }
  void cubic(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    float x0 = lx, y0 = ly;
    for (int i = 1; i <= 10; i++) {
      float t = i / 10.f, u = 1 - t;
      add(u*u*u*x0 + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*x, u*u*u*y0 + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*y);
    }
  }
  void arc(float cx, float cy, float r, float a0, float a1) {   // degrees, 0 = right, 90 = up
    int steps = (int)fabsf(a1 - a0) / 8 + 2;
    for (int i = 0; i <= steps; i++) {
      float a = (a0 + (a1 - a0) * i / steps) * 0.017453292f;
      add(cx + r * cosf(a), cy - r * sinf(a));
    }
  }
  void stroke() {
    if (n == 1) s->fillCircle(lroundf(px[0]), lroundf(py[0]), 3, 1);
    for (int i = 0; i + 1 < n; i++) thickLine(s, px[i], py[i], px[i + 1], py[i + 1], 3);
    n = 0;
  }
  void head(float tx, float ty, float dx, float dy) {          // open chevron head at tip, direction d
    const float L = 12;
    float bx = tx - dx * L, by = ty - dy * L, qx = -dy, qy = dx;
    mv(bx + qx * L, by + qy * L); ln(tx, ty); ln(bx - qx * L, by - qy * L); stroke();
  }
};

void iconPaths(IconCtx& c, int kind) {
  const float S2 = 0.70711f;
  switch (kind) {
    case 1:  c.mv(36, 64); c.ln(36, 12); c.stroke(); c.head(36, 12, 0, -1); break;                    // straight
    case 2:  c.mv(46, 64); c.ln(46, 40); c.quad(46, 28, 34, 28); c.ln(16, 28); c.stroke();            // left
             c.head(14, 28, -1, 0); break;
    case 3:  c.mv(26, 64); c.ln(26, 40); c.quad(26, 28, 38, 28); c.ln(56, 28); c.stroke();            // right
             c.head(58, 28, 1, 0); break;
    case 4:  c.mv(42, 64); c.ln(42, 42); c.ln(24, 20); c.stroke(); c.head(22, 18, -0.6f, -0.8f); break;  // slight left
    case 5:  c.mv(30, 64); c.ln(30, 42); c.ln(48, 20); c.stroke(); c.head(50, 18, 0.6f, -0.8f); break;   // slight right
    case 6:  c.mv(46, 64); c.ln(46, 28); c.ln(22, 52); c.stroke(); c.head(20, 54, -S2, S2); break;       // sharp left
    case 7:  c.mv(26, 64); c.ln(26, 28); c.ln(50, 52); c.stroke(); c.head(52, 54, S2, S2); break;        // sharp right
    case 8:  c.mv(52, 64); c.ln(52, 28); c.arc(36, 28, 16, 0, 180); c.ln(20, 50); c.stroke();            // u-turn
             c.head(20, 52, 0, 1); break;
    case 9:  c.mv(36 + 15, 40); c.arc(36, 40, 15, 0, -360); c.stroke();                                  // roundabout
             c.mv(36, 66); c.ln(36, 56); c.stroke();
             c.mv(36, 25); c.ln(36, 10); c.stroke(); c.head(36, 8, 0, -1); break;
    case 10: c.mv(36, 66); c.cubic(22, 50, 14, 40, 14, 30); c.arc(36, 30, 22, 180, 0);                   // arrive: pin
             c.cubic(58, 40, 50, 50, 36, 66); c.stroke(); break;
  }
}

void buildIcons() {
  for (int i = 0; i < 11; i++) {
    mkSprite(spIcon[i], IC, IC);
    if (i == 0) continue;
    IconCtx c(spIcon[i]);
    iconPaths(c, i);
    if (i == 10) { spIcon[i]->fillCircle(36, 30, 11, 1); spIcon[i]->fillCircle(36, 30, 5, 0); }
  }
  for (int k = 0; k < 8; k++) {                              // reroute: two arrows chasing each other
    mkSprite(spSpin[k], IC, IC);
    IconCtx c(spSpin[k], k * 22.5f);
    c.mv(14, 36); c.arc(36, 36, 22, 180, 0); c.stroke();
    c.mv(51, 29); c.ln(58, 37); c.ln(65, 29); c.stroke();
    c.mv(58, 36); c.arc(36, 36, 22, 0, -180); c.stroke();
    c.mv(7, 43); c.ln(14, 35); c.ln(21, 43); c.stroke();
  }
}

void buildLogo(TFT_eSprite*& sp, int S) {
  mkSprite(sp, S, S);
  const float k = S / 96.f;
  sp->fillCircle(S / 2, S / 2, lroundf(46 * k), 1);
  auto P = [&](float x, float y, int& ox, int& oy) { ox = lroundf(x * k); oy = lroundf(y * k); };
  int ax, ay, bx, by, cx, cy, dx, dy;
  P(48, 16, ax, ay); P(72, 78, bx, by); P(48, 63, cx, cy); P(24, 78, dx, dy);
  sp->fillTriangle(ax, ay, bx, by, cx, cy, 0);
  sp->fillTriangle(ax, ay, cx, cy, dx, dy, 0);
}

void buildMisc() {
  mkSprite(spChev, 28, 28);
  mkSprite(spCompass, 44, 44);
  // white disc with the check carved out (arrived)
  mkSprite(spCheck, 44, 44);
  spCheck->fillCircle(22, 22, 20, 1);
  {
    const float pts[3][2] = {{12, 23}, {19, 30}, {33, 14}};
    for (int q = 0; q < 2; q++) {
      float x0 = pts[q][0], y0 = pts[q][1], x1 = pts[q + 1][0], y1 = pts[q + 1][1];
      int n = (int)ceilf(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0))) + 1;
      for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        spCheck->fillCircle(lroundf(x0 + (x1 - x0) * t), lroundf(y0 + (y1 - y0) * t), 2, 0);
      }
    }
  }
  // dashed accuracy ring 48x48 (r 22, 2 px, dash 4 / gap 4)
  mkSprite(spDash, 48, 48);
  {
    uint8_t* p = rawBits(spDash);
    for (int a = 0; a < 1440; a++) {
      float ang = a * 0.25f * 0.017453292f, s = 22.f * ang;
      if (((int)(s / 4.f)) & 1) continue;
      for (int rr = 21; rr <= 22; rr++)
        setBit(p, 48, 48, lroundf(24 + rr * cosf(ang)), lroundf(24 + rr * sinf(ang)));
    }
  }
  // hatch strip 480x30: 135-degree 1 px lines
  mkSprite(spHatch, SCREEN_W, 30);
  {
    uint8_t* p = rawBits(spHatch);
    for (int y = 0; y < 30; y++)
      for (int x = 0; x < SCREEN_W; x++)
        if (((x + y) % 6) == 0) setBit(p, SCREEN_W, 30, x, y);
  }
  // padlock 14x16
  mkSprite(spLock, 14, 16);
  {
    uint8_t* p = rawBits(spLock);
    for (int y = 7; y <= 14; y++) for (int x = 1; x <= 12; x++) setBit(p, 14, 16, x, y);
    for (int y = 1; y <= 6; y++)
      for (int x = 0; x < 14; x++) {
        float d = hypotf(x + 0.5f - 7.f, y + 0.5f - 5.f);
        bool arc = y <= 5 && d >= 2.f && d <= 4.2f;
        bool leg = y >= 5 && (x == 3 || x == 4 || x == 9 || x == 10);
        if (arc || leg) setBit(p, 14, 16, x, y);
      }
  }
}

// Vehicle chevron, pointing `deg` clockwise from up (quantized; rebuilt only when it changes).
int chevQ = -999;
void updateChevron(float deg) {
  int q = (int)lroundf(deg / 6.f);
  if (q == chevQ) return;
  chevQ = q;
  float a = q * 6.f * 0.017453292f, ca = cosf(a), sa = sinf(a);
  spChev->fillSprite(0);
  const float P[4][2] = {{0, -12}, {9, 7}, {0, 1}, {-9, 7}};
  int X[4], Y[4];
  for (int i = 0; i < 4; i++) { X[i] = lroundf(14 + P[i][0] * ca - P[i][1] * sa); Y[i] = lroundf(14 + P[i][0] * sa + P[i][1] * ca); }
  spChev->fillTriangle(X[0], Y[0], X[1], Y[1], X[2], Y[2], 1);
  spChev->fillTriangle(X[0], Y[0], X[3], Y[3], X[2], Y[2], 1);
  R->markDirtyRect(ANCHOR_X - 14, ANCHOR_Y - 14, ANCHOR_X + 13, ANCHOR_Y + 13);
}

// Compass needle: white north needle + south tick, rotated so the needle points to north on screen.
int compQ = -999;
void updateCompass(float headingDeg, int bx, int by) {
  int q = (int)lroundf(headingDeg / 4.f);
  if (q == compQ) return;
  compQ = q;
  float a = -q * 4.f * 0.017453292f, ca = cosf(a), sa = sinf(a);
  spCompass->fillSprite(0);
  spCompass->fillCircle(22, 22, 21, 1);
  spCompass->fillCircle(22, 22, 19, 0);
  auto X = [&](float x, float y) { return lroundf(22 + x * ca - y * sa); };
  auto Y = [&](float x, float y) { return lroundf(22 + x * sa + y * ca); };
  spCompass->fillTriangle(X(0, -16), Y(0, -16), X(5.5f, 1), Y(5.5f, 1), X(0, -2.5f), Y(0, -2.5f), 1);
  spCompass->fillTriangle(X(0, -16), Y(0, -16), X(-5.5f, 1), Y(-5.5f, 1), X(0, -2.5f), Y(0, -2.5f), 1);
  thickLine(spCompass, X(0, 4), Y(0, 4), X(0, 12), Y(0, 12), 1);
  R->markDirtyRect(bx, by, bx + 43, by + 43);
}

// ---- Number formatting -----------------------------------------------------------------------------
// Splits into number + unit ("120", "m") so they get two type sizes.
void fmtDist(uint16_t m, bool imp, char* num, char* unit, size_t n) {
  num[0] = unit[0] = 0;
  if (m == 0xFFFF) return;
  if (!imp) {
    if (m < 995)       { snprintf(num, n, "%u", (unsigned)(((m + 5) / 10) * 10)); snprintf(unit, n, "m"); }
    else if (m < 9950) { snprintf(num, n, "%.1f", m / 1000.0f);                   snprintf(unit, n, "km"); }
    else               { snprintf(num, n, "%u", (unsigned)((m + 500) / 1000));   snprintf(unit, n, "km"); }
  } else {
    float ft = m * 3.28084f;
    if (ft < 528.f) { snprintf(num, n, "%d", ((int)(ft + 25) / 50) * 50); snprintf(unit, n, "ft"); }
    else {
      float mi = m / 1609.344f;
      if (mi < 10.f) snprintf(num, n, "%.1f", mi); else snprintf(num, n, "%d", (int)(mi + 0.5f));
      snprintf(unit, n, "mi");
    }
  }
}

// ---- Shared pieces (OVERLAY) ---------------------------------------------------------------------------
// Battery 32x14: 28x14 body (2 px outline), nub, 4 segments.
void emitBattery(int bx, int by, int pct, uint32_t now) {
  R->ovRect(bx, by, 28, 14, M_SET);
  R->ovRect(bx + 1, by + 1, 26, 12, M_SET);
  R->ovFillRect(bx + 29, by + 4, 3, 6, M_SET);
  bool lowBlink = UI_ANIMATIONS && pct >= 0 && pct <= 15 && ((now / 500) & 1);
  if (lowBlink || pct <= 0) return;
  int seg = (pct + 24) / 25; if (seg > 4) seg = 4;
  static const int8_t SX[4] = {4, 9, 15, 20};
  for (int i = 0; i < seg; i++) R->ovFillRect(bx + SX[i], by + 4, 4, 6, M_SET);
}

void emitBarFrame() {
  R->ovFillRect(0, BOT_Y, SCREEN_W, BOT_H, M_CLR);
  R->ovFillRect(0, BOT_Y, SCREEN_W, 2, M_SET);
}

void setBattText(const Hud& h) {
  char b[12];
  if (h.battPct < 0) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%d%%", h.battPct);
  setText(tBatt, b, B14, AL_L, 17);
}

// Bluetooth dot: ring while waiting, solid when linked.
void emitBtDot(int cx, int cy, bool linked) {
  R->ovFillCircle(cx, cy, 5, M_SET);
  if (!linked) R->ovFillCircle(cx, cy, 3, M_CLR);
}

// Idle / pairing / update status bar. A screen transition cuts it at the curtain bar, so it comes in two parts:
// statusBase = frame + battery (the same on every screen), statusRight = BLUETOOTH label + dot, and with `withVer` the
// divider + version, limited to screen columns [x0, x1).
void statusBase(const Hud& h) {
  emitBarFrame();
  emitBattery(20, BOT_Y + 12, h.battPct, h.nowMs);
  setBattText(h);
  tBatt.x = 62; tBatt.y = BOT_Y + 7;
  blit(tBatt, M_SET);
}

void statusRight(const Hud& h, bool withVer, int x0, int x1, float verRetract = 0.f) {
  const int by = BOT_Y + 9;                                  // text sprites' top (baseline at BOT_Y+23)
  char v[24]; snprintf(v, sizeof(v), "V%s", FW_VERSION_STRING);
  setText(tBtLbl, "BLUETOOTH", FB11, AL_L, 14, 2);
  setText(tVerLbl, v, FB11, AL_L, 14, 2);
  int wB = textW(FB11, "BLUETOOTH", 2) + 2;
  int cx, labelEnd;
  if (withVer) {
    int wV = textW(FB11, v, 2) + 2;
    int verX = 460 - wV;
    int divX = verX - 11;
    tVerLbl.x = verX; tVerLbl.y = by;
    if (verRetract <= 0.f) {
      blitC(tVerLbl, x0, x1, M_SET);
      rectC(divX, BOT_Y + 12, 1, 14, x0, x1, M_SET);
    } else {                                                 // retracting: the visible part shrinks towards the right edge
      const float e = verRetract >= 1.f ? 1.f : (verRetract < .5f ? 2.f * verRetract * verRetract : 1.f - 2.f * (1.f - verRetract) * (1.f - verRetract));
      const int a = 460 - (int)lroundf(wV * (1.f - e));
      const int aa = a < x0 ? x0 : a, bb = x1 < 460 ? x1 : 460;
      if (a < 460 && aa < bb) blitClip(srcOf(tVerLbl), aa, bb, M_SET);
      if (verRetract < .5f) rectC(divX, BOT_Y + 12, 1, 14, x0, x1, M_SET);
    }
    cx = divX - 15;
    labelEnd = divX - 30;
  } else {
    cx = 455;
    labelEnd = 440;
  }
  if (cx >= x0 && cx < x1) emitBtDot(cx, BOT_Y + 19, h.connected);
  tBtLbl.x = labelEnd - wB; tBtLbl.y = by;
  blitC(tBtLbl, x0, x1, M_SET);
}

void emitStatusBar(const Hud& h, bool withVer, float verRetract = 0.f) {   // verRetract (0..1, only with withVer): the version is being retracted
  statusBase(h);
  statusRight(h, withVer, 0, SCREEN_W, verRetract);
}

// Cut at screen column x: the part to its left has the version (`verLeft`), the part to its right has not, or vice versa.
void statusSplit(const Hud& h, int x, bool verLeft) {
  statusBase(h);
  if (x > 0)         statusRight(h, verLeft, 0, x);
  if (x < SCREEN_W)  statusRight(h, !verLeft, x, SCREEN_W);
}

void emitFps(const Hud& h, bool show, int x, int y, bool halo) {
  if (!show) return;
  char b[40]; snprintf(b, sizeof(b), "%.0f fps  %d kpx", h.fps, h.kpx);
  tFps.x = x; tFps.y = y;
  setText(tFps, b, M14, AL_C, 14);
  if (halo) blitHalo(tFps); else blit(tFps, M_SET);
}

// ---- Map -----------------------------------------------------------------------------------------------
bool clipLine(float& x0, float& y0, float& x1, float& y1, float xmin, float ymin, float xmax, float ymax) {
  float dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1;
  struct T { static bool test(float p, float q, float& a, float& b) {
    if (p == 0) return q >= 0;
    float t = q / p;
    if (p < 0) { if (t > b) return false; if (t > a) a = t; }
    else       { if (t < a) return false; if (t < b) b = t; }
    return true; } };
  if (!T::test(-dx, x0 - xmin, t0, t1) || !T::test(dx, xmax - x0, t0, t1) ||
      !T::test(-dy, y0 - ymin, t0, t1) || !T::test(dy, ymax - y0, t0, t1)) return false;
  float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy, nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
  x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
  return true;
}

void clearMap() { R->baseFillRect(0, 0, SCREEN_W, SCREEN_H, M_CLR); }

void baseSeg(float x0, float y0, float x1, float y1, int w) {
  if (!clipLine(x0, y0, x1, y1, -8, -8, SCREEN_W + 7, SCREEN_H + 7)) return;
  R->baseLine(lroundf(x0), lroundf(y0), lroundf(x1), lroundf(y1), w, M_SET);
}

// Round dots every 9 px (`next` carries the phase across segments).
void dottedSeg(float x0, float y0, float x1, float y1, float& next) {
  float dx = x1 - x0, dy = y1 - y0, L = sqrtf(dx * dx + dy * dy);
  if (L < 0.01f) return;
  float d = next;
  while (d <= L) {
    float x = x0 + dx * d / L, y = y0 + dy * d / L;
    if (x > -6 && x < SCREEN_W + 6 && y > -6 && y < SCREEN_H + 6) R->baseFillCircle(lroundf(x), lroundf(y), 2, M_SET);
    d += 9.f;
  }
  next = d - L;
}

// The next maneuver is PINNED to a point on the route (global segment index + fraction), not recomputed from
// distToTurn every frame. distToTurn is only used to pick that point, and to re-pick it when it disagrees by more
// than TURN_RESYNC_M (maneuver advanced, reroute, new route). If the point lies within TURN_SNAP_M of a waypoint it
// snaps onto it (normal case: the turn IS a waypoint); otherwise it is interpolated on the segment.
const float    TURN_RESYNC_M = 30.f;
const float    TURN_SNAP_M   = 12.f;
const float    TURN_BEND_M   = 35.f;     // a maneuver is pinned to the route vertex where the road bends, if one lies this close to distToTurn
const float    TURN_BEND_COS = 0.9f;     // ... and the bend is sharper than ~26 deg
const uint32_t TURN_WAIT_MS  = 3000;
uint32_t turnIdx = 0;  float turnT = 0;  uint8_t turnRoute = 255;  bool turnOk = false;
// After the vehicle passes the pinned point the phone still reports the old (tiny) distance for a moment: hold
// (no node, thin road) until the maneuver or the distance changes, or TURN_WAIT_MS passes.
bool     turnWait = false;  uint8_t waitIcon = 0, waitRoute = 255;  uint16_t waitD = 0;  uint32_t waitMs = 0;
// Odometer: route arc length (m) at vertex odoIdx. Gives the dotted trail a phase tied to the ROUTE, so the dots
// stay put on the map while you drive instead of crawling whenever the first visible waypoint changes.
double   odoM = 0;  uint32_t odoIdx = 0;  uint8_t odoRoute = 255;  bool odoOk = false;
const float DOT_PX = 9.f;

// What the navigation screen shows, kept for the idle <-> nav transition (see "Idle <-> navigation transition").
// Idle -> nav reads it live (navFrame fills it every frame of the entry); nav -> idle reads the last steady frame, because by
// the time enter(SCR_IDLE) is called the route is gone from the model.
const int NV_RS = 32;                                    // route vertices kept: the marker point + up to 31 projected vertices ahead
struct NavSnap {
  bool    valid = false;                                 // the last nav frame was a complete, steady one
  uint8_t icon = 0, bmode = 0;                           // maneuver icon; banner: 0 normal, 1 rerouting, 2 arrived
  float   bar = 0.f;                                     // proximity bar fill
  bool    imp = false;
  int     limit = 0;                                     // posted speed limit in the displayed unit, 0 = no sign
  char    sp[8] = {0}, dir[4] = {0};                     // speed / heading texts
  int     n = 0;                                         // route polyline in screen px: marker point, then the vertices ahead
  float   rx[NV_RS], ry[NV_RS];
  int     nodeMode = 0;                                  // 0 all wide, 1 turn node at nodeArc px along the polyline, 2 all thin
  float   nodeArc = 0.f;
  bool    endOk = false;                                 // the polyline ends at the real end of the route (destination ring)
};
NavSnap nvS;

// Heading-up projection:
//   sx = AX + s*(e*cos(t) - n*sin(t)),  sy = AY - s*(e*sin(t) + n*cos(t))
// Only points near the cursor are projected, so cost does not depend on the route length.
//   - behind the cursor: dotted trail
//   - up to the next turn: 10 px; after it: 6 px; the turn gets a node; the end gets a destination ring
// draw = false: only project and fill nvS (the idle -> nav transition draws the route itself, in the overlay).
void drawRoute(const NavModel& m, float thetaDeg, float scale, bool arrived, bool draw = true) {
  nvS.n = 0; nvS.nodeMode = 0; nvS.endOk = false;
  const RouteRing& r = *m.active;
  if (r.count() < 2) return;
  const int32_t ix = (int32_t)lround(m.pose.x()), iy = (int32_t)lround(m.pose.y());
  const float th = thetaDeg * 0.017453292f, c = cosf(th), s = sinf(th), k = scale * 0.1f;   // px per dm

  struct P { static void proj(const Pt& p, int32_t ix, int32_t iy, float k, float c, float s, float& sx, float& sy) {
    float e = (float)(p.x - ix) * k, n = (float)(p.y - iy) * k;
    sx = ANCHOR_X + (e * c - n * s);
    sy = ANCHOR_Y - (e * s + n * c);
  } };
  auto isFar = [](float x, float y) { float dx = x - ANCHOR_X, dy = y - ANCHOR_Y; return dx * dx + dy * dy > CULL_R * CULL_R; };

  uint32_t c0 = m.cursor;
  if (c0 < r.tail()) c0 = r.tail();
  if (c0 >= r.head()) c0 = r.head() - 1;

  uint32_t lo = c0;
  for (int i = 0; i < BACK_MAX && lo > r.tail(); i++) {
    lo--;
    float x, y; P::proj(r.at(lo), ix, iy, k, c, s, x, y);
    if (isFar(x, y)) break;
  }

  static float PX[BACK_MAX + FWD_MAX + 2], PY[BACK_MAX + FWD_MAX + 2];
  int n = 0;
  bool reachedEnd = false;
  for (uint32_t i = lo; i < r.head() && n < BACK_MAX + FWD_MAX + 1; i++) {
    P::proj(r.at(i), ix, iy, k, c, s, PX[n], PY[n]);
    n++;
    if (i + 1 == r.head()) reachedEnd = true;
    if (i > c0 && isFar(PX[n - 1], PY[n - 1])) break;
  }
  const int ic = (int)(c0 - lo);

  if (n < 2) return;

  auto gLen = [&](uint32_t gi) {                             // length (m) of the segment gi -> gi+1
    const Pt& p = r.at(gi); const Pt& q = r.at(gi + 1);
    const float ddx = (float)(q.x - p.x), ddy = (float)(q.y - p.y);
    return sqrtf(ddx * ddx + ddy * ddy) * 0.1f;              // dm -> m
  };
  auto lenM = [&](int a) { return gLen(lo + (uint32_t)a); };

  // Odometer: arc length at vertex `lo`, accumulated incrementally (walks a few vertices per redraw).
  if (!odoOk || odoRoute != r.id() || odoIdx < r.tail() || odoIdx >= r.head()) {
    odoOk = true; odoRoute = r.id(); odoIdx = lo; odoM = 0;
  } else {
    while (odoIdx < lo) { odoM += gLen(odoIdx); odoIdx++; }
    while (odoIdx > lo) { odoIdx--; odoM -= gLen(odoIdx); }
  }

  // Vehicle position ON the route. The cursor is only the nearest waypoint, so starting the line there made it
  // jump from waypoint to waypoint. Project the marker (ANCHOR) onto the neighbouring segments instead:
  // segment sj..sj+1, fraction st. The trail ends and the route starts at S, the point under the marker.
  auto segT = [&](int a) -> float {
    const float ex = PX[a + 1] - PX[a], ey = PY[a + 1] - PY[a], L2 = ex * ex + ey * ey;
    if (L2 < 1e-4f) return 0.f;
    return ((ANCHOR_X - PX[a]) * ex + (ANCHOR_Y - PY[a]) * ey) / L2;
  };
  int sj = ic; float st = 0.f;
  if (ic + 1 < n) {
    st = segT(ic);
    if (st < 0.f && ic >= 1) { sj = ic - 1; st = segT(sj); }
  } else { sj = ic - 1; st = 1.f; }                          // cursor is the last point
  if (sj < 0) sj = 0;
  if (st < 0.f) st = 0.f;
  if (st > 1.f) st = 1.f;
  const float sx = PX[sj] + st * (PX[sj + 1] - PX[sj]);
  const float sy = PY[sj] + st * (PY[sj + 1] - PY[sj]);

  // Dotted trail: oldest visible point -> marker. Dots sit at fixed route distances (multiples of DOT_PX).
  if (draw) {
    float next = 0;
    const float ph = fmodf((float)fmod(odoM * scale, (double)DOT_PX), DOT_PX);
    next = ph > 0.001f ? DOT_PX - ph : 0.f;
    for (int q = 0; q < sj; q++) dottedSeg(PX[q], PY[q], PX[q + 1], PY[q + 1], next);
    dottedSeg(PX[sj], PY[sj], sx, sy, next);
  }

  // Route distance (m) from the marker to every projected vertex ahead of it.
  static float ACC[BACK_MAX + FWD_MAX + 2];
  ACC[sj + 1] = (1.f - st) * lenM(sj);
  for (int v = sj + 2; v < n; v++) ACC[v] = ACC[v - 1] + lenM(v - 1);
  // Route distance (m) from the marker to the point at fraction t of segment a (a >= sj, a <= n-1).
  auto arcTo = [&](int a, float t) -> float {
    if (a <= sj) return (t - st) * lenM(sj);
    return ACC[a] + (a < n - 1 ? t * lenM(a) : 0.f);
  };

  // ---- next maneuver node ----
  // distToTurn comes from the phone about once a second while the dead-reckoned marker keeps moving, so the two
  // disagree by up to speed * ~1.5 s. Widen the tolerances with speed, or the node got re-picked (= jumped) at speed.
  const float resyncM = TURN_RESYNC_M + m.pose.speedMs() * 2.0f;
  const float bendM   = TURN_BEND_M + m.pose.speedMs() * 1.5f;
  int mode = 0;                                              // 0 all wide (unknown / not visible), 1 node, 2 thin (just passed)
  int na = -1; float nodeX = 0, nodeY = 0;
  if (m.distToTurn == 0xFFFF || arrived) {
    turnOk = false; turnWait = false;
  } else {
    const float   d   = (float)m.distToTurn;
    const uint8_t rid = r.id();

    if (turnOk && turnRoute == rid && !turnWait) {           // vehicle went past the pinned point?
      const int a0 = (int)(int32_t)(turnIdx - lo);
      if (a0 < sj || (a0 == sj && turnT <= st)) {
        turnOk = false; turnWait = true;
        waitIcon = m.icon; waitD = m.distToTurn; waitMs = millis(); waitRoute = rid;
      }
    }
    if (turnWait && (waitRoute != rid || m.icon != waitIcon || d > waitD + TURN_RESYNC_M ||
                     millis() - waitMs > TURN_WAIT_MS)) turnWait = false;

    if (turnWait) {
      mode = 2;
    } else {
      bool have = turnOk && turnRoute == rid;
      if (have) {                                            // still consistent with the phone's distance?
        const int a0 = (int)(int32_t)(turnIdx - lo);
        if (a0 < sj) have = false;
        else if (a0 <= n - 1) { if (fabsf(arcTo(a0, turnT) - d) > resyncM) have = false; }
        else if (ACC[n - 1] > d + resyncM) have = false;   // should be visible by now
      }
      if (!have) {
        turnOk = false;
        if (d <= ACC[n - 1]) {                               // inside the visible part
          int bv = -1; float bErr = bendM;                   // 1) the bend vertex whose route distance best matches d
          for (int v = sj + 1; v < n - 1; v++) {
            const float err = fabsf(ACC[v] - d);
            if (err >= bErr) continue;
            const float ax = PX[v] - PX[v - 1], ay = PY[v] - PY[v - 1], bx = PX[v + 1] - PX[v], by = PY[v + 1] - PY[v];
            const float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
            if (la < 1e-3f || lb < 1e-3f) continue;
            if ((ax * bx + ay * by) / (la * lb) > TURN_BEND_COS) continue;   // nearly straight: not a corner
            bv = v; bErr = err;
          }
          if (bv >= 0) { turnIdx = lo + (uint32_t)bv; turnT = 0.f; turnRoute = rid; turnOk = true; have = true; }
          // 2) otherwise find its segment
          for (int q = sj; !have && q < n - 1; q++) {
            if (d > ACC[q + 1]) continue;
            const float L = lenM(q);
            float f = L < 1e-3f ? 0.f : (q == sj ? st + d / L : (d - ACC[q]) / L);
            if (f < 0.f) f = 0.f;
            if (f > 1.f) f = 1.f;
            int a = q;
            if (q > sj && f * L <= TURN_SNAP_M) f = 0.f;                       // snap to the waypoint behind
            else if ((1.f - f) * L <= TURN_SNAP_M) { a = q + 1; f = 0.f; }    // snap to the waypoint ahead
            turnIdx = lo + (uint32_t)a; turnT = f; turnRoute = rid; turnOk = true; have = true;
            break;
          }
        }
      }
      if (have) {
        const int a = (int)(int32_t)(turnIdx - lo);
        if (a >= sj && a <= n - 1) {
          if (a >= n - 1) { nodeX = PX[n - 1]; nodeY = PY[n - 1]; }
          else { nodeX = PX[a] + turnT * (PX[a + 1] - PX[a]); nodeY = PY[a] + turnT * (PY[a + 1] - PY[a]); }
          na = a; mode = 1;
        }
      }
    }
  }

  // Snapshot for the idle <-> nav transition: the marker point and up to NV_RS - 1 projected vertices ahead of it.
  {
    int k = 0;
    nvS.rx[k] = sx; nvS.ry[k] = sy; k++;
    for (int v = sj + 1; v < n && k < NV_RS; v++, k++) { nvS.rx[k] = PX[v]; nvS.ry[k] = PY[v]; }
    nvS.n = k;
    nvS.endOk = reachedEnd && k == n - sj;                  // nothing was cut off: the last point is the real end
    nvS.nodeMode = mode;
    if (mode == 1) {                                        // arc length (px) from the marker to the turn node
      const int sg = na - sj;                               // snapshot segment sg -> sg+1 holds the node
      if (sg >= 0 && sg < k) {
        float acc = 0.f;
        for (int i = 1; i <= sg; i++) acc += hypotf(nvS.rx[i] - nvS.rx[i - 1], nvS.ry[i] - nvS.ry[i - 1]);
        nvS.nodeArc = acc + hypotf(nodeX - nvS.rx[sg], nodeY - nvS.ry[sg]);
      } else nvS.nodeMode = 0;                              // beyond the part we keep: the kept road is all wide
    }
  }
  if (!draw) return;

  // Route ahead: marker -> end. 10 px up to the node, 6 px after it.
  float fx = sx, fy = sy;
  for (int v = sj + 1; v < n; v++) {
    const int a = v - 1;                                     // segment a -> v
    if (mode == 1 && a == na) {
      baseSeg(fx, fy, nodeX, nodeY, ROUTE_W);
      baseSeg(nodeX, nodeY, PX[v], PY[v], ROUTE_W2);
    } else {
      const int w = (mode == 2 || (mode == 1 && a > na)) ? ROUTE_W2 : ROUTE_W;
      baseSeg(fx, fy, PX[v], PY[v], w);
    }
    fx = PX[v]; fy = PY[v];
  }
  if (mode == 1) {                                           // node, fixed on the route
    const int nx = lroundf(nodeX), ny = lroundf(nodeY);
    R->baseFillCircle(nx, ny, 12, M_SET);
    R->baseFillCircle(nx, ny, 8, M_CLR);
    R->baseFillCircle(nx, ny, 3, M_SET);
  }
  if (reachedEnd) {                                          // destination
    int ex = lroundf(PX[n - 1]), ey = lroundf(PY[n - 1]);
    R->baseFillCircle(ex, ey, 18, M_SET);
    R->baseFillCircle(ex, ey, 13, M_CLR);
    R->baseFillCircle(ex, ey, 7, M_SET);
  }
}

// Map state at the last redraw: the marker is drawn relative to it (see UI_MARKER_LERP).
int32_t mapIx = 0, mapIy = 0;  float mapTheta = 0, mapScale = 1;  bool mapOk = false;

// Offset (px) of the marker from the anchor: pose now vs pose when the map was drawn, in the map's frame.
void markerOffset(const NavModel& m, float& ox, float& oy) {
  ox = oy = 0.f;
#if UI_MARKER_LERP
  if (!mapOk) return;
  const float th = mapTheta * 0.017453292f, c = cosf(th), s = sinf(th), k = mapScale * 0.1f;   // px per dm
  const float e = (float)(m.pose.x() - mapIx) * k, n = (float)(m.pose.y() - mapIy) * k;
  ox = e * c - n * s;
  oy = -(e * s + n * c);
  const float r2 = ox * ox + oy * oy, lim = UI_MARKER_MAX_PX;
  if (r2 > lim * lim) { const float f = lim / sqrtf(r2); ox *= f; oy *= f; }
#else
  (void)m;
#endif
}

void emitMarker(float ox, float oy, uint32_t now, bool stale) {
  const int cx = ANCHOR_X + (int)lroundf(ox), cy = ANCHOR_Y + (int)lroundf(oy);
  if (stale) {
    R->ovBitmap(cx - 24, cy - 24, 48, 48, ptr(spDash), M_SET);          // dashed accuracy ring
    if ((now / 500) & 1) return;                                        // marker blinks
    R->ovFillCircle(cx, cy, 19, M_CLR);
    R->ovFillCircle(cx, cy, 12, M_SET);
    R->ovFillCircle(cx, cy, 8, M_CLR);
    R->ovBitmap(cx - 14, cy - 14, 28, 28, ptr(spChev), M_SET);
    return;
  }
  R->ovCircle(cx, cy, 22, M_SET);                                       // accuracy ring
  R->ovCircle(cx, cy, 21, M_SET);
#if UI_ANIMATIONS
  int step = (int)(((now % 2200) * 8) / 2200);                          // sonar ping, 8 steps
  if (step < 6) R->ovCircle(cx, cy, 22 + 3 * (step + 1), M_SET);
#else
  (void)now;
#endif
  R->ovFillCircle(cx, cy, 19, M_CLR);                                   // halo over the route
  R->ovFillCircle(cx, cy, 14, M_SET);
  R->ovBitmap(cx - 14, cy - 14, 28, 28, ptr(spChev), M_CLR);            // white chevron
}

struct ViewKey { int32_t qx, qy, qh; uint32_t rev; uint16_t zoom; uint32_t dt; uint8_t hu, fix, st; };
ViewKey lastKey; bool keyValid = false;

const char* cardinal(float h) {
  h = fmodf(h, 360.f); if (h < 0) h += 360.f;
  return lang::cardinal((int)((h + 22.5f) / 45.f));
}

// =============================================================================================
// Idle: the trail
// A dotted road runs in from beyond the left edge and out beyond the right edge. While the phone is not connected a
// marker laps it forever (it leaves on the right and re-enters on the left, so there is never a jump). When the phone
// connects the marker finishes its lap, the road beyond the "?" pin is erased (the pin becomes the destination), and the
// marker re-enters from the left and slows to a stop on the START point; the road behind it is erased as it goes.
// On disconnect everything is restored and the marker resumes lapping.
// The road is a fixed Bezier chain sampled once into a table (distance along the road -> x, y, heading).
// =============================================================================================
const float TR_YS = 0.86f;                    // design y -> panel y (the status bar takes the bottom 36 px)
const int   TR_N = 3 * 48 + 1;
const int   TR_MAX_DOTS = 64;
const float TR_DOT_GAP = 14.f;                // px of road between dots
float trX[TR_N], trY[TR_N], trS[TR_N];
float trL = 1.f, trPinS = 0, trPinX = 0, trPinY = 0, trParkS = 0, trParkX = 0, trParkY = 0;
int16_t trDotX[TR_MAX_DOTS], trDotY[TR_MAX_DOTS];
float   trDotS[TR_MAX_DOTS];
int     trDots = 0;

TFT_eSprite *spCloud = nullptr, *spTreeA = nullptr, *spTreeB = nullptr, *spXMark = nullptr, *spPin = nullptr, *spTrailChev = nullptr, *spPill = nullptr, *spMorph = nullptr;
TextSpr tPinQ, tStartL;
int trailChevQ = -999;

void trailAt(float s, float& x, float& y, float& deg) {
  if (s < 0.f) s = 0.f;
  if (s > trL) s = trL;
  int i = 0;
  while (i < TR_N - 2 && trS[i + 1] < s) i++;
  const float seg = trS[i + 1] - trS[i];
  const float u = seg > 0.f ? (s - trS[i]) / seg : 0.f;
  x = trX[i] + (trX[i + 1] - trX[i]) * u;
  y = trY[i] + (trY[i + 1] - trY[i]) * u;
  deg = atan2f(trX[i + 1] - trX[i], -(trY[i + 1] - trY[i])) * 57.29577951f;   // clockwise from up
}

void buildTrail() {
  static const float C[3][8] = {
    {-30, 268, 50, 268, 110, 190, 200, 196},
    {200, 196, 290, 202, 300, 262, 336, 206},
    {336, 206, 372, 150, 430, 130, 510, 140}};
  int n = 0;
  for (int k = 0; k < 3; k++) {
    const int steps = (k == 2) ? 49 : 48;
    for (int i = 0; i < steps; i++) {
      const float t = i / 48.f, u = 1.f - t;
      const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
      trX[n] = a * C[k][0] + b * C[k][2] + c * C[k][4] + d * C[k][6];
      trY[n] = (a * C[k][1] + b * C[k][3] + c * C[k][5] + d * C[k][7]) * TR_YS;
      n++;
    }
  }
  trS[0] = 0.f;
  for (int i = 1; i < TR_N; i++) trS[i] = trS[i - 1] + hypotf(trX[i] - trX[i - 1], trY[i] - trY[i - 1]);
  trL = trS[TR_N - 1];
  auto atX = [](float x, float& s, float& px, float& py) {        // the road's x only grows: nearest sample in x
    int best = 0;
    for (int i = 1; i < TR_N; i++) if (fabsf(trX[i] - x) < fabsf(trX[best] - x)) best = i;
    s = trS[best]; px = trX[best]; py = trY[best];
  };
  atX(404.f, trPinS, trPinX, trPinY);                              // the pin's tip sits on the road
  atX(56.f, trParkS, trParkX, trParkY);                            // where the marker parks (START)
  trDots = 0;
  for (float s = 0.f; s <= trL && trDots < TR_MAX_DOTS; s += TR_DOT_GAP) {
    float x, y, d; trailAt(s, x, y, d);
    if (x < -3.f || x > SCREEN_W + 3.f) continue;
    trDotX[trDots] = (int16_t)lroundf(x); trDotY[trDots] = (int16_t)lroundf(y); trDotS[trDots] = s; trDots++;
  }
}

// Keep only the outline (t px thick) of a filled 1-bpp shape. The sprite edge counts as outside.
void outlineOf(TFT_eSprite* s, int w, int h, int t) {
  uint8_t* p = rawBits(s);
  static uint8_t tmp[64 * 32];
  if (!p || w * h > (int)sizeof(tmp)) return;
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) tmp[y * w + x] = getBit(p, w, h, x, y);
  memset(p, 0, ((w + 7) >> 3) * h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      if (!tmp[y * w + x]) continue;
      bool edge = false;
      for (int dy = -t; dy <= t && !edge; dy++)
        for (int dx = -t; dx <= t; dx++) {
          const int xx = x + dx, yy = y + dy;
          if (xx < 0 || yy < 0 || xx >= w || yy >= h || !tmp[yy * w + xx]) { edge = true; break; }
        }
      if (edge) setBit(p, w, h, x, y);
    }
}

void buildIdleArt() {
  mkSprite(spCloud, 56, 28);                                 // cloud outline
  spCloud->fillCircle(10, 18, 9, 1); spCloud->fillCircle(26, 12, 12, 1); spCloud->fillCircle(44, 17, 9, 1);
  spCloud->fillRect(10, 18, 35, 10, 1);
  outlineOf(spCloud, 56, 28, 3);

  mkSprite(spTreeA, 28, 44);                                 // round tree (3 px outline) on a trunk
  spTreeA->fillCircle(14, 14, 13, 1); spTreeA->fillCircle(14, 14, 10, 0); spTreeA->fillRect(13, 27, 3, 16, 1);
  mkSprite(spTreeB, 22, 36);
  spTreeB->fillCircle(11, 11, 10, 1); spTreeB->fillCircle(11, 11, 7, 0); spTreeB->fillRect(10, 21, 3, 14, 1);

  mkSprite(spXMark, 14, 14);                                 // "x marks the spot"
  thickLine(spXMark, 2, 2, 11, 11, 1); thickLine(spXMark, 11, 2, 2, 11, 1);

  mkSprite(spPin, 34, 47);                                   // map pin, tip at (17, 46)
  spPin->fillCircle(17, 17, 16, 1);
  spPin->fillTriangle(3, 24, 31, 24, 17, 46, 1);

  mkSprite(spTrailChev, 28, 28);
  trailChevQ = -999;
  mkSprite(spMorph, 28, 28);                                 // idle marker kite -> nav chevron (idle <-> nav transition)

  mkSprite(spPill, 150, 26);                                 // status pill capsule (same shape idleScene() draws with primitives)
  spPill->fillRect(13, 0, 124, 26, 1); spPill->fillCircle(13, 13, 13, 1); spPill->fillCircle(136, 13, 13, 1);

  mk(tPinQ, 0, 0, 24, 26);                                   // "?" knocked out of the pin head (placed every frame)
  setText(tPinQ, "?", S19, AL_C, 20);
  mk(tStartL, (int)lroundf(trParkX) - 40, (int)lroundf(trParkY) + 32, 80, 16);   // 80 wide: "INICIO" is longer than "START"
  setText(tStartL, lang::tr(lang::S_START), FB11, AL_C, 12, 2);
}

// Marker = black disc with a white arrow pointing along the road (the logo's kite).
void emitTrailMarker(int cx, int cy, float deg) {
  const int q = (int)lroundf(deg / 6.f);
  if (q != trailChevQ) {
    trailChevQ = q;
    const float a = q * 6.f * 0.017453292f, ca = cosf(a), sa = sinf(a);
    const float P[4][2] = {{0, -10}, {8, 11}, {0, 6}, {-8, 11}};
    int X[4], Y[4];
    for (int i = 0; i < 4; i++) { X[i] = lroundf(14 + P[i][0] * ca - P[i][1] * sa); Y[i] = lroundf(14 + P[i][0] * sa + P[i][1] * ca); }
    spTrailChev->fillSprite(0);
    spTrailChev->fillTriangle(X[0], Y[0], X[1], Y[1], X[2], Y[2], 1);
    spTrailChev->fillTriangle(X[0], Y[0], X[3], Y[3], X[2], Y[2], 1);
    R->markDirtyRect(cx - 14, cy - 14, cx + 13, cy + 13);
  }
  R->ovFillCircle(cx, cy, 17, M_SET);
  R->ovBitmap(cx - 14, cy - 14, 28, 28, ptr(spTrailChev), M_CLR);
}

// Marker, "damaged": just the 3 px outline of the disc, no arrow.
void emitTrailMarkerHollow(int cx, int cy) {
  R->ovCircle(cx, cy, 17, M_SET); R->ovCircle(cx, cy, 16, M_SET); R->ovCircle(cx, cy, 15, M_SET);
}

// IP_LOST / IP_ACCEL come after IP_READY on purpose: test "parked" with (IP_PARK || IP_READY), never with >=.
enum IdlePhase : uint8_t { IP_SEARCH, IP_CATCH, IP_PARK, IP_READY, IP_LOST, IP_ACCEL };
const float IP_LAP_S = 8.f;          // one lap while searching
const float IP_PARK_S = 1.6f;        // slowing down onto START
const float IP_CATCH_MULT = 4.0f;    // on connect the marker speeds up to this multiple of the lap speed to reach START sooner
const float IP_CATCH_RAMP_S = 0.4f;  // ...ramping up over this long (no jump in speed)
const float IP_TAIL_PXS = 120.f;     // road beyond the pin is erased at this speed (px/s)
const float IP_REGROW_PXS = 300.f;   // ...and regrown at this speed on disconnect (from the pin and from the marker)
// Disconnect ("the marker took damage"): LOST = START ring collapses, marker stands still, flickers disc/outline and jitters;
// ACCEL = it eases from a standstill back up to lap speed while the road regrows; then SEARCH again.
const uint32_t IP_LOST_MS = 350;       // LOST phase length
const uint32_t IP_RING_MS = 250;       // START ring collapse (inside LOST)
const uint32_t IP_BLINK_MS = 58;       // disc / outline flicker half-period (3 blinks in LOST_MS)
const float    IP_ACCEL_S = 1.2f;      // ease-in back to lap speed
const uint32_t IP_LOST_TEXT_MS = 1600; // pill + hint keep saying DISCONNECTED this long after the link dropped

IdlePhase ipPh = IP_SEARCH;
float     ipS = 0.f, ipTrimS = 0.f, ipTrimE = 1.f, ipParkT = 0.f;   // marker / visible road start / visible road end (px along the road)
uint32_t  ipBobT = 0;                                      // the pin bobs on (now - ipBobT): a transition re-bases it so the pin starts at rest
uint32_t  ipLast = 0, ipReadyT = 0, ipLostMs = 0;   // ipLostMs: when the link dropped (0 = not recently)
float     ipBoost = 1.f;                                     // CATCH speed multiplier (1 .. IP_CATCH_MULT)
float     ipRing0 = 0.f;                                     // START ring radius at the moment of the drop
bool      ipFresh = true, ipConn = false;

void idleUpdate(const Hud& h) {                              // the state machine (no drawing)
  const uint32_t now = h.nowMs;
  float dt = (now - ipLast) * 1e-3f;
  ipLast = now;
  if (dt > 0.1f) dt = 0.1f;                                    // first frame / returning from another screen

  if (ipFresh) {                                               // screen just entered: no replay of the transition
    ipFresh = false; ipConn = h.connected; ipLostMs = 0;
    if (ipConn) { ipPh = IP_READY; ipS = trParkS; ipTrimS = trParkS; ipTrimE = trPinS; ipReadyT = now - 1000; }
    else        { ipPh = IP_SEARCH; ipS = 0.f; ipTrimS = 0.f; ipTrimE = trL; }
  }
  if (h.connected != ipConn) {                                 // connect / disconnect
    ipConn = h.connected;
    if (ipConn) {
      ipLostMs = 0;
      if (ipPh == IP_SEARCH || ipPh == IP_LOST || ipPh == IP_ACCEL) ipPh = IP_CATCH;   // reconnected mid-transition: finish the lap, park
    } else {
#if UI_ANIMATIONS
      ipLostMs = now ? now : 1;
      if (ipPh == IP_READY || ipPh == IP_PARK) {
        ipRing0 = ipPh == IP_READY ? 18.f + 8.f * easeOut((now - ipReadyT) / 250.f) : 0.f;
        ipPh = IP_LOST;
      } else ipPh = IP_SEARCH;                                 // dropped while still catching up: just keep lapping
#else
      ipPh = IP_SEARCH;
#endif
    }
  }

#if UI_ANIMATIONS
  const float v = trL / IP_LAP_S;
  if (ipPh != IP_CATCH) ipBoost = 1.f;
  switch (ipPh) {
    case IP_SEARCH: ipS += v * dt; if (ipS >= trL) ipS -= trL; break;
    case IP_CATCH:
      ipBoost += (IP_CATCH_MULT - 1.f) / IP_CATCH_RAMP_S * dt; if (ipBoost > IP_CATCH_MULT) ipBoost = IP_CATCH_MULT;
      ipS += v * ipBoost * dt;
      if (ipS >= trL) { ipS = 0.f; ipParkT = 0.f; ipPh = IP_PARK; } break;
    case IP_PARK: {
      ipParkT += dt;
      float p = ipParkT / IP_PARK_S;
      if (p >= 1.f) { p = 1.f; ipPh = IP_READY; ipReadyT = now; }
      const float e = 1.f - (1.f - p) * (1.f - p) * (1.f - p);
      ipS = trParkS * e;
      break;
    }
    case IP_LOST:
      if ((uint32_t)(now - ipLostMs) >= IP_LOST_MS) { ipPh = IP_ACCEL; ipParkT = 0.f; }
      break;                                                   // marker stands still
    case IP_ACCEL: {
      ipParkT += dt;
      float p = ipParkT / IP_ACCEL_S;
      if (p >= 1.f) { p = 1.f; ipPh = IP_SEARCH; }
      ipS += v * p * p * (3.f - 2.f * p) * dt;                 // smoothstep: 0 -> lap speed
      if (ipS >= trL) ipS -= trL;
      break;
    }
    default: ipS = trParkS; break;
  }
#else
  if (ipConn) { ipPh = IP_READY; ipS = trParkS; ipReadyT = now - 1000; } else { ipPh = IP_SEARCH; ipS = trL * 0.5f; }
#endif

  const bool parked = ipPh == IP_PARK || ipPh == IP_READY;
  if (ipConn) {
    const bool tail = parked || (ipPh == IP_CATCH && ipS >= trPinS);   // erase after the marker passed the pin
    if (tail) { ipTrimE -= IP_TAIL_PXS * dt; if (ipTrimE < trPinS) ipTrimE = trPinS; }
    ipTrimS = parked ? ipS : 0.f;                                        // erased behind the marker
  } else if (ipPh != IP_LOST) {                                // regrowth starts when the marker pulls away
    ipTrimS -= IP_REGROW_PXS * dt; if (ipTrimS < 0.f) ipTrimS = 0.f;
    ipTrimE += IP_REGROW_PXS * dt; if (ipTrimE > trL) ipTrimE = trL;
  }

}

// Draw the scene back to front, limited to screen columns [x0, x1) (a transition cuts it at the curtain).
// The marker is left out during a transition: the curtain's own marker takes its place.
void idleDraw(const Hud& h, int x0, int x1, bool marker) {
  const uint32_t now = h.nowMs;
  const bool full = x0 <= 0 && x1 >= SCREEN_W;

  blitClip(srcOf(spCloud, 276, 12, 56, 28), x0, x1, M_SET);
  blitClip(srcOf(spTreeA, 86, 128, 28, 44), x0, x1, M_SET);
  blitClip(srcOf(spTreeB, 429, 186, 22, 36), x0, x1, M_SET);
  blitClip(srcOf(spXMark, 233, 125, 14, 14), x0, x1, M_SET);

  for (int i = 0; i < trDots; i++)
    if (trDotS[i] >= ipTrimS && trDotS[i] <= ipTrimE) dotC(trDotX[i], trDotY[i], 2, x0, x1, M_SET);

#if UI_ANIMATIONS
  const int bob = (int)lroundf(tri(now - ipBobT, 1800) * 6.f);
#else
  const int bob = 0;
#endif
  const int pinX = (int)lroundf(trPinX) - 17, pinY = (int)lroundf(trPinY) - 46 - bob;
  blitClip(srcOf(spPin, pinX, pinY, 34, 47), x0, x1, M_SET);
  tPinQ.x = pinX + 5; tPinQ.y = pinY + 4;
  blitC(tPinQ, x0, x1, M_CLR);

  if (full && ipPh == IP_READY) {                              // START ring + label
    const float e = easeOut((now - ipReadyT) / 250.f);
    const int r = 18 + (int)lroundf(8.f * e), cx = (int)lroundf(trParkX), cy = (int)lroundf(trParkY);
    R->ovCircle(cx, cy, r, M_SET); R->ovCircle(cx, cy, r - 1, M_SET); R->ovCircle(cx, cy, r - 2, M_SET);
    if (e >= 0.5f) blit(tStartL, M_SET);
  }

  if (full && ipPh == IP_LOST) {                               // START ring collapses (label is already gone)
    const float r = ipRing0 * (1.f - easeOut((now - ipLostMs) / (float)IP_RING_MS));
    if (r >= 3.f) {
      const int ri = (int)lroundf(r), cx = (int)lroundf(trParkX), cy = (int)lroundf(trParkY);
      R->ovCircle(cx, cy, ri, M_SET); R->ovCircle(cx, cy, ri - 1, M_SET); R->ovCircle(cx, cy, ri - 2, M_SET);
    }
  }

  if (!marker) return;
  float mx, my, mdeg;
  trailAt(ipS, mx, my, mdeg);
  if (mx > -20.f && mx < SCREEN_W + 20.f) {
    int cx = (int)lroundf(mx), cy = (int)lroundf(my);
    if (ipPh == IP_LOST) {                                     // damage: flicker disc <-> outline, shudder 2 px
      const uint32_t el = now - ipLostMs;
      static const int8_t JIT[4] = {0, 2, -2, 1};
      cx += JIT[(el / 33) & 3];
      if ((el / IP_BLINK_MS) & 1) emitTrailMarkerHollow(cx, cy);
      else                        emitTrailMarker(cx, cy, mdeg);
    } else {
      emitTrailMarker(cx, cy, mdeg);
    }
  }
}

// True while the pill / hint should say DISCONNECTED. Called by idleFrame() BEFORE idleUpdate(), hence the ipConn test:
// it is still true on the very first frame after the drop, so the text never shows one frame of SEARCHING.
bool idleLostText(const Hud& h) {
  if (h.connected || ipFresh) return false;
  return ipConn || (ipLostMs && (uint32_t)(h.nowMs - ipLostMs) < IP_LOST_TEXT_MS);
}

// Pill + hint texts for the current link state. Must run BEFORE idleUpdate() (see idleLostText).
void idleTexts(const Hud& h) {
  char stat[48];
  if (!h.connected && idleLostText(h)) {                     // just dropped: say so for a moment (see "Idle: the trail")
    setText(tStat, lang::tr(lang::S_DISCONNECTED), B12, AL_C, 17, 2);
    setText(tHint, lang::tr(lang::S_CONN_LOST), S17, AL_L, 18);
  } else if (!h.connected) {
    static const char* DOTS[4] = {"", ".", "..", "..."};
#if UI_ANIMATIONS
    const char* dots = DOTS[(h.nowMs / 350) & 3];
#else
    const char* dots = DOTS[3];
#endif
    snprintf(stat, sizeof(stat), "%s%s", lang::tr(lang::S_SEARCHING), dots);
    if (begin(tStat, stat)) {                                // centred on the full text, so the dots do not shift it
      char full[40]; snprintf(full, sizeof(full), "%s...", lang::tr(lang::S_SEARCHING));
      int w = textW(B12, full, 2);
      put(tStat, B12, stat, (tStat.w - w) / 2, 17, 2);
      finish(tStat);
    }
    setText(tHint, lang::tr(lang::S_NEXT_TRIP), S17, AL_L, 18);
  } else {
    setText(tStat, lang::tr(lang::S_CONNECTED), B12, AL_C, 17, 2);
    setText(tHint, lang::tr(lang::S_PICK_DEST), S17, AL_L, 18);
  }
}

// The whole idle screen except the status bar, limited to screen columns [x0, x1).
void idleScene(const Hud& h, int x0, int x1, bool marker) {
  idleDraw(h, x0, x1, marker);                               // road, pin, marker (see "Idle: the trail")
  blitC(tIdleT, x0, x1, M_SET);
  blitC(tHint, x0, x1, M_SET);
  if (x0 <= 0 && x1 >= SCREEN_W) {                           // status pill: black capsule, text knocked out
    R->ovFillRect(tStat.x + 13, tStat.y, tStat.w - 26, tStat.h, M_SET);
    R->ovFillCircle(tStat.x + 13, tStat.y + 13, 13, M_SET);
    R->ovFillCircle(tStat.x + tStat.w - 14, tStat.y + 13, 13, M_SET);
  } else {
    blitClip(srcOf(spPill, tStat.x, tStat.y, tStat.w, tStat.h), x0, x1, M_SET);
  }
  blitC(tStat, x0, x1, M_CLR);
}

// =============================================================================================
// Pairing screen
// `pt` is the seconds since the pairing items started their own clock; pt >= 1.6 is the finished, steady screen.
//   0.35 - 0.65  underline bars grow from their centres, 30 ms apart        0.40  PAIRING CODE label appears
//   0.60 - 1.24  each digit flickers through random digits for 110 ms, left to right, then locks with a 3 px bump
//   1.30 - 1.50  both hint lines open from the centre outwards
// `inv` draws the code inverted (the flash when pairing succeeded). Everything is limited to screen columns [x0, x1).
// =============================================================================================
const int PCX[6] = {26, 86, 146, 222, 282, 342};             // digit centres inside tPDig
const int PDX[6] = {0, 60, 120, 196, 256, 316};              // digit boxes (52 px) inside tPDig
const int PUX[6] = {56, 116, 176, 252, 312, 372};            // underline bars on screen

void pairDraw(const char* code, float pt, bool inv, int x0, int x1) {
  setText(tPH1, lang::tr(lang::S_PAIR_H1), S17, AL_C, 19);
  setText(tPH2, lang::tr(lang::S_PAIR_H2), M14, AL_C, 16);

  char chars[7]; int dy[6]; bool plain = true;               // plain: all six locked and still, so tPDig is drawn in one piece
  for (int i = 0; i < 6; i++) {
    const float st = 0.60f + 0.09f * i, lk = st + 0.11f;
    dy[i] = 0;
    if (pt < st) { chars[i] = ' '; plain = false; }
    else if (pt < lk) {                                      // same flicker sequence as the prototype: one step per 40 ms
      const int f = (int)(pt * 25.f);
      chars[i] = (char)('0' + (f * (7 + i) + 3 * i) % 10);
      plain = false;
    } else {
      chars[i] = code[i];
      if (pt - lk < 0.08f) { dy[i] = -(int)lroundf(3.f * (1.f - (pt - lk) / 0.08f)); if (dy[i]) plain = false; }
    }
  }
  chars[6] = 0;
  if (begin(tPDig, chars)) {                                 // six digits, each centred in a 52 px box (hidden ones left out)
    for (int i = 0; i < 6; i++) {
      if (chars[i] == ' ') continue;
      const char one[2] = {chars[i], 0};
      put(tPDig, D64, one, PCX[i] - textW(D64, one, 0) / 2, 55, 0);
    }
    finish(tPDig);
  }

  blitClip(srcOf(spLogoSmall, 20, 16, LOGO_SMALL, LOGO_SMALL), x0, x1, M_SET);
  blitC(tPTitle, x0, x1, M_SET);
  blitC(tPTag, x0, x1, M_SET);
  if (pt > 0.40f) blitC(tPLbl, x0, x1, M_SET);

  for (int i = 0; i < 6; i++) {
    const int w = (int)lroundf(52.f * easeOut((pt - 0.35f - 0.03f * i) / 0.15f));
    if (w > 0) rectC(PUX[i] + (52 - w) / 2, 178, w, 4, x0, x1, M_SET);
  }

  const Mode dm = inv ? M_CLR : M_SET;
  if (inv) rectC(56, 104, 368, 72, x0, x1, M_SET);
  if (plain) blitC(tPDig, x0, x1, dm);
  else for (int i = 0; i < 6; i++)
    if (chars[i] != ' ') blitClip(srcOf(tPDig), x0, x1, dm, dy[i], PDX[i], PDX[i] + 52);

  const int hw = (int)lroundf(SCREEN_W * easeOut((pt - 1.3f) / 0.2f));
  if (hw > 0) {
    int a = SCREEN_W / 2 - hw / 2, b = a + hw;
    if (a < x0) a = x0;
    if (b > x1) b = x1;
    blitC(tPH1, a, b, M_SET);
    blitC(tPH2, a, b, M_SET);
  }
}

// =============================================================================================
// Idle <-> pairing transition
// The marker pulls the next screen in. A 3 px curtain bar sweeps left to right along the road (PT_WIPE_MS, linear) with
// the marker riding it; the old screen is on one side of the bar, the new one on the other.
//   IN   (Hud::pairing rises)          idle -> pairing, then the staged build-up of pairDraw().
//   OUT  (Hud::pairing falls)          pairing -> idle. If Hud::connected is true at that moment the code was accepted:
//                                      the digits flash inverted twice, then the wipe runs with the normal marker and
//                                      idle resumes with the marker re-entering at the road start (CATCH). Otherwise
//                                      the wipe starts at once, the marker rides as the damaged outline and idle goes
//                                      back to SEARCHING.
// The idle state machine is frozen (and reset to a calm SEARCHING, road fully drawn, marker off the left edge) for the
// whole transition, so the frame after it simply carries on from there.
// =============================================================================================
enum PairTr : uint8_t { PTR_NONE, PTR_IN, PTR_OUT };
PairTr   ptMode = PTR_NONE;
bool     ptFresh = true, ptPrev = false, ptOk = false;
uint32_t ptStart = 0;
char     ptCode[8] = "000000";                               // last passkey seen while pairing (Hud::passkey may be cleared at the end)

void idleReset(uint32_t now) {
  ipFresh = false; ipConn = false; ipPh = IP_SEARCH;
  ipS = 0.f; ipTrimS = 0.f; ipTrimE = trL; ipBoost = 1.f; ipLostMs = 0; ipLast = now;
}

// Nearest road sample in x (the road's x only grows) -> its distance along the road.
float trailSAtX(float x) {
  int best = 0;
  for (int i = 1; i < TR_N; i++) if (fabsf(trX[i] - x) < fabsf(trX[best] - x)) best = i;
  return trS[best];
}

inline int wipeX(uint32_t el) {
  return el >= PT_WIPE_MS ? SCREEN_W : (int)(SCREEN_W * (el / (float)PT_WIPE_MS) + 0.5f);
}

// The curtain edge: a bar with the marker riding it along the road.
void pairEdge(int wx, bool hollow) {
  R->ovFillRect(wx - 3, 0, 3, SCREEN_H, M_SET);
  float mx, my, md;
  trailAt(trailSAtX((float)wx), mx, my, md);
  if (hollow) emitTrailMarkerHollow(wx, (int)lroundf(my));
  else        emitTrailMarker(wx, (int)lroundf(my), md);
}

void transIn(const Hud& h, uint32_t el) {                    // idle on the right of the curtain, pairing on the left
  const int wx = wipeX(el);
  const float pt = ((int32_t)el - (int32_t)PT_STAGE_MS) * 1e-3f;
  if (wx < SCREEN_W) { idleTexts(h); idleScene(h, wx, SCREEN_W, false); }
  pairDraw(ptCode, pt, false, 0, wx);
  emitStatusBar(h, true);
  if (wx < SCREEN_W) pairEdge(wx, false);
}

void transOut(const Hud& h, uint32_t el) {                   // idle on the left of the curtain, pairing on the right
  const uint32_t fl = ptOk ? PT_FLASH_MS : 0;
  if (el < fl) {                                             // accepted: flash the code
    pairDraw(ptCode, 99.f, (el / PT_FLASH_STEP_MS) % 2 == 0, 0, SCREEN_W);
    emitStatusBar(h, true);
    return;
  }
  const uint32_t ew = el - fl;
  const int wx = wipeX(ew);
  if (wx < SCREEN_W) pairDraw(ptCode, 99.f, false, wx, SCREEN_W);
  if (wx > 0) { idleTexts(h); idleScene(h, 0, wx, false); }
  emitStatusBar(h, true);
  if (wx < SCREEN_W) pairEdge(wx, !ptOk && (ew / IP_BLINK_MS) % 2 == 1);
}

// Edge detection on Hud::pairing, and the end of a running transition.
void pairTrUpdate(const Hud& h) {
#if UI_ANIMATIONS && UI_PAIR_TRANSITION
  const uint32_t now = h.nowMs;
  if (ptFresh) { ptFresh = false; ptPrev = h.pairing; ptMode = PTR_NONE; return; }   // screen just entered: no replay
  if (h.pairing != ptPrev) {
    ptPrev = h.pairing;
    ptStart = now;
    idleReset(now);
    if (h.pairing) ptMode = PTR_IN;
    else { ptMode = PTR_OUT; ptOk = h.connected; }
  }
  const uint32_t el = trElapsed(now, ptStart, (float)UI_PAIR_SPEED_DIV);
  if (ptMode == PTR_IN && el >= PT_IN_MS) ptMode = PTR_NONE;
  else if (ptMode == PTR_OUT && el >= (ptOk ? PT_FLASH_MS : 0) + PT_WIPE_MS) ptMode = PTR_NONE;
#else
#if UI_ANIMATIONS
  // Bespoke transition off: a change of Hud::pairing is a screen change, so it gets the interlock reveal like every other one.
  if (ptFresh) { ptFresh = false; ptPrev = h.pairing; return; }       // screen just entered: enter() already started the reveal
  if (h.pairing != ptPrev) { ptPrev = h.pairing; revealPending = true; }
#else
  (void)h;
#endif
#endif
}

// =============================================================================================
// Idle -> OTA transition (port of "CubiNav: IDLE to OTA transition.html")
// Starts by itself when enter(SCR_UPDATE) is called while the idle screen was showing; it plays inside updateFrame().
// The road becomes the progress ruler: the scenery closes, a wave flattens the dots onto the ruler line while the marker
// drives off, then the update screen builds on top. T = seconds on the scaled clock (see UI_OTA_SPEED_DIV):
//   0.00 - 0.45  scenery closes toward its own centre line, 40 ms apart; START ring collapses; version retracts (0 - 0.25);
//                the marker accelerates away
//   0.25 - 1.10  wave from the marker: each dot eases onto the ruler line; the marker flattens and leaves to the right
//   1.00 - 1.55  rule grows from the centre, ticks drop in, label opens, percent rolls once and locks
//   1.35 - 1.85  KB count and version open from their centres, then the hint with its padlock
// During the transition everything is drawn in the overlay; at the end the ruler moves into BASE (updateRuler()).
// =============================================================================================
const int   OT_RX = 49, OT_RW = 382, OT_RY = 182;            // ruler: x, width, rule y
const float OT_END = 1.85f;

bool     otActive = false, otPending = false, otConn = false, otRing = false;
uint32_t otStart = 0;
float    otS0 = 0.f, otV0 = 0.f, otXm = 0.f;                 // marker start (px along the road), start speed, wave origin x
int      otN = 0;
int16_t  otDX[TR_MAX_DOTS], otDY[TR_MAX_DOTS], otDT[TR_MAX_DOTS];   // dots: start x, y, and target x on the ruler

// Window [a, b) of an element that squeezes shut toward its centre (p 0..1). False once closed.
inline bool shutWin(int x0, int x1, float p, int& a, int& b) {
  if (p >= 1.f) return false;
  const float e = easeOut(p), c = (x0 + x1) * .5f, hw = (x1 - x0) * .5f * (1.f - e);
  a = (int)lroundf(c - hw); b = (int)lroundf(c + hw);
  return b > a;
}
// Window of an element that opens from its centre (p 0..1). Fully open = the whole screen (no clipping).
inline bool openWin(int cx, int hw, float p, int& a, int& b) {
  if (p >= 1.f) { a = 0; b = SCREEN_W; return true; }
  const float e = easeOut(p);
  a = (int)lroundf(cx - hw * e); b = (int)lroundf(cx + hw * e);
  return b > a;
}

// The curtain bar (3 px, its right edge at bx) with the marker riding it, used by the cancel -> idle transition.
void curtainBar(int bx, int my, float deg, bool hollow = false) {
  const int a = bx - 3 < 0 ? 0 : bx - 3;
  if (bx > a) R->ovFillRect(a, 0, bx - a, SCREEN_H, M_SET);
  if (hollow) emitTrailMarkerHollow(bx, my);                 // the damaged marker (failure)
  else        emitTrailMarker(bx, my, deg);
}

// Static ruler under the progress bar (BASE).
void updateRuler() {
  R->baseFillRect(OT_RX, OT_RY, OT_RW, 2, M_SET);
  for (int i = 0; i <= 20; i++) R->baseFillRect(OT_RX + i * 19, 186, 2, 8, M_SET);
  for (int i = 0; i < 5; i++)   R->baseFillRect(OT_RX + i * 95, 186, 2, 14, M_SET);
}

// Called from enter(): snapshot the idle state the transition starts from.
__attribute__((unused)) void otBegin() {
  const float v = trL / IP_LAP_S;
  const bool still = ipPh == IP_PARK || ipPh == IP_READY || ipPh == IP_LOST;
  otConn = ipConn;
  otRing = ipPh == IP_READY;
  otS0 = ipS;
  otV0 = still ? 0.f : v * (ipPh == IP_CATCH ? ipBoost : 1.f);
  float x, y, d; trailAt(ipS, x, y, d);
  otXm = x < 0.f ? 0.f : (x > SCREEN_W ? (float)SCREEN_W : x);
  otN = 0;
  for (int i = 0; i < trDots; i++)
    if (trDotS[i] >= ipTrimS && trDotS[i] <= ipTrimE) { otDX[otN] = trDotX[i]; otDY[otN] = trDotY[i]; otN++; }
  for (int i = 0; i < otN; i++) otDT[i] = otN > 1 ? (int16_t)(OT_RX + (OT_RW * i) / (otN - 1)) : (int16_t)240;
  otActive = otPending = true;
}

// Scenery that closes + the dots that flatten onto the ruler line.
void otIdleLayer(const Hud& h, float T) {
  Hud hh = h; hh.connected = otConn;                         // texts stay as they were on the idle screen
  idleTexts(hh);
  auto p = [T](int i) { return (T - i * .04f) / .2f; };
  int a, b;
  if (shutWin(276, 338, p(0), a, b)) blitClip(srcOf(spCloud, 276, 12, 56, 28), a, b, M_SET);
  if (shutWin(86, 114, p(1), a, b))  blitClip(srcOf(spTreeA, 86, 128, 28, 44), a, b, M_SET);
  if (shutWin(233, 247, p(2), a, b)) blitClip(srcOf(spXMark, 233, 125, 14, 14), a, b, M_SET);
  if (shutWin(429, 451, p(3), a, b)) blitClip(srcOf(spTreeB, 429, 186, 22, 36), a, b, M_SET);

  const float rw = OT_RW * easeOut((T - 1.f) / .25f);        // dots hide under the rule once it has grown over them
  for (int i = 0; i < otN; i++) {
    const float q = easeIO((T - (.25f + fabsf(otDX[i] - otXm) / 900.f)) / .3f);
    if (q >= 1.f && fabsf(otDT[i] - 240.f) <= rw * .5f) continue;
    R->ovFillCircle((int)lroundf(otDX[i] + (otDT[i] - otDX[i]) * q), (int)lroundf(otDY[i] + (OT_RY + 1 - otDY[i]) * q), 2, M_SET);
  }

  const int bob = (int)lroundf(tri(h.nowMs, 1800) * 6.f), pcx = (int)lroundf(trPinX);
  if (shutWin(pcx - 17, pcx + 17, p(4), a, b)) {
    const int pinX = pcx - 17, pinY = (int)lroundf(trPinY) - 46 - bob;
    blitClip(srcOf(spPin, pinX, pinY, 34, 47), a, b, M_SET);
    tPinQ.x = pinX + 5; tPinQ.y = pinY + 4;
    blitC(tPinQ, a, b, M_CLR);
  }
  if (otRing && p(5) < 1.f) {                                // START ring collapses
    const float r = 26.f * (1.f - easeOut(p(5)));
    if (r >= 3.f) {
      const int ri = (int)lroundf(r), cx = (int)lroundf(trParkX), cy = (int)lroundf(trParkY);
      R->ovCircle(cx, cy, ri, M_SET); R->ovCircle(cx, cy, ri - 1, M_SET); R->ovCircle(cx, cy, ri - 2, M_SET);
      if (p(5) < .3f) blit(tStartL, M_SET);
    }
  }
  if (shutWin(28, 270, p(6), a, b)) blitC(tIdleT, a, b, M_SET);
  if (shutWin(28, 280, p(7), a, b)) blitC(tHint, a, b, M_SET);
  if (shutWin(306, 456, p(8), a, b)) {
    blitClip(srcOf(spPill, tStat.x, tStat.y, tStat.w, tStat.h), a, b, M_SET);
    blitC(tStat, a, b, M_CLR);
  }
}

// The marker: accelerates from its idle speed to 900 px/s, flattens onto the ruler line and drives off to the right.
void otMarker(float T) {
  const float acc = 900.f - otV0;
  const float s = otS0 + otV0 * T + acc * (T < .3f ? T * T / .6f : .15f + (T - .3f));
  float x, y, d; trailAt(s, x, y, d);
  const float w = easeIO((T - .15f) / .3f);
  if (x > -20.f && x < SCREEN_W + 20.f)
    emitTrailMarker((int)lroundf(x), (int)lroundf(y + (OT_RY + 1 - y) * w), d + (90.f - d) * w);
}

// Huge percent, centred as digits + "%". withPct = false while the digit is still rolling.
void renderPct(const char* digits, bool withPct) {
  char key[12];
  snprintf(key, sizeof(key), "%s%s", withPct ? "" : "~", digits);
  if (!begin(tUpPct, key)) return;
  const int wD = textW(D104, digits, -4), wP = textW(P40, "%", 0);
  const int x = (tUpPct.w - (wD + 6 + wP)) / 2;
  const int e = put(tUpPct, D104, digits, x, 84, -4);
  if (withPct) put(tUpPct, P40, "%", e + 6 + 4, 84, 0);      // +4: undo the trailing negative tracking
  finish(tUpPct);
}

// "FROM -> TO" under the progress bar.
void upVerText(decltype(ota::Info::version) v) {
  char b[48];
  snprintf(b, sizeof(b), "%u.%u.%u -> %u.%u.%u",
           (unsigned)FW_V_MAJOR(FW_VERSION), (unsigned)FW_V_MINOR(FW_VERSION), (unsigned)FW_V_PATCH(FW_VERSION),
           (unsigned)FW_V_MAJOR(v), (unsigned)FW_V_MINOR(v), (unsigned)FW_V_PATCH(v));
  setText(tUpVer, b, FB11, AL_R, 14, 2, 2);
}

// =============================================================================================
// Update cancelled -> idle (port of "Proposed cancel -> IDLE" in CubiNav_Transition_Lab.html)
// The firmware is told by main calling enter(SCR_IDLE) while a running update is on screen (no protocol change). The last
// live update frame is remembered (up*) so the transition can start from exactly what the screen showed.
//   0.00 - 0.30  drain: the bar drains right to left while the percent and the KB count fall in 50 ms steps;
//                the hint changes to lang::tr(lang::S_UPD_CANCELLED) without the padlock
//   0.30 - 0.55  hold: nothing moves (costs no tiles, gives the message time to be read)
//   0.55 - 1.15  sweep: one curtain bar, the one the pairing transition uses, left to right; the marker starts on the ruler line and lifts
//                onto the road. Behind the bar the idle screen returns (road whole, status bar with the version)
// Then the idle state machine takes over from a reset (marker re-enters from the left and parks on START, like after a
// successful pairing). Drawn in the overlay (BASE is blank). Not played when done, while verifying, or while the idle -> update
// transition is still running: those get the plain curtain wipe.
//
// "Update failed": update -> failed (failBuild, inside updateFrame) and failed -> idle (cxFail: failClose, then the same sweep
// with the damaged marker). Neither is played if the screen was still building up.
// =============================================================================================
const float CX_DRAIN = .30f, CX_TS = .55f, CX_SWEEP = .60f;   // cancel: drain, hold, sweep
const float FT_END = .90f;                                    // update -> failed
const float FX_TS = .35f, FX_SWEEP = .60f;                    // failed -> idle: close, sweep

int  upPct = 0;
decltype(ota::Info::done)    upDone{};
decltype(ota::Info::total)   upTotal{};
decltype(ota::Info::version) upVer{};
uint8_t upErr = 0;
bool upLive = false;                                         // the last update frame showed a running update (not verifying)
bool upShown = false;                                        // ... showed the progress screen (running or verifying)
bool upSteady = false;                                       // ... and that screen was fully built (not in the idle -> update transition)
bool upFailed = false;                                       // ... showed the failure screen
bool cxActive = false, cxPending = false, cxFail = false;    // update -> idle transition (cxFail: after a failure, else after a cancel)
uint32_t cxStart = 0;
bool erActive = false, erPending = false;                    // update -> failed transition
uint32_t erStart = 0;

inline float cxTs()    { return cxFail ? FX_TS : CX_TS; }
inline float cxSweep() { return cxFail ? FX_SWEEP : CX_SWEEP; }
inline float cxEnd()   { return cxTs() + cxSweep(); }

// ---- Failure screen pieces ----------------------------------------------------------------------------
const int FL_CX = 240, FL_CY = 92, FL_R = 36;               // the disc with the cross
inline int  inkX0(const TextSpr& t) { return t.x + t.i0; }
inline int  inkX1(const TextSpr& t) { return t.x + t.i1; }
inline bool shutInk(const TextSpr& t, float p, int& a, int& b) { return shutWin(inkX0(t), inkX1(t), p, a, b); }
inline bool openInk(const TextSpr& t, float p, int& a, int& b) { return openWin((inkX0(t) + inkX1(t)) / 2, (inkX1(t) - inkX0(t)) / 2, p, a, b); }

void failDisc(int r) {                                       // black disc, white cross knocked out once it is big enough
  if (r < 1) return;
  R->ovFillCircle(FL_CX, FL_CY, r, M_SET);
  if (r < 20) return;
  const int o = (int)lroundf(12.f * r / (float)FL_R);
  R->ovLine(FL_CX - o, FL_CY - o, FL_CX + o, FL_CY + o, 7, M_CLR);
  R->ovLine(FL_CX + o, FL_CY - o, FL_CX - o, FL_CY + o, 7, M_CLR);
}

void failTexts() {
  setText(tFailE, ota::errorText(upErr), S19, AL_C, 21);
  setText(tFailH, lang::tr(lang::S_FW_KEPT), M14, AL_C, 14);
}

// Update -> failed. The progress screen squeezes shut toward its centre line (40 ms apart), then the failure screen opens:
// disc, title, error, hint. ft = scaled seconds. The label at the top stays.
//   0.00 - 0.32  ruler + bar | percent | KB + version | hint close
//   0.30 - 0.55  disc grows, the cross appears
//   0.45 - 0.85  title, error text and hint open from their centres
void failBuild(float ft) {
  R->ovFillRect(OT_RX, 172, OT_RW, 30, M_CLR);               // hide the ruler of the BASE layer
  auto p = [ft](int i) { return (ft - i * .04f) / .2f; };
  int a, b;
  if (shutWin(OT_RX, OT_RX + OT_RW, p(0), a, b)) {
    rectC(OT_RX, OT_RY, OT_RW, 2, a, b, M_SET);
    for (int i = 0; i <= 20; i++) rectC(OT_RX + i * 19, 186, 2, i % 5 ? 8 : 14, a, b, M_SET);
    const int fw = OT_RW * upPct / 100;
    if (fw > 0) rectC(OT_RX, 172, fw, 10, a, b, M_SET);
  }
  char s[48];
  snprintf(s, sizeof(s), "%d", upPct);
  renderPct(s, true);
  if (shutInk(tUpPct, p(1), a, b)) blitClip(srcOf(tUpPct), a, b, M_SET);
  snprintf(s, sizeof(s), "%lu / %lu KB", (unsigned long)((upDone + 512) / 1024), (unsigned long)((upTotal + 512) / 1024));
  setText(tUpKB, s, B14, AL_L, 16);
  if (shutInk(tUpKB, p(2), a, b)) blitClip(srcOf(tUpKB), a, b, M_SET);
  upVerText(upVer);
  if (shutInk(tUpVer, p(2), a, b)) blitClip(srcOf(tUpVer), a, b, M_SET);
  const char* msg = lang::tr(lang::S_KEEP_OPEN);
  setText(tUpHint, msg, S14, AL_L, 16);
  const int total = 22 + textW(S14, msg, 0), x0 = (SCREEN_W - total) / 2;
  tUpHint.x = x0 + 22;
  if (shutWin(x0, x0 + total, p(3), a, b)) {
    blitClip(srcOf(spLock, x0, 245, 14, 16), a, b, M_SET);
    blitClip(srcOf(tUpHint), a, b, M_SET);
  }
  failDisc((int)lroundf(FL_R * easeOut((ft - .30f) / .25f)));
  failTexts();
  if (openInk(tFailT, (ft - .45f) / .2f, a, b)) blitClip(srcOf(tFailT), a, b, M_SET);
  if (openInk(tFailE, (ft - .55f) / .2f, a, b)) blitClip(srcOf(tFailE), a, b, M_SET);
  if (openInk(tFailH, (ft - .65f) / .2f, a, b)) blitClip(srcOf(tFailH), a, b, M_SET);
}

// Failed -> idle, first half: the failure screen (and the label) squeeze shut, 40 ms apart. t = scaled seconds.
void failClose(float t) {
  failTexts();
  auto p = [t](int i) { return (t - i * .04f) / .2f; };
  int a, b;
  failDisc((int)lroundf(FL_R * (1.f - easeOut(p(0)))));
  if (shutInk(tFailT, p(1), a, b)) blitClip(srcOf(tFailT), a, b, M_SET);
  if (shutInk(tFailE, p(2), a, b)) blitClip(srcOf(tFailE), a, b, M_SET);
  if (shutInk(tFailH, p(3), a, b)) blitClip(srcOf(tFailH), a, b, M_SET);
  if (shutInk(tUpLbl, p(4), a, b)) blitClip(srcOf(tUpLbl), a, b, M_SET);
}

// Both update -> idle transitions share the sweep; tt = scaled seconds.
//   cancel: the update screen drains, holds, then is swept away by the bar (marker lifts from the ruler line onto the road)
//   failed: the failure screen closes (0 - 0.35), then the bar sweeps left to right with the damaged (hollow) marker on it,
//           the same marker the pairing failure uses
void cxDraw(const Hud& h, float tt) {
  const float ts = cxTs();
  const bool  sweep = tt >= ts;
  const float u = sweep ? (tt - ts) / cxSweep() : 0.f;
  const int   bx = sweep ? (int)lroundf((SCREEN_W + 18) * (u > 1.f ? 1.f : u)) : 0;
  const int   ix = bx < SCREEN_W ? bx : SCREEN_W;            // idle on the columns [0, ix), the update screen on [ix, SCREEN_W)

  if (ix > 0) {
    Hud hh = h; hh.nowMs = ipBobT;                           // pin at rest, no ambient motion
    idleTexts(hh);
    idleScene(hh, 0, ix, false);
  }

  if (ix < SCREEN_W && cxFail) {
    if (!sweep) failClose(tt);                               // after the sweep started the failure screen is gone
  } else if (ix < SCREEN_W) {
    const float q = floorf(tt / .05f) * .05f;                // the numbers count down in 50 ms steps, the bar continuously
    const float keep = 1.f - easeIO(q / CX_DRAIN);
    const int   shown = (int)lroundf(upPct * keep);
    const int   barW = (int)lroundf(OT_RW * upPct / 100.f * (1.f - easeIO(tt / CX_DRAIN)));
    char b[48];

    blitClip(srcOf(tUpLbl), ix, SCREEN_W, M_SET);
    rectC(OT_RX, OT_RY, OT_RW, 2, ix, SCREEN_W, M_SET);
    for (int i = 0; i <= 20; i++) rectC(OT_RX + i * 19, 186, 2, i % 5 ? 8 : 14, ix, SCREEN_W, M_SET);
    if (barW > 0) rectC(OT_RX, 172, barW, 10, ix, SCREEN_W, M_SET);

    snprintf(b, sizeof(b), "%d", shown);
    renderPct(b, true);
    blitClip(srcOf(tUpPct), ix, SCREEN_W, M_SET);

    snprintf(b, sizeof(b), "%lu / %lu KB", (unsigned long)(((float)upDone * keep + 512.f) / 1024.f), (unsigned long)((upTotal + 512) / 1024));
    setText(tUpKB, b, B14, AL_L, 16);
    blitClip(srcOf(tUpKB), ix, SCREEN_W, M_SET);
    upVerText(upVer);
    blitClip(srcOf(tUpVer), ix, SCREEN_W, M_SET);

    const char* msg = lang::tr(lang::S_UPD_CANCELLED);
    setText(tUpHint, msg, S14, AL_L, 16);
    tUpHint.x = (SCREEN_W - textW(S14, msg, 0)) / 2;
    blitClip(srcOf(tUpHint), ix, SCREEN_W, M_SET);
  }

  statusSplit(h, ix, true);                                  // idle bar (with version) behind the bar, update bar ahead of it
  if (sweep && u < 1.f) {
    float x, y, d; trailAt(trailSAtX((float)bx), x, y, d);
    if (cxFail) {
      curtainBar(bx, (int)lroundf(y), d, true);
    } else {
      const float w = easeIO((tt - CX_TS) / .25f);
      curtainBar(bx, (int)lroundf(OT_RY + 1 + (y - (OT_RY + 1)) * w), 90.f + (d - 90.f) * w);
    }
  }
}

// =============================================================================================
// Idle <-> navigation transition (port of "CubiNav Idle-Nav Transitions.html")
// The road you picked becomes the route. Both directions run NV_END (1.30) scaled seconds; T is on that scaled clock
// (UI_NAV_SPEED_DIV / UI_NAV_EXIT_SPEED_DIV). Everything is drawn in the overlay while it runs and BASE stays blank; the last
// frame uses the same projected route drawRoute() puts in BASE, so the hand-off to the map is seamless.
//   Idle -> nav (inside navFrame(), after enter(SCR_NAV) from a connected idle screen; any other entry gets the curtain wipe)
//     0.00 - 0.37  scenery squeezes shut toward its centres, 30 ms apart; START ring collapses; version / Bluetooth retract
//     0.10 - 0.80  camera swing: the road turns rigidly about the parked marker until it points up, then settles vertex by vertex
//                  on the real route; dots swell into dashes and then one solid line; the pin becomes the destination ring
//     0.30 - 0.90  battery slides right, speed and heading open from their centres
//     0.45 - 1.28  banner drops, builds (icon, distance, street, compass, rule), speed limit pops
//   Nav -> idle (inside idleFrame(), after enter(SCR_IDLE) from a steady nav screen): the same, played backwards, ending on the
//     parked marker with the START ring grown. Without a phone link (h.connected false) the marker is the damaged outline, there is
//     no START ring, and idle resumes in the accelerating-SEARCHING state with DISCONNECTED text, like after a link drop.
// The route target is nvS (marker point + up to 31 projected vertices), resampled by arc length onto the idle dots.
// =============================================================================================
const float NV_END  = 1.30f;
const int   NV_MAX  = TR_MAX_DOTS;
const float NV_KEND = 0.8f;                                  // the idle road is squeezed to this length while it swings

bool     nvActive = false, nvPending = false, nvIn = true, nvConn = true;
uint32_t nvStart = 0;
int      nvBob = 0;                                          // pin bob (px) when the entry started
int      nvN = 0;                                            // idle dots START -> pin
float    nvSX = 0.f, nvSY = 0.f, nvDeg0 = 0.f, nvLead = 0.f; // START (parked marker), its heading, offset of the first dot ahead of it
float    nvAX[NV_MAX], nvAY[NV_MAX];                         // the dots on the idle road
float    nvBX[NV_MAX], nvBY[NV_MAX];                         // the same dots laid on the route
float    nvPX[NV_MAX], nvPY[NV_MAX];                         // current positions (morph)
float    nvNodeF = 1e9f;                                     // fractional dot index of the turn node (1e9 all wide, -1 all thin)
bool     nvEndOk = false;
int      nvMkKey = -1;

inline float ramp01(float x, float a, float b) { const float t = (x - a) / (b - a); return t < 0.f ? 0.f : (t > 1.f ? 1.f : t); }
inline float nvLerp(float a, float b, float t) { return a + (b - a) * t; }
inline bool  nvOn(float x, float y) { return x > -10.f && x < SCREEN_W + 10.f && y > -10.f && y < SCREEN_H + 10.f; }
inline void  nvDisc(int x, int y, float r, Mode m) { const int ri = (int)lroundf(r); if (ri >= 1) R->ovFillCircle(x, y, ri, m); }

// Middle fraction v (0..1) of the column range [x0, x1): the "squeeze shut / open from the centre" device. Full = no clipping.
inline bool visWin(int x0, int x1, float v, int& a, int& b) {
  if (v <= .001f) return false;
  if (v >= .999f) { a = 0; b = SCREEN_W; return true; }
  const float c = (x0 + x1) * .5f, hw = (x1 - x0) * .5f * v;
  a = (int)lroundf(c - hw); b = (int)lroundf(c + hw);
  return b > a;
}
// Rect clipped to the screen top (the banner slides in from above) and to the columns [x0, x1).
inline void rectY(int x, int y, int w, int h, int x0, int x1, Mode m) {
  if (y < 0) { h += y; y = 0; }
  if (h > 0) rectC(x, y, w, h, x0, x1, m);
}

// Called once from initRest(), after buildTrail(): the idle dots from START to the pin, on the same 14 px grid idle uses.
void nvBuildDots() {
  float dd;
  trailAt(trParkS, nvSX, nvSY, nvDeg0);
  nvSX = (float)lroundf(nvSX); nvSY = (float)lroundf(nvSY);
  const float s0 = ceilf(trParkS / TR_DOT_GAP) * TR_DOT_GAP;
  nvN = 0;
  for (float s = s0; s < trPinS - 3.f && nvN < NV_MAX - 1; s += TR_DOT_GAP) { trailAt(s, nvAX[nvN], nvAY[nvN], dd); nvN++; }
  trailAt(trPinS, nvAX[nvN], nvAY[nvN], dd); nvN++;          // the last point is the pin's tip: it becomes the route's end
  nvLead = (s0 - trParkS) * NV_KEND;
}

__attribute__((unused)) void nvBegin(bool entering) {
  nvActive = nvPending = true;
  nvIn = entering;
  nvMkKey = -1;
  if (entering) nvBob = (int)lroundf(tri(millis() - ipBobT, 1800) * 6.f);
}

// The transition is over and idle takes the screen: the pin starts at rest. Without a link, idle resumes as after a link drop.
void nvLand(uint32_t now) {
  ipBobT = now;
  if (nvConn) return;                                        // connected: ipFresh gives the parked marker with the START ring grown
  ipFresh = false; ipConn = false; ipPh = IP_ACCEL; ipParkT = 0.f; ipBoost = 1.f; ipRing0 = 0.f;
  ipS = trParkS; ipTrimS = trParkS; ipTrimE = trPinS;        // the road regrows from here while the marker accelerates
  ipLostMs = now ? now : 1; ipLast = now;
}

// Lay the idle dots on the route: resample nvS by arc length, starting nvLead px ahead of the marker (where the first idle dot is).
void nvLayRoute() {
  float qx[NV_RS], qy[NV_RS], qc[NV_RS];
  int qn = 0;
  const bool have = nvS.n >= 2;
  if (have) { qn = nvS.n; for (int i = 0; i < qn; i++) { qx[i] = nvS.rx[i]; qy[i] = nvS.ry[i]; } }
  else      { qx[0] = ANCHOR_X; qy[0] = ANCHOR_Y; qx[1] = ANCHOR_X; qy[1] = ANCHOR_Y - 420.f; qn = 2; }   // no route yet: straight ahead
  qc[0] = 0.f;
  for (int i = 1; i < qn; i++) qc[i] = qc[i - 1] + hypotf(qx[i] - qx[i - 1], qy[i] - qy[i - 1]);
  const float L = qc[qn - 1], s0 = fminf(nvLead, L * .5f);
  int i = 0;
  for (int j = 0; j < nvN; j++) {
    const float s = s0 + (L - s0) * (nvN > 1 ? (float)j / (float)(nvN - 1) : 0.f);
    while (i < qn - 2 && qc[i + 1] < s) i++;
    const float seg = qc[i + 1] - qc[i], u = seg > 1e-3f ? (s - qc[i]) / seg : 0.f;
    nvBX[j] = nvLerp(qx[i], qx[i + 1], u);
    nvBY[j] = nvLerp(qy[i], qy[i + 1], u);
  }
  nvEndOk = have && nvS.endOk;
  const int mode = have ? nvS.nodeMode : 0;
  if (mode == 1 && L > s0 + 1.f) nvNodeF = (nvS.nodeArc - s0) / (L - s0) * (float)(nvN - 1);
  else                           nvNodeF = mode == 2 ? -1.f : 1e9f;
}

struct NvPose { float g, mx, my, mdeg, nx, ny; bool node; };

// p: 0 = the idle road, 1 = the nav route. Swing = rigid rotation about the marker; reshape = dot-wise blend onto the route.
NvPose nvMorph(float p) {
  const float q = easeIO(ramp01(p, 0.f, .62f)), m = easeIO(ramp01(p, .38f, 1.f)), k = 1.f - .2f * q;
  const float phi = -nvDeg0 * q * 0.017453292f, c = cosf(phi), sn = sinf(phi);
  const float px = nvLerp(nvSX, (float)ANCHOR_X, q), py = nvLerp(nvSY, (float)ANCHOR_Y, q);
  for (int i = 0; i < nvN; i++) {
    const float vx = (nvAX[i] - nvSX) * k, vy = (nvAY[i] - nvSY) * k;
    nvPX[i] = nvLerp(px + vx * c - vy * sn, nvBX[i], m);
    nvPY[i] = nvLerp(py + vx * sn + vy * c, nvBY[i], m);
  }
  NvPose o;
  o.g = ramp01(p, .12f, .70f);
  o.mx = nvLerp(px, (float)ANCHOR_X, m); o.my = nvLerp(py, (float)ANCHOR_Y, m);
  o.mdeg = nvLerp(nvDeg0 * (1.f - q), 0.f, m);
  o.node = nvNodeF >= 0.f && nvNodeF <= (float)(nvN - 1);
  int fl = (int)floorf(o.node ? nvNodeF : 0.f);
  if (fl > nvN - 2) fl = nvN - 2;
  const float fr = o.node ? nvNodeF - (float)fl : 0.f;
  o.nx = nvLerp(nvPX[fl], nvPX[fl + 1], fr); o.ny = nvLerp(nvPY[fl], nvPY[fl + 1], fr);
  return o;
}

// The road as it turns into the route: dots swell into dashes, then one line, 10 px up to the node and 6 px after it.
void nvRoute(const NvPose& M) {
  const float fr = easeOut(M.g);
  for (int i = 0; i < nvN; i++) {
    const float w = nvLerp(4.f, ((float)i < nvNodeF) ? 10.f : 6.f, fr);
    const float x = nvPX[i], y = nvPY[i];
    const int r = (int)lroundf(w * .5f);
    if (nvOn(x, y)) R->ovFillCircle((int)lroundf(x), (int)lroundf(y), r < 1 ? 1 : r, M_SET);
    if (i < nvN - 1 && M.g > .02f) {
      float x0 = x, y0 = y, x1 = nvLerp(x, nvPX[i + 1], M.g), y1 = nvLerp(y, nvPY[i + 1], M.g);
      if (clipLine(x0, y0, x1, y1, -8.f, -8.f, SCREEN_W + 7.f, SCREEN_H + 7.f))
        R->ovLine((int)lroundf(x0), (int)lroundf(y0), (int)lroundf(x1), (int)lroundf(y1), (int)lroundf(w), M_SET);
    }
  }
}

// The marker between its two looks: s = 0 idle (black disc, kite), s = 1 nav (ring, halo, smaller disc, chevron).
void emitMorphMarker(float fx, float fy, float deg, float s) {
  const int cx = (int)lroundf(fx), cy = (int)lroundf(fy);
  const int qd = (int)lroundf(deg / 6.f), qs = (int)lroundf(s * 8.f);
  const int key = (qd + 100) * 16 + qs;
  if (key != nvMkKey) {
    nvMkKey = key;
    static const float K0[4][2] = {{0, -10}, {8, 11}, {0, 6}, {-8, 11}};
    static const float K1[4][2] = {{0, -12}, {9, 7}, {0, 1}, {-9, 7}};
    const float a = qd * 6.f * 0.017453292f, ca = cosf(a), sa = sinf(a), t = (float)qs / 8.f;
    int X[4], Y[4];
    for (int i = 0; i < 4; i++) {
      const float kx = nvLerp(K0[i][0], K1[i][0], t), ky = nvLerp(K0[i][1], K1[i][1], t);
      X[i] = lroundf(14 + kx * ca - ky * sa); Y[i] = lroundf(14 + kx * sa + ky * ca);
    }
    spMorph->fillSprite(0);
    spMorph->fillTriangle(X[0], Y[0], X[1], Y[1], X[2], Y[2], 1);
    spMorph->fillTriangle(X[0], Y[0], X[3], Y[3], X[2], Y[2], 1);
    R->markDirtyRect(cx - 14, cy - 14, cx + 13, cy + 13);
  }
  if (s > .15f) { const int rr = (int)lroundf(17.f + 4.5f * s); R->ovCircle(cx, cy, rr, M_SET); R->ovCircle(cx, cy, rr - 1, M_SET); }
  if (s > .01f) nvDisc(cx, cy, 17.f + 2.f * s, M_CLR);
  nvDisc(cx, cy, 17.f - 3.f * s, M_SET);
  R->ovBitmap(cx - 14, cy - 14, 28, 28, ptr(spMorph), M_CLR);
}

__attribute__((unused)) void nvLimitText(int lim) {                                  // same sprite navFrame() uses in steady state
  char ls[8];
  snprintf(ls, sizeof(ls), "%d", lim);
  if (lim >= 100) setText(tLimit, ls, B17, AL_C, 21); else setText(tLimit, ls, B24, AL_C, 24);
}

// Status bar between the idle layout (battery left, Bluetooth + version right) and the nav one (speed | heading | battery).
void nvStatus(const Hud& h, bool conn, float verV, float btV, float batT, float spdV, float dirV) {
  emitBarFrame();
  int a, b;

  char pc[12];
  if (h.battPct < 0) snprintf(pc, sizeof(pc), "--"); else snprintf(pc, sizeof(pc), "%d%%", h.battPct);
  const int rightX = 460 - (32 + 8 + textW(B14, pc, 0));
  const int bx = (int)lroundf(nvLerp(20.f, (float)rightX, batT));
  emitBattery(bx, BOT_Y + 12, h.battPct, h.nowMs);
  setBattText(h);
  tBatt.x = bx + (int)lroundf(nvLerp(42.f, 40.f, batT)); tBatt.y = BOT_Y + 7;
  blit(tBatt, M_SET);

  const char* unitTxt = nvS.imp ? "MPH" : "KM/H";             // speed + unit, then heading (the sprites hold the nav texts)
  const int wSpd = textW(B24, nvS.sp, 0), wUnit = textW(FB11, unitTxt, 1) + 1;
  tSpeed.x = 20; tSpeed.y = BOT_Y + 4;
  tUnit.x = 20 + wSpd + 6; tUnit.y = BOT_Y + 16;
  const int midL = tUnit.x + wUnit;
  if (visWin(20, midL, spdV, a, b)) { blitClip(srcOf(tSpeed), a, b, M_SET); blitClip(srcOf(tUnit), a, b, M_SET); }
  const int wD = textW(FB11, nvS.dir, 2);
  tDir.x = (midL + rightX) / 2 - wD / 2; tDir.y = BOT_Y + 9;
  if (visWin(tDir.x, tDir.x + wD, dirV, a, b)) blitClip(srcOf(tDir), a, b, M_SET);

  char v[24]; snprintf(v, sizeof(v), "V%s", FW_VERSION_STRING);   // Bluetooth group slides right as the version retracts, then shuts
  setText(tBtLbl, "BLUETOOTH", FB11, AL_L, 14, 2);
  setText(tVerLbl, v, FB11, AL_L, 14, 2);
  const int wB = textW(FB11, "BLUETOOTH", 2) + 2, wV = textW(FB11, v, 2) + 2;
  const int verX = 460 - wV, divX = verX - 11;
  const int cxd = (int)lroundf(nvLerp(455.f, (float)(divX - 15), verV));
  const int lend = (int)lroundf(nvLerp(440.f, (float)(divX - 30), verV));
  tBtLbl.x = lend - wB; tBtLbl.y = BOT_Y + 9;
  if (visWin(tBtLbl.x, cxd + 5, btV, a, b)) {
    blitClip(srcOf(tBtLbl), a, b, M_SET);
    if (cxd >= a && cxd < b) emitBtDot(cxd, BOT_Y + 19, conn);
  }
  if (verV > .001f) {
    tVerLbl.x = verX; tVerLbl.y = BOT_Y + 9;
    blitClip(srcOf(tVerLbl), 460 - (int)lroundf(wV * verV), 460, M_SET);
    if (verV > .5f) rectC(divX, BOT_Y + 12, 1, 14, 0, SCREEN_W, M_SET);
  }
}

// One frame. in = idle -> nav. For nav -> idle (in = false) the data comes from nvS, the last steady nav frame.
void nvDraw(const Hud& h, float T, bool in) {
  static const float TI[8] = {0.f, .03f, .06f, .09f, .12f, .15f, .18f, .21f};   // cloud, tree, X, tree, pin, title, hint, pill
  static const float TO[8] = {.60f, .63f, .66f, .69f, .85f, .72f, .76f, .80f};
  const float* t0 = in ? TI : TO;
  auto sv = [&](int i) -> float { const float e = easeOut((T - t0[i]) / .16f); return in ? 1.f - e : e; };   // visible fraction
  const float p = in ? ramp01(T, .10f, .80f) : 1.f - ramp01(T, .18f, .85f);
  int a, b;

  nvLayRoute();
  const NvPose M = nvMorph(p);

  // ---- idle scenery ----
  if (in || nvConn) { Hud hh = h; hh.connected = true; idleTexts(hh); }
  else { setText(tStat, lang::tr(lang::S_DISCONNECTED), B12, AL_C, 17, 2); setText(tHint, lang::tr(lang::S_CONN_LOST), S17, AL_L, 18); }
  if (visWin(276, 332, sv(0), a, b)) blitClip(srcOf(spCloud, 276, 12, 56, 28), a, b, M_SET);
  if (visWin(86, 114, sv(1), a, b))  blitClip(srcOf(spTreeA, 86, 128, 28, 44), a, b, M_SET);
  if (visWin(233, 247, sv(2), a, b)) blitClip(srcOf(spXMark, 233, 125, 14, 14), a, b, M_SET);
  if (visWin(429, 451, sv(3), a, b)) blitClip(srcOf(spTreeB, 429, 186, 22, 36), a, b, M_SET);
  if (visWin(inkX0(tIdleT), inkX1(tIdleT), sv(5), a, b)) blitClip(srcOf(tIdleT), a, b, M_SET);
  if (visWin(inkX0(tHint), inkX1(tHint), sv(6), a, b))   blitClip(srcOf(tHint), a, b, M_SET);
  if (visWin(306, 456, sv(7), a, b)) {
    blitClip(srcOf(spPill, tStat.x, tStat.y, tStat.w, tStat.h), a, b, M_SET);
    blitC(tStat, a, b, M_CLR);
  }

  // ---- trail behind the vehicle (nav) ----
  for (int k = 1; k <= 14; k++) {
    const bool on = in ? T > .80f + .012f * k : T < (14 - k) * .012f + .02f;
    if (on) R->ovFillCircle(ANCHOR_X, ANCHOR_Y + 9 * k, 2, M_SET);
  }

  // ---- road -> route, pin -> destination ring ----
  nvRoute(M);
  const float ex = nvPX[nvN - 1], ey = nvPY[nvN - 1];
  const int   exi = (int)lroundf(ex), eyi = (int)lroundf(ey);
  if (visWin(exi - 17, exi + 17, sv(4), a, b)) {
    const int pinX = exi - 17, pinY = eyi - 46 - (in ? nvBob : 0);
    blitClip(srcOf(spPin, pinX, pinY, 34, 47), a, b, M_SET);
    tPinQ.x = pinX + 5; tPinQ.y = pinY + 4;
    blitC(tPinQ, a, b, M_CLR);
  }
  float rs = in ? easeOut(ramp01(T, .24f, .40f)) : 1.f - easeOut(ramp01(T, .70f, .85f));
  if (!nvEndOk) rs *= 1.f - ramp01(p, .6f, 1.f);             // the real end is off screen: the ring leaves with the route
  if (rs > .01f && nvOn(ex, ey)) {
    nvDisc(exi, eyi, 18.f * rs, M_SET); nvDisc(exi, eyi, 13.f * rs, M_CLR); nvDisc(exi, eyi, 7.f * rs, M_SET);
  }
  const float ns = easeOut(ramp01(p, .85f, 1.f));
  if (M.node && ns > .01f && nvOn(M.nx, M.ny)) {
    const int nx = (int)lroundf(M.nx), ny = (int)lroundf(M.ny);
    nvDisc(nx, ny, 12.f * ns, M_SET); nvDisc(nx, ny, 8.f * ns, M_CLR); nvDisc(nx, ny, 3.f * ns, M_SET);
  }

  // ---- marker ----
  const float ms = easeOut(ramp01(p, .72f, 1.f));
  if (!in && !nvConn && ms < .5f) emitTrailMarkerHollow((int)lroundf(M.mx), (int)lroundf(M.my));
  else                            emitMorphMarker(M.mx, M.my, M.mdeg, ms);

  // ---- START ring ----
  float rr = 0.f; bool lab = false;
  if (in) { const float c1 = easeOut(ramp01(T, 0.f, .2f)); rr = 26.f * (1.f - c1); lab = c1 < .3f; }
  else if (nvConn && T >= 1.f) { const float e2 = easeOut(ramp01(T, 1.f, 1.25f)); rr = 18.f + 8.f * e2; lab = e2 >= .5f; }
  if (rr >= 3.f) {
    const int ri = (int)lroundf(rr), cx = (int)lroundf(nvSX), cy = (int)lroundf(nvSY);
    R->ovCircle(cx, cy, ri, M_SET); R->ovCircle(cx, cy, ri - 1, M_SET); R->ovCircle(cx, cy, ri - 2, M_SET);
    if (lab) blit(tStartL, M_SET);
  }

  // ---- speed limit sign ----
  const float lv = in ? easeOut(ramp01(T, 1.05f, 1.28f)) : 1.f - easeOut(ramp01(T, 0.f, .18f));
  if (nvS.limit > 0 && lv > .01f) {
    const int lx = SCREEN_W - 14 - 34, ly = BOT_Y - 8 - 34;
    nvDisc(lx, ly, 36.f * lv, M_CLR); nvDisc(lx, ly, 33.f * lv, M_SET); nvDisc(lx, ly, 25.f * lv, M_CLR);
    tLimit.x = lx - tLimit.w / 2; tLimit.y = ly - tLimit.h / 2;
    if (visWin(lx - 32, lx + 32, lv, a, b)) blitClip(srcOf(tLimit), a, b, M_SET);
  }

  // ---- banner ----
  int oy, ix; float vI, vD, vS, vC, vR;
  if (in) {
    oy = (int)lroundf(-BAN_H * (1.f - easeOut(ramp01(T, .45f, .80f))));
    ix = (int)lroundf(16.f - 96.f * (1.f - easeOut(ramp01(T, .70f, .95f))));
    vI = 1.f;
    vD = easeOut(ramp01(T, .80f, 1.f));   vS = easeOut(ramp01(T, .88f, 1.08f));
    vC = easeOut(ramp01(T, 1.f, 1.18f));  vR = easeOut(ramp01(T, .95f, 1.2f));
  } else {
    oy = (int)lroundf(-BAN_H * easeIO(ramp01(T, .22f, .52f)));
    ix = 16;
    vI = 1.f - easeOut(ramp01(T, .12f, .28f)); vD = 1.f - easeOut(ramp01(T, .08f, .26f));
    vS = 1.f - easeOut(ramp01(T, .04f, .22f)); vC = 1.f - easeOut(ramp01(T, 0.f, .18f));
    vR = 1.f - easeOut(ramp01(T, .10f, .26f));
  }
  if (oy > -BAN_H) {
    R->ovFillRect(0, 0, SCREEN_W, oy + BAN_H, M_SET);
    TFT_eSprite* ics = nvS.bmode == 2 ? spIcon[10] : (nvS.bmode == 1 ? spSpin[0] : spIcon[nvS.icon]);
    if (visWin(16, 16 + IC, vI, a, b)) blitClip(srcOf(ics, ix, 12, IC, IC), a, b, M_CLR, oy);
    const TextSpr& big = nvS.bmode ? tBig : tDist;
    if (visWin(inkX0(big), inkX1(big), vD, a, b))       blitClip(srcOf(big), a, b, M_CLR, oy);
    if (visWin(inkX0(tStreet), inkX1(tStreet), vS, a, b)) blitClip(srcOf(tStreet), a, b, M_CLR, oy);
    if (visWin(416, 460, vC, a, b))
      blitClip(srcOf(nvS.bmode == 2 ? spCheck : spCompass, 416, 20, 44, 44), a, b, M_CLR, oy);
    if (visWin(16, SCREEN_W - 16, vR, a, b)) {
      rectY(16, BAN_H - 8 + oy, SCREEN_W - 32, 1, a, b, M_CLR);
      const int fw = (int)lroundf((SCREEN_W - 32) * nvS.bar);
      if (fw > 0) rectY(16, BAN_H - 9 + oy, fw, 3, a, b, M_CLR);
    }
  }

  // ---- status bar ----
  if (in) nvStatus(h, true, 1.f - easeOut(ramp01(T, 0.f, .25f)), 1.f - easeOut(ramp01(T, .18f, .40f)),
                   easeIO(ramp01(T, .30f, .65f)), easeOut(ramp01(T, .58f, .80f)), easeOut(ramp01(T, .66f, .88f)));
  else    nvStatus(h, nvConn, easeOut(ramp01(T, .75f, 1.f)), easeOut(ramp01(T, .55f, .80f)),
                   1.f - easeIO(ramp01(T, .30f, .65f)), 1.f - easeOut(ramp01(T, 0.f, .18f)), 1.f - easeOut(ramp01(T, 0.f, .18f)));
}

}  // namespace

// =============================================================================================
// Boot
// =============================================================================================
// =============================================================================================
// Setup screen (first boot: no phone bonded yet)
// Left: step 1, a QR code that opens the app's download page. Right: step 2, the device name to pick in the app (black banner,
// like the turn banner) and step 3, the pairing code that will appear on this screen. The rest of the setup runs on the phone.
// Layout (480x320, status bar below y = 284):
//   header      logo + CubiNav (same as pairing), tag SETUP
//   x 30..190   [1] GET THE APP, QR 125 px (5 px modules) at (48, 98); 20 px clear around it, as a scanner wants
//   x 213       dotted road between the columns
//   x 232..460  [2] PICK THIS DEVICE, banner (232, 92, 228x52) with the name knocked out, hint; [3] ENTER THE CODE, hint
// Everything static is drawn into BASE once (setupBuild); per frame there is only the pulse ring next to the name and the status bar.
// =============================================================================================
const int      SU_QR_X = 48, SU_QR_Y = 98, SU_QR_PX = 5;
const int      SU_BAN_X = 232, SU_BAN_Y = 92, SU_BAN_W = 228, SU_BAN_H = 52;
const int      SU_DOT_X = SU_BAN_X + 26, SU_DOT_Y = SU_BAN_Y + SU_BAN_H / 2;   // Bluetooth dot in the banner
const uint32_t SU_PULSE_MS = 1600;     // ring around the dot: grows from r 7 to 20 in 90 % of this, then rests
bool           suBuilt = false, waBuilt = false;

// "Continue your setup" screen (paired, setup not finished). Layout from CubiNav_Continue_Setup_Screen.html (480x320):
//   header   logo + CubiNav + tag SETUP (same as setup/pairing), 2 px rule at y 56 from x 20 to 460
//   left     app icon: 112 px rounded square (radius 26) at (54, 115), 60 px kite knocked out of it.
//            With setup_mascot_start.h / setup_mascot_loop.h present it is replaced by the dithered mascot animation (same box)
//   right    x 194: headline in 2 lines (X36 bold, baselines y 147 and 185), then the app line (S19, baseline y 222)
// All static: drawn into BASE once (waitBuild); per frame only the status bar.
const int WA_ICON = 112, WA_ICON_X = 54, WA_ICON_Y = 115, WA_ICON_R = 26, WA_KITE = 60;
#ifndef SETUP_MASCOT_SIZE
#define SETUP_MASCOT_SIZE WA_ICON       // output size in px, multiple of 8. 112 = the icon box (64 px frames at x1.75); 128 = exact x2, centred on the same spot
#endif
const int WA_TX = 194, WA_TW = 272;                       // text column (ES line 2 is 262 px wide in X36)
const int WA_H1_Y = 115, WA_H2_Y = 153, WA_H_H = 44, WA_H_BASE = 32;   // headline: sprites 38 px apart, baseline 32 px below the sprite top
const int WA_L3_Y = 203, WA_L3_H = 28, WA_L3_BASE = 19;

static void buildSetup() {
  mkSprite(spQr, QR_APP_N * SU_QR_PX, QR_APP_N * SU_QR_PX);
  if (uint8_t* p = rawBits(spQr)) {
    const int w = QR_APP_N * SU_QR_PX;
    for (int my = 0; my < QR_APP_N; my++)
      for (int mx = 0; mx < QR_APP_N; mx++)
        if ((QR_APP_ROWS[my] >> (QR_APP_N - 1 - mx)) & 1)
          for (int dy = 0; dy < SU_QR_PX; dy++)
            for (int dx = 0; dx < SU_QR_PX; dx++) setBit(p, w, w, mx * SU_QR_PX + dx, my * SU_QR_PX + dy);
  }
  mk(tSTag, 340, 22, 120, 14);
  mk(tSN1, 30, 60, 22, 22);  mk(tSN2, 232, 60, 22, 22);  mk(tSN3, 232, 190, 22, 22);
  mk(tSL1, 62, 60, 140, 22); mk(tSL2, 264, 60, 196, 22); mk(tSL3, 264, 190, 196, 22);
  mk(tSH1, 232, 156, 228, 24);
  mk(tSH2, 232, 220, 228, 20);
  mk(tName, SU_BAN_X + 50, SU_BAN_Y, SU_BAN_W - 58, SU_BAN_H);
  mk(tWA1, WA_TX, WA_H1_Y, WA_TW, WA_H_H); mk(tWA2, WA_TX, WA_H2_Y, WA_TW, WA_H_H); mk(tWA3, WA_TX, WA_L3_Y, WA_TW, WA_L3_H);
#if HAVE_SETUP_MASCOT
  maBuf = (uint8_t*)malloc(SETUP_MASCOT_SIZE / 8 * SETUP_MASCOT_SIZE);   // 1.5 KB at 112 px; if it fails the mascot is simply not drawn
#else
  mkSprite(spAppIcon, WA_ICON, WA_ICON);                         // same kite as buildLogo(), on a rounded square
  spAppIcon->fillRoundRect(0, 0, WA_ICON, WA_ICON, WA_ICON_R, 1);
  {
    const float k = WA_KITE / 24.f;                              // kite drawn in a 24 x 24 box: (12,2) (21,21) (12,16.5) (3,21)
    const float o = (WA_ICON - WA_KITE) / 2.f;
    auto P = [&](float x, float y, int& ox, int& oy) { ox = lroundf(o + x * k); oy = lroundf(o + y * k); };
    int ax, ay, bx, by, cx, cy, dx, dy;
    P(12, 2, ax, ay); P(21, 21, bx, by); P(12, 16.5f, cx, cy); P(3, 21, dx, dy);
    spAppIcon->fillTriangle(ax, ay, bx, by, cx, cy, 0);
    spAppIcon->fillTriangle(ax, ay, cx, cy, dx, dy, 0);
  }
#endif
  setText(tSTag, lang::tr(lang::S_SU_TAG), FB11, AL_R, 11, 2, 2);
  setText(tSN1, "1", B14, AL_C, 16);
  setText(tSN2, "2", B14, AL_C, 16);
  setText(tSN3, "3", B14, AL_C, 16);
  setText(tSL1, lang::tr(lang::S_SU_L1), B12, AL_L, 15, 2);
  setText(tSL2, lang::tr(lang::S_SU_L2), B12, AL_L, 15, 2);
  setText(tSL3, lang::tr(lang::S_SU_L3), B12, AL_L, 15, 2);
  setFit(tSH1, lang::tr(lang::S_SU_H1), S17, M15, 18);
  setFit(tSH2, lang::tr(lang::S_SU_H2), M15, M14, 16);
  setText(tName, devname::get(), BN24, AL_L, 35);                // "CubiNav-A4F2": see device_name.h
}

static inline void baseSpr(const TextSpr& t, Mode m) { R->baseBitmap(t.x, t.y, t.w, t.h, ptr(t.sp), m); }

static void setupBuild() {
  R->baseBitmap(20, 16, LOGO_SMALL, LOGO_SMALL, ptr(spLogoSmall), M_SET);
  baseSpr(tPTitle, M_SET);
  baseSpr(tSTag, M_SET);
  const TextSpr* num[3] = {&tSN1, &tSN2, &tSN3};
  const TextSpr* lbl[3] = {&tSL1, &tSL2, &tSL3};
  for (int i = 0; i < 3; i++) {                                  // numbered black discs with the digit knocked out
    R->baseFillCircle(num[i]->x + 11, num[i]->y + 11, 11, M_SET);
    baseSpr(*num[i], M_CLR);
    baseSpr(*lbl[i], M_SET);
  }
  R->baseBitmap(SU_QR_X, SU_QR_Y, QR_APP_N * SU_QR_PX, QR_APP_N * SU_QR_PX, ptr(spQr), M_SET);
  for (int k = 0; k < 16; k++) R->baseFillCircle(213, 68 + 12 * k, 2, M_SET);   // the road between the columns
  R->baseFillRect(SU_BAN_X, SU_BAN_Y, SU_BAN_W, SU_BAN_H, M_SET);
  R->baseFillCircle(SU_DOT_X, SU_DOT_Y, 5, M_CLR);               // Bluetooth dot, knocked out of the banner
  baseSpr(tName, M_CLR);
  baseSpr(tSH1, M_SET);
  baseSpr(tSH2, M_SET);
}

#if HAVE_SETUP_MASCOT
// ---- Mascot player. Frames are decoded + dithered into maBuf and drawn into BASE; the renderer diffs against the panel, so only
// the pixels that changed go over SPI. Plays "start" once, then "loop" forever. The last start frame and the last loop frame are
// both identical to the first loop frame, so after start the loop continues at frame 1 and wraps from its last frame to frame 1.
static_assert(SETUP_MASCOT_SIZE % 8 == 0 && SETUP_MASCOT_SIZE >= setupmascot::SRC, "SETUP_MASCOT_SIZE: multiple of 8, not smaller than the 64 px frames");
const int MA_S = SETUP_MASCOT_SIZE;
const int MA_X = WA_ICON_X + (WA_ICON - MA_S) / 2, MA_Y = WA_ICON_Y + (WA_ICON - MA_S) / 2;   // centred on the icon box
uint8_t   maPhase = 0;                  // 0 = start, 1 = loop
int       maFrame = -1;                 // frame on screen, -1 = none yet
uint32_t  maNext = 0;                   // when the next frame is due; 0 = clock not started

static void maDraw(uint8_t phase, int f) {
  if (!maBuf) return;
  const uint8_t* rle = phase == 0 ? MASCOT_START_DATA + pgm_read_dword(&MASCOT_START_OFFSETS[f])
                                  : MASCOT_LOOP_DATA  + pgm_read_dword(&MASCOT_LOOP_OFFSETS[f]);
  bootalpha::decodeSized(rle, setupmascot::SRC, setupmascot::SRC, MA_S, MA_S, maBuf);
  R->baseFillRect(MA_X, MA_Y, MA_S, MA_S, M_CLR);                // frames are transparent: clear, then SET
  R->baseBitmap(MA_X, MA_Y, MA_S, MA_S, maBuf, M_SET);
  maPhase = phase; maFrame = f;
}
static void maReset() {                 // BASE was just cleared (enter()): show the first frame again
  maNext = 0;
#if UI_ANIMATIONS
  maDraw(0, 0);                         // start frame 0 is empty: the mascot appears once the screen reveal is over
#else
  maDraw(1, 1);                         // no animations: a still pose (same picture as the end of "start")
#endif
}
static void maTick(uint32_t now) {
  const uint32_t per = 1000u / (maPhase == 0 ? setupmascot::START_FPS : setupmascot::LOOP_FPS);
  if (!maNext) { maNext = now ? now : 1; return; }
  if (now - maNext < per) return;
  uint8_t ph = maPhase;
  int f = maFrame + 1;
  if (ph == 0 && f >= setupmascot::START_N) { ph = 1; f = 1; }
  else if (ph == 1 && f >= setupmascot::LOOP_N) f = 1;
  maDraw(ph, f);
  maNext = (now - maNext >= 2 * per) ? now : maNext + per;       // after a stall resume at normal speed, no burst
}
#endif

static void waitBuild() {
  R->baseBitmap(20, 16, LOGO_SMALL, LOGO_SMALL, ptr(spLogoSmall), M_SET);
  baseSpr(tPTitle, M_SET);
  baseSpr(tSTag, M_SET);
  R->baseFillRect(20, 56, 440, 2, M_SET);                        // header rule
#if HAVE_SETUP_MASCOT
  maReset();
#else
  R->baseBitmap(WA_ICON_X, WA_ICON_Y, WA_ICON, WA_ICON, ptr(spAppIcon), M_SET);
#endif
  setText(tWA1, lang::tr(lang::S_SETUP_L1), X36, AL_L, WA_H_BASE, -1);     // X36: full-ASCII bold, so every language works
  setText(tWA2, lang::tr(lang::S_SETUP_L2), X36, AL_L, WA_H_BASE, -1);
  setText(tWA3, lang::tr(lang::S_SETUP_WAIT), S19, AL_L, WA_L3_BASE);      // S_SETUP_WAIT now holds "on the CubiNav Connect app"
  baseSpr(tWA1, M_SET); baseSpr(tWA2, M_SET); baseSpr(tWA3, M_SET);
}

void waitSetupFrame(const Hud& h) {
  R->overlayBegin();
  clipBegin();
  if (!waBuilt) { waBuilt = true; waitBuild(); }
#if HAVE_SETUP_MASCOT && UI_ANIMATIONS
  if (!transitionBusy(h.nowMs)) maTick(h.nowMs);                 // not during the reveal (same rule as the pulse ring on the setup screen)
#endif
  emitStatusBar(h, true);
  emitFps(h, h.showFps, 140, 264, false);
  emitReveal(h.nowMs);
}

void setupFrame(const Hud& h) {
  const uint32_t now = h.nowMs;
  R->overlayBegin();
  clipBegin();
  if (!suBuilt) { suBuilt = true; setupBuild(); }
#if UI_ANIMATIONS
  // Pulse ring (like the START ring on idle). Not during the reveal: ovRestore only works if nothing is drawn into the old screen.
  if (!transitionBusy(now)) {
    const float p = (now % SU_PULSE_MS) / (float)SU_PULSE_MS;
    if (p < 0.9f) {
      const int r = 7 + (int)lroundf(13.f * easeOut(p / 0.9f));
      R->ovCircle(SU_DOT_X, SU_DOT_Y, r, M_CLR);
      R->ovCircle(SU_DOT_X, SU_DOT_Y, r - 1, M_CLR);
    }
  }
#endif
  emitStatusBar(h, true);
  emitFps(h, h.showFps, 140, 264, false);
  emitReveal(now);
}

void initBoot(TFT_eSPI* t, Renderer* r) {
  tft = t; R = r;
  buildLogo(spLogoBig, LOGO_BIG);
  mk(tBootT, 0, 150, SCREEN_W, 56);
  mk(tBootL, 0, 256, SCREEN_W, 18);
  mk(tBootV, 0, 292, SCREEN_W, 18);
  setText(tBootT, "CubiNav", B42, AL_C, 40, -1);
  char v[24]; snprintf(v, sizeof(v), "V%s", FW_VERSION_STRING);
  setText(tBootV, v, FB11, AL_C, 13, 2);
}

// ---- Boot animation (frames from boot_frames.h, made by png2frames.py) ---------------------------
// Format 0: 1-bpp transparent frames, nearest-neighbour upscaled by BOOT_ANIM_SCALE into a RAM buffer and drawn into BASE.
// Format 1: 4-bit darkness RLE frames, upscaled and Bayer-dithered at output resolution (boot_alpha.h).
// The renderer diffs against what the panel shows, so only the pixels that changed between frames go over SPI.
#if HAVE_BOOT_ANIM
namespace {
constexpr int BA_S = BOOT_ANIM_SCALE;
constexpr int BA_W = BOOT_FRAME_W * BA_S, BA_H = BOOT_FRAME_H * BA_S;
constexpr int BA_STRIDE = BA_W / 8;
constexpr int BA_X = (SCREEN_W - BA_W) / 2, BA_Y = (SCREEN_H - BA_H) / 2;   // centred: the animation IS the boot screen
static_assert(BA_W % 8 == 0, "BOOT_FRAME_W * BOOT_ANIM_SCALE must be a multiple of 8");

uint8_t* baBuf = nullptr;
int      baShown = -1;
uint32_t baNext = 0;     // when the next frame is due; 0 = clock not started yet

void baDraw(int f) {
  if (!baBuf) return;
#if BOOT_FRAME_FORMAT == 1
  bootalpha::decode(BOOT_ALPHA_DATA + pgm_read_dword(&BOOT_ALPHA_OFFSETS[f]), BOOT_FRAME_W, BOOT_FRAME_H, BA_S, baBuf);
#else
  const uint8_t* fr = BOOT_FRAMES[f];
  const int sb = BOOT_FRAME_W / 8;
  for (int y = 0; y < BOOT_FRAME_H; y++) {
    uint8_t* row = baBuf + y * BA_S * BA_STRIDE;
    memset(row, 0, BA_STRIDE);
    for (int x = 0; x < BOOT_FRAME_W; x++)
      if (fr[y * sb + (x >> 3)] & (0x80 >> (x & 7)))
        for (int k = 0; k < BA_S; k++) { int bx = x * BA_S + k; row[bx >> 3] |= (uint8_t)(0x80 >> (bx & 7)); }
    for (int r = 1; r < BA_S; r++) memcpy(row + r * BA_STRIDE, row, BA_STRIDE);
  }
#endif
  R->baseFillRect(BA_X, BA_Y, BA_W, BA_H, M_CLR);        // frames are transparent: clear, then SET
  R->baseBitmap(BA_X, BA_Y, BA_W, BA_H, baBuf, M_SET);
  baShown = f;
}
}  // namespace
#endif

void bootAnimTick(uint32_t now) {
#if HAVE_BOOT_ANIM
  if (baShown < 0 || baShown >= BOOT_FRAME_COUNT - 1) return;   // play once, hold the last frame
  const uint32_t per = 1000 / BOOT_FRAME_FPS;
  if (!baNext) { baNext = now ? now : 1; return; }              // frame 0 stays up until the first tick
  if (now - baNext < per) return;
  baDraw(baShown + 1);                                          // never skip a frame, even after a blocking init
  baNext = (now - baNext >= 2 * per) ? now : baNext + per;      // after a stall resume at normal speed, no burst
#else
  (void)now;
#endif
}

bool bootAnimDone() {
#if HAVE_BOOT_ANIM
  return baShown < 0 || baShown >= BOOT_FRAME_COUNT - 1;
#else
  return true;
#endif
}

void initRest() {
  buildLogo(spLogoSmall, LOGO_SMALL);
  // idle
  mk(tIdleT, 28, 12, 230, 52);                               // idle: title, subtitle, status pill (see "Idle: the trail")
  mk(tStat, 306, 237, 150, 26);
  mk(tHint, 28, 60, 340, 24);
  setText(tIdleT, "CubiNav", B42, AL_L, 40, -1);
  buildTrail();
  buildIdleArt();
  nvBuildDots();                                             // idle <-> nav transition: the road's dots START -> pin
  // pairing
  mk(tPTitle, 54, 16, 100, 26);
  mk(tPTag, 340, 22, 120, 14);
  mk(tPLbl, 0, 68, SCREEN_W, 20);
  mk(tPDig, 56, 108, 368, 68);
  mk(tPH1, 0, 206, SCREEN_W, 24);
  mk(tPH2, 0, 234, SCREEN_W, 20);
  setText(tPTitle, "CubiNav", B17, AL_L, 19);
  setText(tPTag, lang::tr(lang::S_PAIR_TAG), FB11, AL_R, 11, 2, 2);
  setText(tPLbl, lang::tr(lang::S_PAIR_LABEL), B12, AL_C, 14, 2);
  // status bar
  mk(tBatt, 62, BOT_Y + 7, 56, 22);
  mk(tBtLbl, 0, BOT_Y + 9, 100, 16);
  mk(tVerLbl, 0, BOT_Y + 9, 72, 16);
  mk(tFps, 0, 0, 160, 18); mkHalo(tFps);
  // navigation
  mk(tDist, 104, 4, 296, 62);
  mk(tStreet, 104, 64, 296, 26);
  mk(tBig, 104, 6, 296, 58);
  mk(tSpeed, 20, 288, 70, 30);
  mk(tUnit, 0, 300, 52, 16);
  mk(tDir, 0, 293, 36, 16);
  mk(tPill, 0, 100, 160, 18);
  mk(tLimit, 0, 0, 64, 30);                                  // speed limit sign text
  setText(tPill, lang::tr(lang::S_NO_GPS), FB11, AL_L, 14, 2);
  // firmware update
  mk(tUpLbl, 0, 20, SCREEN_W, 20);
  mk(tUpPct, 60, 52, 360, 88);
  mk(tUpKB, 50, 212, 200, 20);
  mk(tUpVer, 250, 214, 180, 18);
  mk(tUpHint, 0, 244, 360, 22);
  mk(tFailT, 0, 146, SCREEN_W, 44);
  mk(tFailE, 0, 196, SCREEN_W, 26);
  mk(tFailH, 0, 232, SCREEN_W, 20);
  setText(tUpLbl, lang::tr(lang::S_FW_UPDATE), B12, AL_C, 14, 2);
  setText(tFailT, lang::tr(lang::S_UPD_FAILED), X36, AL_C, 33, -1);
  buildIcons();
  buildMisc();
  buildSetup();
}

// Boot is over: give the 8 KB animation buffer back. Left allocated, it fragments the heap and the firmware update (Update.begin needs a
// contiguous block) fails with "malloc failed".
void bootEnd() {
#if HAVE_BOOT_ANIM
  free(baBuf); baBuf = nullptr;
#endif
}

void bootDraw() {
  R->baseClear(M_CLR);
#if HAVE_BOOT_ANIM
  baBuf = (uint8_t*)malloc(BA_STRIDE * BA_H);                          // the animation replaces the whole loading screen
  baNext = 0;                                                          // clock starts at the first tick, after the first flush
  baDraw(0);
  return;                                                              // no logo, title, version, label or bar
#else
  R->baseBitmap(196, 52, LOGO_BIG, LOGO_BIG, ptr(spLogoBig), M_SET);
  R->baseBitmap(tBootT.x, tBootT.y, tBootT.w, tBootT.h, ptr(tBootT.sp), M_SET);
#endif
  R->baseBitmap(tBootV.x, tBootV.y, tBootV.w, tBootV.h, ptr(tBootV.sp), M_SET);
  R->baseFillRect(BAR_X, BAR_Y + 2, BAR_W, 2, M_SET);                  // thin rule; the fill grows over it
}

void bootLabel(const char* s) {
#if HAVE_BOOT_ANIM
  (void)s; return;
#endif
  char u[48]; size_t n = 0;
  for (; s[n] && n < sizeof(u) - 1; n++) u[n] = (s[n] >= 'a' && s[n] <= 'z') ? (char)(s[n] - 32) : s[n];
  u[n] = 0;
  if (!setText(tBootL, u, FB11, AL_C, 13, 2)) return;
  R->baseFillRect(tBootL.x, tBootL.y, tBootL.w, tBootL.h, M_CLR);
  R->baseBitmap(tBootL.x, tBootL.y, tBootL.w, tBootL.h, ptr(tBootL.sp), M_SET);
}

void bootBar(int pct) {
#if HAVE_BOOT_ANIM
  (void)pct; return;
#endif
  if (pct > 100) pct = 100;
  int fw = BAR_W * pct / 100;
  if (fw > 0) R->baseFillRect(BAR_X, BAR_Y, fw, 6, M_SET);
}

// =============================================================================================
// Runtime screens
// =============================================================================================
void enter(Screen s) {
  static uint8_t prev = 255;                                // screen shown before this call
  const bool fromIdle = prev == SCR_IDLE, fromUpdate = prev == SCR_UPDATE, fromNav = prev == SCR_NAV;
  const bool navSteady = nvS.valid;                         // the last nav frame was a complete one (not the entry transition)
  nvS.valid = false;
  prev = s;
  const bool otWasRunning = otActive;                       // still building the update screen
  const bool erWasRunning = erActive;                       // still building the failure screen
  (void)fromUpdate; (void)otWasRunning; (void)erWasRunning; (void)fromNav; (void)navSteady;
  R->baseClear(M_CLR);
  keyValid = false;
  turnOk = false; turnWait = false; odoOk = false;
  compQ = chevQ = -999;
  otActive = otPending = false;
  nvActive = nvPending = false;
  cxActive = cxPending = cxFail = false;
  erActive = erPending = false;
#if UI_ANIMATIONS && UI_OTA_TRANSITION
  // Idle -> update: the road turns into the ruler (needs the idle state, so before ipFresh is reset). Not from the pairing screen.
  if (s == SCR_UPDATE && fromIdle && !ipFresh && ptMode == PTR_NONE && !ptPrev) otBegin();
#else
  (void)fromIdle;
#endif
#if UI_ANIMATIONS && UI_OTA_CANCEL_TRANSITION
  // Update -> idle after a cancel: drain, hold, sweep (see "Update cancelled -> idle"). Needs the last live update frame.
  if (s == SCR_IDLE && fromUpdate && upLive && !otWasRunning) cxActive = cxPending = true;
#endif
#if UI_ANIMATIONS && UI_OTA_FAIL_TRANSITION
  // Failure screen -> idle: it closes, then the damaged marker sweeps the idle screen back in.
  if (s == SCR_IDLE && fromUpdate && upFailed && !erWasRunning) { cxActive = cxPending = true; cxFail = true; }
#endif
#if UI_ANIMATIONS && UI_NAV_TRANSITION
  // Idle -> nav: the road becomes the route. Only from a settled idle screen (not fresh, not pairing), else the curtain wipe.
  if (s == SCR_NAV && fromIdle && !ipFresh && ptMode == PTR_NONE && !ptPrev) nvBegin(true);
  // Nav -> idle: the route lets go of the screen. Needs the last steady nav frame (nvS).
  if (s == SCR_IDLE && fromNav && navSteady) nvBegin(false);
#endif
  upLive = upShown = upSteady = upFailed = false;
  ipFresh = true;                                           // idle: no transition replay when the screen is (re)entered
  ptFresh = true; ptMode = PTR_NONE;                         // same for the idle <-> pairing transition
  suBuilt = false; waBuilt = false;                                           // setup: BASE is rebuilt on the first frame
  revealPending = !otActive && !cxActive && !nvActive;       // the OTA, cancel and nav transitions replace the interlock reveal
  if (s == SCR_UPDATE && !otActive) updateRuler();           // static ruler under the progress bar
}

bool transitionBusy(uint32_t nowMs) {
  return otActive || cxActive || nvActive || revealPending || trElapsed(nowMs, revealStart, (float)UI_REVEAL_SPEED_DIV) < REVEAL_MS;
}

// Current screen -> blank, blocking (about 0.5 s). Call right before a restart. BASE is cleared (the blank screen) and the interlock
// reveal runs over it: the part it has not reached yet keeps showing what is on the panel (ovRestore), so no screen content is needed.
// Only from the task that owns the renderer (loop()). Runs once.
void shutdownTransition() {
#if UI_ANIMATIONS
  static bool done = false;
  if (done || !R) return;
  done = true;
  R->baseClear(M_CLR);
  revealPending = true;                                      // emitReveal() starts the clock on its first call
  const float div = (float)UI_SHUTDOWN_SPEED_DIV;
  for (;;) {
    const uint32_t now = millis();
    R->overlayBegin();
    emitReveal(now, div);
    R->flush();
    if (trElapsed(now, revealStart, div) >= REVEAL_MS) break;   // this last frame was the blank screen
    delay(5);
  }
#endif
}

// ---- Idle / pairing -------------------------------------------------------------------
void idleFrame(const Hud& h) {
  const uint32_t now = h.nowMs;

  R->overlayBegin();
  clipBegin();

  if (h.pairing) snprintf(ptCode, sizeof(ptCode), "%06lu", (unsigned long)(h.passkey % 1000000UL));
  float cxT = 0.f;
#if UI_ANIMATIONS && UI_OTA_CANCEL_TRANSITION
  if (cxActive) {
    if (cxPending) { cxPending = false; cxStart = now; idleReset(now); }   // idle restarts from a calm SEARCHING behind the bar
    cxT = (float)trElapsed(now, cxStart, cxFail ? (float)UI_OTA_FAIL_EXIT_SPEED_DIV : (float)UI_OTA_CANCEL_SPEED_DIV) * 1e-3f;
    if (cxT >= cxEnd()) { cxActive = false; ipBobT = now; }                  // idle takes over, the pin starts at rest
  }
#endif
  float nvT = 0.f;
#if UI_ANIMATIONS && UI_NAV_TRANSITION
  if (nvActive && !nvIn) {                                   // nav -> idle: the route lets go of the screen
    if (nvPending) { nvPending = false; nvStart = now; nvConn = h.connected; }
    nvT = (float)trElapsed(now, nvStart, (float)UI_NAV_EXIT_SPEED_DIV) * 1e-3f;
    if (nvT >= NV_END) { nvActive = false; nvLand(now); }    // idle takes over this very frame
  }
#endif
  if (!cxActive && !nvActive) pairTrUpdate(h);

  if (nvActive && !nvIn) {
    ipLast = now;                                            // the idle sim is frozen during a transition
    nvDraw(h, nvT, false);
  } else if (cxActive) {
    ipLast = now;                                            // the idle sim is frozen during a transition
    cxDraw(h, cxT);
  } else if (ptMode == PTR_IN) {
    ipLast = now;                                            // the idle sim is frozen during a transition
    transIn(h, trElapsed(now, ptStart, (float)UI_PAIR_SPEED_DIV));
  } else if (ptMode == PTR_OUT) {
    ipLast = now;
    transOut(h, trElapsed(now, ptStart, (float)UI_PAIR_SPEED_DIV));
  } else if (h.pairing) {
    pairDraw(ptCode, 99.f, false, 0, SCREEN_W);              // steady pairing screen
    emitStatusBar(h, true);
  } else {
    idleTexts(h);                                            // before idleUpdate(): see idleLostText()
    idleUpdate(h);
    idleScene(h, 0, SCREEN_W, true);
    emitStatusBar(h, true);
  }
  emitFps(h, h.showFps, 140, 264, false);
  emitReveal(now);
}

// ---- Navigation -----------------------------------------------------------------------------
void navFrame(NavModel& m, const Hud& h) {
  const NavConfig& cfg = m.cfg;
  const uint32_t now = h.nowMs;
  const bool  fix   = m.pose.hasFix();
  const bool  stale = m.pose.stale();
  const float scale = cfg.zoom / 100.0f;                    // px per metre
  const float theta = cfg.headingUp ? m.pose.heading() : 0.f;
  const bool  arrived   = m.navState == proto::NAV_ARRIVED;
  const bool  rerouting = !arrived && m.navState == proto::NAV_REROUTING;
  const bool  nogps     = !fix || stale || m.navState == proto::NAV_NO_GPS;

  // ---- texts (re-rendered only on change) ----
  char num[16], unit[16], key[40];
  fmtDist(m.distToTurn, cfg.imperial, num, unit, sizeof(num));
  snprintf(key, sizeof(key), "%s|%s", num, unit);
  if (begin(tDist, key)) {
    if (num[0]) {
      int x = put(tDist, D60, num, 0, 56, -2);
      put(tDist, U24, unit, x + 6, 56, 0);
    }
    finish(tDist);
  }
  if (rerouting)    setText(tStreet, lang::tr(lang::S_REROUTING), S19, AL_L, 17);
  else              setFit(tStreet, m.street, S19, M15, 17);
  if (rerouting) {
#if UI_ANIMATIONS
    static const char* DOTS[4] = {"", ".", "..", "..."};
    snprintf(key, sizeof(key), "%s%s", lang::tr(lang::S_REROUTE_BIG), lang::get() == lang::EN ? DOTS[(now / 350) & 3] : "");   // ES word is too wide for the dots
#else
    snprintf(key, sizeof(key), "%s%s", lang::tr(lang::S_REROUTE_BIG), lang::get() == lang::EN ? "..." : "");
#endif
    setText(tBig, key, B46, AL_L, 44, -1);
  } else if (arrived) {
    setText(tBig, lang::tr(lang::S_ARRIVED), B56, AL_L, 50, -2);
  }

  float kmh = m.pose.speedMs() * (cfg.imperial ? 2.236936f : 3.6f);
  char sp[8];
  if (fix) snprintf(sp, sizeof(sp), "%d", (int)(kmh + 0.5f)); else snprintf(sp, sizeof(sp), "--");
  setText(tSpeed, sp, B24, AL_L, 24);
  const char* unitTxt = cfg.imperial ? "MPH" : "KM/H";
  setText(tUnit, unitTxt, FB11, AL_L, 12, 1);
  const char* dirTxt = fix ? cardinal(m.pose.heading()) : "--";
  setText(tDir, dirTxt, FB11, AL_L, 14, 2);
  setBattText(h);

  // Maneuver change: icon slides in, proximity bar restarts and then eases to its target.
  static uint8_t  lastIcon = 255;
  static uint32_t iconStart = 0;
  static float    barShown = 0;
  if (m.icon != lastIcon) { lastIcon = m.icon; iconStart = now; barShown = 0; }
  float barTarget = arrived ? 1.f
                  : (m.distToTurn == 0xFFFF || m.distToTurn >= TURN_BAR_M) ? 0.f
                  : 1.f - (float)m.distToTurn / TURN_BAR_M;
  barShown += (barTarget - barShown) * 0.25f;
  if (fabsf(barTarget - barShown) < 0.004f) barShown = barTarget;

  // What the screen shows, for the nav -> idle transition (nvS), and the live target of the idle -> nav one.
  nvS.icon = m.icon <= 10 ? m.icon : 0;
  nvS.bmode = arrived ? 2 : (rerouting ? 1 : 0);
  nvS.bar = barShown;
  nvS.imp = cfg.imperial;
  strncpy(nvS.sp, sp, sizeof(nvS.sp) - 1);          nvS.sp[sizeof(nvS.sp) - 1] = 0;
  strncpy(nvS.dir, dirTxt, sizeof(nvS.dir) - 1);    nvS.dir[sizeof(nvS.dir) - 1] = 0;
  int limShown = 0;                                          // posted limit in the displayed unit (see the sign below)
  if (m.speedLimitKmh > 0 && !arrived) {
    limShown = m.speedLimitKmh;
    if (cfg.imperial) limShown = (int)(limShown / 1.609344f / 5.f + 0.5f) * 5;
  }
  nvS.limit = limShown;

#if UI_ANIMATIONS && UI_NAV_TRANSITION
  if (nvActive && nvIn) {                                    // idle -> nav: the road becomes the route (BASE stays blank meanwhile)
    if (nvPending) { nvPending = false; nvStart = now; }
    const float tt = (float)trElapsed(now, nvStart, (float)UI_NAV_SPEED_DIV) * 1e-3f;
    if (tt >= NV_END) nvActive = false;                      // the map takes over this very frame
    else {
      if (fix) drawRoute(m, theta, scale, arrived, false);   // project only: the target follows the vehicle
      else     nvS.n = 0;
      if (limShown > 0) nvLimitText(limShown);
      if (!arrived) updateCompass(theta, 416, 20);
      R->overlayBegin();
      clipBegin();
      nvDraw(h, tt, true);
      return;
    }
  }
#endif

  // ---- BASE: map. Redrawn only when the quantized view changed, at most every UI_MAP_MIN_MS ----
  ViewKey k; memset(&k, 0, sizeof(k));
  k.qx = (int32_t)lround(m.pose.x() * scale * (0.1 / UI_MAP_POS_Q_PX));     // 1 dm = scale*0.1 px
  k.qy = (int32_t)lround(m.pose.y() * scale * (0.1 / UI_MAP_POS_Q_PX));
  k.qh = (int32_t)lroundf(theta / UI_MAP_HDG_Q_DEG);
  k.rev = m.routeRev; k.zoom = cfg.zoom; k.hu = cfg.headingUp; k.fix = fix;
  k.dt = ((uint32_t)m.icon << 1) | (m.distToTurn == 0xFFFF ? 1u : 0u);   // maneuver changed -> re-pick the turn node
  k.st = (uint8_t)m.navState;
  static uint32_t lastMapMs = 0;
  const bool changed = !keyValid || memcmp(&k, &lastKey, sizeof(k)) != 0;
  const bool urgent  = !keyValid || k.zoom != lastKey.zoom || k.hu != lastKey.hu || k.fix != lastKey.fix;
  if (changed && (urgent || now - lastMapMs >= UI_MAP_MIN_MS)) {
    lastKey = k; keyValid = true; lastMapMs = now;
    mapIx = (int32_t)lround(m.pose.x()); mapIy = (int32_t)lround(m.pose.y());   // same rounding as drawRoute
    mapTheta = theta; mapScale = scale; mapOk = fix;
    clearMap();
    nvS.n = 0;
    if (fix) drawRoute(m, theta, scale, arrived);
  }

  // ---- OVERLAY ----
  R->overlayBegin();
  if (fix) {
    updateChevron(cfg.headingUp ? 0.f : m.pose.heading());
    float ox, oy; markerOffset(m, ox, oy);
    emitMarker(ox, oy, now, stale || nogps);
  }

  // No-GPS strip (hatched, with a pill) under the banner
  if (nogps) {
    R->ovBitmap(0, BAN_H, SCREEN_W, 30, ptr(spHatch), M_SET);
    R->ovFillRect(0, BAN_H + 28, SCREEN_W, 2, M_SET);
    const int tw = textW(FB11, lang::tr(lang::S_NO_GPS), 2) + 2;
    const int pw = tw + 36, px = (SCREEN_W - pw) / 2;
    fillRR(px, BAN_H + 3, pw, 22, 11, M_SET);
    fillRR(px + 2, BAN_H + 5, pw - 4, 18, 9, M_CLR);
    tPill.x = px + 18; tPill.y = BAN_H + 4;
    blit(tPill, M_SET);
  }

  // Banner: black slab
  R->ovFillRect(0, 0, SCREEN_W, BAN_H, M_SET);
  int ic = m.icon <= 10 ? m.icon : 0;
  int icX = 16, icY = 12;
#if UI_ANIMATIONS
  if (now - iconStart < ICON_IN_MS)
    icX = 16 - (int)((1.f - easeOut((now - iconStart) / (float)ICON_IN_MS)) * 96);
  else if (ic >= 1 && ic <= 9 && m.distToTurn <= TURN_NEAR_M && !arrived && !rerouting)
    icY -= (int)(tri(now, 700) * 4.f);
#endif
  if (arrived) {
    R->ovBitmap(16, 12, IC, IC, ptr(spIcon[10]), M_CLR);
    blit(tBig, M_CLR);
    blit(tStreet, M_CLR);
    R->ovBitmap(416, 20, 44, 44, ptr(spCheck), M_CLR);
  } else if (rerouting) {
    R->ovBitmap(16, 12, IC, IC, ptr(spSpin[(now / 110) & 7]), M_CLR);
    blit(tBig, M_CLR);
    blit(tStreet, M_CLR);
  } else {
    R->ovBitmap(icX, icY, IC, IC, ptr(spIcon[ic]), M_CLR);
    blit(tDist, M_CLR);
    blit(tStreet, M_CLR);
  }
  if (!arrived) {
    updateCompass(theta, 416, 20);
    R->ovBitmap(416, 20, 44, 44, ptr(spCompass), M_CLR);
  }
  R->ovFillRect(16, BAN_H - 8, SCREEN_W - 32, 1, M_CLR);     // rule
  if (rerouting) {                                           // indeterminate sweep
    int sx = 16 + (int)(tri(now, 1200) * (SCREEN_W - 32 - 80));
    R->ovFillRect(sx, BAN_H - 9, 80, 3, M_CLR);
  } else if (barShown > 0.f) {                               // proximity bar
    int fw = (int)((SCREEN_W - 32) * barShown);
    if (fw > 0) R->ovFillRect(16, BAN_H - 9, fw, 3, M_CLR);
  }

  // Status bar: speed | heading | battery
  emitBarFrame();
  const int base = BOT_Y + 28;                               // 312
  tSpeed.x = 20; tSpeed.y = base - 24;
  blit(tSpeed, M_SET);
  const int wSpd = textW(B24, sp, 0);
  const int wUnit = textW(FB11, unitTxt, 1) + 1;
  tUnit.x = 20 + wSpd + 6; tUnit.y = base - 12;
  blit(tUnit, M_SET);
  char pc[12];
  if (h.battPct < 0) snprintf(pc, sizeof(pc), "--"); else snprintf(pc, sizeof(pc), "%d%%", h.battPct);
  const int rightX = 460 - (32 + 8 + textW(B14, pc, 0));
  emitBattery(rightX, BOT_Y + 12, h.battPct, now);
  tBatt.x = rightX + 40; tBatt.y = BOT_Y + 7;
  blit(tBatt, M_SET);
  const int midL = tUnit.x + wUnit;
  tDir.x = (midL + rightX) / 2 - (textW(FB11, dirTxt, 2)) / 2; tDir.y = BOT_Y + 9;
  blit(tDir, M_SET);

  // Posted speed limit sign (bottom right, above the status bar). speedLimitKmh = 0 -> none known yet.
  if (m.speedLimitKmh > 0 && !arrived) {
    int lim = m.speedLimitKmh;                                // km/h on the wire
    if (cfg.imperial) lim = (int)(lim / 1.609344f / 5.f + 0.5f) * 5;   // mph, nearest 5
    char ls[8];
    snprintf(ls, sizeof(ls), "%d", lim);
    if (lim >= 100) setText(tLimit, ls, B17, AL_C, 21); else setText(tLimit, ls, B24, AL_C, 24);
    const int lx = SCREEN_W - 14 - 34, ly = BOT_Y - 8 - 34;
    // Over the limit (compared in the displayed unit, needs a live fix): the sign flashes between normal and inverted.
    const bool over = fix && !stale && (int)(kmh + 0.5f) > lim;
    const bool inv  = over && ((now / 400) & 1);
    R->ovFillCircle(lx, ly, 36, M_CLR);                       // white halo: readable over the route
    R->ovFillCircle(lx, ly, 33, M_SET);                       // thick ring
    if (!inv) R->ovFillCircle(lx, ly, 25, M_CLR);             // inverted phase: solid black disc, white digits
    tLimit.x = lx - tLimit.w / 2; tLimit.y = ly - tLimit.h / 2;
    blit(tLimit, inv ? M_CLR : M_SET);
  }

  emitFps(h, cfg.showFps, 8, 134, true);
  emitReveal(now);
  nvS.valid = true;                                          // a steady nav frame: enter(SCR_IDLE) may play the exit from it
}

// =============================================================================================
// Firmware update (OTA)
// =============================================================================================
void updateFrame(const ota::Info& o, const Hud& h) {
  const bool err  = o.state == proto::OTA_ERROR;
  const bool done = o.state == proto::OTA_DONE;
  const bool ver  = o.state == proto::OTA_VERIFYING;
  int pct = o.total ? (int)(((uint64_t)o.done * 100) / o.total) : 0;
  if (done) pct = 100;
  if (pct > 100) pct = 100;
  upPct = pct; upDone = o.done; upTotal = o.total; upVer = o.version;   // what a cancel transition starts from
  upLive = !err && !done && !ver;
  const bool prevShown = upShown && upSteady;               // the previous frame showed the complete progress screen
  upShown = !err && !done; upFailed = err;
  if (err) upErr = o.err;

  // Idle -> update transition clock: tt = scaled seconds (99 = steady screen). An error ends it at once.
  float tt = 99.f;
  if (otActive) {
    if (otPending) { otPending = false; otStart = h.nowMs; }
    tt = (float)trElapsed(h.nowMs, otStart, (float)UI_OTA_SPEED_DIV) * 1e-3f;
    if (err || tt >= OT_END) { otActive = false; tt = 99.f; updateRuler(); }   // ruler moves into BASE (same pixels)
  }
  const bool trans = tt < OT_END;
  upSteady = !trans;

  // Update -> failed transition clock: ft = scaled seconds (99 = steady failure screen).
  float ft = 99.f;
#if UI_ANIMATIONS && UI_OTA_FAIL_TRANSITION
  if (err && prevShown && !erActive) erActive = erPending = true;
  if (erActive) {
    if (erPending) { erPending = false; erStart = h.nowMs; }
    ft = (float)trElapsed(h.nowMs, erStart, (float)UI_OTA_FAIL_SPEED_DIV) * 1e-3f;
    if (ft >= FT_END) { erActive = false; ft = 99.f; }
  }
#else
#if UI_ANIMATIONS
  // Bespoke transition off: update -> failed is a screen change, so it gets the interlock reveal.
  if (err && prevShown) revealPending = true;
#else
  (void)prevShown;
#endif
#endif

  R->overlayBegin();
  clipBegin();
  if (trans) otIdleLayer(h, tt);
  { int a, b; if (openWin(240, 110, (tt - 1.05f) / .25f, a, b)) blitClip(srcOf(tUpLbl), a, b, M_SET); }

  if (err && ft < FT_END) {
    failBuild(ft);                                           // progress screen closes, failure screen opens
  } else if (err) {
    setText(tFailE, ota::errorText(o.err), S19, AL_C, 21);
    setText(tFailH, lang::tr(lang::S_FW_KEPT), M14, AL_C, 14);
    R->ovFillRect(49, 172, 382, 30, M_CLR);                  // hide the ruler
    R->ovFillCircle(240, 92, 36, M_SET);
    R->ovLine(228, 80, 252, 104, 7, M_CLR);
    R->ovLine(252, 80, 228, 104, 7, M_CLR);
    blit(tFailT, M_SET);
    blit(tFailE, M_SET);
    blit(tFailH, M_SET);
  } else {
    char b[48];
    if (trans) {                                             // the ruler is drawn here until it moves into BASE
      const int rw = (int)lroundf(OT_RW * easeOut((tt - 1.f) / .25f));
      if (rw > 0) R->ovFillRect(240 - rw / 2, OT_RY, rw, 2, M_SET);
      for (int i = 0; i <= 20; i++) {
        const int th = (int)lroundf((i % 5 ? 8 : 14) * easeOut((tt - 1.1f - i * .012f) / .12f));
        if (th > 0) R->ovFillRect(OT_RX + i * 19, 186, 2, th, M_SET);
      }
    }
    if (tt >= 1.15f) {                                       // huge percent: one roll, then it locks with a 3 px bump
      int dy = 0;
      if (tt < 1.29f) {
        snprintf(b, sizeof(b), "%d", (int)(((long)(tt * 25.f) * 7) % 10));
        renderPct(b, false);
      } else {
        snprintf(b, sizeof(b), "%d", pct);
        renderPct(b, true);
        if (tt < 1.37f) dy = -(int)lroundf(3.f * (1.f - (tt - 1.29f) / .08f));
      }
      blitClip(srcOf(tUpPct), 0, SCREEN_W, M_SET, dy);
    }

    int fw = 382 * pct / 100;
    if (tt >= 1.f) {
      if (ver) {                                             // verifying: indeterminate sweep
        int sx = (int)(tri(h.nowMs, 1200) * (382 - 80));
        R->ovFillRect(49 + sx, 172, 80, 10, M_SET);
      } else if (fw > 0) {
        R->ovFillRect(49, 172, fw, 10, M_SET);
      }
    }

    int wa, wb;
    snprintf(b, sizeof(b), "%lu / %lu KB", (unsigned long)((o.done + 512) / 1024), (unsigned long)((o.total + 512) / 1024));
    setText(tUpKB, b, B14, AL_L, 16);
    if (openWin(150, 110, (tt - 1.35f) / .2f, wa, wb)) blitClip(srcOf(tUpKB), wa, wb, M_SET);
    snprintf(b, sizeof(b), "%u.%u.%u -> %u.%u.%u",
             (unsigned)FW_V_MAJOR(FW_VERSION), (unsigned)FW_V_MINOR(FW_VERSION), (unsigned)FW_V_PATCH(FW_VERSION),
             (unsigned)FW_V_MAJOR(o.version),  (unsigned)FW_V_MINOR(o.version),  (unsigned)FW_V_PATCH(o.version));
    setText(tUpVer, b, FB11, AL_R, 14, 2, 2);
    if (openWin(340, 100, (tt - 1.35f) / .2f, wa, wb)) blitClip(srcOf(tUpVer), wa, wb, M_SET);

    const char* msg = done ? lang::tr(lang::S_RESTARTING) : ver ? lang::tr(lang::S_VERIFYING) : lang::tr(lang::S_KEEP_OPEN);
    setText(tUpHint, msg, S14, AL_L, 16);
    const bool lock = !done && !ver;
    const int tw = textW(S14, msg, 0);
    const int total = (lock ? 22 : 0) + tw;
    const int x0 = (SCREEN_W - total) / 2;
    tUpHint.x = x0 + (lock ? 22 : 0);
    if (openWin(240, 170, (tt - 1.55f) / .25f, wa, wb)) {
      if (lock) blitClip(srcOf(spLock, x0, 245, 14, 16), wa, wb, M_SET);
      blitClip(srcOf(tUpHint), wa, wb, M_SET);
    }
  }

  if (trans) emitStatusBar(h, tt < .25f, tt / .25f);        // version retracts first, then the layout swaps at 250 ms
  else       emitStatusBar(h, false);
  if (trans) otMarker(tt);
  emitFps(h, h.showFps, 140, 264, false);
  emitReveal(h.nowMs);
}

}  // namespace ui