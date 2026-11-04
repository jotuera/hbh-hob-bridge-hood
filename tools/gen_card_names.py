#!/usr/bin/env python3
"""Check the language files and generate the NAMES and TEXTS tables of cards/hbh-cards.js.

- NAMES: original entity names (n_*) of every esphome/lang/hbh_*.yaml – the cards find the device's entities by
  these names in every language and show them in the language of Home Assistant.
- STATES: state texts (t_mode_*, t_dry_*, options) – the cards translate the texts coming from the device.
- TEXTS: texts of the cards from cards/i18n/*.json.

Run after adding or changing a language:

    python3 tools/gen_card_names.py

With --check it writes nothing and fails when cards/hbh-cards.js is not up to date (used by CI).

It stops with an error when a language file misses a key, changes a placeholder ({x}, {n} …), uses "/" in an entity
name (reserved by ESPHome) or gives two entities of one file the same name.
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARD = os.path.join(ROOT, 'cards', 'hbh-cards.js')
LINE = re.compile(r'^\s+([nt]_[a-z0-9_]+):\s*"(.*)"\s*(#.*)?$')
PH = re.compile(r'\{\w+\}')
errors = []


def read_lang(path):
    out = {}
    for line in open(path, encoding='utf-8'):
        m = LINE.match(line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def check_lang(prefix):
    files = sorted(glob.glob(os.path.join(ROOT, 'esphome', 'lang', prefix + '_*.yaml')))
    ref = read_lang(os.path.join(ROOT, 'esphome', 'lang', prefix + '_en.yaml'))
    langs = {}
    for f in files:
        lang = os.path.basename(f)[len(prefix) + 1:-5]
        d = read_lang(f)
        langs[lang] = d
        name = os.path.basename(f)
        for k in ref:
            if k not in d:
                errors.append(f'{name}: missing {k}')
        for k in d:
            if k not in ref:
                errors.append(f'{name}: unknown key {k}')
        names = [v for k, v in d.items() if k.startswith('n_')]
        for v in set(names):
            if names.count(v) > 1:
                errors.append(f'{name}: entity name used twice: "{v}"')
        for k, v in d.items():
            if k.startswith('n_') and '/' in v:
                errors.append(f'{name}: "/" in {k}')
        opts = [d.get(k) for k in ('t_off', 't_boost', 't_cool', 't_warm') if k in d]
        if len(set(opts)) != len(opts):
            errors.append(f'{name}: t_off / t_boost / t_cool / t_warm must differ')
    return langs


def check_texts():
    files = sorted(glob.glob(os.path.join(ROOT, 'cards', 'i18n', '*.json')))
    ref = json.load(open(os.path.join(ROOT, 'cards', 'i18n', 'en.json'), encoding='utf-8'))
    texts = {}

    def walk(r, d, where):
        for k, v in r.items():
            if k not in d:
                errors.append(f'{where}: missing {k}')
            elif isinstance(v, dict):
                walk(v, d[k], where + '.' + k)
            elif sorted(PH.findall(v)) != sorted(PH.findall(d[k])):
                errors.append(f'{where}: placeholders of {k} differ: {d[k]!r}')

    for f in files:
        lang = os.path.basename(f)[:-5]
        d = json.load(open(f, encoding='utf-8'))
        walk(ref, d, os.path.basename(f))
        texts[lang] = d
    return texts


hood = check_lang('hbh')
check_lang('remote_rx')
texts = check_texts()
if errors:
    print('\n'.join(errors))
    sys.exit(1)

# n_* -> {language: entity name}; t_* state texts -> {language: text} (the cards translate the device's texts)
names, states = {}, {}
for lang in ['en'] + sorted(k for k in hood if k != 'en'):
    for k, v in hood[lang].items():
        if k.startswith('n_'):
            names.setdefault(k, {})[lang] = v
        elif k.startswith(('t_mode_', 't_dry_', 't_cal_')) or k in ('t_off', 't_boost', 't_cool', 't_warm'):
            states.setdefault(k, {})[lang] = v

src = open(CARD, encoding='utf-8').read()


def put(src, tag, body):
    a = src.index(f'// {tag}-BEGIN\n') + len(f'// {tag}-BEGIN\n')
    b = src.index(f'// {tag}-END')
    return src[:a] + body + src[b:]


src = put(src, 'NAMES', 'const NAMES = {\n' + ''.join(f'  {k}: {json.dumps(v, ensure_ascii=False)},\n' for k, v in sorted(names.items())) + '};\n')
src = put(src, 'STATES', 'const STATES = {\n' + ''.join(f'  {k}: {json.dumps(v, ensure_ascii=False)},\n' for k, v in sorted(states.items())) + '};\n')
order = ['en'] + sorted(k for k in texts if k != 'en')
src = put(src, 'TEXTS', 'const TEXTS = {\n' + ''.join(f'  {k}: {json.dumps(texts[k], ensure_ascii=False)},\n' for k in order) + '};\n')
if '--check' in sys.argv[1:]:
    if src != open(CARD, encoding='utf-8').read():
        sys.exit('cards/hbh-cards.js is not up to date - run python3 tools/gen_card_names.py and commit it')
    print('cards/hbh-cards.js is up to date')
else:
    open(CARD, 'w', encoding='utf-8', newline='\n').write(src)
print(f'{len(names)} entity names in {len(hood)} languages, card texts in {len(texts)} languages: {", ".join(order)}')
