#!/usr/bin/env python3
"""Check every hood profile in esphome/profiles/ – without a hood.

    python3 tools/test_profiles.py            # profile checks + round trips (offline)
    python3 tools/test_profiles.py --online   # also decode published raw captures of real remotes
    python3 tools/test_profiles.py --cpp      # also check the C++ decoder (needs g++)

1. Structure: all rf_* settings and all key_* keys present, codes made of 0/1, frames of the right length, no two
   keys with the same frame, every key of rf_learn_keys defined.
2. Round trip: every key is turned into pulses the way the transmitter does it (hbh.yaml, script send_rf) and
   decoded the way the receiver does it (hbh-remote-rx.yaml, on_raw) – it has to come back as the same key; also
   with timing jitter (±15 %) and marks shortened by 90 µs, like a cheap receiver reports them.
3. --cpp: the same packets are decoded by the C++ decoder of the devices (esphome/hbh_rx.h, compiled on the host
   with g++) – it has to give exactly the bits of the Python mirror below.
4. --online: raw captures of real remotes published by others are downloaded (not stored in this repo) and decoded
   with the profile – every capture has to come out as its key:
   Elica (Flipper Zero, UberGuidoZ/Flipper), Neff SF01 01319004 (SIGNALduino, RFD-FHEM/RFFHEM).

The encoder and decoder below mirror the C++ lambdas of the YAML files – change both together.
"""
import glob
import os
import random
import re
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYS = ['light', 'fan_minus', 'fan_plus', 'timer', 'brighter', 'dimmer', 'light_on', 'light_off', 'fan_off', 'power',
        'speed_1', 'speed_2', 'speed_3', 'speed_4']
RF = ['rf_one_high', 'rf_one_low', 'rf_zero_high', 'rf_zero_low', 'rf_start_high', 'rf_bit_order', 'rf_sync_high',
      'rf_sync_low', 'rf_frame_bits', 'rf_toggle_bit', 'rf_short_frames', 'rf_hold_frames', 'rf_colour_frames',
      'rf_learn_keys', 'profile_name']
errors = []
VECTORS = []   # (profile, packet, bits of the Python decoder) – for --cpp


def load(path):
    return dict(re.findall(r'^  (\w+): "([^"]*)"', open(path, encoding='utf-8').read(), re.M))


def key_codes(P):
    return {k: P['key_' + k] for k in KEYS if P.get('key_' + k)}


# ------------------------------------------------------------------ transmitter (hbh.yaml, send_rf)
def encode(code, P, frames=3):
    start, sh, sl = int(P['rf_start_high']), int(P['rf_sync_high']), int(P['rf_sync_low'])
    lh = P['rf_bit_order'] == 'lh'
    out = [-20000]
    for _ in range(frames):
        if start > 0:
            out.append(start)
        for b in code:
            h = int(P['rf_one_high'] if b == '1' else P['rf_zero_high'])
            l = int(P['rf_one_low'] if b == '1' else P['rf_zero_low'])
            out += [-l, h] if lh else [h, -l]
        if sh > 0:
            out.append(sh)
        out.append(-sl)
    return out


# ------------------------------------------------------------------ remote_receiver: filter 120 µs, idle 3 ms
def packets(raw, idle=3000, filt=120):
    out, cur = [], []
    for v in raw:
        if abs(v) < filt:
            continue
        if v < 0 and -v > idle:
            if cur:
                out.append(cur)
            cur = []
            continue
        if cur and (cur[-1] > 0) == (v > 0):
            cur[-1] += v
        elif cur or v > 0:
            cur.append(v)
    if cur:
        out.append(cur)
    return out


