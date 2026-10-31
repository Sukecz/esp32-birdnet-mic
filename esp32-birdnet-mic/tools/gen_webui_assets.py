#!/usr/bin/env python3
"""Generate deterministic minified/gzip assets, keeping readable UI sources."""
import argparse
import gzip
from pathlib import Path
import sys
import subprocess

try:
    import zopfli.gzip
except ImportError:
    raise SystemExit('Missing Zopfli. Install tools/requirements-webui.txt in your Python environment.')

ROOT = Path(__file__).resolve().parents[1]


def make_header(assets):
    output = ['#pragma once', '#include <Arduino.h>', '#include <pgmspace.h>', '']
    for symbol, filename in assets:
        raw = (ROOT / 'webui' / filename).read_bytes()
        if filename.endswith(('.html', '.js', '.css')):
            raw = subprocess.check_output(['node', str(ROOT.parent / 'tools/minify_webui.cjs'), filename.rsplit('.', 1)[-1]], input=raw)
        packed = zopfli.gzip.compress(raw, numiterations=15)
        if gzip.decompress(packed) != raw:
            raise RuntimeError(f'Compression roundtrip failed: {filename}')
        output.append(f'static const uint8_t {symbol}[] PROGMEM = {{')
        for start in range(0, len(packed), 12):
            output.append('  ' + ', '.join(f'0x{byte:02x}' for byte in packed[start:start + 12]) + ',')
        output.extend(['};', f'static const size_t {symbol}_LEN = {len(packed)};', ''])
        print(f'{filename}: {len(raw)} -> {len(packed)} bytes', file=sys.stderr)
    return '\n'.join(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Verify generated headers without changing them')
    args = parser.parse_args()
    for filename, assets in [
        ('WebUI_gz.h', [('WEBUI_INDEX_GZ', 'index.html')]),
        ('OtaAssets_gz.h', [('OTA_CSS_GZ', 'ota.css'), ('OTA_JS_GZ', 'ota.js')]),
        ('SetupAssets_gz.h', [('SETUP_HTML_GZ', 'setup.html'), ('SETUP_CSS_GZ', 'setup.css'), ('SETUP_JS_GZ', 'setup.js'), ('AP_PASSWORD_JS_GZ', 'ap-password.js')]),
    ]:
        path = ROOT / filename
        expected = make_header(assets)
        if args.check:
            if not path.is_file() or path.read_text() != expected:
                raise SystemExit(f'Stale {filename}; run tools/gen_webui_gzip_header.sh')
        else:
            path.write_text(expected)


if __name__ == '__main__':
    main()
