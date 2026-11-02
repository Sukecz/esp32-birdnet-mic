#!/usr/bin/env python3
"""Assemble readable UI sources without adding runtime requests or dependencies."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'esp32-birdnet-mic/webui/src'
OUTPUT = ROOT / 'esp32-birdnet-mic/webui/index.html'
INCLUDE = re.compile(r'<!-- @include "([^"]+)" -->|/\* @include "([^"]+)" \*/')


def assemble(source=SOURCE, entry='index.html'):
    source = Path(source).resolve()

    def render(path, stack):
        path = path.resolve()
        if not path.is_relative_to(source):
            raise ValueError('UI include escapes source directory')
        if path in stack:
            raise ValueError('Circular UI include: ' + str(path.relative_to(source)))
        text = path.read_text(encoding='utf-8')
        if '@include' in INCLUDE.sub('', text):
            raise ValueError('Malformed UI include: ' + str(path.relative_to(source)))
        return INCLUDE.sub(lambda m: render(path.parent / (m[1] or m[2]), (*stack, path)), text)

    return render(source / entry, ())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    expected = assemble().encode('utf-8')
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_bytes() != expected:
            raise SystemExit('Stale webui/index.html; run esp32-birdnet-mic/tools/gen_webui_gzip_header.sh')
    elif not OUTPUT.is_file() or OUTPUT.read_bytes() != expected:
        OUTPUT.write_bytes(expected)


if __name__ == '__main__':
    main()
