// HbH – Hob bridge Hood: decoder of the original remote's frames, shared by hbh-remote-rx.yaml (a receiver of its
// own) and hbh-one.yaml (receiver and transmitter on one Atom). Plain C++ without ESPHome – tools/test_profiles.py
// compiles it on the host and checks it against the profiles and published captures of real remotes.
// Copyright (c) 2026 JoTu – PolyForm Noncommercial 1.0.0 (see LICENSE).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace hbh_rx {

// RF frame of the profile (rf_* substitutions, µs)
struct Frame {
  int one_h = 0, one_l = 0, zero_h = 0, zero_l = 0;
  int start = 0;      // start pulse (CAME), 0 = none
  int sync_h = 0;     // sync mark after the bits (PT2262 / Ciarko), 0 = none
  int sync_l = 0;     // gap after a frame
  int nb = 0;         // frame bits (nominal)
  int tb = -1;        // toggle bit, -1 = none
  bool lh = false;    // bit = (space, mark) – CAME; else (mark, space)
  int frame_ms() const { return (nb * (one_h + one_l + zero_h + zero_l) / 2 + start + sync_h + sync_l) / 1000; }
};

// one packet of remote_receiver (marks > 0, spaces < 0; it ends at a space longer than idle) -> bits, "" = none.
// lens: frame lengths of the profile (CAME remotes mix 12- and 18-bit frames; others are cut-off frames).
// candidate: a frame start / sync was found (counts incomplete frames).
inline std::string decode(const Frame &F, const std::vector<int> &lens, const std::vector<int32_t> &x, bool *candidate = nullptr) {
  const int pmin = std::min(std::min(F.one_h, F.one_l), std::min(F.zero_h, F.zero_l)) * 45 / 100;
  const int pmax = std::max(std::max(F.one_h, F.one_l), std::max(F.zero_h, F.zero_l)) * 3 / 2;
  auto bit = [&](int h, int l) {
    return (std::abs(h - F.one_h) + std::abs(l - F.one_l) < std::abs(h - F.zero_h) + std::abs(l - F.zero_l)) ? '1' : '0';
  };
  auto bad = [&](int h, int l) { return h < pmin || h > pmax || l < pmin || l > pmax; };
  if (candidate) *candidate = false;
  std::string bits;
  if (F.lh) {
    // CAME: start pulse, then (space, mark) per bit up to the end of the packet – the gap after a frame is longer
    // than idle, so every frame is its own packet
    int end = (int) x.size();
    if (end > 0 && x[end - 1] < 0) end--;
    for (int st = 0; st + 16 < end; st++) {
      if (x[st] < F.start * 6 / 10 || x[st] > F.start * 16 / 10) continue;
      if (candidate) *candidate = true;
      bits.clear();
      int i = st + 1;
      for (; i + 1 < end; i += 2) {
        int l = -x[i], h = x[i + 1];
        if (bad(h, l)) break;
        bits += bit(h, l);
      }
      if (i + 1 >= end && std::find(lens.begin(), lens.end(), (int) bits.size()) != lens.end()) return bits;
    }
    return "";
  }
  // nb bits (mark, space) followed by the frame-end pulse (sync mark); noise can contain a fake candidate, so try
  // all of them. Receivers report marks up to ~90 µs short – a short sync (PT2262, 1 T) still has to pass.
  for (int sync = 2 * F.nb; sync < (int) x.size(); sync++) {
    if (x[sync] < F.sync_h * 55 / 100 || x[sync] > F.sync_h * 135 / 100) continue;
    if (candidate) *candidate = true;
    bits.clear();
    int i = sync - 2 * F.nb;
    for (; i < sync; i += 2) {
      int h = x[i], l = -x[i + 1];
      if (bad(h, l)) break;
      bits += bit(h, l);
    }
    if (i >= sync) return bits;
  }
  return "";
}

// the toggle bit flips on every press – ignored for recognition
inline std::string norm(std::string b, int tb) {
  if (tb >= 0 && tb < (int) b.size()) b[tb] = '0';
  return b;
}
inline char toggle(const std::string &b, int tb) { return (tb >= 0 && tb < (int) b.size()) ? b[tb] : '0'; }

// a press: consecutive frames with the same bits and the same toggle bit
struct Press {
  std::string key;    // key name, "unknown" = not in the profile (nor learned)
  std::string bits;   // the frame, toggle bit 0
  int frames = 0;
  int held_ms = 0;
};

class Tracker {
 public:
  // a decoded frame; returns true when it ends the previous press (then `done` holds it)
  bool feed(const std::string &key, const std::string &nbits, char tgl, uint32_t now, int frame_ms, Press *done) {
    bool same = frames_ > 0 && nbits == bits_ && tgl == tgl_ && now - last_ < 300;
    bool out = false;
    if (same) {
      frames_++;
    } else {
      if (frames_ > 0) out = finish(frame_ms, done);
      key_ = key; bits_ = nbits; tgl_ = tgl; frames_ = 1; start_ = now;
    }
    last_ = now;
    return out;
  }
  // no frame for > 250 ms = the key was released
  bool poll(uint32_t now, int frame_ms, Press *done) {
    if (frames_ > 0 && now - last_ > 250) return finish(frame_ms, done);
    return false;
  }

 private:
  // an unknown frame counts only when repeated: a remote sends a burst, a single frame is more likely another device
  bool finish(int frame_ms, Press *done) {
    bool report = key_ != "unknown" || frames_ >= 2;
    if (report && done) {
      done->key = key_; done->bits = bits_; done->frames = frames_;
      done->held_ms = (int) (last_ - start_) + frame_ms;
    }
    frames_ = 0;
    return report;
  }
  std::string key_, bits_;
  char tgl_ = 0;
  int frames_ = 0;
  uint32_t start_ = 0, last_ = 0;
};

}  // namespace hbh_rx
