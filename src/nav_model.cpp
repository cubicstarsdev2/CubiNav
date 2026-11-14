#include "nav_model.h"
#include <stdlib.h>

namespace {
const uint32_t KEEP_BEHIND = 32;                    // points kept behind the cursor
const double   ROUTE_HDG_MAX_DM = 300.0;            // further than 30 m from the route: not on it, use the GPS course
const float    ROUTE_HDG_MAX_DIFF = 110.f;          // route runs against the GPS course (wrong way / U-turn): ignore the route
inline int64_t d2(const Pt& p, int32_t x, int32_t y) {
  int64_t dx = (int64_t)p.x - x, dy = (int64_t)p.y - y;
  return dx * dx + dy * dy;
}
}

void NavModel::resetSession() {
  ringA.clear(); ringB.clear(); active = &ringA; shadow = &ringB;
  navState = proto::NAV_IDLE; icon = 0; street[0] = 0; distToTurn = 0xFFFF; speedLimitKmh = 0;
  cursor = 0; pose.reset(); routeRev++;
}

void NavModel::pickNearestStart() {
  int32_t px = (int32_t)lround(pose.x()), py = (int32_t)lround(pose.y());
  uint32_t end = active->tail() + 128;
  if (end > active->head()) end = active->head();
  int64_t best = INT64_MAX;
  for (uint32_t i = active->tail(); i < end; i++) {
    int64_t d = d2(active->at(i), px, py);
    if (d < best) { best = d; cursor = i; }
  }
}

void NavModel::commitShadow() {
  RouteRing* t = active; active = shadow; shadow = t;
  shadow->clear();
  cursor = active->tail();
  if (pose.hasFix()) pickNearestStart();
  routeRev++;
}

void NavModel::update(float dt, uint32_t nowMs) {
  pose.update(dt, nowMs);
  if (!active->valid() || !pose.hasFix()) { pose.setRouteHeading(0.f, false); return; }
  RouteRing& r = *active;
  if (cursor < r.tail()) cursor = r.tail();
  if (cursor >= r.head()) cursor = r.head() - 1;

  // Greedy forward walk: advance while the next point is not farther than the current one.
  int32_t px = (int32_t)lround(pose.x()), py = (int32_t)lround(pose.y());
  for (int steps = 0; steps < 32 && cursor + 1 < r.head(); steps++) {
    if (d2(r.at(cursor + 1), px, py) <= d2(r.at(cursor), px, py)) cursor++;
    else break;
  }
  r.trimBefore(cursor > KEEP_BEHIND ? cursor - KEEP_BEHIND : 0);   // frees slots for the phone
  updateRouteHeading();
}

// Map-matched heading. GPS course only changes AFTER the vehicle has started turning, so the view used to keep
// going straight past the corner and rotate late. Instead: project the vehicle onto the route, look L metres
// ahead ALONG the route and use the bearing S -> that point. Around a corner it sweeps smoothly from the old
// to the new direction and finishes exactly at the corner.
void NavModel::updateRouteHeading() {
  const RouteRing& r = *active;
  if (r.count() < 2 || pose.stale()) { pose.setRouteHeading(0.f, false); return; }
  const double px = pose.x(), py = pose.y();

  // Closest point on the two segments around the cursor (segment s = s -> s+1).
  const uint32_t s0 = cursor > r.tail() ? cursor - 1 : r.tail();
  const uint32_t s1 = cursor < r.head() - 1 ? cursor : r.head() - 2;
  double bestD = 1e30, sx = 0, sy = 0, segDx = 0, segDy = 0;
  uint32_t bs = s0;
  for (uint32_t s = s0; s <= s1; s++) {
    const Pt& a = r.at(s); const Pt& b = r.at(s + 1);
    const double ex = (double)b.x - a.x, ey = (double)b.y - a.y, L2 = ex * ex + ey * ey;
    double t = L2 > 0 ? ((px - a.x) * ex + (py - a.y) * ey) / L2 : 0.0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const double qx = a.x + t * ex, qy = a.y + t * ey;
    const double dd = (px - qx) * (px - qx) + (py - qy) * (py - qy);
    if (dd < bestD) { bestD = dd; bs = s; sx = qx; sy = qy; segDx = ex; segDy = ey; }
  }
  if (bestD > ROUTE_HDG_MAX_DM * ROUTE_HDG_MAX_DM) { pose.setRouteHeading(0.f, false); return; }

  const float spd = pose.speedMs();
  // Wrong direction check against the GPS course (only meaningful while moving).
  if (spd > 1.5f && (segDx != 0 || segDy != 0)) {
    float dh = fabsf((float)(atan2(segDx, segDy) * 57.29577951308232) - pose.rawHeading());
    dh = fmodf(dh, 360.f); if (dh > 180.f) dh = 360.f - dh;
    if (dh > ROUTE_HDG_MAX_DIFF) { pose.setRouteHeading(0.f, false); return; }
  }

  // Walk L metres along the route from S (L grows with speed: ~1 s of travel, 8..25 m).
  float lookM = spd * 1.0f; if (lookM < 8.f) lookM = 8.f; if (lookM > 25.f) lookM = 25.f;
  double remain = lookM * 10.0, cx = sx, cy = sy;            // dm
  uint32_t i = bs + 1;
  for (int steps = 0; steps < 48 && i < r.head() && remain > 0; steps++, i++) {
    const Pt& p = r.at(i);
    const double ex = p.x - cx, ey = p.y - cy, len = sqrt(ex * ex + ey * ey);
    if (len >= remain) { cx += ex * remain / len; cy += ey * remain / len; remain = 0; break; }
    cx = p.x; cy = p.y; remain -= len;
  }
  const double dx = cx - sx, dy = cy - sy;
  if (dx * dx + dy * dy < 25.0) { pose.setRouteHeading(0.f, false); return; }   // < 0.5 m (end of route)
  float brg = (float)(atan2(dx, dy) * 57.29577951308232);
  if (brg < 0.f) brg += 360.f;
  pose.setRouteHeading(brg, true);
}
bool NavModel::handleAt(const uint8_t* d, size_t n) {
  if (n < 3 || d[0] != 'A' || d[1] != 'T' || d[2] != ':')
    return false;

  size_t i = 3;
  size_t k = 0;

  while (i < n && d[i] == ' ') i++;
  if (i < n && d[i] == '"') i++;

  constexpr size_t MAX_ALERT_LEN = sizeof(alertText) - 1;

  for (; i < n && d[i] != '"' && d[i] != '\r' && d[i] != '\n'; i++) {
    if (k >= MAX_ALERT_LEN) break;

    uint8_t c = d[i];
    alertText[k++] = (c >= 32 && c < 127) ? (char)c : '?';
  }

  alertText[k] = '\0';
  alertRev++;

  return true;
}