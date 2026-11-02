#!/usr/bin/env python3
"""Reuse verified development builds for packaging, without compiling again."""
from pathlib import Path
import shutil
from board_registry import ROOT, boards
from build_receipt import verify
from check_memory_budget import check


def main():
    selected = []
    for board in boards():
        source = ROOT / 'tmp/build' / board['id']
        verify(source)
        report = check(board['id'], source)
        if not report['ok']:
            raise ValueError(str(report['errors']))
        selected.append((board, source))
    for board, source in selected:
        destination = ROOT / 'esp32-birdnet-mic/build' / board['fqbn'].replace(':', '.')
        destination.mkdir(parents=True, exist_ok=True)
        for path in source.iterdir():
            if path.is_file() and path.name != 'build-receipt.json':
                shutil.copy2(path, destination / path.name)
        shutil.copy2(source / 'build-receipt.json', destination / 'build-receipt.json')
        verify(destination)
        print(board['id'] + ': verified build reused for packaging')


if __name__ == '__main__':
    main()
