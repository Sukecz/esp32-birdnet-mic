#!/usr/bin/env python3
"""Rebuild the shared browser flasher with the S31-capable esptool-js dependency."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    lockdir = ROOT / 'tools/web-tools'
    meta = json.loads((lockdir / 'provenance.json').read_text())
    with tempfile.TemporaryDirectory(dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        archive = work / 'source.tar.gz'
        with urllib.request.urlopen(meta['source_url'], timeout=60) as response:
            archive.write_bytes(response.read())
        if hashlib.sha256(archive.read_bytes()).hexdigest() != meta['source_sha256']:
            raise ValueError('Upstream source archive differs from reviewed source')
        with tarfile.open(archive) as source:
            source.extractall(work, filter='data')
        source = next(p for p in work.iterdir() if p.is_dir())
        for name in ('package.json', 'package-lock.json'):
            shutil.copy2(lockdir / name, source / name)
        subprocess.run(['npm', 'ci', '--ignore-scripts', '--no-audit', '--no-fund'], cwd=source, check=True)
        subprocess.run(['bash', 'script/build'], cwd=source, check=True)
        destination = ROOT / 'web-flasher/vendor/esp-web-tools' / meta['bundle_version']
        generated = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (source / 'dist/web').glob('*.js')}
        if generated != meta['outputs']:
            raise ValueError('Generated bundle differs; review before updating provenance')
        destination.mkdir(parents=True, exist_ok=True)
        for name in generated:
            shutil.copy2(source / 'dist/web' / name, destination / name)
    print('Browser flasher rebuilt; output hashes match reviewed bundle')


if __name__ == '__main__':
    main()
