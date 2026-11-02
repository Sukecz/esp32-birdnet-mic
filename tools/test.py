#!/usr/bin/env python3
"""Run reproducible host/browser/release checks with timings and JSON results."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from board_registry import ROOT, boards


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('suite', choices=['host', 'browser', 'release', 'all'])
    p.add_argument('--filter', default='', help='Run matching JS filenames only')
    args = p.parse_args()
    env = dict(os.environ)
    env['NODE_PATH'] = str(ROOT / 'tools/node_modules') + os.pathsep + env.get('NODE_PATH', '')
    if args.suite in ('browser', 'all') and not env.get('CHROMIUM'):
        try:
            env['CHROMIUM'] = subprocess.check_output(
                ['node', '-e', "process.stdout.write(require('playwright-core').chromium.executablePath())"],
                cwd=ROOT, env=env, text=True, stderr=subprocess.PIPE)
        except (OSError, subprocess.CalledProcessError):
            p.error('Install browser dependencies: npm ci --prefix tools --ignore-scripts')
    if args.suite in ('browser', 'all') and not Path(env['CHROMIUM']).is_file():
        p.error('Chromium missing: run tools/node_modules/.bin/playwright-core install chromium, or set CHROMIUM to an installed executable')
    reports = []
    def run(name, command, timeout=300):
        started = time.monotonic()
        print(f'Running {name}', flush=True)
        try:
            code = subprocess.run(command, cwd=ROOT, env=env, timeout=timeout).returncode
        except (subprocess.TimeoutExpired, OSError) as error:
            print(str(error), file=sys.stderr)
            code = 1
        reports.append({'name': name, 'seconds': round(time.monotonic()-started, 2), 'exit_code': code})
        return code == 0
    with tempfile.TemporaryDirectory() as directory:
        if args.suite in ('host', 'all'):
            boards()  # Validate registry before expensive checks.
            run('python + native regressions', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py'])
            for name in ('discovery_progress', 'discovery_client', 'time_zone_schedule'):
                binary = str(Path(directory) / name)
                if run(name + ' compile', ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-Itests/stubs', f'tests/{name}_test.cpp', '-o', binary]):
                    run(name, [binary])
        if args.suite in ('host', 'browser', 'all'):
            for path in sorted((ROOT / 'tests').glob('*_test.js')):
                browser = 'playwright' in path.read_text()
                if args.suite == 'host' and browser or args.suite == 'browser' and not browser:
                    continue
                if args.filter not in path.name:
                    continue
                run(path.name, ['node', str(path)], timeout=180)
        if args.suite in ('release', 'all'):
            run('release artifact contract', [sys.executable, 'tests/validate_ota_contract.py'])
    destination = ROOT / 'tmp/test-results'
    destination.mkdir(parents=True, exist_ok=True)
    suffix = '-' + args.filter.replace('/', '_').replace('\\', '_') if args.filter else ''
    (destination / f'{args.suite}{suffix}.json').write_text(json.dumps(reports, indent=2) + '\n')
    for report in reports:
        print(f"{'PASS' if report['exit_code'] == 0 else 'FAIL'} {report['seconds']:7.2f}s {report['name']}")
    return int(any(r['exit_code'] for r in reports))


if __name__ == '__main__':
    raise SystemExit(main())
