#!/usr/bin/env python3
"""Shared target configurations for local builds, packaging and CI."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def boards():
    data = json.loads((ROOT / 'boards.json').read_text())
    if data['schema_version'] != 1:
        raise ValueError('Unsupported board registry schema')
    result = data['boards']
    for field in ('id', 'fqbn'):
        if len({b[field] for b in result}) != len(result):
            raise ValueError(f'Duplicate {field}')
    for b in result:
        if not (0 < b['static_data_limit_bytes'] <= b['sram_bytes'] < b['flash_bytes']):
            raise ValueError(f"Invalid RAM limits: {b['id']}")
        if 2 * b['ota_slot_bytes'] >= b['flash_bytes']:
            raise ValueError(f"OTA slots do not fit: {b['id']}")
        if bool(b['psram_bytes']) != bool(b['psram_option']):
            raise ValueError(f"Invalid PSRAM configuration: {b['id']}")
    return result


def target(board_id, no_psram=False):
    b = next(b for b in boards() if b['id'] == board_id)
    if no_psram and not b['psram_bytes']:
        raise ValueError(f'{board_id} has no PSRAM variant')
    option = 'disabled' if no_psram else b['psram_option']
    return {**b, 'key': board_id + ('-no-psram' if no_psram else ''),
            'fqbn': b['fqbn'] + (f':PSRAM={option}' if option else ''),
            'enabled_psram_bytes': 0 if no_psram else b['psram_bytes']}


def matrix():
    return {'include': [target(b['id'], disabled) for b in boards()
                        for disabled in ([False, True] if b['psram_bytes'] else [False])]}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--matrix', action='store_true')
    p.add_argument('--package-list', action='store_true')
    args = p.parse_args()
    if args.package_list:
        for b in boards():
            print('|'.join((b['id'], b['chip'], b['fqbn'])))
    else:
        print(json.dumps(matrix() if args.matrix else boards()))


if __name__ == '__main__':
    main()
