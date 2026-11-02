#!/usr/bin/env python3
"""Compile registry targets without modifying release/public firmware artifacts."""
import argparse
import json
import subprocess
from board_registry import ROOT, boards, target
from check_memory_budget import check


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--board', choices=[b['id'] for b in boards()])
    p.add_argument('--all', action='store_true')
    p.add_argument('--no-psram', action='store_true')
    p.add_argument('--matrix', action='store_true', help='Include no-PSRAM variants')
    p.add_argument('--clean', action='store_true')
    args = p.parse_args()
    if bool(args.board) == bool(args.all) or (args.no_psram and args.all):
        p.error('Choose --board ID [--no-psram], or --all [--matrix]')
    subprocess.run(['python3', 'tools/check_build_dependencies.py'], cwd=ROOT, check=True)
    subprocess.run(['esp32-birdnet-mic/tools/gen_webui_gzip_header.sh', '--check'], cwd=ROOT, check=True)
    selected = [target(args.board, args.no_psram)] if args.board else [
        target(b['id'], disabled) for b in boards()
        for disabled in ([False, True] if args.matrix and b['psram_bytes'] else [False])]
    for b in selected:
        directory = ROOT / 'tmp' / 'build' / b['key']
        command = ['tools/compile_firmware.sh', '--fqbn', b['fqbn'], '--output-dir', str(directory)]
        if args.clean:
            command.append('--clean')
        subprocess.run(command, cwd=ROOT, check=True)
        report = check(b['id'], directory, b['enabled_psram_bytes'] == 0 and b['psram_bytes'] > 0)
        (directory / 'memory-report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report), flush=True)
        if not report['ok']:
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
