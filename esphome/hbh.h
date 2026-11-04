// HbH – Hob bridge Hood: the "brain" running on the transmitter ESP.
// Copyright (c) 2026 JoTu – PolyForm Noncommercial 1.0.0 (see LICENSE).
//
// 1. Hood state from the hood's supply power (Shelly). The power ADDS UP: standby + fan + light. The values come
//    from the hood profile (e.g. Ciarko Duo 74: 0.5 W + fan 4.4 / 10.0 / 27.5 / boost ≥ 100 W + light 3.2–11.9 W).
//    Values overlap (light 100 % ≈ fan 1 + light 50 %), so the state is tracked by TRANSITIONS: from the previous
//    state the cheapest change that explains the new power wins; the last command is a soft hint, not a lock.
// 2. Jobs: set speed / brightness / colour / timer through the RF transmitter, checked against the power.
// 3. Hob → hood: hob switched ON → light; cooking → fan from the profile table; run-on; manual takeover;
//    colour by time of day; hob alarm; pairing after a power cut.
// 4. Drying: kitchen humid and the outdoor air drier (absolute humidity) → fan 1 until the humidity drops.
// 5. Night reset: at a set time the hood relay is cut for 15 s when the hood still draws power.
// 6. Statistics: hob energy today / of the last cooking, grease-filter usage (fan hours weighted by speed).
// 7. Calibration: measures the hood's power table by itself (any hood) and keeps it across restarts.
// 8. Learning the remote: the codes of the user's remote, learned through the receiver, kept across restarts.
// All sources are Home Assistant entities chosen at runtime (text entities, applied after a restart).
#pragma once
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/api/api_server.h"

