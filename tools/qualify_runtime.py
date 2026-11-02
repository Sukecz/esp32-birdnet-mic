#!/usr/bin/env python3
"""Read-only capture/memory soak; optional Listen consumer, no settings writes."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import threading
import time
import urllib.request
from urllib.parse import urlsplit, urlunsplit
from board_registry import boards
from check_device_status import validate


def sample(status, audio, board, microphone_absent=False, sd_absent=False):
    errors = validate(status, audio, datetime.now(timezone.utc), 'xiao-esp32' + board['id'])['errors']
    if microphone_absent:
        errors = [e for e in errors if e != 'Microphone capture is not healthy']
    if sd_absent and status.get('recording', {}).get('card_ready'):
        errors.append('Unexpected ready SD card in a peripherals-absent test')
    result = {'microphone': audio.get('mic_signal'), 'uptime': status.get('uptime'), 'streaming': status.get('streaming'),
              'firmware': status.get('fw_version'), 'i2s_errors': audio.get('i2s_error_count'),
              'rb_drops': audio.get('rb_drops'), 'dma_overruns': audio.get('dma_overruns_total')}
    for key in ('internal_heap_free', 'internal_heap_min', 'internal_heap_largest', 'psram_total', 'psram_free'):
        value = status.get(key + '_kb')
        if not isinstance(value, (int, float)) or value < 0:
            errors.append('Missing/invalid memory telemetry: ' + key)
            continue
        result[key + '_bytes'] = int(value * 1024)  # Existing API fields are integer KiB.
    if result.get('psram_total_bytes', 0) > board['psram_bytes']:
        errors.append('Reported PSRAM exceeds board capacity')
    if result.get('psram_free_bytes', 0) > result.get('psram_total_bytes', 0):
        errors.append('Free PSRAM exceeds detected PSRAM')
    if result.get('internal_heap_free_bytes', 0) > board['sram_bytes']:
        errors.append('Internal heap exceeds physical SRAM')
    result['errors'] = errors
    return result


def summarize(samples):
    errors = sorted({error for s in samples for error in s['errors']})
    if not samples:
        errors.append('No samples collected')
    if len({s.get('firmware') for s in samples}) > 1:
        errors.append('Firmware changed during observation')
    for key in ('i2s_errors', 'dma_overruns'):
        values = [s[key] for s in samples if isinstance(s.get(key), (float, int))]
        if values and max(values) > min(values):
            errors.append(key + ' increased')
    if samples and all(s.get('streaming') for s in samples):
        values = [s['rb_drops'] for s in samples if isinstance(s.get('rb_drops'), int)]
        if values and max(values) > min(values):
            errors.append('RTSP ring drops increased while streaming')
    minima = {key: min(s[key] for s in samples if key in s) for key in
              ('internal_heap_free_bytes', 'internal_heap_min_bytes', 'internal_heap_largest_bytes', 'psram_free_bytes')
              if any(key in s for s in samples)}
    return {'ok': not errors, 'errors': errors, 'minima': minima,
            'streaming_all_samples': bool(samples) and all(s.get('streaming') for s in samples)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('url')
    p.add_argument('--board', required=True, choices=[b['id'] for b in boards()])
    p.add_argument('--seconds', type=float, default=300)
    p.add_argument('--interval', type=float, default=2)
    p.add_argument('--listen', action='store_true')
    p.add_argument('--microphone-absent', action='store_true', help='Validate web/memory with no microphone; never qualifies audio')
    p.add_argument('--sd-absent', action='store_true', help='Expect storage unavailable; never qualifies recording')
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    if args.seconds <= 0 or args.interval <= 0:
        p.error('Duration and interval must be positive')
    board = next(b for b in boards() if b['id'] == args.board)
    # Verify identity before opening even the optional audio consumer.
    def get(path):
        with urllib.request.urlopen(args.url.rstrip('/') + path, timeout=8) as response:
            return json.load(response)
    status = get('/api/status')
    if status.get('board_id') != 'xiao-esp32' + args.board:
        raise SystemExit('Unexpected board identity')
    stop = threading.Event()
    listen = {'bytes': 0, 'error': None}
    def drain():
        address = urlsplit(args.url)
        url = urlunsplit((address.scheme, address.hostname + ':81', '/listen.wav', '', ''))
        try:
            with urllib.request.urlopen(url, timeout=5) as response:
                while not stop.is_set():
                    data = response.read(8192)
                    if not data:
                        raise OSError('Listen ended before observation completed')
                    listen['bytes'] += len(data)
        except Exception as error:
            if not stop.is_set():
                listen['error'] = type(error).__name__
    thread = threading.Thread(target=drain, daemon=True)
    if args.listen:
        thread.start()
    samples, started = [], time.monotonic()
    started_utc = datetime.now(timezone.utc).isoformat()
    try:
        while True:
            try:
                samples.append(sample(get('/api/status'), get('/api/audio_status'), board, args.microphone_absent, args.sd_absent))
            except Exception as error:
                samples.append({'errors': ['Status read failed: ' + type(error).__name__]})
            if time.monotonic() - started >= args.seconds:
                break
            time.sleep(min(args.interval, max(0, args.seconds - (time.monotonic() - started))))
    finally:
        stop.set()
        if args.listen:
            thread.join(timeout=7)
    report = {'board': board['id'], 'firmware': status.get('fw_version'),
              'started_utc': started_utc, 'memory_resolution_bytes': 1024, 'duration_seconds': round(time.monotonic()-started, 2),
              'listen_requested': args.listen, 'listen': listen, 'samples': samples, **summarize(samples),
              'qualification': 'web_memory_missing_peripherals' if args.microphone_absent else 'observed_workload_only', 'microphone_expected': not args.microphone_absent, 'storage_qualified': False, 'unmeasured_boards': [b['id'] for b in boards() if b['id'] != args.board]}
    if args.listen and (listen['error'] or listen['bytes'] <= 44):
        report['ok'] = False; report['errors'].append('Listen did not remain healthy')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'samples'}, indent=2))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
