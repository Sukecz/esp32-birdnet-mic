#!/usr/bin/env python3
"""Prepare a project-local Arduino SD library with stable SPI ready handling.

The installed ESP32 core is never modified. All four supported XIAO targets
use this generated library. Adafruit SdFat also primes MISO after CS assertion,
waits for exactly 0xFF, and clocks the bus after CS release.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_VERSION = '3.3.12'
EXPECTED_SOURCE_SHA256 = 'fda00b53f23d4d93ab63a506b3197f27e24278a3f0d5da9c7590fff5ec37aa05'


def function_span(source, signature):
    start = source.index(signature)
    at = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return start, at


def patch_source(source):
    if hashlib.sha256(source.encode()).hexdigest() != EXPECTED_SOURCE_SHA256:
        raise ValueError('Arduino SD source differs from reviewed ESP32 3.3.12; review the SPI compatibility patch before building')
    start, end = function_span(source, 'bool sdWait(')
    ready = source[start:end]
    ready = ready.replace('char resp;', 'uint8_t resp;')
    ready = ready.replace('resp == 0x00', 'resp != 0xFF')
    ready = ready.replace('if (!resp)', 'if (resp != 0xFF)')
    ready = ready.replace('return (resp > 0x00);', 'return resp == 0xFF;')
    source = source[:start] + ready + source[end:]
    for signature, level, comment in [
        ('bool sdSelectCard(', 'LOW', 'Prime MISO busy status after asserting CS.'),
        ('void sdDeselectCard(', 'HIGH', 'Release MISO with eight extra clocks after deasserting CS.'),
    ]:
        start, end = function_span(source, signature)
        function = source[start:end]
        anchor = f'  digitalWrite(card->ssPin, {level});'
        if function.count(anchor) != 1:
            raise ValueError('Unexpected SD SPI select function')
        function = function.replace(anchor, anchor + f'\n  // BirdNETMic: {comment}\n  card->spi->transfer(0xFF);')
        source = source[:start] + function + source[end:]
    return source


def prepare(source_dir, output_dir):
    source_dir, output_dir = Path(source_dir).resolve(), Path(output_dir).resolve()
    if source_dir == output_dir or source_dir in output_dir.parents:
        raise ValueError('Generated SD library must be outside the installed library')
    original = (source_dir / 'src/sd_diskio.cpp').read_text()
    patched = patch_source(original)  # Validate before creating any output.
    props = (source_dir / 'library.properties').read_text()
    if f'version={EXPECTED_VERSION}\n' not in props:
        raise ValueError('Unexpected Arduino SD library version')
    for source in source_dir.rglob('*'):
        if not source.is_file():
            continue
        rel = source.relative_to(source_dir)
        dest = output_dir / rel
        data = source.read_bytes()
        if rel.as_posix() == 'src/sd_diskio.cpp':
            data = patched.encode()
        elif rel.as_posix() == 'library.properties':
            data = props.replace('name=SD\n', 'name=BirdNETMic SD SPI compatibility\n').encode()
        if not dest.exists() or dest.read_bytes() != data:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
    return output_dir


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'esp32-birdnet-mic/build/sd-compat')
    args = parser.parse_args()
    lock = json.loads((ROOT / 'build-dependencies.json').read_text())
    if lock['core'] != {'id': 'esp32:esp32', 'version': EXPECTED_VERSION}:
        raise SystemExit('Review SD SPI compatibility patch when changing the pinned ESP32 core')
    source = args.source_dir
    if source is None:
        data = subprocess.check_output(['arduino-cli', 'config', 'get', 'directories.data'], text=True).strip()
        source = Path(data).expanduser() / 'packages/esp32/hardware/esp32' / EXPECTED_VERSION / 'libraries/SD'
    try:
        print(prepare(source, args.output_dir))
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from None


if __name__ == '__main__':
    main()