namespace hbh {

static const char *const TAG = "hbh";

inline uint32_t now_ms() { return esphome::millis(); }
// seconds since ts (0 = never); a time in the future (end of a burst being sent) counts as now
inline float age_s(uint32_t ts) {
  if (ts == 0) return 1e9f;
  int32_t d = (int32_t) (now_ms() - ts);
  return d < 0 ? 0.0f : d / 1000.0f;
}

// ================================================================== Home Assistant sources
// source = "domain.entity" or "domain.entity:attribute" (e.g. climate.kitchen:current_temperature)
inline void subscribe(const std::string &source, std::function<void(const std::string &)> cb) {
  if (source.empty() || source.find('.') == std::string::npos) return;
  size_t c = source.find(':');
  std::string entity = c == std::string::npos ? source : source.substr(0, c);
  esphome::optional<std::string> attr;
  if (c != std::string::npos && c + 1 < source.size()) attr = source.substr(c + 1);
  ESP_LOGI(TAG, "source: %s", source.c_str());
  esphome::api::global_api_server->subscribe_home_assistant_state(
      entity, attr, std::function<void(const std::string &)>(std::move(cb)));
}

// sum of several sources "sensor.l1+sensor.l2+…" (e.g. a hob on 2 or 3 phases measured per phase);
// the callback gets the sum once every part has a value
inline void subscribe_sum(const std::string &spec, std::function<void(float)> cb);

inline float to_float(const std::string &s) {
  if (s.empty()) return NAN;
  char *end = nullptr;
  float v = strtof(s.c_str(), &end);
  return (end == s.c_str()) ? NAN : v;
}

inline void subscribe_sum(const std::string &spec, std::function<void(float)> cb) {
  std::vector<std::string> parts;
  size_t a = 0;
  while (a <= spec.size()) {
    size_t b = spec.find('+', a);
    std::string p = spec.substr(a, b == std::string::npos ? std::string::npos : b - a);
    while (!p.empty() && p.front() == ' ') p.erase(0, 1);
    while (!p.empty() && p.back() == ' ') p.pop_back();
    if (!p.empty()) parts.push_back(p);
    if (b == std::string::npos) break;
    a = b + 1;
  }
  if (parts.size() == 1) { subscribe(parts[0], [cb](const std::string &v) { cb(to_float(v)); }); return; }
  auto vals = std::make_shared<std::vector<float>>(parts.size(), NAN);
  for (size_t i = 0; i < parts.size(); i++) {
    subscribe(parts[i], [vals, i, cb](const std::string &v) {
      (*vals)[i] = to_float(v);
      float sum = 0;
      for (float x : *vals) { if (std::isnan(x)) return; sum += x; }
      cb(sum);
    });
  }
}

// a source is "active": number above the threshold, or an on-like state
inline bool active_value(const std::string &v, float thr) {
  float f = to_float(v);
  if (!std::isnan(f)) return f > thr;
  return v == "on" || v == "heating" || v == "true" || v == "running" || v == "cooking" || v == "boiling";
}

// state with a delay on / delay off (like a template binary_sensor with delay_on / delay_off)
struct Delayed {
  bool state = false, raw = false;
  uint32_t since = 0, changed = 0;
  bool update(bool r, float on_s, float off_s) {
    uint32_t t = now_ms();
    if (r != raw) { raw = r; since = t; }
    float held = (t - since) / 1000.0f;
    bool old = state;
    if (raw && !state && held >= on_s) state = true;
    if (!raw && state && held >= off_s) state = false;
    if (state != old) changed = t;
    return state != old;
  }
};

struct Inputs {
  float hood_p = NAN;           // hood power, W
  uint32_t hood_p_change = 0;
  bool hood_p_evaluated = true;
  std::string hood_sw;          // hood relay state (on / off / unavailable)
  float hob_p = NAN;            // hob active power, W
  float hob_va = NAN;           // hob apparent power, VA (hob switched ON ≈ 400 VA)
  bool has_hob_va = false;
  std::string aux1, aux2;       // extra sources (Thermomix, kettle …)
  float home = NAN;             // people at home (zone.home)
  float room_rh = NAN, room_t = NAN, out_t = NAN, out_rh = NAN;   // drying: kitchen and outdoor air
  uint32_t room_rh_above_since = 0;
};
inline Inputs IN;

// ================================================================== settings (from the ESP's config entities)
struct Cfg {
  bool auto_on = true, auto_light = true, auto_colour = true, alarm_on = true, aux1_on = false, aux2_on = false;
  int profile = 5;              // H0–H6
  float runon_min = 4, runon_aux_min = 2, light_after_min = 2, p2 = 900, p3 = 1800, alarm_min = 120;
  float aux1_thr = 20, aux2_thr = 1000;
  float hob_on_thr = 150;       // hob switched ON: apparent power (VA) or current (A) above this
  int cold_from = 6 * 60, warm_from = 17 * 60;   // minutes of the day
  int now_min = -1;                               // minutes of the day, -1 = no time yet
  // drying
  bool dry_on = false; float dry_start = 63, dry_stop = 56, dry_diff = 2, dry_max_min = 60, dry_tmin = 0;
  int dry_from = 8 * 60, dry_to = 22 * 60;
  // night reset
  bool night_on = false; int night_at = 60;
};
inline Cfg C;

// ================================================================== actions provided by the YAML
inline std::function<void(const std::string &, int)> tx_send;      // RF key, frames
inline std::function<bool()> tx_busy;                               // RF script running
inline std::function<void()> hood_power_on;                         // HA turn_on of the hood relay
inline std::function<void()> hood_power_off;                        // HA turn_off of the hood relay
// hob alarm: kind = "nobody_home" / "too_long" / "end", hob power W, heating minutes (texts are in the YAML / language file)
inline std::function<void(const std::string &, float, float)> notify;
inline uint32_t last_tx = 0;                                        // end of the last own transmission

// ================================================================== hood state estimator
struct Est {
  int pb = 0; bool pl = false; float L = 3.2f;
  std::string cmd_sw; uint32_t sw_ts = 0;
  std::string cmd_fan; uint32_t fan_ts = 0;
  uint32_t rz_ts = 0;
  uint32_t pb_changed = 0;
};
inline Est E;

// ---- from the hood profile (set_profile() at boot)
inline float PW_STANDBY = 0.5f, PW_BOOST = 100.0f;
inline float W_FAN[4] = {0.0f, 4.4f, 10.0f, 27.5f};       // net fan power of speed 0–3
inline float LV[4] = {3.2f, 6.3f, 8.9f, 11.9f};           // net light power of 25 / 50 / 75 / 100 %
inline int RF_SHORT = 8, RF_HOLD = 60, RF_COLOUR = 140;    // frames: short press, stop/boost hold, colour hold
inline void set_profile(float standby, float f1, float f2, float f3, float boost, float l1, float l2, float l3, float l4,
                        int short_frames, int hold_frames, int colour_frames) {
  PW_STANDBY = standby; PW_BOOST = boost;
  W_FAN[1] = f1; W_FAN[2] = f2; W_FAN[3] = f3;
  LV[0] = l1; LV[1] = l2; LV[2] = l3; LV[3] = l4;
  RF_SHORT = short_frames; RF_HOLD = hold_frames; RF_COLOUR = colour_frames;
}

inline float clampL(float l) { return std::fmax(LV[0] * 0.75f, std::fmin(LV[3] * 1.05f, l)); }
inline int level_of(float l) {
  int best = 0;
  for (int i = 1; i < 4; i++)
    if (std::fabs(l - LV[i]) < std::fabs(l - LV[best])) best = i;
  return best + 1;
}
inline int light_level() { return E.pl ? level_of(E.L) : 0; }

inline void set_state(int f, bool s, float l) {
  if (f != E.pb) E.pb_changed = now_ms();
  E.pb = f; E.pl = s; E.L = clampL(l);
}

inline void on_cmd(const std::string &cmd) {
  uint32_t t = now_ms();
  if (cmd == "light_on" || cmd == "light_off" || cmd == "brightness") { E.cmd_sw = cmd; E.sw_ts = t; }
  if (cmd == "fan_plus" || cmd == "fan_minus" || cmd == "boost" || cmd == "stop") { E.cmd_fan = cmd; E.fan_ts = t; }
  // at speed 3–4 the light cannot be measured -> take it from the command
  if ((cmd == "light_on" || cmd == "light_off") && E.pb >= 3) set_state(E.pb, cmd == "light_on", E.L);
  ESP_LOGD(TAG, "cmd %s", cmd.c_str());
}

// trigger: 'p' = power stable 5 s, 'r' = resync (every minute), 's' = start
inline void evaluate(char trig) {
  float p = IN.hood_p;
  if (std::isnan(p)) return;
  if (trig == 'p') {
    float x = p - PW_STANDBY - (E.pl ? E.L : 0.0f);
    if (E.pb == 0 && x > W_FAN[2] * 1.2f && x < PW_BOOST) E.rz_ts = now_ms();   // motor start spike
  }
  const int pb = E.pb; const bool pl = E.pl; const float L = E.L;
  const float n = std::fmax(p - PW_STANDBY, 0.0f);
  const float asw = age_s(E.sw_ts), afan = age_s(E.fan_ts);
  const bool jas = asw < 30 && E.cmd_sw == "brightness";
  const int want_light = (asw < 90 && E.cmd_sw == "light_on") ? 1 : (asw < 90 && E.cmd_sw == "light_off") ? 0 : -1;
  const bool fanc = afan < 60;
  const bool stopc = afan < 30 && E.cmd_fan == "stop";
  const bool rz = age_s(E.rz_ts) < 25;

  float e = 999, cur = 999, fe = 999;
  int bf = pb, ff = pb; bool bs = pl, fs = pl; float bl = L, fl = L;
  for (int f = 0; f < 5; f++) {
    for (int si = 0; si < 2; si++) {
      bool s = si == 1;
      if ((rz && f == 0 && n > 1.5f) || ((f >= 3 || pb >= 3) && s != pl && want_light < 0)) continue;
      float err, Ln, fitv, fitL, e1 = 999;
      if (f == 4) {
        err = n >= PW_BOOST ? 0 : 999; Ln = L; fitv = err; fitL = L;
      } else {
        float tol = f == 3 ? 1.5f : 0.0f;
        float res = n - W_FAN[f];
        if (!s) {
          err = std::fmax(std::fabs(res) - tol, 0.0f); Ln = L; fitv = err; fitL = L;
        } else {
          e1 = std::fmax(std::fabs(res - L) - tol, 0.0f);
          float lvd = 999;
          for (float lv : LV) lvd = std::fmin(lvd, std::fabs(res - lv));
          float e2 = jas ? 0.0f : (!pl ? lvd : (fanc ? 999.0f : lvd + 0.5f));  // light switched on: last brightness
          bool lfree = f <= 3 && res > 1.5f && std::fmax(lvd - tol, 0.0f) < e1;
          fitv = lfree ? std::fmax(lvd - tol, 0.0f) : e1; fitL = lfree ? res : L;
          if (f <= 2 && res > 1.5f && e2 < e1) { err = e2; Ln = res; } else { err = e1; Ln = L; }
        }
      }
      if (f == pb && s == pl) cur = std::fmin(cur, (s && f < 4) ? e1 : fitv);
      if (fitv < fe) { fe = fitv; ff = f; fs = s; fl = fitL; }
      int df = std::abs(f - pb);
      err += 0.3f * df + (s != pl ? 0.8f : 0) + ((jas && f != pb) ? 1.5f : 0) + ((jas && !s) ? 1.5f : 0)
           + ((df > 1 && f != 0 && f != 4 && pb != 4) ? (fanc ? 0.6f : 1.2f) * (df - 1) : 0)
           + ((want_light >= 0 && (s ? 1 : 0) != want_light) ? 1.2f : 0) + ((stopc && f > 0) ? 2.0f : 0)
           + ((fanc && s != pl && want_light < 0) ? 1.5f : 0);
      if (err < e) { e = err; bf = f; bs = s; bl = Ln; }
    }
  }
  if ((trig == 'r' || trig == 's') && cur > 2.5f && fe < 1.2f) set_state(ff, fs, fl);
  else if (e > 3) set_state(pb, pl, L);
  else set_state(bf, bs, bl);
  ESP_LOGD(TAG, "eval %c p=%.1f -> %d|%d|%.1f (e=%.2f)", trig, p, E.pb, E.pl ? 1 : 0, E.L, e);
}

inline void on_hood_power(float v) {
  if (std::isnan(v)) return;
  if (std::isnan(IN.hood_p) || std::fabs(v - IN.hood_p) > 0.001f) {
    bool first = std::isnan(IN.hood_p);
    IN.hood_p = v; IN.hood_p_change = now_ms(); IN.hood_p_evaluated = false;
    if (first) evaluate('s');
  }
}
inline float power_stable_s() { return age_s(IN.hood_p_change); }

// ================================================================== jobs (RF actions checked against the power)
enum JobType { J_SPEED, J_LIGHT, J_COLOUR, J_TIMER, J_RAW };
struct Job {
  JobType type; int arg = 0; std::string key; int frames = 0;
  int phase = 0; uint32_t t = 0; int n = 0; int b = 0; float p0 = 0;
};
inline std::deque<Job> Q;
inline bool job_active = false;
inline int target_speed = -1;      // shown on the card while a speed job runs
inline int target_light = -1;      // 0–4 while a light job runs
inline bool rf_problem = false;
inline std::string colour = "";    // "cool" / "warm" (restored by the YAML)
inline uint32_t timer_end = 0;     // countdown of the 10-min timer

inline void press(const std::string &key, int frames = 0) { if (tx_send) tx_send(key, frames > 0 ? frames : RF_SHORT); }
inline bool tx_idle() { return !(tx_busy && tx_busy()); }

// a new speed / light request replaces a pending or running one of the same kind (like mode: restart)
inline void request(JobType type, int arg) {
  for (auto it = Q.begin(); it != Q.end();) {
    if (it->type == type && !(it == Q.begin() && job_active)) it = Q.erase(it); else ++it;
  }
  if (job_active && !Q.empty() && Q.front().type == type && type != J_RAW) { Q.pop_front(); job_active = false; }
  Job j; j.type = type; j.arg = arg;
  Q.push_back(j);
  if (type == J_SPEED) target_speed = arg;
  if (type == J_LIGHT) target_light = arg;
  ESP_LOGI(TAG, "request %d (%d), queue %d", (int) type, arg, (int) Q.size());
}
inline void request_raw(const std::string &key, int frames) {
  Job j; j.type = J_RAW; j.key = key; j.frames = frames; Q.push_back(j);
}
inline bool speed_job_pending() {
  for (auto &j : Q) if (j.type == J_SPEED) return true;
  return false;
}

// one step of the job at the head of the queue; returns true when finished
inline bool step(Job &j) {
  uint32_t t = now_ms();
  auto el = [&]() { return (t - j.t) / 1000.0f; };
  auto go = [&](int ph) { j.phase = ph; j.t = t; };
  switch (j.type) {
    case J_RAW:
      if (j.phase == 0) { press(j.key, j.frames); go(1); return false; }
      return tx_idle();

    case J_TIMER:
      if (j.phase == 0) { if (E.pb > 0) press("timer"); timer_end = t + 600000; go(1); return false; }
      return tx_idle();

    case J_COLOUR:  // hold key 4 (cool) / 5 (warm) ~6 s – reaches the end of the scale from any colour
      if (j.phase == 0) {
        if (!E.pl) return true;
        press(j.arg == 0 ? "brighter" : "dimmer", RF_COLOUR);
        colour = j.arg == 0 ? "cool" : "warm";
        go(1); return false;
      }
      return tx_idle() && el() > 1;

    case J_SPEED: {
      int target = j.arg < 0 ? 0 : (j.arg > 4 ? 4 : j.arg);
      switch (j.phase) {
        case 0:
          target_speed = target;
          if (IN.hood_sw == "off") {
            if (target == 0) return true;
            if (hood_power_on) hood_power_on();
            go(1); return false;
          }
          go(2); return false;
        case 1:  // relay switched on – wait for the supply
          if (el() < 3) return false;
          go(2); return false;
        case 2:
          if (target == 0) {
            if (E.pb == 0) return true;
            press("fan_minus", RF_HOLD);   // stop = hold fan −
            go(10); return false;
          }
          j.b = E.pb;
          for (int i = 0; i < std::abs(target - j.b); i++) press(target > j.b ? "fan_plus" : "fan_minus");
          go(3); return false;
        case 3:  // fast path: all presses at once, one check
          if (!tx_idle()) { j.t = t; return false; }
          if (E.pb == target) { rf_problem = false; return true; }
          if (el() < 25) return false;
          j.n = 0; go(4); return false;
        case 4:  // correction step by step (max 6)
          if (E.pb == target) { rf_problem = false; return true; }
          if (j.n >= 6 || (j.n > 3 && E.pb == 0)) { rf_problem = true; ESP_LOGW(TAG, "speed %d not reached", target); return true; }
          j.b = E.pb; j.n++;
          press(E.pb < target ? "fan_plus" : "fan_minus");
          go(5); return false;
        case 5:  // wait for a change (12 s), then for a stable power
          if (!tx_idle()) { j.t = t; return false; }
          if (E.pb != j.b || el() > 12) go(6);
          return false;
        case 6:
          if (el() > 2 && (power_stable_s() > 6 || el() > 22)) go(4);
          return false;
        case 10:  // stop: wait for 0, else fan − step by step
          if (!tx_idle()) { j.t = t; return false; }
          if (E.pb == 0) { rf_problem = false; return true; }
          if (el() < 15) return false;
          j.n = 0; go(11); return false;
        case 11:
          if (E.pb == 0) { rf_problem = false; return true; }
          if (j.n >= 7) { rf_problem = true; return true; }
          press("fan_minus"); j.n++; go(12); return false;
        case 12:
          if (tx_idle() && el() > 3) go(11);
          return false;
      }
      return true;
    }

    case J_LIGHT: {
      int target = j.arg;  // 0 = off, 1–4 = 25–100 %, 9 = on at the last brightness (the hood restores it)
      switch (j.phase) {
        case 0:
          target_light = target;
          if (target == 0) {
            if (!E.pl) return true;
            j.p0 = IN.hood_p; j.n = 0; press("light"); go(20); return false;
          }
          if (!E.pl) { j.p0 = IN.hood_p; j.n = 0; press("light"); go(1); return false; }
          if (target == 9) return true;
          go(3); return false;
        case 1:  // light on: wait up to 15 s; resend only when the power did not move at all (the code was lost)
          if (!tx_idle()) { j.t = t; return false; }
          if (E.pl) { go(2); return false; }
          if (el() < 15) return false;
          if (j.n == 0 && E.pb <= 2 && std::fabs(IN.hood_p - j.p0) < 1.5f) { j.n = 1; press("light"); go(1); return false; }
          return true;
        case 2:
          if (target == 9) return true;
          if (el() >= 2) go(3);
          return false;
        case 3: {  // fast path: all brightness steps at once
          int j0 = light_level();
          int d = target - j0;
          for (int i = 0; i < std::abs(d); i++) press(d > 0 ? "brighter" : "dimmer");
          if (E.pb >= 3) {  // light not measurable on 3 / boost – trust the presses
            if (d != 0) set_state(E.pb, true, LV[target - 1]);
            return true;
          }
          j.n = 0; go(4); return false;
        }
        case 4:
          if (!tx_idle()) { j.t = t; return false; }
          if (light_level() == target) return true;
          if (el() < 12) return false;
          go(5); return false;
        case 5:  // correction (max 4 steps)
          if (light_level() == target || !E.pl || j.n >= 4) return true;
          j.n++; j.b = light_level();
          press(light_level() < target ? "brighter" : "dimmer");
          go(6); return false;
        case 6:
          if (!tx_idle()) { j.t = t; return false; }
          if (light_level() != j.b || el() > 10) go(5);
          return false;
        case 20:  // light off
          if (!tx_idle()) { j.t = t; return false; }
          if (!E.pl) return true;
          if (el() < 15) return false;
          if (j.n == 0 && E.pb <= 2 && std::fabs(IN.hood_p - j.p0) < 1.5f) {
            j.n = 1; press("light"); go(20); return false;
          }
          return true;
      }
      return true;
    }
  }
  return true;
}

inline void loop_jobs() {
  if (Q.empty()) { job_active = false; target_speed = -1; target_light = -1; return; }
  job_active = true;
  Job &j = Q.front();
  if (step(j)) {
    if (j.type == J_SPEED) target_speed = -1;
    if (j.type == J_LIGHT) target_light = -1;
    Q.pop_front();
    job_active = false;
  }
}

// ================================================================== hob → hood
struct Brain {
  // hob
  std::vector<std::pair<uint32_t, float>> hob_hist;   // (time, power) for the 2-min average
  float avg2 = 0;
  Delayed cook, hob_on, aux1, aux2;
  bool hob_on_edge = false, hob_off_edge = false;
  bool cooking = false, prev_cooking = false;
  bool init = false;
  uint32_t boot_t = 0;
  int level = 0, rec = 0, prev_rec = 0;
  // session
  bool session = false, manual = false, auto_lit = false;
  uint32_t cooking_since = 0;
  bool cook_seen = false, aux1_seen = false;
  int phase = 0;            // 0 idle, 1 cooking, 2 run-on
  uint32_t runon_end = 0;
  uint32_t rec_change = 0;  // debounce of a new recommended speed
  bool rec_pending = false;
  int start_wait = 0;       // 1 = waiting for the recommended speed after start
  uint32_t start_t = 0;
  // light after the hob is switched off
  uint32_t hob_off_t = 0; bool hob_off_check = false;
  // colour
  uint32_t light_on_t = 0; bool light_on_pending = false; bool prev_pl = false; int prev_min = -1;
  // manual detection
  int stable_pb = -1; uint32_t stable_since = 0;
  // alarm
  bool alarm = false; bool alarm_sent = false; float prev_home = NAN;
  // pairing
  std::string prev_sw; uint32_t pair_at = 0;
  // timer
  bool timer_was = false;
  // drying: 0 off, 1 waiting for the conditions, 2 drying
  int dry_state = 0; bool drying = false; uint32_t dry_t = 0; int prev_min15 = -1; bool dry_started_fan = false;
  float room_ah = NAN, out_ah = NAN;
  // night reset
  int night_phase = 0; uint32_t night_t = 0; int night_done_day_min = -1;
  int mode = 1;
};
inline Brain B;

// absolute humidity, g/m³
inline float abs_hum(float t, float rh) {
  if (std::isnan(t) || std::isnan(rh)) return NAN;
  return 6.112f * expf((17.67f * t) / (t + 243.5f)) * rh * 2.1674f / (273.15f + t);
}
inline bool in_window(int now, int from, int to) {
  if (now < 0) return false;
  return from <= to ? (now >= from && now < to) : (now >= from || now < to);
}

static const int TABLE[7][3] = {{0, 0, 0}, {0, 0, 0}, {1, 1, 1}, {0, 1, 1}, {1, 1, 2}, {1, 2, 3}, {2, 3, 3}};

inline void on_hob_power(float v) {
  if (std::isnan(v)) return;
  IN.hob_p = v;
  B.hob_hist.emplace_back(now_ms(), v);
}

inline void update_avg() {
  uint32_t t = now_ms();
  while (B.hob_hist.size() > 1 && t - B.hob_hist[1].first > 120000) B.hob_hist.erase(B.hob_hist.begin());
  if (B.hob_hist.empty()) { B.avg2 = 0; return; }
  // time-weighted average over the last 120 s (the first sample covers from the window start)
  double sum = 0, dur = 0;
  uint32_t start = t - 120000;
  for (size_t i = 0; i < B.hob_hist.size(); i++) {
    uint32_t a = B.hob_hist[i].first;
    if ((int32_t) (a - start) < 0) a = start;
    uint32_t b = (i + 1 < B.hob_hist.size()) ? B.hob_hist[i + 1].first : t;
    if ((int32_t) (b - a) <= 0) continue;
    sum += B.hob_hist[i].second * (double) (b - a);
    dur += (double) (b - a);
  }
  B.avg2 = dur > 0 ? sum / dur : B.hob_hist.back().second;
}

inline int recommended() {
  if (!B.cooking) { B.level = 0; return 0; }
  int prof = C.profile < 0 || C.profile > 6 ? 5 : C.profile;
  if (!B.cook.state) { B.level = 1; return TABLE[prof][0]; }   // Thermomix / kettle only = light cooking
  float p = B.avg2;
  int up = p >= C.p3 ? 3 : p >= C.p2 ? 2 : 1;
  int down = p >= C.p3 * 0.8f ? 3 : p >= C.p2 * 0.8f ? 2 : 1;
  int prev = B.level;
  B.level = (prev == 0 || up >= prev) ? up : std::min(down, prev);
  return TABLE[prof][B.level - 1];
}

inline void light_on_auto() {
  if (!E.pl && !(target_light > 0)) { B.auto_lit = true; request(J_LIGHT, 9); }
}

// manual change from the card (ESP controls) or the original remote
inline void manual_fan() { if (B.session) { B.manual = true; ESP_LOGI(TAG, "manual fan – automation leaves the fan"); } }
inline void manual_light() { B.auto_lit = false; }

// ================================================================== learning the remote's codes
// The receiver (hbh-remote-rx.yaml) sends the bits of every press – also of a remote it does not know. The wizard
// asks for the six keys in a fixed order, each pressed twice (two equal frames, the toggle bit ignored). The codes
// are kept in the ESP across restarts and override the key_* codes of the profile – no YAML, no re-flashing.
inline const char *const KEY_NAMES[6] = {"light", "fan_minus", "fan_plus", "timer", "brighter", "dimmer"};
struct Learn {
  bool active = false;
  int step = 0;              // 0–11: key = step / 2, press = step % 2
  std::string first;         // the first press of the current key
  std::string codes[6];
  uint32_t t = 0;            // last activity (timeout)
  std::string result;        // "", "ok", "aborted", "no_rx"
};
inline Learn LRN;
inline std::string KEYS[6];                 // learned codes, toggle bit 0
inline bool keys_learned = false;
inline bool has_receiver = false;
inline int FRAME_BITS = 0, TOGGLE_BIT = -1;
inline std::function<void(const std::string *, bool)> keys_store;   // saves the codes – provided by the YAML
inline std::function<void(const std::string &)> remote_pub;           // recognised press "seq|key|frames|held_ms" for the cards

inline std::string norm_bits(std::string b) {
  if (TOGGLE_BIT >= 0 && TOGGLE_BIT < (int) b.size()) b[TOGGLE_BIT] = '0';
  return b;
}
// learned code of a key, "" = use the profile
inline std::string learned_code(const std::string &key) {
  if (!keys_learned) return "";
  for (int i = 0; i < 6; i++) if (key == KEY_NAMES[i]) return KEYS[i];
  return "";
}
// key of a received frame by the learned codes, "" = unknown
inline std::string key_of_bits(const std::string &bits) {
  if (!keys_learned || bits.empty()) return "";
  std::string n = norm_bits(bits);
  for (int i = 0; i < 6; i++) if (KEYS[i] == n) return KEY_NAMES[i];
  return "";
}

inline void learn_frame(const std::string &bits) {
  LRN.t = now_ms();
  if ((int) bits.size() != FRAME_BITS) {
    ESP_LOGW(TAG, "learning: a frame of %d bits, the profile has %d – another remote family?", (int) bits.size(), FRAME_BITS);
    return;
  }
  std::string n = norm_bits(bits);
  int k = LRN.step / 2;
  for (int i = 0; i < k; i++)
    if (LRN.codes[i] == n) { ESP_LOGW(TAG, "learning: that was %s again – press %s", KEY_NAMES[i], KEY_NAMES[k]); return; }
  if (LRN.step % 2 == 0) {
    LRN.first = n; LRN.step++;
    ESP_LOGI(TAG, "learning %s: once more", KEY_NAMES[k]);
    return;
  }
  if (n != LRN.first) {      // a different frame: it becomes the first press
    LRN.first = n;
    ESP_LOGW(TAG, "learning %s: a different frame – press the same key once more", KEY_NAMES[k]);
    return;
  }
  LRN.codes[k] = n; LRN.step++;
  ESP_LOGI(TAG, "learned %s: %s", KEY_NAMES[k], n.c_str());
  if (LRN.step < 12) { ESP_LOGI(TAG, "learning: press %s twice", KEY_NAMES[k + 1]); return; }
  for (int i = 0; i < 6; i++) KEYS[i] = LRN.codes[i];
  keys_learned = true;
  if (keys_store) keys_store(KEYS, true);
  LRN.active = false; LRN.result = "ok";
  B.boot_t = now_ms();       // the presses changed the hood – not a manual takeover of a session
  ESP_LOGI(TAG, "remote learned – lines for a profile:");
  for (int i = 0; i < 6; i++) ESP_LOGI(TAG, "  key_%s: \"%s\"", KEY_NAMES[i], KEYS[i].c_str());
}

// event from the receiver of the original remote: "seq|key|frames|held_ms|bits" (bits since 0.2.0;
// key = "unknown" for a frame the receiver's profile does not know)
inline void on_remote(const std::string &v) {
  static std::string last_seq;
  size_t a = v.find('|'); if (a == std::string::npos) return;
  std::string seq = v.substr(0, a);
  if (seq == last_seq) return;
  bool first = last_seq.empty(); last_seq = seq;
  if (first) return;                                   // state restored at boot, not a new press
  size_t b = v.find('|', a + 1), c = v.find('|', b + 1);
  if (b == std::string::npos || c == std::string::npos) return;
  std::string k = v.substr(a + 1, b - a - 1);
  int held = atoi(v.substr(c + 1).c_str());
  size_t d = v.find('|', c + 1);
  std::string bits = d == std::string::npos ? "" : v.substr(d + 1);
  if (LRN.active) { if (!bits.empty()) learn_frame(bits); return; }
  std::string lk = key_of_bits(bits);                  // learned codes win over the receiver's profile
  if (!lk.empty()) k = lk;
  if (k == "unknown") return;
  if (remote_pub) remote_pub(seq + "|" + k + "|" + v.substr(b + 1, c - b - 1) + "|" + std::to_string(held));
  if (age_s(last_tx) < held / 1000.0f + 2.5f) return;  // echo of the own transmitter
  ESP_LOGI(TAG, "remote: %s (%d ms)", k.c_str(), held);
  if (k == "light") { on_cmd(E.pl ? "light_off" : "light_on"); manual_light(); }
  else if (k == "fan_plus") { on_cmd(held >= 1200 ? "boost" : "fan_plus"); manual_fan(); }
  else if (k == "fan_minus") { on_cmd(held >= 1200 ? "stop" : "fan_minus"); manual_fan(); }
  else if ((k == "brighter" || k == "dimmer") && held >= 2500) colour = k == "brighter" ? "cool" : "warm";
  else if (k == "brighter" || k == "dimmer") on_cmd("brightness");
  else if (k == "timer" && E.pb > 0) timer_end = now_ms() + 600000;
}

inline void end_session() {
  B.session = B.manual = B.auto_lit = false;
  B.phase = 0; B.start_wait = 0; B.rec_pending = false;
}

inline void loop_brain() {
  uint32_t t = now_ms();
  update_avg();
  float hp = std::isnan(IN.hob_p) ? 0 : IN.hob_p;
  bool hob_on_raw = IN.has_hob_va ? (!std::isnan(IN.hob_va) && IN.hob_va > C.hob_on_thr) : (hp > 60 || B.avg2 > 30);
  bool hob_changed = B.hob_on.update(hob_on_raw, 0, 5);
  // hob switched OFF (apparent power source): cooking ends after 10 s and the average restarts – without it the
  // 2-min average and the 90 s pause tolerance kept "cooking" ~3 min after the hob was off (test 04.11)
  bool hob_off = IN.has_hob_va && !B.hob_on.state;
  if (hob_off && !B.hob_hist.empty() && B.avg2 > 0) { B.hob_hist.clear(); B.avg2 = 0; }
  B.cook.update(!hob_off && (hp > 60 || B.avg2 > 30), 10, hob_off ? 10 : 90);
  B.hob_on_edge = hob_changed && B.hob_on.state;
  B.hob_off_edge = hob_changed && !B.hob_on.state;
  B.aux1.update(active_value(IN.aux1, C.aux1_thr), 20, 180);
  B.aux2.update(active_value(IN.aux2, C.aux2_thr), 0, 60);
  B.cooking = B.cook.state || (C.aux1_on && B.aux1.state) || (C.aux2_on && B.aux2.state);
  B.rec = recommended();

  if (!B.init) {   // first pass: take the current states as they are, no edges – except cooking (see below)
    if (std::isnan(IN.hood_p)) return;
    // after a restart during cooking the session starts again (automatic): prev_cooking = false makes the edge
    B.init = true; B.boot_t = t; B.prev_cooking = false; B.prev_rec = B.rec; B.prev_pl = E.pl; B.prev_sw = IN.hood_sw;
    B.prev_home = IN.home;
    return;
  }
  bool enabled = C.auto_on && C.profile != 0;

  // ---------------- cooking start / end
  if (B.cooking && !B.prev_cooking) {
    B.cooking_since = t;
    if (enabled) {
      // fan running before cooking = manual, unless it was started by the drying (the session takes it over)
      // a fan running in the first minute after a restart was the automation's own, not a manual start
      bool was_manual = B.session ? B.manual : (E.pb > 0 && !B.drying && age_s(B.boot_t) > 60);
      if (B.drying) { B.drying = false; ESP_LOGI(TAG, "drying ends – cooking takes over"); }
      B.manual = was_manual; B.session = true; B.phase = 1;
      B.cook_seen = B.cook.state; B.aux1_seen = C.aux1_on && B.aux1.state;
      if (C.auto_light) light_on_auto();
      B.start_wait = 1; B.start_t = t; B.rec_pending = false;
      ESP_LOGI(TAG, "cooking start (manual=%d)", was_manual);
    }
  }
  if (B.cooking) { B.cook_seen |= B.cook.state; B.aux1_seen |= (C.aux1_on && B.aux1.state); }
  if (!B.cooking && B.prev_cooking && B.session) {
    bool aux2_only = !B.cook_seen && !B.aux1_seen;
    float runon = aux2_only ? C.runon_aux_min : C.runon_min;
    if (!B.manual && runon > 0 && E.pb > 0) request(J_SPEED, 1);
    B.phase = 2; B.runon_end = t + (uint32_t) (runon * 60000); B.start_wait = 0; B.rec_pending = false;
    ESP_LOGI(TAG, "cooking end – run-on %.0f min", runon);
  }
  B.prev_cooking = B.cooking;

  // start: speed when the recommendation is known (up to 10 s), after the light job
  if (B.start_wait == 1 && B.phase == 1) {
    if (B.rec > 0 || age_s(B.start_t) > 10) {
      B.start_wait = 0;
      if (!B.manual && B.rec > 0) request(J_SPEED, B.rec);
    }
  }
  // recommendation changed during cooking: debounce 20 s
  if (B.rec != B.prev_rec) {
    if (B.phase == 1 && B.session && !B.manual && B.start_wait == 0 && age_s(B.cooking_since) > 15) {
      B.rec_pending = true; B.rec_change = t;
    }
    B.prev_rec = B.rec;
  }
  if (B.rec_pending && age_s(B.rec_change) >= 20) {
    B.rec_pending = false;
    if (B.phase == 1 && !B.manual) request(J_SPEED, B.rec);
  }
  // run-on finished
  if (B.phase == 2 && (int32_t) (t - B.runon_end) >= 0) {
    if (!B.manual && E.pb > 0) request(J_SPEED, 0);
    if (B.auto_lit && E.pl) request(J_LIGHT, 0);
    end_session();
    ESP_LOGI(TAG, "session end");
  }
  if (!C.auto_on && B.session) end_session();

  // ---------------- hob switched ON (without heating) -> light; OFF for N min -> light off
  if (B.hob_on_edge) {
    if (enabled && C.auto_light && !E.pl) light_on_auto();
    B.hob_off_check = false;
  }
  if (B.hob_off_edge) { B.hob_off_t = t; B.hob_off_check = true; }
  if (B.hob_off_check && age_s(B.hob_off_t) >= C.light_after_min * 60 + 5) {
    B.hob_off_check = false;
    if (enabled && !B.session && !B.cooking && !B.hob_on.state && B.auto_lit) {
      if (E.pl) request(J_LIGHT, 0);
      B.auto_lit = false;
    }
  }

  // ---------------- manual change detected from the power (speed stable 15 s, differs from the target)
  if (E.pb != B.stable_pb) { B.stable_pb = E.pb; B.stable_since = t; }
  if (B.session && !B.manual && Q.empty() && B.phase == 1 && B.start_wait == 0 && !B.rec_pending &&
      age_s(B.stable_since) > 15 && age_s(last_tx) > 15 && E.pb != B.rec && age_s(B.cooking_since) > 60) {
    B.manual = true;
    ESP_LOGI(TAG, "manual change detected: speed %d (recommended %d)", E.pb, B.rec);
  }

  // ---------------- colour by time of day
  if (E.pl && !B.prev_pl) { B.light_on_t = t; B.light_on_pending = true; }
  if (!E.pl) B.light_on_pending = false;
  B.prev_pl = E.pl;
  if (C.auto_colour && E.pl && C.now_min >= 0) {
    bool due = (B.light_on_pending && age_s(B.light_on_t) >= 4) ||
               (B.prev_min >= 0 && C.now_min != B.prev_min && (C.now_min == C.cold_from || C.now_min == C.warm_from));
    if (due) {
      B.light_on_pending = false;
      bool zimna = C.cold_from <= C.warm_from ? (C.now_min >= C.cold_from && C.now_min < C.warm_from)
                                              : !(C.now_min >= C.warm_from && C.now_min < C.cold_from);
      std::string want = zimna ? "cool" : "warm";
      bool queued = false;
      for (auto &j : Q) if (j.type == J_COLOUR) queued = true;
      if (want != colour && !queued) { Job c; c.type = J_COLOUR; c.arg = zimna ? 0 : 1; Q.push_back(c); }
    }
  }
  B.prev_min = C.now_min;

  // ---------------- timer
  bool timer_on = timer_end && (int32_t) (timer_end - t) > 0;
  if (B.timer_was && !timer_on && timer_end) { timer_end = 0; if (E.pb > 0) request(J_SPEED, 0); }
  B.timer_was = timer_on;

  // ---------------- hob alarm: heating longer than N min, or nobody home while heating
  if (C.alarm_on && B.cook.state) {
    bool too_long = age_s(B.cook.changed) >= C.alarm_min * 60;
    bool nobody = !std::isnan(IN.home) && IN.home == 0 && (std::isnan(B.prev_home) || B.prev_home > 0);
    if ((too_long || nobody) && !B.alarm_sent) {
      B.alarm = true; B.alarm_sent = true;
      if (notify) notify(nobody ? "nobody_home" : "too_long", hp, age_s(B.cook.changed) / 60.0f);
    }
  }
  if (!B.cook.state && B.alarm) { B.alarm = false; B.alarm_sent = false; if (notify) notify("end", 0, 0); }
  B.prev_home = IN.home;

  // ---------------- pairing after a power cut: relay back on -> fan − within the 60 s pairing window
  if (IN.hood_sw == "on" && B.prev_sw != "on" && !B.prev_sw.empty() && (std::isnan(IN.hood_p) || IN.hood_p < 2 || B.prev_sw == "off"))
    B.pair_at = t + 5000;
  B.prev_sw = IN.hood_sw;
  if (B.pair_at && (int32_t) (t - B.pair_at) >= 0) { B.pair_at = 0; request_raw("fan_minus", RF_SHORT); ESP_LOGI(TAG, "pairing after power-up"); }

  // ---------------- drying: kitchen humid, outdoor air drier → fan 1 until the humidity drops (max N min)
  B.room_ah = abs_hum(IN.room_t, IN.room_rh);
  B.out_ah = abs_hum(IN.out_t, IN.out_rh);
  if (!std::isnan(IN.room_rh) && IN.room_rh > C.dry_start) { if (!IN.room_rh_above_since) IN.room_rh_above_since = t; }
  else IN.room_rh_above_since = 0;
  if (B.drying) {
    bool done = (!std::isnan(IN.room_rh) && IN.room_rh <= C.dry_stop) || age_s(B.dry_t) >= C.dry_max_min * 60 || !C.dry_on;
    bool manual_change = B.dry_started_fan && Q.empty() && age_s(B.dry_t) > 40 && E.pb != 1;   // someone changed the speed
    if (manual_change) { B.drying = false; ESP_LOGI(TAG, "drying ends – manual change"); }
    else if (done) { B.drying = false; if (E.pb == 1) request(J_SPEED, 0); ESP_LOGI(TAG, "drying ends"); }
    if (!B.dry_started_fan && E.pb >= 1) B.dry_started_fan = true;
  } else if (C.dry_on && !B.session && !B.cooking) {
    int m15 = C.now_min < 0 ? -1 : C.now_min / 15;
    // like the HA trigger: humidity above the start level for 10 min, or every 15 min
    static bool above_checked = false;
    if (!IN.room_rh_above_since) above_checked = false;
    bool above10 = IN.room_rh_above_since && age_s(IN.room_rh_above_since) >= 600 && !above_checked;
    if (above10) above_checked = true;
    bool check = (m15 >= 0 && m15 != B.prev_min15) || above10;
    B.prev_min15 = m15;
    if (check && in_window(C.now_min, C.dry_from, C.dry_to) && IN.room_rh > C.dry_start && IN.out_t > C.dry_tmin &&
        E.pb == 0 && Q.empty() && !std::isnan(B.room_ah) && !std::isnan(B.out_ah) && B.room_ah - B.out_ah >= C.dry_diff) {
      B.drying = true; B.dry_t = t; B.dry_started_fan = false;
      request(J_SPEED, 1);
      ESP_LOGI(TAG, "drying starts: %.0f %% RH, %.1f vs %.1f g/m3", IN.room_rh, B.room_ah, B.out_ah);
    }
  }
  B.dry_state = !C.dry_on ? 0 : B.drying ? 2 : 1;

  // ---------------- night reset: relay off 15 s when the hood still draws power (fan / light certainly off, no RF)
  if (C.night_on && C.now_min == C.night_at && B.night_done_day_min != C.now_min && B.night_phase == 0) {
    B.night_done_day_min = C.now_min;
    if (B.drying) B.drying = false;
    if (!std::isnan(IN.hood_p) && IN.hood_p > 1 && hood_power_off && IN.hood_sw.size()) {
      hood_power_off(); B.night_phase = 1; B.night_t = t; ESP_LOGI(TAG, "night reset: relay off");
    }
  }
  if (C.now_min != C.night_at) B.night_done_day_min = -1;
  if (B.night_phase == 1 && age_s(B.night_t) >= 15) {
    if (hood_power_on) hood_power_on();
    on_cmd("stop"); set_state(0, false, E.L);
    B.night_phase = 0;
  }

  // 0 automation off, 1 standby, 2 cooking – automatic, 3 cooking – manual, 4 run-on
  B.mode = !C.auto_on ? 0 : B.phase == 1 ? (B.manual ? 3 : 2) : B.phase == 2 ? 4 : 1;
}

// ================================================================== statistics (energy, grease filter)
struct Stats {
  double today_wh = 0;          // hob energy today
  int day = -1;                 // day of the year of today_wh
  double filter_h = 0;          // weighted fan hours since the filter was cleaned
  double sess_wh = 0;           // energy of the running cooking
  uint32_t sess_start = 0;
  float last_kwh = NAN, last_min = NAN;
  bool cooking = false;
  uint32_t last_t = 0;
};
inline Stats S;
// how fast each speed loads the grease filter (speed 0–3, boost)
static const float FILTER_W[5] = {0.0f, 0.5f, 1.0f, 1.5f, 2.5f};

inline void loop_stats(int day_of_year) {
  uint32_t t = now_ms();
  if (S.last_t == 0) { S.last_t = t; return; }
  float dt = (t - S.last_t) / 1000.0f;
  S.last_t = t;
  if (dt <= 0 || dt > 10) return;               // skipped loop – do not integrate a gap
  if (day_of_year >= 0) {
    if (S.day >= 0 && day_of_year != S.day) S.today_wh = 0;   // new day
    S.day = day_of_year;
  }
  float hp = std::isnan(IN.hob_p) ? 0.0f : std::fmax(IN.hob_p, 0.0f);
  S.today_wh += hp * dt / 3600.0;
  if (E.pb >= 1 && E.pb <= 4) S.filter_h += FILTER_W[E.pb] * dt / 3600.0;
  // cooking session: start / end follow the brain's cooking state
  if (B.cooking && !S.cooking) { S.sess_wh = 0; S.sess_start = t; }
  if (B.cooking) S.sess_wh += hp * dt / 3600.0;
  if (!B.cooking && S.cooking && S.sess_start) {
    S.last_kwh = S.sess_wh / 1000.0;
    S.last_min = (t - S.sess_start) / 60000.0f;
  }
  S.cooking = B.cooking;
}

// ================================================================== calibration of the power table
// Presses the remote keys directly (normal jobs check against the table being measured), waits for a stable power
// at every step and takes the median of 4 s of readings. Order: standby, fan 1–3, boost, stop, light 25–100 %, off.
// While it runs, the hob automation and the jobs are paused; cooking or a second press of the button aborts it.
struct Cal {
  bool active = false;
  int step = 0;            // 0–10
  int phase = 0;           // 0 act, 1 wait for a stable power, 2 sample
  uint32_t t = 0;
  std::vector<float> samples;
  float v[10] = {0};       // standby, f1, f2, f3, boost, (stop), l1, l2, l3, l4 – raw W
  std::string result;      // "", "ok", "close:a~b", "failed", "aborted"
};
inline Cal CAL;
inline std::function<void(const float *, bool)> cal_store;   // saves the table (net W) – provided by the YAML
inline const char *const CAL_NAMES[10] = {"standby", "fan1", "fan2", "fan3", "boost", "stop", "light1", "light2", "light3", "light4"};

inline void cal_start() {
  if (CAL.active) { CAL.active = false; CAL.result = "aborted"; ESP_LOGW(TAG, "calibration aborted"); return; }
  Q.clear(); job_active = false; target_speed = -1; target_light = -1;
  CAL = Cal(); CAL.active = true; CAL.t = now_ms();
  ESP_LOGI(TAG, "calibration started");
}

inline float median(std::vector<float> v) {
  if (v.empty()) return NAN;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

inline void cal_finish() {
  CAL.active = false;
  float sb = CAL.v[0];
  float f[4] = {0, CAL.v[1] - sb, CAL.v[2] - sb, CAL.v[3] - sb};
  float boost = CAL.v[4] - sb;
  float l[4] = {CAL.v[6] - sb, CAL.v[7] - sb, CAL.v[8] - sb, CAL.v[9] - sb};
  bool mono = f[1] > 0.5f && f[2] > f[1] && f[3] > f[2] && boost > f[3] && l[0] > 0.5f && l[1] > l[0] && l[2] > l[1] && l[3] > l[2];
  if (!mono) {
    CAL.result = "failed";
    ESP_LOGW(TAG, "calibration failed: %.1f | %.1f %.1f %.1f %.1f | %.1f %.1f %.1f %.1f", sb, f[1], f[2], f[3], boost, l[0], l[1], l[2], l[3]);
    return;
  }
  // states that differ by < 0.8 W are hard to tell apart (e.g. fan 1 vs light 25 %)
  CAL.result = "ok";
  const char *fn[3] = {"fan1", "fan2", "fan3"}, *ln[4] = {"light1", "light2", "light3", "light4"};
  for (int i = 1; i <= 3 && CAL.result == "ok"; i++)
    for (int k = 0; k < 4; k++)
      if (std::fabs(f[i] - l[k]) < 0.8f) { CAL.result = std::string("close:") + fn[i - 1] + "~" + ln[k]; break; }
  float table[9] = {sb, f[1], f[2], f[3], boost * 0.8f, l[0], l[1], l[2], l[3]};   // boost threshold: 80 % of the measured
  set_profile(sb, f[1], f[2], f[3], boost * 0.8f, l[0], l[1], l[2], l[3], RF_SHORT, RF_HOLD, RF_COLOUR);
  if (cal_store) cal_store(table, true);
  set_state(0, false, l[3]);
  ESP_LOGI(TAG, "calibration %s – profile lines:", CAL.result.c_str());
  ESP_LOGI(TAG, "  pw_standby: \"%.1f\"\n  pw_fan1: \"%.1f\"\n  pw_fan2: \"%.1f\"\n  pw_fan3: \"%.1f\"\n  pw_boost_min: \"%.0f\"", sb, f[1], f[2], f[3], boost * 0.8f);
  ESP_LOGI(TAG, "  pw_light1: \"%.1f\"\n  pw_light2: \"%.1f\"\n  pw_light3: \"%.1f\"\n  pw_light4: \"%.1f\"", l[0], l[1], l[2], l[3]);
}

inline void loop_cal() {
  if (!CAL.active) return;
  uint32_t t = now_ms();
  float el = (t - CAL.t) / 1000.0f;
  auto go = [&](int ph) { CAL.phase = ph; CAL.t = t; };
  if (CAL.phase == 0) {          // act
    switch (CAL.step) {
      case 0:  // stop and light off
        if (E.pb > 0) press("fan_minus", RF_HOLD);
        if (E.pl) press("light");
        break;
      case 1: case 2: case 3: case 4: press("fan_plus"); break;
      case 5: press("fan_minus", RF_HOLD); break;
      case 6: press("light"); for (int i = 0; i < 4; i++) press("dimmer"); break;   // on, then down to 25 %
      case 7: case 8: case 9: press("brighter"); break;
      case 10: press("light"); cal_finish(); return;
    }
    go(1);
    return;
  }
  if (!tx_idle()) { CAL.t = t; return; }
  if (CAL.phase == 1) {          // wait for a stable power
    // fan: the motor ramps up (~12 s); light: the LEDs settle after a change (test 04.11: 25 % read 0.8 W high at 13 s)
    bool light = CAL.step >= 6 && CAL.step <= 9;
    float min_wait = light ? 20.0f : 10.0f;
    float stable = light ? 8.0f : 5.0f;
    float max_wait = light ? 50.0f : 45.0f;
    if ((el >= min_wait && power_stable_s() >= stable) || el >= max_wait) { CAL.samples.clear(); go(2); }
    return;
  }
  // sample 4 s
  if (!std::isnan(IN.hood_p)) CAL.samples.push_back(IN.hood_p);
  if (el < 4) return;
  CAL.v[CAL.step] = median(CAL.samples);
  ESP_LOGI(TAG, "calibration %d/10 %s: %.1f W", CAL.step + 1, CAL_NAMES[CAL.step], CAL.v[CAL.step]);
  CAL.step++;
  go(0);
}

inline void learn_start() {
  if (LRN.active) { LRN.active = false; LRN.result = "aborted"; ESP_LOGW(TAG, "learning aborted"); return; }
  if (CAL.active) return;
  if (!has_receiver) { LRN.result = "no_rx"; ESP_LOGW(TAG, "learning: set \"Source: remote receiver\" first"); return; }
  Q.clear(); job_active = false; target_speed = -1; target_light = -1;
  LRN = Learn(); LRN.active = true; LRN.t = now_ms();
  ESP_LOGI(TAG, "learning started – press %s twice", KEY_NAMES[0]);
}

// everything, every ~200 ms
inline void loop() {
  if (!IN.hood_p_evaluated && now_ms() - IN.hood_p_change >= 5000) { IN.hood_p_evaluated = true; evaluate('p'); }
  static uint32_t last_resync = 0;
  if (now_ms() - last_resync > 60000) { last_resync = now_ms(); evaluate('r'); }
  if (CAL.active) {
    // the hob automation is paused, so look at the hob directly: heating or switched ON aborts the calibration
    bool hob = (!std::isnan(IN.hob_p) && IN.hob_p > 60) || (IN.has_hob_va && !std::isnan(IN.hob_va) && IN.hob_va > C.hob_on_thr);
    if (hob) {
      CAL.active = false; CAL.result = "aborted";
      B.boot_t = now_ms();   // the fan running from the calibration is not a manual start: the session starts automatic
      ESP_LOGW(TAG, "calibration aborted – the hob is on");
    }
    loop_cal();
    return;
  }
  if (LRN.active) {           // the automation waits; 90 s without a press ends the wizard
    if (now_ms() - LRN.t > 90000) { LRN.active = false; LRN.result = "aborted"; ESP_LOGW(TAG, "learning aborted – no press for 90 s"); }
    return;
  }
  loop_jobs();
  loop_brain();
}

}  // namespace hbh
