"""Post-OTA validation must reject the original NO/never bug even with a valid RTC."""
import copy
from datetime import datetime, timezone
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('device_status_check', ROOT / 'tools/check_device_status.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class DeviceStatusCheckTests(unittest.TestCase):
    def setUp(self):
        self.now = datetime(2026, 10, 28, 9, 0, 0, tzinfo=timezone.utc)
        self.status = {'board_id': 'xiao-esp32c6', 'fw_version': '1.36',
                       'time_sync_enabled': True, 'time_synced': True, 'time_valid': True,
                       'last_time_sync': '2s ago', 'utc_time': '2026-10-28 09:00:00',
                       'local_time': '2026-10-28 10:00:00', 'effective_time_offset_min': 60,
                       'setup': {'connected': True, 'connection': 'home'}}
        self.audio = {'producer_running': True, 'mic_signal': 'ok', 'i2s_error_count': 0,
                      'dma_overruns_available': True, 'dma_overruns_total': 0}

    def test_rejects_original_bug_stale_clock_and_wrong_timezone(self):
        self.assertTrue(checker.validate(self.status, self.audio, self.now)['ok'])
        for changes in ({'time_synced': False, 'last_time_sync': 'never'},
                        {'time_valid': False}, {'utc_time': '2026-10-28 08:59:00'},
                        {'local_time': '2026-10-28 09:00:00'}, {'utc_time': 'unavailable'}):
            with self.subTest(changes=changes):
                status = {**self.status, **changes}
                self.assertFalse(checker.validate(status, self.audio, self.now)['ok'])

    def test_offline_and_disabled_ntp_do_not_require_internet(self):
        status = copy.deepcopy(self.status)
        status.update(time_synced=False, time_valid=False, last_time_sync='never')
        status['setup'].update(connected=False, connection='direct')
        result = checker.validate(status, self.audio, self.now)
        self.assertTrue(result['ok'])
        self.assertFalse(result['ntp_required'])
        status['setup']['connected'] = True
        status['time_sync_enabled'] = False
        self.assertTrue(checker.validate(status, self.audio, self.now)['ok'])

    def test_still_requires_matching_board_and_healthy_microphone(self):
        self.assertFalse(checker.validate(self.status, self.audio, self.now, 'xiao-esp32c3')['ok'])
        for changes in ({'producer_running': False}, {'mic_signal': 'flatline'},
                        {'i2s_error_count': 1}, {'dma_overruns_total': 1}):
            with self.subTest(changes=changes):
                self.assertFalse(checker.validate(self.status, {**self.audio, **changes}, self.now)['ok'])


if __name__ == '__main__':
    unittest.main()
