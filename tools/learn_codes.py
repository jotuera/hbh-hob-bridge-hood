#!/usr/bin/env python3
"""Learn the keys of a hood remote from the log of hbh-remote-rx.yaml and write the key_* lines of a profile.

The receiver logs every frame that matches no key of its profile as

    [10:12:31.402][I][rx:150]: unknown frame: 101000010000100100011000100000111010

so with the profile of the same remote family (same RF frame, other codes) you get your remote's frames. Press every
key of YOUR remote twice, in this order (default --keys):

    light, fan -, fan +, timer, brighter, dimmer

then stop the log with Ctrl+C. Live:

    esphome logs hbh-remote-rx.yaml | python3 tools/learn_codes.py -o my_remote.yaml

or from a saved log:

    python3 tools/learn_codes.py rx.log -o my_remote.yaml

Keys are taken in order of first appearance. The toggle bit is found as the bit that changes between two presses
of the same key (so press every key twice); it is ignored when grouping. Paste the printed key_* lines into the
substitutions of BOTH device files (hood and receiver) - they override the example codes of the profile. A remote of
another family: put them, with rf_frame_bits and rf_toggle_bit, into a new profile (esphome/profiles/...).
"""
import argparse
import re
import sys

FRAME = re.compile(r'(?:unknown frame|foreign remote): ([01]{8,})')
NAMES = ['light', 'fan_minus', 'fan_plus', 'timer', 'brighter', 'dimmer']
COMMENTS = {
    'light': 'light on / off',
    'fan_minus': 'fan - (hold = stop)',
    'fan_plus': 'fan +',
    'timer': 'timer',
    'brighter': 'brighter (hold = cooler colour)',
    'dimmer': 'dimmer (hold = warmer colour)',
}


def read(stream):
    frames = []
    try:
        for line in stream:
            m = FRAME.search(line)
            if m:
                frames.append(m.group(1))
                print('  frame', m.group(1), file=sys.stderr)
    except KeyboardInterrupt:
        pass
    return frames


def find_toggle(presses):
    """Bit that differs between consecutive presses of nearly the same frame (1 bit apart)."""
    votes = {}
    for a, b in zip(presses, presses[1:]):
        if len(a) != len(b):
            continue
        diff = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
        if len(diff) == 1:
            votes[diff[0]] = votes.get(diff[0], 0) + 1
    return max(votes, key=votes.get) if votes else -1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', nargs='?', help='saved ESPHome log (default: stdin)')
    ap.add_argument('-o', '--output', help='write the key_* lines here (default: stdout)')
    ap.add_argument('--keys', default=','.join(NAMES), help='order in which the keys were pressed')
    args = ap.parse_args()
    order_names = [k.strip() for k in args.keys.split(',') if k.strip()]

    frames = read(open(args.log, encoding='utf-8', errors='replace') if args.log else sys.stdin)
    if not frames:
        sys.exit('no "unknown frame" lines found - is the receiver on G32, with a profile of the same RF frame?')
    length = max(set(len(f) for f in frames), key=lambda n: sum(len(f) == n for f in frames))
    frames = [f for f in frames if len(f) == length]

    # one entry per press: consecutive identical frames are one press
    presses = [f for i, f in enumerate(frames) if i == 0 or f != frames[i - 1]]
    tb = find_toggle(presses)

    def norm(f):
        return f if tb < 0 else f[:tb] + '0' + f[tb + 1:]

    order, codes = [], {}
    for f in presses:
        k = norm(f)
        if k not in codes:
            codes[k] = f
            order.append(k)

    out = ['# Learned by tools/learn_codes.py - paste into substitutions: of both device files (or a new profile)',
           f'  rf_frame_bits: "{length}"',
           f'  rf_toggle_bit: "{tb}"' + ('' if tb >= 0 else '        # no toggle bit found - press every key twice')]
    for i, k in enumerate(order):
        name = order_names[i] if i < len(order_names) else f'extra_{i + 1}'
        line = f'  key_{name}: "{codes[k]}"'
        out.append(line.ljust(60) + f'# {COMMENTS.get(name, "")}')
    text = '\n'.join(out) + '\n'
    if args.output:
        open(args.output, 'w', encoding='utf-8').write(text)
        print(f'written {args.output}', file=sys.stderr)
    else:
        sys.stdout.write(text)
    print(f'{len(frames)} frames, {len(order)} keys, frame {length} bits, toggle bit {tb}', file=sys.stderr)


if __name__ == '__main__':
    main()