# ------------------------------------------------------------------ receiver (hbh-remote-rx.yaml, on_raw)
def decode(x, P):
    nb, sh, start = int(P['rf_frame_bits']), int(P['rf_sync_high']), int(P['rf_start_high'])
    h1, l1, h0, l0 = (int(P[k]) for k in ('rf_one_high', 'rf_one_low', 'rf_zero_high', 'rf_zero_low'))
    pmin = min(h1, l1, h0, l0) * 45 // 100
    pmax = max(h1, l1, h0, l0) * 3 // 2
    lens = {nb} | {len(c) for c in key_codes(P).values()}

    def bit(h, l):
        return '1' if abs(h - h1) + abs(l - l1) < abs(h - h0) + abs(l - l0) else '0'

    if P['rf_bit_order'] == 'lh':
        end = len(x) - (1 if x and x[-1] < 0 else 0)
        for st in range(0, max(0, end - 16)):
            if not (start * 6 // 10 <= x[st] <= start * 16 // 10):
                continue
            bits, i = '', st + 1
            while i + 1 < end:
                l, h = -x[i], x[i + 1]
                if h < pmin or h > pmax or l < pmin or l > pmax:
                    break
                bits += bit(h, l)
                i += 2
            if i + 1 >= end and len(bits) in lens:
                return bits
        return None
    for sync in range(2 * nb, len(x)):
        if not (sh * 55 // 100 <= x[sync] <= sh * 135 // 100):
            continue
        bits, i, ok = '', sync - 2 * nb, True
        while i < sync:
            h, l = x[i], -x[i + 1]
            if h < pmin or h > pmax or l < pmin or l > pmax:
                ok = False
                break
            bits += bit(h, l)
            i += 2
        if ok:
            return bits
    return None


def dec(x, P):
    bits = decode(x, P)
    VECTORS.append((P, x, bits or ''))
    return bits


def norm(code, P):
    tb = int(P['rf_toggle_bit'])
    return code if tb < 0 or tb >= len(code) else code[:tb] + '0' + code[tb + 1:]


def key_of(bits, P):
    for k, c in key_codes(P).items():
        if norm(c, P) == norm(bits, P):
            return k
    return '?'


# ------------------------------------------------------------------ checks
def check_profile(name, P):
    for k in RF:
        if k not in P:
            errors.append(f'{name}: missing {k}')
    for k in KEYS:
        if 'key_' + k not in P:
            errors.append(f'{name}: missing key_{k} (use "" when the remote has no such key)')
    if any(k not in P for k in RF):
        return
    if P['rf_bit_order'] not in ('hl', 'lh'):
        errors.append(f'{name}: rf_bit_order must be hl or lh')
    codes = key_codes(P)
    seen = {}
    for k, c in codes.items():
        if not re.fullmatch(r'[01]{8,58}', c):
            errors.append(f'{name}: key_{k} is not 8–58 bits of 0/1')
        if P['rf_bit_order'] == 'hl' and len(c) != int(P['rf_frame_bits']):
            errors.append(f'{name}: key_{k} has {len(c)} bits, rf_frame_bits is {P["rf_frame_bits"]}')
        n = norm(c, P)
        if n in seen:
            errors.append(f'{name}: key_{k} and key_{seen[n]} send the same frame')
        seen[n] = k
    for k in [w.strip() for w in P['rf_learn_keys'].split(',') if w.strip()]:
        if k not in KEYS:
            errors.append(f'{name}: rf_learn_keys has an unknown key "{k}"')
        elif k not in codes:
            errors.append(f'{name}: rf_learn_keys asks for "{k}" but the profile has no key_{k}')
    # round trips
    rnd = random.Random(1)
    for k, c in codes.items():
        for variant in ('clean', 'jitter', 'short marks'):
            raw = encode(c, P)
            if variant == 'jitter':
                raw = [int(v * rnd.uniform(0.85, 1.15)) for v in raw]
            elif variant == 'short marks':
                raw = [v - 90 if v > 0 else v + 90 for v in raw]
            got = [dec(p, P) for p in packets(raw)]
            if not got or any(g is None or norm(g, P) != norm(c, P) for g in got):
                errors.append(f'{name}: key_{k} does not survive the round trip ({variant}): {got}')


# ------------------------------------------------------------------ published captures (--online)
def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read().decode('utf-8', 'replace')


def check_online(profiles):
    n = 0
    # Elica, Flipper Zero RAW files
    P = profiles.get('elica_came18')
    base = 'https://raw.githubusercontent.com/UberGuidoZ/Flipper/main/Sub-GHz/Cooker_Hoods/Elica/'
    for f, key in [('light_toggle', 'light'), ('light_on', 'light_on'), ('light_off', 'light_off'),
                   ('fan_plus', 'fan_plus'), ('fan_minus', 'fan_minus'), ('fan_off', 'fan_off')]:
        txt = fetch(base + f + '_raw.sub')
        raw = [int(v) for line in txt.splitlines() if line.startswith('RAW_Data:') for v in line.split()[1:]]
        got = sorted({key_of(b, P) for b in (dec(p, P) for p in packets(raw)) if b})
        n += 1
        if got != [key]:
            errors.append(f'capture Elica {f}: decoded as {got}, expected {key}')
    # Neff SF01 01319004, SIGNALduino raw messages in the comments of 14_SD_UT.pm
    P = profiles.get('bsh_sf01_01319004')
    src = fetch('https://raw.githubusercontent.com/RFD-FHEM/RFFHEM/master/FHEM/14_SD_UT.pm')
    sec = src[src.index('(NEFF / Refsta Topdraft kitchen hood)'):src.index('(SF01_01319004_Typ2)')]
    want = {'plus': 'fan_plus', 'minus': 'fan_minus', 'light_on_off': 'light', 'delay': 'timer'}
    for m in re.finditer(r'#\s+(\w+)\s+\| get sduino_dummy raw (MU;;\S+)', sec):
        if m.group(1) not in want:
            continue
        p = {int(a): int(b) for a, b in re.findall(r'P(\d)=(-?\d+)', m.group(2))}
        raw = [-20000] + [p[int(c)] for c in re.search(r'D=(\d+)', m.group(2)).group(1)] + [-20000]
        got = sorted({key_of(b, P) for b in (dec(q, P) for q in packets(raw)) if b})
        n += 1
        if got != [want[m.group(1)]]:
            errors.append(f'capture Neff {m.group(1)}: decoded as {got}, expected {want[m.group(1)]}')
    return n


CPP = r'''
#include "hbh_rx.h"
#include <iostream>
#include <sstream>
int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    hbh_rx::Frame f;
    int lh, nl, n;
    in >> f.one_h >> f.one_l >> f.zero_h >> f.zero_l >> f.start >> f.sync_h >> f.sync_l >> f.nb >> f.tb >> lh >> nl;
    f.lh = lh != 0;
    std::vector<int> lens(nl);
    for (auto &v : lens) in >> v;
    in >> n;
    std::vector<int32_t> x(n);
    for (auto &v : x) in >> v;
    std::cout << "=" << hbh_rx::decode(f, lens, x) << "\n";
  }
  // the press tracker: 3 frames of a key, then release; a single unknown frame is not reported
  hbh_rx::Tracker t;
  hbh_rx::Press p;
  bool r1 = t.feed("light", "0101", '0', 1000, 40, &p) || t.feed("light", "0101", '0', 1045, 40, &p) ||
            t.feed("light", "0101", '0', 1090, 40, &p);
  bool r2 = t.poll(1400, 40, &p);
  std::cout << "T" << (r1 ? 1 : 0) << (r2 ? 1 : 0) << " " << p.key << " " << p.frames << " " << p.held_ms << "\n";
  hbh_rx::Press q;
  bool r3 = t.feed("unknown", "1111", '0', 2000, 40, &q);
  bool r4 = t.poll(2400, 40, &q);
  std::cout << "U" << (r3 ? 1 : 0) << (r4 ? 1 : 0) << "\n";
}
'''


def check_cpp():
    with tempfile.TemporaryDirectory() as d:
        src, exe = os.path.join(d, 't.cpp'), os.path.join(d, 't')
        open(src, 'w').write(CPP)
        r = subprocess.run(['g++', '-std=c++17', '-O1', '-I', os.path.join(ROOT, 'esphome'), src, '-o', exe], capture_output=True, text=True)
        if r.returncode:
            errors.append('C++ decoder does not compile:\n' + r.stderr[-2000:])
            return 0
        lines = []
        for P, x, _ in VECTORS:
            lens = sorted({int(P['rf_frame_bits'])} | {len(c) for c in key_codes(P).values()})
            f = [P[k] for k in ('rf_one_high', 'rf_one_low', 'rf_zero_high', 'rf_zero_low', 'rf_start_high', 'rf_sync_high',
                                'rf_sync_low', 'rf_frame_bits', 'rf_toggle_bit')]
            lines.append(' '.join(f + ['1' if P['rf_bit_order'] == 'lh' else '0', str(len(lens))] + [str(v) for v in lens] +
                                  [str(len(x))] + [str(v) for v in x]))
        out = subprocess.run([exe], input='\n'.join(lines) + '\n', capture_output=True, text=True).stdout.splitlines()
        res = [o[1:] for o in out if o.startswith('=')]
        if len(res) != len(VECTORS):
            errors.append(f'C++ decoder: {len(res)} results for {len(VECTORS)} packets')
            return 0
        diff = [(i, VECTORS[i][2], res[i]) for i in range(len(res)) if VECTORS[i][2] != res[i]]
        for i, py, cpp in diff[:5]:
            errors.append(f'C++ decoder differs from the Python mirror (packet {i}): python {py!r}, c++ {cpp!r}')
        if 'T01 light 3 130' not in out:
            errors.append(f'C++ press tracker: expected "T01 light 3 130", got {[o for o in out if o.startswith("T")]}')
        if 'U00' not in out:
            errors.append('C++ press tracker reported a single unknown frame')
        return len(res)


def main():
    profiles = {os.path.basename(f)[:-5]: load(f) for f in sorted(glob.glob(os.path.join(ROOT, 'esphome', 'profiles', '*.yaml')))}
    for name, P in profiles.items():
        check_profile(name, P)
    print(f'{len(profiles)} profiles, {sum(len(key_codes(P)) for P in profiles.values())} keys: structure and round trips checked')
    if '--online' in sys.argv[1:]:
        print(f'{check_online(profiles)} published captures decoded')
    if '--cpp' in sys.argv[1:]:
        print(f'C++ decoder (esphome/hbh_rx.h): {check_cpp()} packets compared with the Python mirror')
    if errors:
        print('\n'.join(errors))
        sys.exit(1)
    print('ok')


if __name__ == '__main__':
    main()
