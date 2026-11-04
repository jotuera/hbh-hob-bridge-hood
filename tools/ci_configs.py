#!/usr/bin/env python3
"""Write ESPHome device files for every language and profile into build/ci/ (used by CI, also handy locally).

    python3 tools/ci_configs.py
    esphome config build/ci/hood_en.yaml          # validate one
    esphome compile build/ci/hood_en.yaml         # build one

build/ci/ gets a copy of esphome/ as hbh/ (the layout of the README), dummy secrets and one file per device,
language and profile: hood_<lang>.yaml, rx_<lang>.yaml (profile ciarko_xy8011) and hood_<profile>.yaml for the
other profiles.
"""
import base64
import glob
import os
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'ci')
DEFAULT_PROFILE = 'ciarko_xy8011'

SECRETS = f'''wifi_ssid: "ci"
wifi_password: "ci-password"
fallback_ap_password: "ci-password"
hbh_api_key: "{base64.b64encode(bytes(range(32))).decode()}"
hbh_remote_rx_api_key: "{base64.b64encode(bytes(range(32, 64))).decode()}"
'''

HOOD = '''substitutions:
  name: hbh-hood
  friendly_name: HbH Hood
  src_hood_power: sensor.hood_power
  src_hob_power: sensor.hob_l1_power+sensor.hob_l2_power

packages:
  profile: !include hbh/profiles/{profile}.yaml
  lang: !include hbh/lang/hbh_{lang}.yaml
  hood: !include hbh/hbh.yaml
'''

RX = '''packages:
  profile: !include hbh/profiles/{profile}.yaml
  lang: !include hbh/lang/remote_rx_{lang}.yaml
  rx: !include hbh/hbh-remote-rx.yaml
'''


def main():
    if os.path.isdir(os.path.join(OUT, 'hbh')):
        shutil.rmtree(os.path.join(OUT, 'hbh'))
    os.makedirs(OUT, exist_ok=True)
    for f in glob.glob(os.path.join(OUT, '*.yaml')):
        os.remove(f)
    shutil.copytree(os.path.join(ROOT, 'esphome'), os.path.join(OUT, 'hbh'),
                    ignore=shutil.ignore_patterns('secrets.yaml', '.esphome'))

    def write(name, text):
        with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)

    write('secrets.yaml', SECRETS)
    langs = sorted(os.path.basename(f)[4:-5] for f in glob.glob(os.path.join(ROOT, 'esphome', 'lang', 'hbh_*.yaml')))
    profiles = sorted(os.path.basename(f)[:-5] for f in glob.glob(os.path.join(ROOT, 'esphome', 'profiles', '*.yaml')))
    for lang in langs:
        write(f'hood_{lang}.yaml', HOOD.format(profile=DEFAULT_PROFILE, lang=lang))
        write(f'rx_{lang}.yaml', RX.format(profile=DEFAULT_PROFILE, lang=lang))
    for p in profiles:
        if p != DEFAULT_PROFILE:
            write(f'hood_{p}.yaml', HOOD.format(profile=p, lang='en'))
            write(f'rx_{p}.yaml', RX.format(profile=p, lang='en'))
    print(f'{OUT}: {len(langs)} languages, profiles: {", ".join(profiles)}')


if __name__ == '__main__':
    main()
