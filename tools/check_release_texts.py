#!/usr/bin/env python3
"""Check current release copy without rejecting historical changelog entries."""
import json
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[1]
def check(html, version):
    # The hero must not carry a second, manually maintained version number.
    if re.search(r'Install BirdNETMic\s+v?\d+\.\d+', html):
        raise ValueError('Hardcoded version in installation text; use the manifest badge')
    if 'Install BirdNETMic from your browser.' not in html:
        raise ValueError('Installation text missing; review current public copy')
    if 'id="fw-version-chip"' not in html:
        raise ValueError('Current release badge missing')
    if not re.fullmatch(r'\d+\.\d+',version):
        raise ValueError('Invalid release version')
def main():
    version=json.loads((ROOT/'web-flasher/manifest.json').read_text())['version']
    check((ROOT/'web-flasher/index.html').read_text(),version)
    for name in ['README.md','esp32-birdnet-mic/README.md','web-flasher/README.md','manual-ota-firmware/README.md']:
        text=(ROOT/name).read_text()
        if not re.search(r'(?:Firmware|firmware):?\s*\*{0,2}v?'+re.escape(version)+r'\b',text[:4000]):
            raise ValueError('Current firmware text missing: '+name)
    print('Current release copy checked; installation version comes from manifest')
if __name__=='__main__': main()
