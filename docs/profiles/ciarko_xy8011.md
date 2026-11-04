# Profile: Ciarko XY8011-RF – remote protocol

Measured on the remote of a Ciarko Duo 74 (Broadlink capture, RTL-SDR with `rtl_433`, the RF433R receiver)
and cross-checked with the XY8011-RF remote of a Ciarko SU Mini
([AdamWeglarz/ciarko-hood-433-esphome](https://github.com/AdamWeglarz/ciarko-hood-433-esphome)).

## Radio

- OOK at **433.92 MHz** (the remote measured ~433.93 MHz). A Broadlink RM4 Pro transmits at ~433.795 MHz,
  which the hood receives only sometimes.
- PWM bits, period ≈ 1.05 ms:

| | HIGH | LOW |
|---|---|---|
| `1` | 790 µs | 263 µs |
| `0` | 263 µs | 790 µs |
| frame end (sync) | 1710 µs | ~4900 µs |

- A frame is 36 bits followed by the sync; one frame takes ≈ 44.5 ms.
- A key press is a burst of frames **back to back**; holding a key simply continues the burst. A short press of
  the remote is 2–4 frames; the transmitter sends 8.
- Different receivers report pulses up to ~90 µs shorter or longer – compare captures on the same receiver.

## Frame

```
 IIIIIIIIIIIIIIII | 00011000 | CCCCCCCC | T | 010
 remote ID (16)   | key (8)  | check (8)| T | fixed
```

- **Remote ID** – differs between remotes (e.g. SU Mini: `1010000100001001`). The profile uses the
  placeholder `0x1234` – learn your own.
- **Key code** – the same on both remotes:

| Key code | Duo 74 key | SU Mini label | Short press | Hold |
|---|---|---|---|---|
| `0x18` | light | Light | light on / off | – |
| `0x50` | fan − | Left | one speed down | ~2 s = **stop** |
| `0xFD` | fan + | Right | one speed up (3 → boost) | from a stopped fan: speed 1–2 only, not boost |
| `0xFE` | timer | Timer | hood timer (LED blinks, stops after ~10 min) | – |
| `0x11` | key 4 | Fan + | brighter (4 levels) | ≥3 s = **cooler colour**, continuously |
| `0x0B` | key 5 | Fan − | dimmer | ≥3 s = **warmer colour** |

- **Check byte** – depends on the ID and the key. It is not a plain CRC-8 over ID + key (all polynomials and init
  values tested, MSB- and LSB-first), nor a sum or XOR of the bytes or nibbles – so codes of a new remote must be learned (`tools/learn_codes.py`, profile `esphome/profiles/ciarko_xy8011.yaml`).
- **T – toggle bit** (bit 33 counted from 1). It flips on every new press and stays the same while a key is held.
  The hood ignores a press whose toggle bit equals the previous one – it takes it as the same key still held.
  A transmitter with a fixed toggle bit works only every second press of the same key.
- The last three bits are always `010`.

## Behaviour of the hood

- After power-up the hood pairs for **60 s** with the first remote it hears. If that is the original remote, it
  ignores a second transmitter until the next power cycle.
- Two presses need a gap of silence between them (~0.4 s works; 0.14 s is taken as one press).
- The light comes back at the **last brightness**; there are 4 brightness levels.
- The colour changes smoothly while key 4/5 is held; a 6 s hold reaches the end of the scale from any colour.
