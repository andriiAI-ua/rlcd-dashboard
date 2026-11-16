#!/usr/bin/env python3
# Make the GitHub Pages site of a release: the web flasher (ESP Web Tools: Chrome or Edge flash the board through
# Web Serial) and the archive for the update over Wi-Fi.
#
#   python3 tools/web-flasher/make_site.py <build folder> <site folder> <version>
#
# The site: index.html (the flasher page), manifest.json (the parts and their offsets from flasher_args.json of
# the build: no offsets in this script), firmware/*.bin, rlcd-dashboard.tar (the last release),
# rlcd-dashboard-<version>.tar and dashboard.png (docs/screens/uk).
import glob
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
CHIP_FAMILY = {'esp32s3': 'ESP32-S3', 'esp32': 'ESP32', 'esp32s2': 'ESP32-S2', 'esp32c3': 'ESP32-C3'}


def main():
    if len(sys.argv) != 4:
        sys.exit('usage: make_site.py <build folder> <site folder> <version>')
    build, site, version = sys.argv[1:]
    os.makedirs(os.path.join(site, 'firmware'), exist_ok=True)

    with open(os.path.join(build, 'flasher_args.json'), encoding='utf-8') as f:
        args = json.load(f)
    chip = args['extra_esptool_args']['chip']
    parts = []
    for offset, rel in sorted(args['flash_files'].items(), key=lambda item: int(item[0], 16)):
        name = os.path.basename(rel)
        shutil.copyfile(os.path.join(build, rel), os.path.join(site, 'firmware', name))
        parts.append({'path': 'firmware/' + name, 'offset': int(offset, 16)})
    manifest = {
        'name': 'rlcd-dashboard',
        'version': version,
        'new_install_prompt_erase': True,
        'new_install_improv_wait_time': 0,      # the firmware has no Improv Wi-Fi: its own access point
        'builds': [{'chipFamily': CHIP_FAMILY[chip], 'parts': parts}],
    }
    with open(os.path.join(site, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=2)

    archives = glob.glob(os.path.join(build, 'rlcd-dashboard-*.tar'))
    if len(archives) != 1:
        sys.exit('make_site.py: one archive rlcd-dashboard-*.tar is necessary, found %d' % len(archives))
    archive = os.path.basename(archives[0])
    shutil.copyfile(archives[0], os.path.join(site, archive))
    shutil.copyfile(archives[0], os.path.join(site, 'rlcd-dashboard.tar'))
    shutil.copyfile(os.path.join(ROOT, 'docs', 'screens', 'uk', 'dashboard.png'), os.path.join(site, 'dashboard.png'))

    with open(os.path.join(HERE, 'index.html'), encoding='utf-8') as f:
        page = f.read()
    page = page.replace('{{VERSION}}', version).replace('{{ARCHIVE}}', archive)
    with open(os.path.join(site, 'index.html'), 'w', encoding='utf-8') as f:
        f.write(page)
    for part in parts:
        print('%#08x  %s' % (part['offset'], part['path']))
    print('site: %s (%s)' % (site, version))


if __name__ == '__main__':
    main()
