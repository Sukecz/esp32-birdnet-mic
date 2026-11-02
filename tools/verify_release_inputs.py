#!/usr/bin/env python3
"""Verify packaged firmware against source-bound local builds before publication."""
from pathlib import Path
from board_registry import ROOT, boards
from build_receipt import digest, verify


def main():
    for b in boards():
        directory = ROOT / 'esp32-birdnet-mic/build' / b['fqbn'].replace(':', '.')
        verify(directory)
        pairs = {
            f"firmware-app-{b['id']}.bin": 'esp32-birdnet-mic.ino.bin',
            f"firmware-{b['id']}.bin": 'esp32-birdnet-mic.ino.merged.bin',
            f"bootloader-{b['id']}.bin": 'esp32-birdnet-mic.ino.bootloader.bin',
            f"partitions-{b['id']}.bin": 'esp32-birdnet-mic.ino.partitions.bin',
            f"boot_app0-{b['id']}.bin": 'boot_app0.bin',
        }
        for public, local in pairs.items():
            if digest(ROOT / 'web-flasher' / public) != digest(directory / local):
                raise ValueError(f'Packaged artifact differs from verified build: {public}')
    print('Packaged firmware matches current source-bound builds')


if __name__ == '__main__':
    main()
