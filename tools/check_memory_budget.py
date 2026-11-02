#!/usr/bin/env python3
"""Check actual partition table, app size and ELF static RAM before packaging.

Static RAM is not a runtime heap guarantee: stacks, DMA, fragmentation and
capability-specific allocation must also be qualified on hardware.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from board_registry import target


def partitions(raw):
    result = []
    for pos in range(0, len(raw), 32):
        entry = raw[pos:pos + 32]
        if len(entry) != 32:
            raise ValueError('Truncated partition entry')
        magic = struct.unpack_from('<H', entry)[0]
        if magic in (0xffff, 0xebeb):
            break
        if magic != 0x50aa:
            raise ValueError('Invalid partition table')
        _, kind, subtype, offset, size, label, _ = struct.unpack('<HBBII16sI', entry)
        result.append((kind, subtype, offset, size, label.rstrip(b'\0').decode()))
    if not result:
        raise ValueError('Empty partition table')
    return result


def validate(b, app_bytes, table, sections):
    errors = []
    ordered = sorted(table, key=lambda p: p[2])
    previous_end = 0x9000
    for kind, subtype, offset, length, label in ordered:
        if length <= 0 or offset < previous_end or offset + length > b['flash_bytes']:
            errors.append(f'Invalid/overlapping/out-of-flash partition: {label}')
        previous_end = offset + length
    slots = [p for p in table if p[0] == 0 and p[1] in (0x10, 0x11)]
    if len(slots) != 2 or {p[1] for p in slots} != {0x10, 0x11}:
        errors.append('Two OTA application slots are required')
    if any(p[3] != b['ota_slot_bytes'] for p in slots):
        errors.append('OTA slot size differs from board registry')
    if app_bytes <= 0 or app_bytes > b['ota_slot_bytes']:
        errors.append('Application does not fit OTA slot')
    names = ('.dram0.data', '.dram0.bss', '.dram1.data', '.dram1.bss', '.noinit')
    if not any(n in sections for n in names) or '.iram0.text' not in sections:
        errors.append('Missing internal memory sections in ELF')
    data = sum(sections.get(n, 0) for n in names)
    iram = sum(size for name, size in sections.items()
               if name.startswith('.iram') and not name.endswith(('.end', '.text_end')))
    if data > b['static_data_limit_bytes']:
        errors.append('Static data exceeds Arduino target RAM limit')
    if data + iram > b['sram_bytes']:
        errors.append('Static internal RAM exceeds physical SRAM')
    external = sum(size for name, size in sections.items()
                   if name.startswith('.ext_ram.') and not name.endswith('.dummy'))
    if external > b['enabled_psram_bytes']:
        errors.append('Static external RAM exceeds enabled PSRAM')
    return {'board': b['key'], 'app_bytes': app_bytes,
            'ota_remaining_bytes': b['ota_slot_bytes'] - app_bytes,
            'static_data_bytes': data, 'static_iram_bytes': iram,
            'static_psram_bytes': external, 'errors': errors, 'ok': not errors}


def check(board_id, directory, no_psram=False):
    b = target(board_id, no_psram)
    prefix = Path(directory) / 'esp32-birdnet-mic.ino'
    output = subprocess.check_output(['size', '-A', str(prefix) + '.elf'], text=True)
    sections = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0].startswith('.'):
            sections[parts[0]] = int(parts[1])
    report = validate(b, Path(str(prefix) + '.bin').stat().st_size,
                      partitions(Path(str(prefix) + '.partitions.bin').read_bytes()), sections)
    config = json.loads((Path(directory) / 'build.options.json').read_text())
    if config['fqbn'] != b['fqbn']:
        report['errors'].append('Build FQBN differs from selected registry target')
        report['ok'] = False
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('board')
    p.add_argument('directory', type=Path)
    p.add_argument('--no-psram', action='store_true')
    args = p.parse_args()
    report = check(args.board, args.directory, args.no_psram)
    print(json.dumps(report, indent=2))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
