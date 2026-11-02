#!/usr/bin/env python3
"""Bind exported binaries to source/configuration inputs; reject stale packaging."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from board_registry import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest():
    files = [p for p in (ROOT / 'esp32-birdnet-mic').rglob('*')
             if p.is_file() and not any(part.startswith('build') for part in p.relative_to(ROOT / 'esp32-birdnet-mic').parts[:-1])
             and p.suffix in ('.h', '.cpp', '.ino', '.html', '.js', '.css', '.sh')]
    files += [ROOT / name for name in ('boards.json', 'build-dependencies.json',
              'tools/compile_firmware.sh', 'tools/prepare_sd_library.py',
              'tools/board_registry.py', 'tools/build_receipt.py',
              'tools/assemble_webui.py', 'tools/minify_webui.cjs',
              'tools/package-lock.json', 'tools/requirements-webui.txt',
              'esp32-birdnet-mic/tools/gen_webui_assets.py',
              'tools/partitions/birdnet_4mb.csv')]
    fingerprint = hashlib.sha256()
    for path in sorted(files):
        fingerprint.update(str(path.relative_to(ROOT)).encode() + b'\0')
        fingerprint.update(bytes.fromhex(digest(path)))
    return fingerprint.hexdigest()


def artifacts(directory):
    # Includes ELF/config and all exported flash payloads, but excludes the receipt.
    paths = list(directory.glob('*.bin')) + list(directory.glob('*.elf'))
    paths += [directory / 'build.options.json']
    if (directory / 'flash_args').exists():
        paths.append(directory / 'flash_args')
    return {p.name: digest(p) for p in sorted(paths)}


def record(directory, expected_source=None):
    source = source_digest()
    if expected_source is not None and source != expected_source:
        raise ValueError('Sources changed during compilation; compile again')
    try:
        revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True, stderr=subprocess.DEVNULL).strip()
        dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True))
    except (OSError, subprocess.CalledProcessError):
        revision, dirty = None, None  # Source ZIP builds remain supported.
    receipt = {'schema_version': 1, 'source_sha256': source,
               'source_commit': revision, 'working_tree_dirty': dirty,
               'artifacts': artifacts(directory)}
    (directory / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')


def verify(directory):
    receipt = json.loads((directory / 'build-receipt.json').read_text())
    if receipt.get('schema_version') != 1 or receipt['source_sha256'] != source_digest():
        raise ValueError('Stale build: sources/configuration changed; compile again')
    if receipt['artifacts'] != artifacts(directory):
        raise ValueError('Build artifacts changed after compilation; compile again')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['record', 'verify', 'fingerprint'])
    p.add_argument('directory', type=Path, nargs='?')
    p.add_argument('--expected-source')
    args = p.parse_args()
    if args.action == 'fingerprint':
        print(source_digest())
    elif args.directory is None:
        p.error('A build directory is required')
    elif args.action == 'record':
        record(args.directory, args.expected_source)
    else:
        verify(args.directory)


if __name__ == '__main__':
    main()
