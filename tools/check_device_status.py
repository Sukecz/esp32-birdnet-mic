#!/usr/bin/env python3
"""Read-only post-OTA smoke check, including actual NTP confirmation and clock skew.

Usage: python3 tools/check_device_status.py http://DEVICE_IP --board xiao-esp32c6
Only a small health summary is printed/saved; raw settings and credentials are not.
"""
import argparse
from datetime import datetime, timezone, timedelta
import json
from pathlib import Path
import time
import urllib.request


def validate(status, audio, now, expected_board=None):
    errors = []
    if expected_board and status.get('board_id') != expected_board:
        errors.append('Unexpected board identity')
    if not audio.get('producer_running') or audio.get('mic_signal') != 'ok':
        errors.append('Microphone capture is not healthy')
    if audio.get('i2s_error_count') != 0:
        errors.append('I2S error counter is not zero')
    if audio.get('dma_overruns_available') and audio.get('dma_overruns_total') != 0:
        errors.append('DMA overrun counter is not zero')
    setup = status.get('setup', {})
    needs_ntp = bool(status.get('time_sync_enabled') and setup.get('connected'))
    skew = None
    if needs_ntp:
        if not status.get('time_synced') or not status.get('time_valid'):
            errors.append('NTP has not confirmed a valid clock')
        if status.get('last_time_sync') in (None, '', 'never'):
            errors.append('No successful synchronization recorded')
        try:
            utc = datetime.strptime(status['utc_time'], '%Y-%m-%d %H:%M:%S').replace(tzinfo=timezone.utc)
            skew = round(abs((now-utc).total_seconds()), 2)
            if skew > 5:
                errors.append('UTC clock differs by more than 5 seconds')
            local = datetime.strptime(status['local_time'], '%Y-%m-%d %H:%M:%S')
            expected_local = utc.replace(tzinfo=None) + timedelta(minutes=status['effective_time_offset_min'])
            if abs((local-expected_local).total_seconds()) > 1:
                errors.append('Local time does not match the selected UTC offset')
        except (KeyError, TypeError, ValueError):
            errors.append('Missing or invalid UTC/local time')
    summary = {
        'ok': not errors, 'board': status.get('board_id'), 'firmware': status.get('fw_version'),
        'microphone': audio.get('mic_signal'), 'i2s_errors': audio.get('i2s_error_count'),
        'ntp_required': needs_ntp, 'time_synced': status.get('time_synced'),
        'utc_skew_seconds': skew, 'errors': errors,
    }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url')
    parser.add_argument('--board')
    parser.add_argument('--wait', type=float, default=60, help='Seconds to allow initial Wi-Fi/NTP startup')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    deadline = time.monotonic() + max(0, args.wait)
    while True:
        try:
            values = []
            for path in ('/api/status', '/api/audio_status'):
                with urllib.request.urlopen(args.url.rstrip('/') + path, timeout=8) as response:
                    values.append(json.load(response))
            summary = validate(*values, datetime.now(timezone.utc), args.board)
            setup = values[0].get('setup', {})
            if setup.get('connection') == 'home' and not setup.get('connected'):
                summary['ok'] = False
                summary['errors'].append('Home Wi-Fi is not connected')
        except (OSError, ValueError):
            summary = {'ok': False, 'errors': ['Device status is not reachable or invalid']}
        if summary['ok'] or time.monotonic() >= deadline:
            break
        time.sleep(1)
    text = json.dumps(summary, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')
    return 0 if summary['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
