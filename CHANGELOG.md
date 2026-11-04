# Changelog

## Unreleased

- **Remote learning wizard**: *Learn remote* – the cards show which key of the original remote to press (each
  twice); the hood ESP keeps the codes and sends them instead of the profile's – no YAML, no re-flashing. The
  receiver now reports presses of an unknown remote too (`key: unknown` with the frame bits), and the hood ESP
  recognises the keys by the learned codes (new sensor *Remote key*). Cards 0.8.0.

## 0.1.0-beta – 2026-11-04

First public version of HbH – Hob bridge Hood (formerly CIARKO Hob Hood Bridge), with the
Ciarko XY8011-RF profile.

- **Transmitter** (M5Stack Atom Lite + Unit RF433T): the remote's frames at exactly 433.92 MHz, the whole burst in one
  buffer, the toggle bit flipped on every press like the original remote, 400 ms of silence between presses.
- **Hood profiles** (`esphome/profiles/`): RF frame, key codes and power table per hood – the core is the
  same for every hood; first profile: Ciarko XY8011-RF.
- **Brain on the ESP** (`hbh.h`):
  - hood state (fan 0–4, light, brightness 25–100 %) computed from the hood's supply power;
  - jobs that set the speed / light / brightness / colour / timer and check the result against the power;
  - hob → hood: light when the hob is switched on, fan from the hob's power with profiles H0–H6, run-on,
    manual takeover, colour by time of day, hob alarm, pairing after a power cut;
  - drying (kitchen vs outdoor absolute humidity) and a night reset of the hood relay;
  - all sources are Home Assistant entities chosen at runtime (also `entity:attribute`).
- **Receiver** (Atom Lite + Unit RF433R on G32): decodes the original remote, length of a hold, toggle bit;
  event `esphome.hbh_remote` and a text sensor for the brain; logs foreign remotes for `tools/learn_codes.py`.
- **Cards**: `hbh-card`, `hbh-automation-card`, `hbh-drying-card` – auto-discovery of the
  device, editor of the sources, last key of the original remote.
- **11 languages** of the entity names and the cards: en, cs, de, es, fr, hu, it, nl, pl, pt, sk; texts coming from the
  device are shown in the language of Home Assistant.
- **Calibration** of the hood's power table by the ESP itself (any hood), kept across restarts, printed as profile lines.
- **Statistics**: hob energy today and of the last cooking, grease-filter usage with a cleaning reminder.
- **Hobs on 1, 2 or 3 phases**: sources can be summed with `+`, configurable *Hob ON threshold* (VA or A).
- Fixes found in the first kitchen test: a 6 s colour hold no longer trips the task watchdog; switching the hob off ends
  cooking after 10 s (the 2-min average kept it ~3 min); a restart during cooking resumes the automatic session.
- **Example key codes** in the profile (placeholder remote ID) – every user learns their own remote's codes and puts
  them into the substitutions of both device files.
- **CI**: language files and card tables, `esphome config` of both devices in all languages, compile with ESPHome
  2026.10.2; issue forms for new hood profiles, bugs and questions.
